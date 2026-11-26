// SPDX-License-Identifier: ISC
// Copyright (c) 2026 Andrew Druchenko
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

/*
 * One BSS of the core. Allocated separately from its net_device: with cfg80211 the net_device is
 * created and destroyed as interfaces are added and deleted, while the core keeps the wl_if.
 * dev may be NULL (no interface right now); it is changed under the perimeter lock.
 */
struct wl_if {
	struct wl_info *wl;
	struct net_device *dev;
	struct wlc_if *wlcif;		/* NULL until the core created the bsscfg */
	int idx;			/* the core's bsscfg index: 0 primary, 1-3 AP BSSes */
	bool open;
};

/* net_device private data: just the BSS it belongs to */
static inline struct wl_if *WLIF(struct net_device *dev)
{
	return *(struct wl_if **)netdev_priv(dev);
}

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
	struct wl_if *wlif;		/* primary BSS (bsscfg 0) */
	struct wl_if *pending_if;	/* handed to the core by wl_add_if() */
	int n_open;			/* open net_devices; the radio is up while > 0 */
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
	/*
	 * Changing between AP and STA mode resets "mpc" to 1, and with MPC an unassociated STA powers
	 * the radio down (no scans, no join). Turn it off each time the radio comes up.
	 */
	if (nompc)
		wlc_iovar_setint(wl->wlc, "mpc", 0);
	err = wlc_up(wl->wlc);
	if (!err && wl->wlif->dev)
		netif_carrier_on(wl->wlif->dev);
	return err;
}

void wl_down(struct wl_info *wl)
{
	if (wl->wlif && wl->wlif->dev)
		netif_carrier_off(wl->wlif->dev);
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
#define WLC_E_SET_SSID		0
#define WLC_E_LINK		16
#define WLC_E_SCAN_COMPLETE	26
#define WLC_E_PSK_SUP		46

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

#define BCMEV_OFF_FLAGS		26
#define BCMEV_OFF_STATUS	32
#define BCMEV_OFF_REASON	36
#define BCMEV_OFF_BSSCFGIDX	71	/* v2: ifidx at 70, bsscfgidx at 71 */
#define BCMEV_OFF_ADDR		48

/*
 * Core events for cfg80211. Returns true if skb is a core event: with cfg80211 nothing in userspace
 * reads those (no nas), and passing them up only feeds the bridge frames with the BSS's own address.
 */
static bool wl_event_to_cfg(struct wl_info *wl, struct wl_if *ifp, struct sk_buff *skb)
{
	u8 *d = skb->data;
	struct wl_cfg_event ev;

	if (!wl->cfg || skb->len < BCMEV_V2_HDRLEN || get_unaligned_be16(d + 12) != 0x886c ||
	    d[BCMEV_OFF_OUI] != 0x00 || d[BCMEV_OFF_OUI + 1] != 0x10 || d[BCMEV_OFF_OUI + 2] != 0x18 ||
	    get_unaligned_be16(d + BCMEV_OFF_USRSUB) != 1 || get_unaligned_be16(d + BCMEV_OFF_VER) != 2)
		return false;
	ev.type = get_unaligned_be32(d + BCMEV_OFF_TYPE);
	ev.status = get_unaligned_be32(d + BCMEV_OFF_STATUS);
	ev.reason = get_unaligned_be32(d + BCMEV_OFF_REASON);
	ev.flags = get_unaligned_be16(d + BCMEV_OFF_FLAGS);
	ev.len = get_unaligned_be32(d + BCMEV_OFF_DATALEN);
	if (BCMEV_V2_HDRLEN + ev.len > skb->len)
		ev.len = 0;
	ev.data = d + BCMEV_V2_HDRLEN;
	ev.bsscfgidx = d[BCMEV_OFF_BSSCFGIDX];	/* the core may send another BSS's events on bss 0 */
	if (trace_ioctl)
		pr_info("wl event type %u status %u reason %u flags 0x%x bss %d (rx on %s) %pM len %u\n",
			ev.type, ev.status, ev.reason, ev.flags, ev.bsscfgidx,
			ifp == wl->wlif ? "bss 0" : "bss 1", d + BCMEV_OFF_ADDR, ev.len);
	if (trace_ioctl > 1 && ev.len)
		print_hex_dump(KERN_INFO, "wl event data ", DUMP_PREFIX_OFFSET, 32, 1, ev.data,
			       min_t(u32, ev.len, 96), false);
	memcpy(ev.addr, d + BCMEV_OFF_ADDR, ETH_ALEN);
	wl_cfg80211_event(wl->cfg, ifp, &ev);
	return true;
}

/* RX: shadow chain -> native skbs -> stack */
void wl_sendup(struct wl_info *wl, struct wl_if *wlif, void *p, int numpkt)
{
	struct wl_if *ifp = wlif ? wlif : wl->wlif;
	struct net_device *dev = ifp->dev;
	struct sk_buff *skb, *nxt;

	for (skb = osl_pkt_tonative(wl->osh, p); skb; skb = nxt) {
		nxt = skb->next;
		skb->next = NULL;
		if (wl_event_to_cfg(wl, ifp, skb) || !dev) {	/* consumed, or no interface now */
			dev_kfree_skb_any(skb);
			continue;
		}
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
	struct net_device *dev = (wlif ? wlif : wl->wlif)->dev;

	if (!dev)
		return;
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

/*
 * The core calls this when it creates a bsscfg (under the perimeter lock, from inside the ioctl
 * that created it). The interface was allocated beforehand by wl_cfg_if_bss_create(); a bsscfg
 * created some other way (e.g. "wl ssid -C 1" from the stock tool) is refused.
 */
struct wl_if *wl_add_if(struct wl_info *wl, struct wlc_if *wlcif, uint unit, struct ether_addr *remote)
{
	struct wl_if *ifp = wl->pending_if;

	if (!ifp || remote) {
		pr_warn("wl%d: virtual interface %u requested: only through cfg80211 (AP+STA)\n",
			PUB_UNIT(wl->pub), unit);
		return NULL;
	}
	wl->pending_if = NULL;
	ifp->wlcif = wlcif;
	return ifp;
}

void wl_del_if(struct wl_info *wl, struct wl_if *wlif)
{
	if (wlif && wlif != wl->wlif)
		wlif->wlcif = NULL;	/* the core freed the bsscfg; the wl_if stays with cfg80211 */
}

char *wl_ifname(struct wl_info *wl, struct wl_if *wlif)
{
	struct net_device *dev = (wlif ? wlif : wl->wlif)->dev;

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

/* the radio is up while any of its interfaces is open */
static int wl_open(struct net_device *dev)
{
	struct wl_if *ifp = WLIF(dev);
	struct wl_info *wl = ifp->wl;
	int err = 0;

	WL_LOCK(wl);
	if (!wl->n_open)
		err = wl_up(wl);
	if (!err) {
		wl->n_open++;
		ifp->open = TRUE;
	}
	WL_UNLOCK(wl);
	if (err) {
		netdev_err(dev, "wlc_up failed: %d\n", err);
		return -ENODEV;
	}
	netif_carrier_on(dev);
	netif_start_queue(dev);
	return 0;
}

static int wl_close(struct net_device *dev)
{
	struct wl_if *ifp = WLIF(dev);
	struct wl_info *wl = ifp->wl;

	netif_stop_queue(dev);
	netif_carrier_off(dev);
	wl_cfg80211_if_down(wl->cfg, dev);
	WL_LOCK(wl);
	if (ifp->open) {
		ifp->open = FALSE;
		if (--wl->n_open == 0)
			wl_down(wl);
	}
	WL_UNLOCK(wl);
	return 0;
}

static netdev_tx_t wl_start(struct sk_buff *skb, struct net_device *dev)
{
	struct wl_if *wlif = WLIF(dev);
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
	if (!wlif->wlcif) {		/* BSS not created in the core (yet) */
		WL_UNLOCK(wl);
		PKTFREE(wl->osh, pkt, TRUE);
		dev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}
	wlc_sendpkt(wl->wlc, pkt, wlif->wlcif);	/* core owns pkt now, frees it via PKTFREE */
	WL_UNLOCK(wl);
	return NETDEV_TX_OK;
}

/* SIOCDEVPRIVATE: the Broadcom wl_ioctl_t protocol used by the stock `wl` tool */
static int wl_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	struct wl_if *wlif = WLIF(dev);
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
	if (trace_ioctl > 1 && buf && ioc.len >= 8)	/* debugging only: never with real keys */
		pr_info("wl ioctl %s cmd %d head %*ph\n", current->comm, ioc.cmd, 8, buf);
	if (!err && ioc.cmd == WLC_SET_SSID && buf && ioc.len >= sizeof(wlc_ssid_t))
		wl_cfg80211_ssid_changed(wl->cfg, wlif, ((wlc_ssid_t *)buf)->SSID,
					 min_t(uint32, ((wlc_ssid_t *)buf)->SSID_len, 32));
	if (!err && buf && copy_to_user(ioc.buf, buf, ioc.len))
		err = BCME_BADADDR;
	kfree(buf);
	return err ? -EINVAL : 0;
}

/*
 * A bsscfg's own address, perimeter lock held. The core takes it only with the radio down
 * (BCME_NOTDOWN otherwise, also for a bsscfg that is itself down): then the radio goes down and up
 * around it. BSSes that were up come back by themselves and their stations stay associated (tested
 * with an AP on bsscfg 1). Nothing is done if the address is already the one asked for.
 */
static int wl_bss_mac_apply(struct wl_info *wl, struct wl_if *ifp, const u8 *mac)
{
	u8 cur[WLC_IOCTL_SMLEN];
	int err;

	if (!ifp->wlcif)
		return BCME_NOTFOUND;
	if (!wlc_iovar_op(wl->wlc, "cur_etheraddr", NULL, 0, cur, sizeof(cur), FALSE, ifp->wlcif) &&
	    ether_addr_equal(cur, mac))
		return 0;
	err = wlc_iovar_op(wl->wlc, "cur_etheraddr", NULL, 0, (void *)mac, ETHER_ADDR_LEN, TRUE,
			   ifp->wlcif);
	if (err != BCME_NOTDOWN)
		return err;
	if (trace_ioctl)
		pr_info("wl: radio down/up for bss %d address %pM\n", ifp->idx, mac);
	wlc_ioctl(wl->wlc, WLC_DOWN, NULL, 0, wl->wlif->wlcif);
	err = wlc_iovar_op(wl->wlc, "cur_etheraddr", NULL, 0, (void *)mac, ETHER_ADDR_LEN, TRUE,
			   ifp->wlcif);
	if (wl->n_open) {
		int e = wlc_ioctl(wl->wlc, WLC_UP, NULL, 0, wl->wlif->wlcif);

		err = err ?: e;
	}
	return err;
}

static int wl_set_mac_address(struct net_device *dev, void *addr)
{
	struct sockaddr *sa = addr;
	struct wl_if *ifp = WLIF(dev);
	struct wl_info *wl = ifp->wl;
	int err = 0;

	if (!is_valid_ether_addr(sa->sa_data))
		return -EADDRNOTAVAIL;
	WL_LOCK(wl);
	if (ifp->wlcif)		/* else applied when the bsscfg is created */
		err = wl_bss_mac_apply(wl, ifp, sa->sa_data);
	WL_UNLOCK(wl);
	if (err) {
		netdev_warn(dev, "cannot set the BSS address %pM: %d\n", sa->sa_data, err);
		return -EINVAL;
	}
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

struct wl_if *wl_cfg_primary(struct wl_info *wl)
{
	return wl->wlif;
}

struct net_device *wl_cfg_if_dev(struct wl_if *ifp)
{
	return ifp->dev;
}

bool wl_cfg_if_has_bss(struct wl_if *ifp)
{
	return ifp->wlcif != NULL;
}

static struct net_device *wl_netdev_alloc(struct wl_if *ifp, const char *name,
					  unsigned char name_assign_type)
{
	struct wl_info *wl = ifp->wl;
	struct net_device *dev = alloc_netdev(sizeof(struct wl_if *), name, name_assign_type,
					      ether_setup);

	if (!dev)
		return NULL;
	*(struct wl_if **)netdev_priv(dev) = ifp;
	dev->netdev_ops = &wl_netdev_ops;
	dev->ethtool_ops = &wl_ethtool_ops;
	dev->needed_headroom = WL_TXHEADROOM;
	dev->base_addr = (unsigned long)wl->regsva;
	dev->irq = wl->irq;
	dev->needs_free_netdev = true;	/* freed by unregister_netdev(ice) */
	SET_NETDEV_DEV(dev, &wl->core->dev);
	return dev;
}

/* RTNL held (cfg80211 add_virtual_intf): allocate, then the caller sets ieee80211_ptr, then register */
struct net_device *wl_cfg_netdev_alloc(struct wl_if *ifp, const char *name,
				       unsigned char name_assign_type, const u8 *mac)
{
	struct net_device *dev;

	if (ifp->dev)
		return ERR_PTR(-EBUSY);
	dev = wl_netdev_alloc(ifp, name, name_assign_type);
	if (!dev)
		return ERR_PTR(-ENOMEM);
	memcpy(dev->dev_addr, mac, ETH_ALEN);
	return dev;
}

int wl_cfg_netdev_register(struct wl_if *ifp, struct net_device *dev)
{
	struct wl_info *wl = ifp->wl;
	int err;

	ASSERT_RTNL();
	err = register_netdevice(dev);
	if (err) {
		free_netdev(dev);
		return err;
	}
	WL_LOCK(wl);
	ifp->dev = dev;
	/* the BSS sends from the interface's address (hostapd/mac80211.sh may change it later) */
	err = ifp->wlcif ? wl_bss_mac_apply(wl, ifp, dev->dev_addr) : 0;
	if (err)
		pr_warn("%s: cannot set the BSS address %pM: %d\n", dev->name, dev->dev_addr, err);
	WL_UNLOCK(wl);
	return 0;
}

int wl_cfg_if_set_mac(struct wl_if *ifp, const u8 *mac)
{
	struct wl_info *wl = ifp->wl;
	int err = -ENODEV;

	WL_LOCK(wl);
	if (ifp->wlcif)
		err = wl_bss_mac_apply(wl, ifp, mac);
	WL_UNLOCK(wl);
	if (err)
		pr_warn("wl: cannot set a BSS address to %pM: %d\n", mac, err);
	return err ? -EIO : 0;
}

int wl_cfg_netdev_move(struct wl_if *from, struct wl_if *to, const u8 *mac)
{
	struct wl_info *wl = from->wl;
	struct net_device *dev = from->dev;
	int err = 0;

	ASSERT_RTNL();
	if (!dev || to->dev || netif_running(dev))
		return -EBUSY;
	WL_LOCK(wl);
	from->dev = NULL;
	to->dev = dev;
	*(struct wl_if **)netdev_priv(dev) = to;
	if (to->wlcif)
		err = wl_bss_mac_apply(wl, to, mac);
	WL_UNLOCK(wl);
	if (err)
		pr_warn("%s: cannot set the BSS address %pM: %d\n", dev->name, mac, err);
	if (!ether_addr_equal(dev->dev_addr, mac)) {
		memcpy(dev->dev_addr, mac, ETH_ALEN);
		call_netdevice_notifiers(NETDEV_CHANGEADDR, dev);
	}
	return 0;
}

/* RTNL held (cfg80211 del_virtual_intf, detach) */
void wl_cfg_netdev_destroy(struct wl_if *ifp)
{
	struct wl_info *wl = ifp->wl;
	struct net_device *dev = ifp->dev;

	ASSERT_RTNL();
	if (!dev)
		return;
	if (dev->flags & IFF_UP)
		dev_close(dev);
	WL_LOCK(wl);
	ifp->dev = NULL;		/* no more RX or flow control for it */
	WL_UNLOCK(wl);
	unregister_netdevice(dev);	/* frees it (needs_free_netdev) */
}

struct wl_if *wl_cfg_if_alloc(struct wl_info *wl)
{
	struct wl_if *ifp = kzalloc(sizeof(*ifp), GFP_KERNEL);

	if (ifp)
		ifp->wl = wl;
	return ifp;
}

void wl_cfg_if_free(struct wl_if *ifp)
{
	kfree(ifp);
}

/* "bsscfg:<name>" iovar: int32 bsscfg index, then the value */
static int wl_bssiovar_set(struct wl_info *wl, int idx, const char *name, const void *val, int len)
{
	int nlen = strlen("bsscfg:") + strlen(name) + 1;
	u8 *buf = kzalloc(max_t(int, nlen + 4 + len, WLC_IOCTL_SMLEN), GFP_KERNEL);
	int32 i = idx;
	int err;

	if (!buf)
		return -ENOMEM;
	snprintf(buf, nlen, "bsscfg:%s", name);
	memcpy(buf + nlen, &i, 4);
	memcpy(buf + nlen + 4, val, len);
	WL_LOCK(wl);
	err = wlc_ioctl(wl->wlc, WLC_SET_VAR, buf, nlen + 4 + len, wl->wlif->wlcif);
	WL_UNLOCK(wl);
	if (trace_ioctl || err)
		pr_info("wl cfg bssiovar %s idx %d len %d err %d\n", buf, idx, len, err);
	kfree(buf);
	return err ? -EIO : 0;
}

/*
 * The "bss" iovar takes the bsscfg index itself: { int32 idx, int32 val } (val 0 down, 1 up,
 * 2 sta, 3 ap), as the stock wl sends "bss -C 1 up". The "bsscfg:" prefix form is refused
 * (BCME_BUFTOOSHORT).
 */
static int wl_bss_set(struct wl_info *wl, int idx, int32 val)
{
	int32 p[2] = { idx, val };
	u8 *buf = kzalloc(WLC_IOCTL_SMLEN, GFP_KERNEL);
	int err;

	if (!buf)
		return -ENOMEM;
	memcpy(buf, "bss", 4);
	memcpy(buf + 4, p, sizeof(p));
	WL_LOCK(wl);
	err = wlc_ioctl(wl->wlc, WLC_SET_VAR, buf, 4 + sizeof(p), wl->wlif->wlcif);
	WL_UNLOCK(wl);
	if (trace_ioctl || err)
		pr_info("wl cfg bss idx %d val %d err %d\n", idx, val, err);
	kfree(buf);
	return err ? -EIO : 0;
}

/*
 * Create the core's bsscfg idx for ifp. Setting its SSID makes the core allocate the bsscfg and call
 * wl_add_if() synchronously, which picks up ifp from pending_if. The real SSID comes with start_ap.
 */
int wl_cfg_if_bss_create(struct wl_if *ifp, int idx, const u8 *mac)
{
	struct wl_info *wl = ifp->wl;
	wlc_ssid_t ssid;
	int err;

	if (ifp->wlcif)
		return 0;
	ifp->idx = idx;
	memset(&ssid, 0, sizeof(ssid));
	ssid.SSID_len = 1;
	ssid.SSID[0] = '-';
	WL_LOCK(wl);
	wl->pending_if = ifp;
	WL_UNLOCK(wl);
	err = wl_bssiovar_set(wl, idx, "ssid", &ssid, sizeof(ssid));
	WL_LOCK(wl);
	wl->pending_if = NULL;
	WL_UNLOCK(wl);
	if (!err && !ifp->wlcif)
		err = -ENODEV;
	if (err)
		return err;
	wl_bss_set(wl, idx, 3);	/* make it an AP */
	if (mac) {
		WL_LOCK(wl);
		err = wl_bss_mac_apply(wl, ifp, mac);
		WL_UNLOCK(wl);
		if (err)
			pr_warn("wl: cannot set bss %d address %pM: %d\n", idx, mac, err);
	}
	return 0;
}

/*
 * Every core call goes through a heap buffer of at least WLC_IOCTL_SMLEN bytes, and only len bytes
 * are copied back. Some queries return more than the structures in our headers describe (a STA-mode
 * query overran a stack buffer: "stack-protector: Kernel stack is corrupted"), and the stock glue
 * also always hands the core at least that much.
 */
#define WL_CFG_BOUNCE_MIN	WLC_IOCTL_SMLEN

int wl_cfg_ioctl(struct wl_if *ifp, int cmd, void *buf, int len)
{
	struct wl_info *wl = ifp->wl;
	int blen = max_t(int, len, WL_CFG_BOUNCE_MIN);
	u8 *b;
	int err;

	if (!ifp->wlcif)
		return -ENODEV;
	b = kzalloc(blen, GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	if (buf && len > 0)
		memcpy(b, buf, len);
	WL_LOCK(wl);
	err = wlc_ioctl(wl->wlc, cmd, buf ? b : NULL, len, ifp->wlcif);
	WL_UNLOCK(wl);
	if (trace_ioctl)
		pr_info("wl cfg ioctl cmd %d len %d err %d (%s)\n", cmd, len, err,
			ifp == wl->wlif ? "bss 0" : "bss 1");
	if (!err && buf && len > 0)
		memcpy(buf, b, len);
	kfree(b);
	return err ? -EIO : 0;
}

static int wl_cfg_iovar(struct wl_if *ifp, const char *name, void *params, int plen, void *arg,
			int len, bool set)
{
	struct wl_info *wl = ifp->wl;
	int blen = max_t(int, len, WL_CFG_BOUNCE_MIN);
	u8 *b;
	int err;

	if (!ifp->wlcif)
		return -ENODEV;
	b = kzalloc(blen, GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	if (arg && len > 0)
		memcpy(b, arg, len);
	WL_LOCK(wl);
	err = wlc_iovar_op(wl->wlc, name, params, plen, b, len, set, ifp->wlcif);
	WL_UNLOCK(wl);
	if (!err && !set && arg && len > 0)
		memcpy(arg, b, len);
	kfree(b);
	return err ? -EIO : 0;
}

int wl_cfg_iovar_getint(struct wl_if *ifp, const char *name, int *val)
{
	return wl_cfg_iovar(ifp, name, NULL, 0, val, sizeof(*val), FALSE);
}

int wl_cfg_iovar_setint(struct wl_if *ifp, const char *name, int val)
{
	return wl_cfg_iovar(ifp, name, NULL, 0, &val, sizeof(val), TRUE);
}

static int wl_cfg_setint(struct wl_if *ifp, int cmd, int val)
{
	return wl_cfg_ioctl(ifp, cmd, &val, sizeof(val));
}

int wl_cfg_channel(struct wl_if *ifp, int *chan)
{
	channel_info_t ci;
	int err = wl_cfg_ioctl(ifp, WLC_GET_CHANNEL, &ci, sizeof(ci));

	if (!err)
		*chan = ci.hw_channel;
	return err;
}

int wl_cfg_chanspec(struct wl_if *ifp, int *ctl, int *center, int *width)
{
	int cs, err = wl_cfg_iovar_getint(ifp, "chanspec", &cs);

	if (err)
		return err;
	*center = cs & WL_CHANSPEC_CHAN_MASK;
	if ((cs & WL_CHANSPEC_BW_MASK) == WL_CHANSPEC_BW_40) {
		*width = 40;
		*ctl = (cs & WL_CHANSPEC_CTL_SB_MASK) == WL_CHANSPEC_CTL_SB_LOWER ? *center - 2
										      : *center + 2;
	} else {
		*width = 20;
		*ctl = *center;
	}
	return 0;
}

/* 2.4 GHz chanspec for a 20 MHz channel, or a 40 MHz pair given by its control and centre channel */
static int wl_cfg_mk_chanspec(int ctl, int center)
{
	if (!center)
		return WL_CHANSPEC_BAND_2G | WL_CHANSPEC_BW_20 | WL_CHANSPEC_CTL_SB_NONE | ctl;
	return WL_CHANSPEC_BAND_2G | WL_CHANSPEC_BW_40 | center |
	       (ctl < center ? WL_CHANSPEC_CTL_SB_LOWER : WL_CHANSPEC_CTL_SB_UPPER);
}

int wl_cfg_valid_channels(struct wl_if *ifp, u32 *chans, int max)
{
	uint buf[1 + 64];	/* wl_uint32_list_t */
	int i, n, err;

	buf[0] = 64;
	err = wl_cfg_ioctl(ifp, WLC_GET_VALID_CHANNELS, buf, sizeof(buf));
	if (err)
		return err;
	n = min_t(int, buf[0], max);
	for (i = 0; i < n; i++)
		chans[i] = buf[1 + i];
	return n;
}

int wl_cfg_assoclist(struct wl_if *ifp, u8 (*macs)[6], int max)
{
	int i, n, err, len = sizeof(uint) + max * ETHER_ADDR_LEN;
	struct maclist *ml = kmalloc(len, GFP_KERNEL);

	if (!ml)
		return -ENOMEM;
	ml->count = max;
	err = wl_cfg_ioctl(ifp, WLC_GET_ASSOCLIST, ml, len);
	n = err ? err : min_t(int, ml->count, max);
	for (i = 0; i < n; i++)
		memcpy(macs[i], &ml->ea[i], ETHER_ADDR_LEN);
	kfree(ml);
	return n;
}

int wl_cfg_sta_info(struct wl_if *ifp, const u8 *mac, struct wl_cfg_sta *st)
{
	sta_info_t *si = kzalloc(WLC_IOCTL_SMLEN, GFP_KERNEL);
	struct ether_addr ea;
	scb_val_t sv;
	int err;

	if (!si)
		return -ENOMEM;
	memcpy(&ea, mac, ETHER_ADDR_LEN);
	err = wl_cfg_iovar(ifp, "sta_info", &ea, ETHER_ADDR_LEN, si, WLC_IOCTL_SMLEN, FALSE);
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

	/* AP mode: that station's RSSI; STA mode: the AP's (with the zero address too) */
	memset(&sv, 0, sizeof(sv));
	memcpy(&sv.ea, mac, ETHER_ADDR_LEN);
	if (!wl_cfg_ioctl(ifp, WLC_GET_RSSI, &sv, sizeof(sv)))
		st->rssi = (int32)sv.val;
	if (!st->rssi) {
		memset(&sv, 0, sizeof(sv));
		if (!wl_cfg_ioctl(ifp, WLC_GET_RSSI, &sv, sizeof(sv)))
			st->rssi = (int32)sv.val;
	}
	return 0;
}

int wl_cfg_noise(struct wl_if *ifp, int *dbm)
{
	int32 v;
	int err = wl_cfg_ioctl(ifp, WLC_GET_PHY_NOISE, &v, sizeof(v));

	if (!err)
		*dbm = v;
	return err;
}

/*
 * The core only queues events whose bit is set in "event_msgs"; nas used to set them. Add the ones
 * cfg80211 needs (AP stations, STA join/link/supplicant, scan), keeping whatever else is enabled.
 */
static int wl_cfg_enable_events(struct wl_info *wl)
{
	static const int ev[] = { WLC_E_SET_SSID, WLC_E_DEAUTH, WLC_E_DEAUTH_IND, WLC_E_ASSOC_IND,
				  WLC_E_REASSOC_IND, WLC_E_DISASSOC, WLC_E_DISASSOC_IND, WLC_E_LINK,
				  WLC_E_SCAN_COMPLETE, WLC_E_PSK_SUP };
	u8 mask[WL_EVENTING_MASK_LEN];
	int i, err;

	err = wl_cfg_iovar(wl->wlif, "event_msgs", NULL, 0, mask, sizeof(mask), FALSE);
	if (!err) {
		for (i = 0; i < ARRAY_SIZE(ev); i++)
			mask[ev[i] / 8] |= 1 << (ev[i] % 8);
		err = wl_cfg_iovar(wl->wlif, "event_msgs", NULL, 0, mask, sizeof(mask), TRUE);
	}
	return err;
}

/*
 * Radio mode. AP: the primary bsscfg is an AP (ap 1, apsta 0), as brcmwl.sh sets it up.
 * STA: ap 0. AP+STA: apsta 1, which makes the primary bsscfg the STA; AP bsscfgs are added with
 * wl_cfg_if_bss_create() and follow the STA's channel. Changing it needs the radio down.
 */
int wl_cfg_set_mode(struct wl_info *wl, bool primary_sta, bool apsta)
{
	struct wl_if *ifp = wl->wlif;
	int err;

	/*
	 * Always the full sequence: after a module load the core is ap 0 but also infra 0 (IBSS),
	 * so "already a STA" is not a safe shortcut (it joined as Ad Hoc).
	 */
	err = wl_cfg_ioctl(ifp, WLC_DOWN, NULL, 0);
	if (nompc)
		err = err ?: wl_cfg_iovar_setint(ifp, "mpc", 0);
	/*
	 * mbss: without it the receive address match only covers the primary MAC, so a secondary AP
	 * beacons and answers broadcast probes but never sees frames sent to its BSSID (clients time
	 * out at authentication). Its BSSIDs must then follow the MBSS pattern (wl_cfg80211.c).
	 */
	if (apsta) {
		err = err ?: wl_cfg_iovar_setint(ifp, "apsta", 1);
		err = err ?: wl_cfg_iovar_setint(ifp, "mbss", 1);
	} else {
		err = err ?: wl_cfg_iovar_setint(ifp, "mbss", 0);
		err = err ?: wl_cfg_iovar_setint(ifp, "apsta", 0);
		err = err ?: wl_cfg_setint(ifp, WLC_SET_AP, !primary_sta);
	}
	err = err ?: wl_cfg_setint(ifp, WLC_SET_INFRA, 1);
	/* a STA (and an AP next to it) may join a 40 MHz BSS; a lone AP sets its own in start_ap */
	if (!err && primary_sta)
		err = wl_cfg_iovar_setint(ifp, "mimo_bw_cap", 1);
	if (!err && nompc)		/* the mode change set it back to 1 */
		err = wl_cfg_iovar_setint(ifp, "mpc", 0);
	err = err ?: wl_cfg_enable_events(wl);
	if (!err && wl->n_open)
		err = wl_cfg_ioctl(ifp, WLC_UP, NULL, 0);
	return err;
}

static void wl_cfg_sec(u32 wsec_in, u32 auth_in, int *wsec, int *wpa_auth)
{
	*wsec = ((wsec_in & WL_CFG_WSEC_WEP) ? WEP_ENABLED : 0) |
		((wsec_in & WL_CFG_WSEC_TKIP) ? TKIP_ENABLED : 0) |
		((wsec_in & WL_CFG_WSEC_AES) ? AES_ENABLED : 0);
	*wpa_auth = ((auth_in & WL_CFG_AUTH_WPA_PSK) ? WPA_AUTH_PSK : 0) |
		    ((auth_in & WL_CFG_AUTH_WPA2_PSK) ? WPA2_AUTH_PSK : 0) |
		    ((auth_in & WL_CFG_AUTH_WPA_8021X) ? WPA_AUTH_UNSPECIFIED : 0) |
		    ((auth_in & WL_CFG_AUTH_WPA2_8021X) ? WPA2_AUTH_UNSPECIFIED : 0);
}

/*
 * The radio's channel and width, set by the first AP to start while the primary (STA) bsscfg is
 * idle; with a STA connected (repeater) the radio follows the STA instead. First while up, so that
 * the BSSes stay as they are; the core may want the radio down for some of these.
 */
static int wl_cfg_radio_chan(struct wl_info *wl, const struct wl_cfg_ap *ap)
{
	struct wl_if *pri = wl->wlif;
	int bw = ap->center_channel ? 1 : 0;
	int cs = wl_cfg_mk_chanspec(ap->channel, ap->center_channel);
	int err;

	err = wl_cfg_iovar_setint(pri, "mimo_bw_cap", bw);
	err = err ?: wl_cfg_iovar_setint(pri, "chanspec", cs);
	if (!err && ap->beacon_int && wl_cfg_setint(pri, WLC_SET_BCNPRD, ap->beacon_int))
		err = -EBUSY;
	if (!err)
		return 0;
	if (trace_ioctl)
		pr_info("wl: radio channel 0x%04x needs the radio down\n", cs);
	err = wl_cfg_ioctl(pri, WLC_DOWN, NULL, 0);
	if (nompc)
		err = err ?: wl_cfg_iovar_setint(pri, "mpc", 0);
	err = err ?: wl_cfg_iovar_setint(pri, "mimo_bw_cap", bw);
	err = err ?: wl_cfg_iovar_setint(pri, "chanspec", cs);
	if (!err && ap->beacon_int)
		err = wl_cfg_setint(pri, WLC_SET_BCNPRD, ap->beacon_int);
	if (wl->n_open) {
		int e = wl_cfg_ioctl(pri, WLC_UP, NULL, 0);

		err = err ?: e;
	}
	return err;
}

/*
 * AP on the primary bsscfg: the same sequence as brcmwl.sh, known to work with this core.
 * AP on a secondary bsscfg (AP+STA): security per bsscfg, channel and beacon timing come from the
 * radio (the STA), and the BSS is brought up with the "bss" iovar instead of WLC_UP.
 */
int wl_cfg_start_ap(struct wl_if *ifp, const struct wl_cfg_ap *ap)
{
	struct wl_info *wl = ifp->wl;
	bool primary = ifp == wl->wlif;
	bool eap = ap->auth & (WL_CFG_AUTH_WPA_8021X | WL_CFG_AUTH_WPA2_8021X);
	int wsec, wpa_auth, err;
	wlc_ssid_t ssid;

	if (ap->ssid_len < 0 || ap->ssid_len > sizeof(ssid.SSID))
		return -EINVAL;
	wl_cfg_sec(ap->wsec, ap->auth, &wsec, &wpa_auth);
	if (primary) {
		err = wl_cfg_ioctl(ifp, WLC_DOWN, NULL, 0);
		if (nompc)
			err = err ?: wl_cfg_iovar_setint(ifp, "mpc", 0);
		err = err ?: wl_cfg_iovar_setint(ifp, "apsta", 0);
		err = err ?: wl_cfg_setint(ifp, WLC_SET_AP, 1);
		err = err ?: wl_cfg_setint(ifp, WLC_SET_INFRA, 1);
		/*
		 * Bandwidth: mimo_bw_cap 1 allows 40 MHz on 2.4 GHz (the default, 2, only on 5 GHz), and
		 * the chanspec picks the pair. hostapd has already done the 20/40 coexistence scan.
		 */
		err = err ?: wl_cfg_iovar_setint(ifp, "mimo_bw_cap", ap->center_channel ? 1 : 0);
		err = err ?: wl_cfg_iovar_setint(ifp, "chanspec",
						 wl_cfg_mk_chanspec(ap->channel, ap->center_channel));
		if (!err && ap->beacon_int)
			err = wl_cfg_setint(ifp, WLC_SET_BCNPRD, ap->beacon_int);
		if (!err && ap->dtim)
			err = wl_cfg_setint(ifp, WLC_SET_DTIMPRD, ap->dtim);
	} else {
		wl_bss_set(wl, ifp->idx, 0);	/* down first; fails harmlessly if it already is */
		err = ap->set_radio ? wl_cfg_radio_chan(wl, ap) : 0;
		if (!err && ap->dtim && wl_cfg_setint(ifp, WLC_SET_DTIMPRD, ap->dtim))
			pr_info("wl: DTIM period %d not set on bss %d\n", ap->dtim, ifp->idx);
	}
	err = err ?: wl_cfg_setint(ifp, WLC_SET_CLOSED, ap->hidden);
	err = err ?: wl_cfg_setint(ifp, WLC_SET_AUTH, ap->shared_key ? 1 : 0);
	err = err ?: wl_cfg_setint(ifp, WLC_SET_WSEC, wsec);
	err = err ?: wl_cfg_setint(ifp, WLC_SET_WPA_AUTH, wpa_auth);
	err = err ?: wl_cfg_iovar_setint(ifp, "wsec_restrict", wpa_auth || wsec ? 1 : 0);
	err = err ?: wl_cfg_setint(ifp, WLC_SET_EAP_RESTRICT, eap);
	if (!err && ap->inactivity_s > 0 && wl_cfg_iovar_setint(ifp, "scb_activity_time", ap->inactivity_s))
		pr_warn("wl: could not set scb_activity_time %d\n", ap->inactivity_s);
	err = err ?: wl_cfg_enable_events(wl);
	if (primary)
		err = err ?: wl_cfg_ioctl(ifp, WLC_UP, NULL, 0);
	if (err)
		return err;
	memset(&ssid, 0, sizeof(ssid));
	ssid.SSID_len = ap->ssid_len;
	memcpy(ssid.SSID, ap->ssid, ap->ssid_len);
	if (primary)
		return wl_cfg_ioctl(ifp, WLC_SET_SSID, &ssid, sizeof(ssid));
	err = wl_bssiovar_set(wl, ifp->idx, "ssid", &ssid, sizeof(ssid));
	return err ?: wl_bss_set(wl, ifp->idx, 1);
}

int wl_cfg_stop_ap(struct wl_if *ifp)
{
	if (ifp == ifp->wl->wlif)
		return wl_cfg_ioctl(ifp, WLC_DOWN, NULL, 0);
	return wl_bss_set(ifp->wl, ifp->idx, 0);
}

int wl_cfg_set_key(struct wl_if *ifp, int idx, const u8 *mac, enum wl_cfg_cipher cipher,
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
	return wl_cfg_ioctl(ifp, WLC_SET_KEY, &k, sizeof(k));
}

int wl_cfg_del_key(struct wl_if *ifp, int idx, const u8 *mac)
{
	wl_wsec_key_t k;

	memset(&k, 0, sizeof(k));
	k.index = idx;
	if (mac)
		memcpy(&k.ea, mac, ETHER_ADDR_LEN);
	return wl_cfg_ioctl(ifp, WLC_SET_KEY, &k, sizeof(k));
}

/* group key TSC/PN, least significant byte first (as nas reads it for the GTK RSC) */
int wl_cfg_key_seq(struct wl_if *ifp, int idx, u8 seq[6])
{
	union { int32 idx; u8 b[8]; } buf;
	int err;

	memset(&buf, 0, sizeof(buf));
	buf.idx = idx;
	err = wl_cfg_ioctl(ifp, WLC_GET_KEY_SEQ, &buf, sizeof(buf));
	if (!err)
		memcpy(seq, buf.b, 6);
	return err;
}

int wl_cfg_authorize(struct wl_if *ifp, const u8 *mac, bool authorized)
{
	struct ether_addr ea;

	memcpy(&ea, mac, ETHER_ADDR_LEN);
	return wl_cfg_ioctl(ifp, authorized ? WLC_SCB_AUTHORIZE : WLC_SCB_DEAUTHORIZE, &ea, sizeof(ea));
}

int wl_cfg_deauth(struct wl_if *ifp, const u8 *mac, int reason)
{
	scb_val_t sv;

	memset(&sv, 0, sizeof(sv));
	sv.val = reason;
	memcpy(&sv.ea, mac, ETHER_ADDR_LEN);
	return wl_cfg_ioctl(ifp, WLC_SCB_DEAUTHENTICATE_FOR_REASON, &sv, sizeof(sv));
}

/* ---- station side ---- */

/*
 * Join. With a PMK, the core's own supplicant does the 4-way and group handshakes (sup_wpa 1, raw
 * 32-byte PMK); success is reported as WLC_E_PSK_SUP "keyed". Without one,
 * an external supplicant does them and installs keys with add_key (sup_wpa 0, its IE via "wpaie").
 */
int wl_cfg_join(struct wl_if *ifp, const struct wl_cfg_join *j)
{
	int wsec, wpa_auth, err;
	const char *step;
	wlc_ssid_t ssid;

	if (j->ssid_len < 0 || j->ssid_len > sizeof(ssid.SSID))
		return -EINVAL;
	wl_cfg_sec(j->wsec, j->auth, &wsec, &wpa_auth);
	/* infra/ap are set by wl_cfg_set_mode(): the core refuses WLC_SET_INFRA while up */
	step = "auth";
	err = wl_cfg_setint(ifp, WLC_SET_AUTH, j->shared_key ? 1 : 0);
	if (!err) {
		step = "wsec";
		err = wl_cfg_setint(ifp, WLC_SET_WSEC, wsec);
	}
	if (!err) {
		step = "wpa_auth";
		err = wl_cfg_setint(ifp, WLC_SET_WPA_AUTH, wpa_auth);
	}
	if (!err) {
		step = "sup_wpa";
		err = wl_cfg_iovar_setint(ifp, "sup_wpa", j->pmk ? 1 : 0);
	}
	if (!err && j->pmk) {
		/*
		 * wsec_pmk_t with the PMK as 32 binary bytes (key_len 32, flags 0). This core rejects the
		 * 64-hex-digit form that newer firmware (and brcmfmac) uses; the stock wl's set_pmk only
		 * ever sends a passphrase (flags WSEC_PASSPHRASE). Tested with wl-port/d0/pmktest.c.
		 */
		union { wsec_pmk_t pmk; u8 raw[128]; } b;

		step = "pmk";
		memset(&b, 0, sizeof(b));
		memcpy(b.pmk.key, j->pmk, 32);
		b.pmk.key_len = 32;
		b.pmk.flags = 0;
		err = wl_cfg_ioctl(ifp, WLC_SET_WSEC_PMK, &b, sizeof(b.pmk));
		memzero_explicit(&b, sizeof(b));
	} else if (!err && j->wpaie && j->wpaie_len > 0) {
		step = "wpaie";
		err = wl_cfg_iovar(ifp, "wpaie", NULL, 0, (void *)j->wpaie, j->wpaie_len, TRUE);
	}
	if (!err) {
		step = "ssid";
		memset(&ssid, 0, sizeof(ssid));
		ssid.SSID_len = j->ssid_len;
		memcpy(ssid.SSID, j->ssid, j->ssid_len);
		err = wl_cfg_ioctl(ifp, WLC_SET_SSID, &ssid, sizeof(ssid));
	}
	if (err)
		pr_warn("wl: join failed at %s: %d\n", step, err);
	return err;
}

int wl_cfg_disassoc(struct wl_if *ifp)
{
	return wl_cfg_ioctl(ifp, WLC_DISASSOC, NULL, 0);
}

int wl_cfg_bssid(struct wl_if *ifp, u8 bssid[6])
{
	struct ether_addr ea;
	int err = wl_cfg_ioctl(ifp, WLC_GET_BSSID, &ea, sizeof(ea));

	if (err)
		return -ENOTCONN;
	memcpy(bssid, &ea, ETHER_ADDR_LEN);
	return is_zero_ether_addr(bssid) ? -ENOTCONN : 0;
}

static void wl_cfg_bss_conv(const wl_bss_info_t *bi, struct wl_cfg_bss *b)
{
	memcpy(b->bssid, &bi->BSSID, ETHER_ADDR_LEN);
	b->channel = bi->n_cap && bi->ctl_ch ? bi->ctl_ch : CHSPEC_CHANNEL(bi->chanspec);
	b->capability = bi->capability;
	b->beacon_period = bi->beacon_period;
	b->rssi = bi->RSSI;
	b->ies = (const u8 *)bi + bi->ie_offset;
	b->ies_len = bi->ie_length;
}

#define WL_BSS_BUF_LEN	2048
#define WL_SCAN_BUF_LEN	(32 * 1024)

int wl_cfg_cur_bss(struct wl_if *ifp, void (*cb)(void *ctx, const struct wl_cfg_bss *), void *ctx)
{
	u8 *buf = kzalloc(WL_BSS_BUF_LEN, GFP_KERNEL);
	wl_bss_info_t *bi = (wl_bss_info_t *)(buf + 4);
	struct wl_cfg_bss b;
	int err;

	if (!buf)
		return -ENOMEM;
	*(uint32 *)buf = WL_BSS_BUF_LEN;
	err = wl_cfg_ioctl(ifp, WLC_GET_BSS_INFO, buf, WL_BSS_BUF_LEN);
	if (!err && (bi->version != WL_BSS_INFO_VERSION ||
		     bi->ie_offset + bi->ie_length > WL_BSS_BUF_LEN - 4))
		err = -EINVAL;
	if (!err) {
		wl_cfg_bss_conv(bi, &b);
		cb(ctx, &b);
	}
	kfree(buf);
	return err;
}

int wl_cfg_assoc_ies(struct wl_if *ifp, u8 **req, u32 *req_len, u8 **resp, u32 *resp_len)
{
	wl_assoc_info_t ai;
	int err;

	*req = *resp = NULL;
	*req_len = *resp_len = 0;
	err = wl_cfg_iovar(ifp, "assoc_info", NULL, 0, &ai, sizeof(ai), FALSE);
	if (err)
		return err;
	if (ai.req_len > 4 * 1024 || ai.resp_len > 4 * 1024)
		return -EINVAL;
	if (ai.req_len) {
		*req = kzalloc(ai.req_len, GFP_KERNEL);
		if (*req && !wl_cfg_iovar(ifp, "assoc_req_ies", NULL, 0, *req, ai.req_len, FALSE))
			*req_len = ai.req_len;
	}
	if (ai.resp_len) {
		*resp = kzalloc(ai.resp_len, GFP_KERNEL);
		if (*resp && !wl_cfg_iovar(ifp, "assoc_resp_ies", NULL, 0, *resp, ai.resp_len, FALSE))
			*resp_len = ai.resp_len;
	}
	return 0;
}

int wl_cfg_scan(struct wl_if *ifp, const u8 *ssid, int ssid_len)
{
	wl_scan_params_t sp;

	if (ssid_len < 0 || ssid_len > sizeof(sp.ssid.SSID))
		return -EINVAL;
	memset(&sp, 0, sizeof(sp));
	sp.ssid.SSID_len = ssid_len;
	memcpy(sp.ssid.SSID, ssid, ssid_len);
	memset(&sp.bssid, 0xff, ETHER_ADDR_LEN);
	sp.bss_type = DOT11_BSSTYPE_ANY;
	sp.scan_type = 0;		/* active */
	sp.nprobes = -1;
	sp.active_time = -1;
	sp.passive_time = -1;
	sp.home_time = -1;
	sp.channel_num = 0;		/* all valid channels */
	return wl_cfg_ioctl(ifp, WLC_SCAN, &sp, WL_SCAN_PARAMS_FIXED_SIZE);
}

int wl_cfg_scan_results(struct wl_if *ifp, void (*cb)(void *ctx, const struct wl_cfg_bss *),
			void *ctx)
{
	wl_scan_results_t *r = kzalloc(WL_SCAN_BUF_LEN, GFP_KERNEL);
	const u8 *p, *end;
	struct wl_cfg_bss b;
	uint32 i;
	int err;

	if (!r)
		return -ENOMEM;
	r->buflen = WL_SCAN_BUF_LEN;
	err = wl_cfg_ioctl(ifp, WLC_SCAN_RESULTS, r, WL_SCAN_BUF_LEN);
	if (err)
		goto out;
	p = (const u8 *)r->bss_info;
	end = (const u8 *)r + min_t(uint32, r->buflen, WL_SCAN_BUF_LEN);
	for (i = 0; i < r->count; i++) {
		const wl_bss_info_t *bi = (const wl_bss_info_t *)p;

		if (p + sizeof(*bi) > end || bi->length < sizeof(*bi) || p + bi->length > end)
			break;
		if (bi->version == WL_BSS_INFO_VERSION && bi->ie_offset + bi->ie_length <= bi->length) {
			wl_cfg_bss_conv(bi, &b);
			cb(ctx, &b);
		}
		p += bi->length;
	}
out:
	kfree(r);
	return err;
}

extern char *nvram_get(const char *name);	/* osl/linux_osl_414.c */

/*
 * The core has no "current TX power" query on this chip; with no user limit set it transmits at the
 * board maximum (per chain, minus per-rate offsets), capped by its country rules. The maximum is in
 * the SROM-format variables the core itself reads; on the BCM5357 they live in NVRAM under sb/1/.
 */
int wl_cfg_board_maxpower(struct wl_if *ifp, int *qdbm)
{
	static const char * const pfx[] = { "sb/1/", "" };
	char name[24], *v;
	int i, c, best = 0;
	unsigned long q;

	for (i = 0; i < ARRAY_SIZE(pfx) && !best; i++) {
		for (c = 0; c < 2; c++) {
			snprintf(name, sizeof(name), "%smaxp2ga%d", pfx[i], c);
			v = nvram_get(name);
			if (v && !kstrtoul(v, 0, &q) && q > best && q < 0xff)
				best = q;
		}
	}
	if (!best)
		return -ENODATA;
	*qdbm = best;
	return 0;
}

static void wl_free(struct wl_info *wl)
{
	struct wl_if *ifp = wl->wlif;
	struct net_device *dev = ifp ? ifp->dev : NULL;

	if (dev && dev->reg_state == NETREG_REGISTERED) {
		unregister_netdev(dev);		/* frees it (needs_free_netdev) */
		dev = NULL;
	}
	if (ifp)
		ifp->dev = NULL;
	wl_cfg80211_detach(wl->cfg);	/* other net_devices, wiphy_unregister */
	if (wl->irq_requested)
		free_irq(wl->irq, wl);
	tasklet_kill(&wl->tasklet);
	if (wl->wlc)
		wlc_detach(wl->wlc);
	wl_cfg80211_free(wl->cfg);	/* after the core: it may still call wl_del_if() */
	wl->cfg = NULL;
	while (wl->timers)
		wl_free_timer(wl, wl->timers);
	if (wl->regsva)
		iounmap(wl->regsva);
	if (dev)
		free_netdev(dev);	/* never registered */
	kfree(ifp);
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
	wlif = kzalloc(sizeof(*wlif), GFP_KERNEL);
	if (!wlif)
		goto fail;
	wlif->wl = wl;
	wl->wlif = wlif;

	wl->regsva = ioremap_nocache(regs, PCI_BAR0_WINSZ);
	if (!wl->regsva)
		goto fail;
#ifdef WL_CFG80211
	dev = wl_netdev_alloc(wlif, "wlan%d", NET_NAME_ENUM);	/* what OpenWrt's mac80211.sh expects */
#else
	dev = wl_netdev_alloc(wlif, "wl%d", NET_NAME_ENUM);	/* what brcmwl.sh/wlc expect */
#endif
	if (!dev)
		goto fail;
	dev->base_addr = regs;
	wlif->dev = dev;

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
	WL_LOCK(wl);
	if (wl->pub && PUB_UP(wl->pub))
		wl_down(wl);
	WL_UNLOCK(wl);
	wl_free(wl);		/* unregister_netdev() closes the interfaces */
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
