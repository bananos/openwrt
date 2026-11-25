/*
 * wl_linux_414.c - minimal Linux 4.14 glue for the closed Broadcom wl 5.110.27.20012 core
 * (RT-N12 D1, BCM5357C0, OpenWrt 19.07.10). Replaces the closed 2.6.22 wl_linux.c that was cut out of
 * the stock wl.ko (see scripts/wl_unlink.py).
 *
 * Scope (first bring-up): SoC (bcma via OpenWrt's wl_glue) only, primary interface only, no WDS/MBSS
 * virtual interfaces, no monitor mode, events not interpreted (carrier is set on open).
 *
 * Locking (same model as Broadcom's 5.10.56 wl_linux.c): one perimeter lock wl->lock, taken with
 * spin_lock_bh around every call INTO the core from process/softirq context (open/stop/xmit/ioctl/
 * timers/DPC). The core calls back into the wl_* functions below with that lock already held, so
 * they never take it. The hard IRQ handler only runs wlc_isr() under isr_lock.
 *
 * Interface facts recovered from the stock 5.110 glue (see WIFI_PLAN.md, phase 2/3):
 *   wlc_attach() has the 5.10.56 10-argument signature; wlc_pub(wlc) returns wlc_pub_t;
 *   pub->unit @4, pub->up @24 (u8), pub->cur_etheraddr @76; primary wlcif = wlc_wlcif_get_by_index(wlc, 0).
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/interrupt.h>
#include <linux/timer.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/rtnetlink.h>
#include <linux/uaccess.h>
#include <linux/bcma/bcma.h>
#include <asm/unaligned.h>

#include <typedefs.h>
#include <bcmdefs.h>
#include <osl.h>
#include <bcmutils.h>
#include <epivers.h>
#include <wlioctl.h>
#include <siutils.h>
#include <pcicfg.h>
#include <skb2622.h>
#include "wl_cfg80211.h"
#include <wl_export_5110.h>
#include "wl_glue.h"

/* ------------------------------------------------------------------ core API (closed, 5.110.27) */
struct wlc_info; struct wlc_if; struct wlc_event; struct wl_rxsts;
extern void *wlc_attach(void *wl, uint16 vendor, uint16 device, uint unit, bool piomode,
			osl_t *osh, void *regsva, uint bustype, void *btparam, uint *perr);
extern uint wlc_detach(struct wlc_info *wlc);
extern void *wlc_pub(struct wlc_info *wlc);
extern struct wlc_if *wlc_wlcif_get_by_index(struct wlc_info *wlc, uint idx);
extern int wlc_up(struct wlc_info *wlc);
extern uint wlc_down(struct wlc_info *wlc);
extern void wlc_init(struct wlc_info *wlc);
extern void wlc_reset(struct wlc_info *wlc);
extern void wlc_intrson(struct wlc_info *wlc);
extern uint32 wlc_intrsoff(struct wlc_info *wlc);
extern void wlc_intrsrestore(struct wlc_info *wlc, uint32 macintmask);
extern bool wlc_intrsupd(struct wlc_info *wlc);
extern bool wlc_isr(struct wlc_info *wlc, bool *wantdpc);
extern bool wlc_dpc(struct wlc_info *wlc, bool bounded);
extern bool wlc_sendpkt(struct wlc_info *wlc, void *sdu, struct wlc_if *wlcif);
extern int wlc_ioctl(struct wlc_info *wlc, int cmd, void *arg, int len, struct wlc_if *wlcif);
extern int wlc_iovar_setint(struct wlc_info *wlc, const char *name, int arg);
extern int wlc_iovar_getint(struct wlc_info *wlc, const char *name, int *arg);
extern int wlc_iovar_op(struct wlc_info *wlc, const char *name, void *params, int p_len, void *arg,
			int len, bool set, struct wlc_if *wlcif);

/* OSL extras (osl/linux_osl_414.c) */
extern int osl_module_init(void);
extern void osl_module_exit(void);
extern void nvram_release(void);

/* wlc_pub_t fields, offsets taken from the stock glue's loads */
#define PUB_UNIT(pub)		(*(uint *)((char *)(pub) + 4))
#define PUB_UP(pub)		(*(uint8 *)((char *)(pub) + 24))
#define PUB_ETHERADDR(pub)	((uint8 *)((char *)(pub) + 76))
#define PUB_BCMERROR(pub)	(*(int *)((char *)(pub) + 104))	/* stock wl_ioctl: sw err,104(pub) */

#define WL_IOCTL_LEN_CAP	(128 * 1024)

#define WL_TXHEADROOM	256	/* d11 txh + plcp + 802.11 hdr + llc/snap + slack */
#define WL_DRV_NAME	"wl"

/* ------------------------------------------------------------------ private state */
struct wl_info;

struct wl_if {
	struct wl_info *wl;
	struct net_device *dev;
	struct wlc_if *wlcif;
};

struct wl_timer {
	struct timer_list timer;
	struct wl_info *wl;
	void (*fn)(void *);
	void *arg;
	uint ms;
	bool periodic;
	bool set;
	struct wl_timer *next;
	const char *name;
};

struct wl_info {
	struct wlc_info *wlc;
	void *pub;
	osl_t *osh;
	struct bcma_device *core;
	void __iomem *regsva;
	uint irq;
	bool irq_requested;
	struct wl_if *wlif;		/* primary interface */
	struct net_device *dev;
	spinlock_t lock;		/* perimeter lock */
	spinlock_t isr_lock;
	struct tasklet_struct tasklet;
	bool resched;
	struct wl_timer *timers;
	uint callbacks;
	struct wl_cfg *cfg;		/* cfg80211 front end, NULL if not built */
};

#define WL_LOCK(wl)	spin_lock_bh(&(wl)->lock)
#define WL_UNLOCK(wl)	spin_unlock_bh(&(wl)->lock)

static int wl_found;
static int nompc = 1;
module_param(nompc, int, 0);
static int event_v1 = 1;
module_param(event_v1, int, 0);
static int trace_ioctl;
static int event_ie_trim = 1;
module_param(event_ie_trim, int, 0644);
module_param(trace_ioctl, int, 0644);

/* read-only debug counters: cat /sys/module/wl/parameters/memstat */
extern int osl_dbg_stats(char *buf, size_t n);
static int memstat_get(char *buf, const struct kernel_param *kp)
{
	return osl_dbg_stats(buf, PAGE_SIZE);
}
static const struct kernel_param_ops memstat_ops = { .get = memstat_get };
module_param_cb(memstat, &memstat_ops, NULL, 0444);
MODULE_PARM_DESC(event_ie_trim, "1 = pass only the RSN/WPA IE of (re)assoc events to nas 5.10.56 (0: all IEs, debugging)");
MODULE_PARM_DESC(trace_ioctl, "1 = log every wl ioctl from userspace (debugging)");
MODULE_PARM_DESC(event_v1, "1 = convert wl_event_msg_t v2 frames to v1 for OpenWrt nas 5.10.56");
MODULE_PARM_DESC(nompc, "1 (default) = disable Minimum Power Consumption: radio stays on while unassociated");

/* ------------------------------------------------------------------ callbacks from the core (lock held) */

void wl_init(struct wl_info *wl)
{
	wl_reset(wl);
	wlc_init(wl->wlc);
}

uint wl_reset(struct wl_info *wl)
{
	wlc_reset(wl->wlc);
	wl->resched = 0;
	return 0;
}

int wl_up(struct wl_info *wl)
{
	int err;

	if (PUB_UP(wl->pub))
		return 0;
	err = wlc_up(wl->wlc);
	if (!err)
		netif_carrier_on(wl->dev);
	return err;
}

void wl_down(struct wl_info *wl)
{
	if (wl->dev)
		netif_carrier_off(wl->dev);
	wl->callbacks = wlc_down(wl->wlc);
}

void wl_dump_ver(struct wl_info *wl, struct bcmstrbuf *b)
{
	bcm_bprintf(b, "wl%d: %s %s version %s (4.14 port)\n", PUB_UNIT(wl->pub),
		    __DATE__, __TIME__, EPI_VERSION_STR);
}

void wl_intrson(struct wl_info *wl)
{
	unsigned long flags;

	local_irq_save(flags);
	wlc_intrson(wl->wlc);
	local_irq_restore(flags);
}

uint32 wl_intrsoff(struct wl_info *wl)
{
	unsigned long flags;
	uint32 mask;

	local_irq_save(flags);
	mask = wlc_intrsoff(wl->wlc);
	local_irq_restore(flags);
	return mask;
}

void wl_intrsrestore(struct wl_info *wl, uint32 macintmask)
{
	unsigned long flags;

	local_irq_save(flags);
	wlc_intrsrestore(wl->wlc, macintmask);
	local_irq_restore(flags);
}

/* timers: callbacks run in softirq context, take the perimeter lock, then call into the core */
static void wl_timer(unsigned long data)
{
	struct wl_timer *t = (struct wl_timer *)data;
	struct wl_info *wl = t->wl;

	WL_LOCK(wl);
	if (t->set) {
		if (t->periodic)
			mod_timer(&t->timer, jiffies + msecs_to_jiffies(t->ms));
		else
			t->set = FALSE;
		t->fn(t->arg);
	}
	WL_UNLOCK(wl);
}

struct wl_timer *wl_init_timer(struct wl_info *wl, void (*fn)(void *arg), void *arg, const char *name)
{
	struct wl_timer *t = kzalloc(sizeof(*t), GFP_ATOMIC);

	if (!t)
		return NULL;
	setup_timer(&t->timer, wl_timer, (unsigned long)t);
	t->wl = wl;
	t->fn = fn;
	t->arg = arg;
	t->name = name;
	t->next = wl->timers;
	wl->timers = t;
	return t;
}

void wl_add_timer(struct wl_info *wl, struct wl_timer *t, uint ms, int periodic)
{
	t->ms = ms;
	t->periodic = (bool)periodic;
	t->set = TRUE;
	mod_timer(&t->timer, jiffies + msecs_to_jiffies(ms));
}

bool wl_del_timer(struct wl_info *wl, struct wl_timer *t)
{
	if (t->set) {
		t->set = FALSE;
		if (!del_timer(&t->timer))
			return FALSE;	/* handler running: it will see set == FALSE */
	}
	return TRUE;
}

void wl_free_timer(struct wl_info *wl, struct wl_timer *t)
{
	struct wl_timer **pp;

	for (pp = &wl->timers; *pp; pp = &(*pp)->next)
		if (*pp == t) {
			*pp = t->next;
			break;
		}
	t->set = FALSE;
	del_timer(&t->timer);	/* not _sync: we may be inside this timer's handler */
	kfree(t);
}

/*
 * Broadcom event frames (ethertype 0x886c, OUI 00:10:18, usr_subtype 1) from the 5.110 core carry
 * wl_event_msg_t version 2, which appends ifidx + bsscfgidx (2 bytes) before the event data.
 * OpenWrt's nas 5.10.56 only understands version 1, so convert in place:
 *   eth(14) | bcmeth: subtype(2) length(2) version(1) oui(3) usr_subtype(2) | event: ... ifname[16] | ifidx bsscfgidx | data
 *   offsets:  ethertype @12, bcmeth length @16, oui @19, usr_subtype @22, event version @24, ifidx @70, data @72
 */
#define BCMEV_OFF_LEN		16
#define BCMEV_OFF_OUI		19
#define BCMEV_OFF_USRSUB	22
#define BCMEV_OFF_VER		24
#define BCMEV_OFF_V2EXTRA	70
#define BCMEV_V2_HDRLEN		72

#define BCMEV_OFF_TYPE		28
#define BCMEV_OFF_DATALEN	44
#define BCMEV_V1_HDRLEN		70
#define WLC_E_ASSOC_IND		8
#define WLC_E_REASSOC_IND	10
#define WLC_E_DEAUTH		5
#define WLC_E_DEAUTH_IND	6
#define WLC_E_DISASSOC		11
#define WLC_E_DISASSOC_IND	12
#define WLC_E_STATUS_SUCCESS	0

/*
 * nas 5.10.56 (wpa_driver_assoc_msg) requires the (RE)ASSOC_IND payload to be exactly one IE, the
 * station's WPA/RSN IE (it checks datalen == ie[1] + 2). The 5.110 core sends the whole association
 * request IE list instead, so keep only the RSN IE (or the WPA vendor IE). Operates on a v1 frame.
 */
static void wl_event_assoc_ie_only(struct sk_buff *skb)
{
	u8 *d = skb->data, *ies = d + BCMEV_V1_HDRLEN, *p, *pick = NULL;
	u32 type = get_unaligned_be32(d + BCMEV_OFF_TYPE);
	u32 len = get_unaligned_be32(d + BCMEV_OFF_DATALEN);
	u32 keep;

	if ((type != WLC_E_ASSOC_IND && type != WLC_E_REASSOC_IND) ||
	    len < 2 || BCMEV_V1_HDRLEN + len > skb->len)
		return;
	if (trace_ioctl)
		print_hex_dump(KERN_INFO, "wl assoc-ies ", DUMP_PREFIX_OFFSET, 32, 1, ies, len, false);
	for (p = ies; p + 2 <= ies + len && p + 2 + p[1] <= ies + len; p += 2 + p[1]) {
		if (p[0] == 0x30) {				/* RSN IE: preferred */
			pick = p;
			break;
		}
		if (!pick && p[0] == 0xdd && p[1] >= 4 &&	/* WPA vendor IE 00:50:f2 type 1 */
		    p[2] == 0x00 && p[3] == 0x50 && p[4] == 0xf2 && p[5] == 0x01)
			pick = p;
	}
	if (!pick || !event_ie_trim)
		return;				/* open/WEP station: leave as is */
	keep = 2 + pick[1];
	if (keep == len)
		return;
	memmove(ies, pick, keep);
	skb_trim(skb, BCMEV_V1_HDRLEN + keep);
	put_unaligned_be32(keep, d + BCMEV_OFF_DATALEN);
	put_unaligned_be16(get_unaligned_be16(d + BCMEV_OFF_LEN) - (len - keep), d + BCMEV_OFF_LEN);
}

static void wl_event_to_v1(struct sk_buff *skb)
{
	u8 *d = skb->data;

	if (skb->len < BCMEV_V2_HDRLEN || get_unaligned_be16(d + 12) != 0x886c ||
	    d[BCMEV_OFF_OUI] != 0x00 || d[BCMEV_OFF_OUI + 1] != 0x10 || d[BCMEV_OFF_OUI + 2] != 0x18 ||
	    get_unaligned_be16(d + BCMEV_OFF_USRSUB) != 1 || get_unaligned_be16(d + BCMEV_OFF_VER) != 2)
		return;
	memmove(d + BCMEV_OFF_V2EXTRA, d + BCMEV_V2_HDRLEN, skb->len - BCMEV_V2_HDRLEN);
	skb_trim(skb, skb->len - 2);
	put_unaligned_be16(1, d + BCMEV_OFF_VER);
	put_unaligned_be16(get_unaligned_be16(d + BCMEV_OFF_LEN) - 2, d + BCMEV_OFF_LEN);
	wl_event_assoc_ie_only(skb);
}

#define BCMEV_OFF_STATUS	32
#define BCMEV_OFF_ADDR		48

/* station events for cfg80211; the frame itself still goes up for nas */
static void wl_event_to_cfg(struct wl_info *wl, struct sk_buff *skb)
{
	u8 *d = skb->data;
	u32 type, len;

	if (!wl->cfg || skb->len < BCMEV_V2_HDRLEN || get_unaligned_be16(d + 12) != 0x886c ||
	    d[BCMEV_OFF_OUI] != 0x00 || d[BCMEV_OFF_OUI + 1] != 0x10 || d[BCMEV_OFF_OUI + 2] != 0x18 ||
	    get_unaligned_be16(d + BCMEV_OFF_USRSUB) != 1 || get_unaligned_be16(d + BCMEV_OFF_VER) != 2)
		return;
	type = get_unaligned_be32(d + BCMEV_OFF_TYPE);
	len = get_unaligned_be32(d + BCMEV_OFF_DATALEN);
	if (BCMEV_V2_HDRLEN + len > skb->len)
		len = 0;
	switch (type) {
	case WLC_E_ASSOC_IND:
	case WLC_E_REASSOC_IND:
		if (get_unaligned_be32(d + BCMEV_OFF_STATUS) == WLC_E_STATUS_SUCCESS)
			wl_cfg80211_sta_assoc(wl->cfg, d + BCMEV_OFF_ADDR, d + BCMEV_V2_HDRLEN, len);
		break;
	case WLC_E_DISASSOC_IND:
	case WLC_E_DEAUTH_IND:
	case WLC_E_DISASSOC:
	case WLC_E_DEAUTH:
		wl_cfg80211_sta_left(wl->cfg, d + BCMEV_OFF_ADDR);
		break;
	}
}

/* RX: shadow chain -> native skbs -> stack */
void wl_sendup(struct wl_info *wl, struct wl_if *wlif, void *p, int numpkt)
{
	struct net_device *dev = wlif ? wlif->dev : wl->dev;
	struct sk_buff *skb, *nxt;

	for (skb = osl_pkt_tonative(wl->osh, p); skb; skb = nxt) {
		nxt = skb->next;
		skb->next = NULL;
		wl_event_to_cfg(wl, skb);
		if (event_v1)
			wl_event_to_v1(skb);
		skb->dev = dev;
		skb->protocol = eth_type_trans(skb, dev);
		dev->stats.rx_packets++;
		dev->stats.rx_bytes += skb->len;
		netif_rx(skb);
	}
}

void wl_txflowcontrol(struct wl_info *wl, struct wl_if *wlif, bool state, int prio)
{
	struct net_device *dev = wlif ? wlif->dev : wl->dev;

	if (state)
		netif_stop_queue(dev);
	else
		netif_wake_queue(dev);
}

void wl_event(struct wl_info *wl, char *ifname, struct wlc_event *e)
{
	/* not interpreted yet: wlc_event_t layout for 5.110 still to be confirmed */
}

void wl_event_sync(struct wl_info *wl, char *ifname, struct wlc_event *e) { }

struct wl_if *wl_add_if(struct wl_info *wl, struct wlc_if *wlcif, uint unit, struct ether_addr *remote)
{
	pr_warn("wl%d: virtual interface %u requested: not supported by this port yet\n",
		PUB_UNIT(wl->pub), unit);
	return NULL;
}

void wl_del_if(struct wl_info *wl, struct wl_if *wlif) { }

char *wl_ifname(struct wl_info *wl, struct wl_if *wlif)
{
	struct net_device *dev = wlif ? wlif->dev : wl->dev;

	return dev ? dev->name : "wl";
}

void wl_monitor(struct wl_info *wl, struct wl_rxsts *rxsts, void *p) { }
void wl_set_monitor(struct wl_info *wl, int val) { }
bool wl_alloc_dma_resources(struct wl_info *wl, uint dmaddrwidth) { return TRUE; }

/* ------------------------------------------------------------------ interrupts */

static irqreturn_t wl_isr(int irq, void *dev_id)
{
	struct wl_info *wl = dev_id;
	bool ours, wantdpc = FALSE;
	unsigned long flags;

	spin_lock_irqsave(&wl->isr_lock, flags);
	ours = wlc_isr(wl->wlc, &wantdpc);
	if (ours && wantdpc)
		tasklet_schedule(&wl->tasklet);
	spin_unlock_irqrestore(&wl->isr_lock, flags);
	return IRQ_RETVAL(ours);
}

static void wl_dpc(unsigned long data)
{
	struct wl_info *wl = (struct wl_info *)data;

	WL_LOCK(wl);
	if (PUB_UP(wl->pub)) {
		if (wl->resched) {
			unsigned long flags;

			spin_lock_irqsave(&wl->isr_lock, flags);
			wlc_intrsupd(wl->wlc);
			spin_unlock_irqrestore(&wl->isr_lock, flags);
		}
		wl->resched = wlc_dpc(wl->wlc, TRUE);
	}
	if (PUB_UP(wl->pub)) {
		if (wl->resched)
			tasklet_schedule(&wl->tasklet);
		else
			wl_intrson(wl);
	}
	WL_UNLOCK(wl);
}

/* ------------------------------------------------------------------ net_device */

static int wl_open(struct net_device *dev)
{
	struct wl_info *wl = ((struct wl_if *)netdev_priv(dev))->wl;
	int err;

	WL_LOCK(wl);
	err = wl_up(wl);
	WL_UNLOCK(wl);
	if (err) {
		netdev_err(dev, "wlc_up failed: %d\n", err);
		return -ENODEV;
	}
	netif_start_queue(dev);
	return 0;
}

static int wl_close(struct net_device *dev)
{
	struct wl_info *wl = ((struct wl_if *)netdev_priv(dev))->wl;

	netif_stop_queue(dev);
	WL_LOCK(wl);
	wl_down(wl);
	WL_UNLOCK(wl);
	return 0;
}

static netdev_tx_t wl_start(struct sk_buff *skb, struct net_device *dev)
{
	struct wl_if *wlif = netdev_priv(dev);
	struct wl_info *wl = wlif->wl;
	void *pkt;

	/* the core prepends d11 headers in place and edits the ethernet header */
	if (skb_cow_head(skb, WL_TXHEADROOM)) {
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	pkt = osl_pkt_frmnative(wl->osh, skb);
	if (!pkt) {
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	dev->stats.tx_packets++;
	dev->stats.tx_bytes += skb->len;
	WL_LOCK(wl);
	wlc_sendpkt(wl->wlc, pkt, wlif->wlcif);	/* core owns pkt now, frees it via PKTFREE */
	WL_UNLOCK(wl);
	return NETDEV_TX_OK;
}

/* SIOCDEVPRIVATE: the Broadcom wl_ioctl_t protocol used by the stock `wl` tool */
static int wl_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	struct wl_if *wlif = netdev_priv(dev);
	struct wl_info *wl = wlif->wl;
	wl_ioctl_t ioc;
	void *buf = NULL;
	char vname[32] = "";
	int err;

	if (cmd != SIOCDEVPRIVATE)
		return -EOPNOTSUPP;
	if (copy_from_user(&ioc, ifr->ifr_data, sizeof(ioc)))
		return -EFAULT;
	/* the stock tool uses up to 64 KB (e.g. scanresults); Broadcom allocates MAX(len, MAXLEN) */
	if (ioc.len > WL_IOCTL_LEN_CAP)
		return -EINVAL;
	if (ioc.set && !capable(CAP_NET_ADMIN))
		return -EPERM;
	if (ioc.buf) {
		buf = kmalloc(max_t(uint, ioc.len, WLC_IOCTL_MAXLEN), GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
		if (copy_from_user(buf, ioc.buf, ioc.len)) {
			kfree(buf);
			return -EFAULT;
		}
	}
	if (trace_ioctl && buf && (ioc.cmd == WLC_GET_VAR || ioc.cmd == WLC_SET_VAR))
		strlcpy(vname, buf, min_t(uint, sizeof(vname), ioc.len + 1));
	WL_LOCK(wl);
	err = wlc_ioctl(wl->wlc, ioc.cmd, buf, ioc.len, wlif->wlcif);
	if (err)
		PUB_BCMERROR(wl->pub) = err;	/* read back by the tool via the "bcmerror" iovar */
	WL_UNLOCK(wl);
	if (trace_ioctl)
		pr_info("wl ioctl %s cmd %d %s len %u set %u err %d\n", current->comm, ioc.cmd,
			vname, ioc.len, ioc.set, err);
	if (!err && ioc.cmd == WLC_SET_SSID && buf && ioc.len >= sizeof(wlc_ssid_t))
		wl_cfg80211_ssid_changed(wl->cfg, ((wlc_ssid_t *)buf)->SSID,
					 min_t(uint32, ((wlc_ssid_t *)buf)->SSID_len, 32));
	if (!err && buf && copy_to_user(ioc.buf, buf, ioc.len))
		err = BCME_BADADDR;
	kfree(buf);
	return err ? -EINVAL : 0;
}

static int wl_set_mac_address(struct net_device *dev, void *addr)
{
	struct sockaddr *sa = addr;
	struct wl_info *wl = ((struct wl_if *)netdev_priv(dev))->wl;
	int err;

	if (!is_valid_ether_addr(sa->sa_data))
		return -EADDRNOTAVAIL;
	WL_LOCK(wl);
	err = wlc_iovar_op(wl->wlc, "cur_etheraddr", NULL, 0, sa->sa_data, ETHER_ADDR_LEN, TRUE, NULL);
	WL_UNLOCK(wl);
	if (err)
		return -EINVAL;
	memcpy(dev->dev_addr, sa->sa_data, ETH_ALEN);
	return 0;
}

static void wl_get_drvinfo(struct net_device *dev, struct ethtool_drvinfo *info)
{
	/* the stock `wl` tool identifies the adapter by driver name "wl" */
	strlcpy(info->driver, WL_DRV_NAME, sizeof(info->driver));
	strlcpy(info->version, EPI_VERSION_STR, sizeof(info->version));
}

static const struct ethtool_ops wl_ethtool_ops = {
	.get_drvinfo	= wl_get_drvinfo,
	.get_link	= ethtool_op_get_link,
};

static const struct net_device_ops wl_netdev_ops = {
	.ndo_open		= wl_open,
	.ndo_stop		= wl_close,
	.ndo_start_xmit		= wl_start,
	.ndo_do_ioctl		= wl_ioctl,
	.ndo_set_mac_address	= wl_set_mac_address,
};

/* ------------------------------------------------------------------ attach / detach */

/* ------------------------------------------------------------------ helpers for wl_cfg80211.c */
/* Process context: they may allocate with GFP_KERNEL and take the perimeter lock. */

int wl_cfg_ioctl(struct wl_info *wl, int cmd, void *buf, int len)
{
	int err;

	WL_LOCK(wl);
	err = wlc_ioctl(wl->wlc, cmd, buf, len, wl->wlif->wlcif);
	WL_UNLOCK(wl);
	return err ? -EIO : 0;
}

int wl_cfg_iovar_getint(struct wl_info *wl, const char *name, int *val)
{
	int err;

	WL_LOCK(wl);
	err = wlc_iovar_getint(wl->wlc, name, val);
	WL_UNLOCK(wl);
	return err ? -EIO : 0;
}

int wl_cfg_iovar_setint(struct wl_info *wl, const char *name, int val)
{
	int err;

	WL_LOCK(wl);
	err = wlc_iovar_setint(wl->wlc, name, val);
	WL_UNLOCK(wl);
	return err ? -EIO : 0;
}

int wl_cfg_channel(struct wl_info *wl, int *chan)
{
	channel_info_t ci;
	int err = wl_cfg_ioctl(wl, WLC_GET_CHANNEL, &ci, sizeof(ci));

	if (!err)
		*chan = ci.hw_channel;
	return err;
}

int wl_cfg_valid_channels(struct wl_info *wl, u32 *chans, int max)
{
	uint buf[1 + 64];	/* wl_uint32_list_t */
	int i, n, err;

	buf[0] = 64;
	err = wl_cfg_ioctl(wl, WLC_GET_VALID_CHANNELS, buf, sizeof(buf));
	if (err)
		return err;
	n = min_t(int, buf[0], max);
	for (i = 0; i < n; i++)
		chans[i] = buf[1 + i];
	return n;
}

int wl_cfg_assoclist(struct wl_info *wl, u8 (*macs)[6], int max)
{
	int i, n, err, len = sizeof(uint) + max * ETHER_ADDR_LEN;
	struct maclist *ml = kmalloc(len, GFP_KERNEL);

	if (!ml)
		return -ENOMEM;
	ml->count = max;
	err = wl_cfg_ioctl(wl, WLC_GET_ASSOCLIST, ml, len);
	n = err ? err : min_t(int, ml->count, max);
	for (i = 0; i < n; i++)
		memcpy(macs[i], &ml->ea[i], ETHER_ADDR_LEN);
	kfree(ml);
	return n;
}

int wl_cfg_sta_info(struct wl_info *wl, const u8 *mac, struct wl_cfg_sta *st)
{
	sta_info_t *si = kzalloc(WLC_IOCTL_SMLEN, GFP_KERNEL);
	struct ether_addr ea;
	scb_val_t sv;
	int err;

	if (!si)
		return -ENOMEM;
	memcpy(&ea, mac, ETHER_ADDR_LEN);
	WL_LOCK(wl);
	err = wlc_iovar_op(wl->wlc, "sta_info", &ea, ETHER_ADDR_LEN, si, WLC_IOCTL_SMLEN, FALSE,
			   wl->wlif->wlcif);
	WL_UNLOCK(wl);
	if (err) {
		kfree(si);
		return -ENOENT;
	}
	memset(st, 0, sizeof(*st));
	st->flags = ((si->flags & WL_STA_WME) ? WL_CFG_STA_WME : 0) |
		    ((si->flags & WL_STA_AUTHE) ? WL_CFG_STA_AUTHE : 0) |
		    ((si->flags & WL_STA_ASSOC) ? WL_CFG_STA_ASSOC : 0) |
		    ((si->flags & WL_STA_AUTHO) ? WL_CFG_STA_AUTHO : 0) |
		    ((si->flags & WL_STA_PS) ? WL_CFG_STA_PS : 0) |
		    ((si->flags & WL_STA_N_CAP) ? WL_CFG_STA_HT : 0);
	st->idle_s = si->idle;
	st->connected_s = si->in;
	if (si->len >= sizeof(sta_info_t)) {	/* version 3: counters and rates present */
		st->tx_pkts = si->tx_pkts;
		st->tx_failures = si->tx_failures;
		st->rx_pkts = si->rx_ucast_pkts + si->rx_mcast_pkts;
		st->tx_rate_kbps = si->tx_rate;
		st->rx_rate_kbps = si->rx_rate;
	}
	kfree(si);

	memset(&sv, 0, sizeof(sv));
	memcpy(&sv.ea, mac, ETHER_ADDR_LEN);
	if (!wl_cfg_ioctl(wl, WLC_GET_RSSI, &sv, sizeof(sv)))
		st->rssi = (int32)sv.val;
	return 0;
}

int wl_cfg_noise(struct wl_info *wl, int *dbm)
{
	int32 v;
	int err = wl_cfg_ioctl(wl, WLC_GET_PHY_NOISE, &v, sizeof(v));

	if (!err)
		*dbm = v;
	return err;
}

static int wl_cfg_setint(struct wl_info *wl, int cmd, int val)
{
	return wl_cfg_ioctl(wl, cmd, &val, sizeof(val));
}

/*
 * The core only queues events whose bit is set in "event_msgs"; nas used to set them. Add the
 * station events cfg80211 needs, keeping whatever else is enabled.
 */
static int wl_cfg_enable_events(struct wl_info *wl)
{
	static const int ev[] = { WLC_E_DEAUTH, WLC_E_DEAUTH_IND, WLC_E_ASSOC_IND, WLC_E_REASSOC_IND,
				  WLC_E_DISASSOC, WLC_E_DISASSOC_IND };
	u8 mask[WL_EVENTING_MASK_LEN];
	int i, err;

	WL_LOCK(wl);
	err = wlc_iovar_op(wl->wlc, "event_msgs", NULL, 0, mask, sizeof(mask), FALSE, wl->wlif->wlcif);
	if (!err) {
		for (i = 0; i < ARRAY_SIZE(ev); i++)
			mask[ev[i] / 8] |= 1 << (ev[i] % 8);
		err = wlc_iovar_op(wl->wlc, "event_msgs", NULL, 0, mask, sizeof(mask), TRUE,
				   wl->wlif->wlcif);
	}
	WL_UNLOCK(wl);
	return err ? -EIO : 0;
}

/* same sequence as brcmwl.sh, which is known to work with this core */
int wl_cfg_start_ap(struct wl_info *wl, const struct wl_cfg_ap *ap)
{
	int wsec = ((ap->wsec & WL_CFG_WSEC_WEP) ? WEP_ENABLED : 0) |
		   ((ap->wsec & WL_CFG_WSEC_TKIP) ? TKIP_ENABLED : 0) |
		   ((ap->wsec & WL_CFG_WSEC_AES) ? AES_ENABLED : 0);
	int wpa_auth = ((ap->auth & WL_CFG_AUTH_WPA_PSK) ? WPA_AUTH_PSK : 0) |
		       ((ap->auth & WL_CFG_AUTH_WPA2_PSK) ? WPA2_AUTH_PSK : 0) |
		       ((ap->auth & WL_CFG_AUTH_WPA_8021X) ? WPA_AUTH_UNSPECIFIED : 0) |
		       ((ap->auth & WL_CFG_AUTH_WPA2_8021X) ? WPA2_AUTH_UNSPECIFIED : 0);
	bool eap = ap->auth & (WL_CFG_AUTH_WPA_8021X | WL_CFG_AUTH_WPA2_8021X);
	wlc_ssid_t ssid;
	int err;

	if (ap->ssid_len < 0 || ap->ssid_len > sizeof(ssid.SSID))
		return -EINVAL;
	err = wl_cfg_ioctl(wl, WLC_DOWN, NULL, 0);
	if (nompc)
		err = err ?: wl_cfg_iovar_setint(wl, "mpc", 0);
	err = err ?: wl_cfg_setint(wl, WLC_SET_AP, 1);
	err = err ?: wl_cfg_setint(wl, WLC_SET_INFRA, 1);
	err = err ?: wl_cfg_setint(wl, WLC_SET_CHANNEL, ap->channel);
	if (!err && ap->beacon_int)
		err = wl_cfg_setint(wl, WLC_SET_BCNPRD, ap->beacon_int);
	if (!err && ap->dtim)
		err = wl_cfg_setint(wl, WLC_SET_DTIMPRD, ap->dtim);
	err = err ?: wl_cfg_setint(wl, WLC_SET_CLOSED, ap->hidden);
	err = err ?: wl_cfg_setint(wl, WLC_SET_AUTH, ap->shared_key ? 1 : 0);
	err = err ?: wl_cfg_setint(wl, WLC_SET_WSEC, wsec);
	err = err ?: wl_cfg_setint(wl, WLC_SET_WPA_AUTH, wpa_auth);
	err = err ?: wl_cfg_iovar_setint(wl, "wsec_restrict", wpa_auth || wsec ? 1 : 0);
	err = err ?: wl_cfg_setint(wl, WLC_SET_EAP_RESTRICT, eap);
	if (!err && ap->inactivity_s > 0 && wl_cfg_iovar_setint(wl, "scb_activity_time", ap->inactivity_s))
		pr_warn("%s: could not set scb_activity_time %d\n", wl->dev->name, ap->inactivity_s);
	err = err ?: wl_cfg_enable_events(wl);
	err = err ?: wl_cfg_ioctl(wl, WLC_UP, NULL, 0);
	if (err)
		return err;
	memset(&ssid, 0, sizeof(ssid));
	ssid.SSID_len = ap->ssid_len;
	memcpy(ssid.SSID, ap->ssid, ap->ssid_len);
	return wl_cfg_ioctl(wl, WLC_SET_SSID, &ssid, sizeof(ssid));
}

int wl_cfg_stop_ap(struct wl_info *wl)
{
	return wl_cfg_ioctl(wl, WLC_DOWN, NULL, 0);
}

int wl_cfg_set_key(struct wl_info *wl, int idx, const u8 *mac, enum wl_cfg_cipher cipher,
		   const u8 *key, int key_len, bool primary)
{
	static const uint32 algo[] = {
		[WL_CFG_CIPHER_WEP40] = CRYPTO_ALGO_WEP1, [WL_CFG_CIPHER_WEP104] = CRYPTO_ALGO_WEP128,
		[WL_CFG_CIPHER_TKIP] = CRYPTO_ALGO_TKIP, [WL_CFG_CIPHER_CCMP] = CRYPTO_ALGO_AES_CCM,
	};
	wl_wsec_key_t k;

	if (cipher >= ARRAY_SIZE(algo) || key_len < 0 || key_len > sizeof(k.data))
		return -EINVAL;
	memset(&k, 0, sizeof(k));
	k.index = idx;
	k.len = key_len;
	memcpy(k.data, key, key_len);
	k.algo = algo[cipher];
	if (mac)
		memcpy(&k.ea, mac, ETHER_ADDR_LEN);	/* AP mode: TKIP MIC keys are not swapped */
	else if (primary)
		k.flags = WL_PRIMARY_KEY;
	return wl_cfg_ioctl(wl, WLC_SET_KEY, &k, sizeof(k));
}

int wl_cfg_del_key(struct wl_info *wl, int idx, const u8 *mac)
{
	wl_wsec_key_t k;

	memset(&k, 0, sizeof(k));
	k.index = idx;
	if (mac)
		memcpy(&k.ea, mac, ETHER_ADDR_LEN);
	return wl_cfg_ioctl(wl, WLC_SET_KEY, &k, sizeof(k));
}

/* group key TSC/PN, least significant byte first (as nas reads it for the GTK RSC) */
int wl_cfg_key_seq(struct wl_info *wl, int idx, u8 seq[6])
{
	union { int32 idx; u8 b[8]; } buf;
	int err;

	memset(&buf, 0, sizeof(buf));
	buf.idx = idx;
	err = wl_cfg_ioctl(wl, WLC_GET_KEY_SEQ, &buf, sizeof(buf));
	if (!err)
		memcpy(seq, buf.b, 6);
	return err;
}

int wl_cfg_authorize(struct wl_info *wl, const u8 *mac, bool authorized)
{
	struct ether_addr ea;

	memcpy(&ea, mac, ETHER_ADDR_LEN);
	return wl_cfg_ioctl(wl, authorized ? WLC_SCB_AUTHORIZE : WLC_SCB_DEAUTHORIZE, &ea, sizeof(ea));
}

int wl_cfg_deauth(struct wl_info *wl, const u8 *mac, int reason)
{
	scb_val_t sv;

	memset(&sv, 0, sizeof(sv));
	sv.val = reason;
	memcpy(&sv.ea, mac, ETHER_ADDR_LEN);
	return wl_cfg_ioctl(wl, WLC_SCB_DEAUTHENTICATE_FOR_REASON, &sv, sizeof(sv));
}

int wl_cfg_ssid(struct wl_info *wl, u8 *ssid, int *len)
{
	wlc_ssid_t s;
	int err = wl_cfg_ioctl(wl, WLC_GET_SSID, &s, sizeof(s));

	if (err)
		return err;
	*len = min_t(uint32, s.SSID_len, sizeof(s.SSID));
	memcpy(ssid, s.SSID, *len);
	return 0;
}

static void wl_free(struct wl_info *wl)
{
	if (wl->dev && wl->dev->reg_state == NETREG_REGISTERED)
		unregister_netdev(wl->dev);
	wl_cfg80211_detach(wl->cfg);
	wl->cfg = NULL;
	if (wl->dev)
		wl->dev->ieee80211_ptr = NULL;
	if (wl->irq_requested)
		free_irq(wl->irq, wl);
	tasklet_kill(&wl->tasklet);
	if (wl->wlc)
		wlc_detach(wl->wlc);
	while (wl->timers)
		wl_free_timer(wl, wl->timers);
	if (wl->regsva)
		iounmap(wl->regsva);
	if (wl->dev)
		free_netdev(wl->dev);
	if (wl->osh)
		osl_detach(wl->osh);
	kfree(wl);
}

static void *wl_attach(u16 vendor, u16 device, ulong regs, void *btparam, u32 irq)
{
	struct bcma_device *core = btparam;
	struct net_device *dev;
	struct wl_info *wl;
	struct wl_if *wlif;
	uint err = 0;
	int unit = wl_found;

	wl = kzalloc(sizeof(*wl), GFP_KERNEL);
	if (!wl)
		return NULL;
	spin_lock_init(&wl->lock);
	spin_lock_init(&wl->isr_lock);
	tasklet_init(&wl->tasklet, wl_dpc, (unsigned long)wl);
	wl->core = core;
	wl->irq = irq;

	wl->osh = osl_attach(wl_glue_get_dmadev(core), SI_BUS, TRUE);
	if (!wl->osh)
		goto fail;

	dev = alloc_etherdev(sizeof(struct wl_if));
	if (!dev)
		goto fail;
	wl->dev = dev;
	wlif = netdev_priv(dev);
	wlif->wl = wl;
	wlif->dev = dev;
	wl->wlif = wlif;
#ifdef WL_CFG80211
	strlcpy(dev->name, "wlan%d", IFNAMSIZ);	/* what OpenWrt's mac80211.sh expects */
#else
	strlcpy(dev->name, "wl%d", IFNAMSIZ);	/* what brcmwl.sh/wlc expect */
#endif
	dev->netdev_ops = &wl_netdev_ops;
	dev->ethtool_ops = &wl_ethtool_ops;
	dev->needed_headroom = WL_TXHEADROOM;
	dev->base_addr = regs;
	SET_NETDEV_DEV(dev, &core->dev);

	wl->regsva = ioremap_nocache(regs, PCI_BAR0_WINSZ);
	if (!wl->regsva)
		goto fail;

	pr_info("wl%d: attaching Broadcom wl %s core 0x%x at 0x%lx irq %u\n",
		unit, EPI_VERSION_STR, device, regs, irq);
	wl->wlc = wlc_attach(wl, vendor, device, unit, FALSE, wl->osh, wl->regsva, SI_BUS, core, &err);
	if (!wl->wlc) {
		pr_err("wl%d: wlc_attach failed with code %u\n", unit, err);
		goto fail;
	}
	wl->pub = wlc_pub(wl->wlc);
	wlif->wlcif = wlc_wlcif_get_by_index(wl->wlc, 0);
	memcpy(dev->dev_addr, PUB_ETHERADDR(wl->pub), ETH_ALEN);
	if (nompc && wlc_iovar_setint(wl->wlc, "mpc", 0))
		pr_warn("wl%d: failed to disable MPC\n", unit);

	if (request_irq(irq, wl_isr, IRQF_SHARED, "wl", wl)) {
		pr_err("wl%d: request_irq(%u) failed\n", unit, irq);
		goto fail;
	}
	wl->irq_requested = TRUE;
	dev->irq = irq;

	wl->cfg = wl_cfg80211_attach(wl, dev, &core->dev);
	if (register_netdev(dev)) {
		pr_err("wl%d: register_netdev failed\n", unit);
		goto fail;
	}
	pr_info("%s: Broadcom BCM%04x 802.11 Wireless Controller %s, MAC %pM\n",
		dev->name, device, EPI_VERSION_STR, dev->dev_addr);
	if (wl->cfg)
		pr_info("%s: registered with cfg80211 as %s\n", dev->name, wl_cfg80211_name(wl->cfg));
	wl_found++;
	return wl;
fail:
	wl_free(wl);
	return NULL;
}

static void wl_remove(void *wldev)
{
	struct wl_info *wl = wldev;

	if (!wl)
		return;
	if (wl->dev) {
		rtnl_lock();		/* dev_close() needs RTNL */
		if (netif_running(wl->dev))
			dev_close(wl->dev);
		rtnl_unlock();
	}
	WL_LOCK(wl);
	if (wl->pub && PUB_UP(wl->pub))
		wl_down(wl);
	WL_UNLOCK(wl);
	wl_free(wl);
}

static int __init wl_module_init(void)
{
	int err = osl_module_init();

	if (err)
		return err;
	wl_glue_set_attach_callback(&wl_attach);
	wl_glue_set_remove_callback(&wl_remove);
	err = wl_glue_register();
	if (err) {
		wl_glue_set_attach_callback(NULL);
		wl_glue_set_remove_callback(NULL);
		osl_module_exit();
	}
	return err;
}

static void __exit wl_module_exit(void)
{
	wl_glue_unregister();
	wl_glue_set_attach_callback(NULL);
	wl_glue_set_remove_callback(NULL);
	nvram_release();
	osl_module_exit();
}

module_init(wl_module_init);
module_exit(wl_module_exit);
MODULE_DESCRIPTION("Broadcom wl 5.110.27.20012 core (from stock RT-N12 D1 firmware) on Linux 4.14");
MODULE_LICENSE("Proprietary");
