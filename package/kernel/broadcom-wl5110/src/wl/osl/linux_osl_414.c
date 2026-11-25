/*
 * linux_osl_414.c - Broadcom OSL for the closed wl 5.110.27.20012 core on Linux 4.14 (OpenWrt 19.07.10).
 *
 * The closed core was compiled against Linux 2.6.22 and touches struct sk_buff fields inline, so every
 * packet it sees is a *shadow* packet (struct skb2622, see glue/skb2622.h) whose head/data/tail/end point
 * into the buffer of a real 4.14 sk_buff (shadow->native). Conversion happens only here:
 *   osl_pktget()        new native skb + shadow (RX buffers, core-generated frames)
 *   osl_pkt_frmnative() native -> shadow  (TX from the network stack, called by the glue)
 *   osl_pkt_tonative()  shadow -> native  (RX up the stack, called by the glue)
 *   osl_pktfree()       frees both
 * Semantics follow Broadcom's linux_osl.c of the same version (Asuswrt-Merlin legacy tree).
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/skbuff.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/netdevice.h>
#include <linux/bcm47xx_nvram.h>
#include <linux/moduleparam.h>

#include <typedefs.h>
#include <bcmdefs.h>
#include <osl.h>
#include <bcmutils.h>
#include <skb2622.h>

#define OS_HANDLE_MAGIC		0x1234abcd
#define OSL_PKTTAG_SZ		32	/* bytes of cb[] the core uses as wlc_pkttag_t */

struct osl_info {
	osl_pubinfo_t pub;	/* MUST be first: the core writes tx_fn/tx_ctx inline (PKTFREESETCB) */
	uint magic;
	struct device *dev;	/* DMA device (bcma 802.11 core) */
	atomic_t malloced;
	atomic_t pktalloced;
	uint failed;
	uint bustype;
};

uint32 g_assert_type = 0;		/* read by the core; 0 = Broadcom default */

static struct kmem_cache *shadow_cache;
int pktget_kmalloc = 1;
module_param(pktget_kmalloc, int, 0644);
osl_t *osl_dbg_osh;			/* last attached handle, for the 'memstat' module parameter */
static atomic_t osl_dbg_native_out = ATOMIC_INIT(0);	/* native skbs owned by shadows */

int osl_module_init(void)
{
	skb2622_layout_check();
	shadow_cache = kmem_cache_create("skb2622", sizeof(struct skb2622), 8, 0, NULL);
	return shadow_cache ? 0 : -ENOMEM;
}

void osl_module_exit(void)
{
	kmem_cache_destroy(shadow_cache);
}

/* ---------------------------------------------------------------- attach */

osl_t *osl_attach(void *pdev, uint bustype, bool pkttag)
{
	osl_t *osh = kzalloc(sizeof(*osh), GFP_ATOMIC);

	if (!osh)
		return NULL;
	osh->magic = OS_HANDLE_MAGIC;
	osh->dev = pdev;
	osh->bustype = bustype;
	osh->pub.pkttag = pkttag;
	osh->pub.mmbus = TRUE;		/* SI_BUS: memory-mapped registers */
	atomic_set(&osh->malloced, 0);
	atomic_set(&osh->pktalloced, 0);
	osl_dbg_osh = osh;
	return osh;
}

void osl_detach(osl_t *osh)
{
	if (osh) {
		WARN_ON(osh->magic != OS_HANDLE_MAGIC);
		pr_info("wl: osl_detach: pktalloced %d malloced %d native %d failed %u\n",
			atomic_read(&osh->pktalloced), atomic_read(&osh->malloced),
			atomic_read(&osl_dbg_native_out), osh->failed);
		if (osl_dbg_osh == osh)
			osl_dbg_osh = NULL;
		kfree(osh);
	}
}

/* ---------------------------------------------------------------- memory */

void *osl_malloc(osl_t *osh, uint size)
{
	void *p = kmalloc(size, in_interrupt() || irqs_disabled() ? GFP_ATOMIC : GFP_KERNEL);

	if (!p) {
		if (osh)
			osh->failed++;
		return NULL;
	}
	if (osh)
		atomic_add(size, &osh->malloced);
	return p;
}

void osl_mfree(osl_t *osh, void *addr, uint size)
{
	if (osh && addr)
		atomic_sub(size, &osh->malloced);
	kfree(addr);
}

int osl_dbg_stats(char *buf, size_t n)
{
	osl_t *osh = osl_dbg_osh;

	if (!osh)
		return scnprintf(buf, n, "no osh\n");
	return scnprintf(buf, n, "pktalloced %d malloced %d native %d failed %u\n",
		atomic_read(&osh->pktalloced), atomic_read(&osh->malloced),
		atomic_read(&osl_dbg_native_out), osh->failed);
}

uint osl_malloced(osl_t *osh)
{
	return atomic_read(&osh->malloced);
}

uint osl_malloc_failed(osl_t *osh)
{
	return osh->failed;
}

void osl_delay(uint usec)
{
	while (usec > 1000) {
		udelay(1000);
		usec -= 1000;
	}
	udelay(usec);
}

/* ---------------------------------------------------------------- shadow packets */

static struct skb2622 *shadow_bind(osl_t *osh, struct sk_buff *skb)
{
	struct skb2622 *s = kmem_cache_zalloc(shadow_cache, GFP_ATOMIC);

	if (!s)
		return NULL;
	s->native = skb;
	atomic_inc(&osl_dbg_native_out);
	s->head = skb->head;
	s->data = skb->data;
	s->tail = skb_tail_pointer(skb);
	s->end = skb_end_pointer(skb);
	s->len = skb->len;
	s->priority = skb->priority;
	s->truesize = skb->truesize;
	s->users = 1;
	if (skb_cloned(skb))
		s->flags1 |= 1 << 1;
	SKB2622_SET_IP_SUMMED(s, skb->ip_summed);
	return s;
}

/* write the core's view (data/len/tail/priority) back into the native skb */
static void shadow_sync(struct skb2622 *s)
{
	struct sk_buff *skb = s->native;

	skb->data = s->data;
	skb->len = s->len;
	skb_set_tail_pointer(skb, s->tail - s->data);
	skb->priority = s->priority;
}

void *osl_pktget(osl_t *osh, uint len)
{
	struct sk_buff *skb;
	struct skb2622 *s;

	/* kmalloc-backed data, not netdev_alloc_frag(): a long-lived RX ring buffer must not pin a whole
	 * 32 KB frag page (pktget_kmalloc=0 restores dev_alloc_skb for comparison) */
	if (pktget_kmalloc) {
		skb = alloc_skb(len + NET_SKB_PAD, GFP_ATOMIC);
		if (skb)
			skb_reserve(skb, NET_SKB_PAD);
	} else
		skb = dev_alloc_skb(len);
	if (!skb)
		return NULL;
	skb_put(skb, len);
	s = shadow_bind(osh, skb);
	if (!s) {
		dev_kfree_skb_any(skb);
		return NULL;
	}
	atomic_inc(&osh->pktalloced);
	return s;
}

/* free a chain linked through ->next; 'send' => TX completion, run the PKTFREESETCB callback first */
void osl_pktfree(osl_t *osh, void *p, bool send)
{
	struct skb2622 *s = p, *nxt;

	if (send && osh->pub.tx_fn)
		osh->pub.tx_fn(osh->pub.tx_ctx, p, 0);

	for (; s; s = nxt) {
		nxt = s->next;
		s->next = NULL;
		if (s->native) {
			dev_kfree_skb_any(s->native);
			atomic_dec(&osl_dbg_native_out);
		}
		kmem_cache_free(shadow_cache, s);
		atomic_dec(&osh->pktalloced);
	}
}

/* PKTDUP: share the data buffer (skb_clone), mark both as shared so the core copies before writing */
void *osl_pktdup(osl_t *osh, void *p)
{
	struct skb2622 *s = p, *d;
	struct sk_buff *c = skb_clone(s->native, GFP_ATOMIC);

	if (!c)
		return NULL;
	d = kmem_cache_alloc(shadow_cache, GFP_ATOMIC);
	if (!d) {
		dev_kfree_skb_any(c);
		return NULL;
	}
	memcpy(d, s, sizeof(*d));	/* same head/data/tail/end/len/cb/priority */
	d->next = d->prev = NULL;
	d->native = c;
	atomic_inc(&osl_dbg_native_out);
	s->flags1 |= 1 << 1;		/* cloned */
	d->flags1 |= 1 << 1;
	atomic_inc(&osh->pktalloced);
	return d;
}

uint osl_pktalloced(osl_t *osh)
{
	return atomic_read(&osh->pktalloced);
}

/* native (from the stack) -> shadow. The skb must be linear: the core assumes data_len == 0. */
void *osl_pkt_frmnative(osl_t *osh, void *pkt)
{
	struct sk_buff *skb = pkt;
	struct skb2622 *s;

	if (skb_linearize(skb))
		return NULL;
	s = shadow_bind(osh, skb);
	if (s) {
		memset(s->cb, 0, OSL_PKTTAG_SZ);
		atomic_inc(&osh->pktalloced);
	}
	return s;
}

/* shadow -> native (to the stack). Consumes the shadow; chains are converted element by element. */
struct sk_buff *osl_pkt_tonative(osl_t *osh, void *pkt)
{
	struct skb2622 *s = pkt, *nxt;
	struct sk_buff *first = NULL, **link = &first;

	for (; s; s = nxt) {
		nxt = s->next;
		shadow_sync(s);
		*link = s->native;
		link = &s->native->next;
		*link = NULL;
		atomic_dec(&osl_dbg_native_out);
		kmem_cache_free(shadow_cache, s);
		atomic_dec(&osh->pktalloced);
	}
	return first;
}

/* inline skb_push in the core calls this when it runs out of headroom (2.6.22 semantics: fatal) */
void skb_under_panic(void *skb, int len, void *here)
{
	struct skb2622 *s = skb;

	pr_emerg("wl: skb_under_panic: shadow %p len %d here %pS data %p head %p\n",
		 s, len, here, s->data, s->head);
	BUG();
}

/* ---------------------------------------------------------------- DMA */

#define BCM_DMA_TX	1	/* hnddma.h DMA_TX */
#define BCM_DMA_RX	2	/* hnddma.h DMA_RX */

void *osl_dma_alloc_consistent(osl_t *osh, uint size, uint16 align_bits, uint *alloced, ulong *pap)
{
	dma_addr_t pa;
	void *va;
	uint align = 1u << align_bits;

	if (align > PAGE_SIZE)		/* dma_alloc_coherent() is page aligned already */
		size += align;
	va = dma_alloc_coherent(osh->dev, size, &pa, GFP_ATOMIC);
	*alloced = va ? size : 0;
	*pap = (ulong)pa;
	return va;
}

void osl_dma_free_consistent(osl_t *osh, void *va, uint size, ulong pa)
{
	dma_free_coherent(osh->dev, size, va, (dma_addr_t)pa);
}

uint osl_dma_map(osl_t *osh, void *va, uint size, int direction)
{
	return (uint)dma_map_single(osh->dev, va, size,
				    direction == BCM_DMA_TX ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
}

void osl_dma_unmap(osl_t *osh, uint pa, uint size, int direction)
{
	dma_unmap_single(osh->dev, (dma_addr_t)pa, size,
			 direction == BCM_DMA_TX ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
}

/* ---------------------------------------------------------------- PCI / PCMCIA (unused on SI_BUS) */

uint32 osl_pci_read_config(osl_t *osh, uint offset, uint size) { return 0xffffffff; }
void osl_pci_write_config(osl_t *osh, uint offset, uint size, uint val) { }
uint osl_pci_bus(osl_t *osh) { return 0; }
uint osl_pci_slot(osl_t *osh) { return 0; }
void osl_pcmcia_read_attr(osl_t *osh, uint offset, void *buf, int size) { memset(buf, 0xff, size); }
void osl_pcmcia_write_attr(osl_t *osh, uint offset, void *buf, int size) { }

/* ---------------------------------------------------------------- NVRAM (on top of bcm47xx_nvram) */

static char *nvram_buf;
static size_t nvram_len;

int nvram_init(void *sih)
{
	if (!nvram_buf)
		nvram_buf = bcm47xx_nvram_get_contents(&nvram_len);
	return nvram_buf ? 0 : -ENODEV;
}

/* returns a pointer into the persistent NVRAM copy ("name=value\0name=value\0...\0\0") */
char *nvram_get(const char *name)
{
	size_t n = strlen(name);
	char *p;

	if (!nvram_buf && nvram_init(NULL))
		return NULL;
	for (p = nvram_buf; p < nvram_buf + nvram_len && *p; p += strlen(p) + 1)
		if (!strncmp(p, name, n) && p[n] == '=')
			return p + n + 1;
	return NULL;
}

int nvram_getall(char *buf, int count)
{
	if (!nvram_buf && nvram_init(NULL))
		return -ENODEV;
	if (count < 2)
		return -EOVERFLOW;
	memset(buf, 0, count);
	memcpy(buf, nvram_buf, min_t(size_t, nvram_len, count - 2));
	return 0;
}

void nvram_release(void)
{
	/* 4.14: bcm47xx_nvram_get_contents() returns a vmalloc'd copy -> must be vfree'd (kfree() BUGs) */
	if (nvram_buf)
		bcm47xx_nvram_release_contents(nvram_buf);
	nvram_buf = NULL;
}

/* ---------------------------------------------------------------- EMF / IGS (WMF) stubs: feature off */

void *emfc_init(int8 *inst_id, void *emfi, int32 fwd_cfg_fn_id, void *a, void *b, void *c) { return NULL; }
void emfc_exit(void *emfc) { }
int32 emfc_input(void *emfc, void *sdu, void *ifp, uint8 *iph, bool rt_port) { return 0; }
int32 emfc_cfg_request_process(void *emfc, void *cfg) { return -1; }
void *igsc_init(int8 *inst_id, void *igs_info, osl_t *osh, void *a, void *b) { return NULL; }
int32 igsc_exit(void *igsc) { return 0; }
int32 igsc_sdb_interface_del(void *igsc, void *ifp) { return 0; }
int32 igsc_interface_rtport_del(void *igsc, void *ifp) { return 0; }
