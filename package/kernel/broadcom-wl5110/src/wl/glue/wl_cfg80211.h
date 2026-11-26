/* SPDX-License-Identifier: ISC */
/*
 * Interface between the wl 5.110 glue (wl_linux_414.c, which knows the Broadcom structures) and
 * its cfg80211 front end (wl_cfg80211.c, which knows cfg80211). ISC, see wl_cfg80211.c.
 *
 * A struct wl_if is one BSS of the core (bsscfg): index 0 is the primary, created at probe; a
 * second one is created on demand for AP+STA. It outlives its net_device, which wl_cfg80211.c
 * creates and destroys as OpenWrt adds and deletes interfaces.
 */
#ifndef WL_CFG80211_H
#define WL_CFG80211_H

#include <linux/types.h>

struct wl_info;
struct wl_if;
struct wl_cfg;
struct net_device;
struct device;
struct wireless_dev;

/* one associated station, from the core's sta_info and per-station RSSI */
struct wl_cfg_sta {
	u32 flags;		/* WL_CFG_STA_* */
	u32 idle_s;		/* seconds since last data frame from the station */
	u32 connected_s;	/* seconds since association */
	u32 tx_pkts, tx_failures, rx_pkts;
	u32 tx_rate_kbps, rx_rate_kbps;
	s32 rssi;		/* dBm, 0 if unknown */
};

#define WL_CFG_STA_WME		0x01
#define WL_CFG_STA_AUTHE	0x02
#define WL_CFG_STA_ASSOC	0x04
#define WL_CFG_STA_AUTHO	0x08
#define WL_CFG_STA_PS		0x10
#define WL_CFG_STA_HT		0x20

/* AP or STA security settings, in neutral terms (wl_linux_414.c maps them to wsec/wpa_auth) */
#define WL_CFG_WSEC_WEP		0x1
#define WL_CFG_WSEC_TKIP	0x2
#define WL_CFG_WSEC_AES		0x4

#define WL_CFG_AUTH_WPA_PSK	0x1
#define WL_CFG_AUTH_WPA2_PSK	0x2
#define WL_CFG_AUTH_WPA_8021X	0x4
#define WL_CFG_AUTH_WPA2_8021X	0x8

struct wl_cfg_ap {
	int channel;		/* control channel (used if set_radio) */
	int center_channel;	/* 40 MHz: centre of the pair (channel +/- 2); 0 = 20 MHz */
	bool set_radio;		/* set the radio's channel; not while a STA (repeater) decides it */
	bool ht;		/* 802.11n (with set_radio): off for hostapd's legacy mode */
	int beacon_int;		/* TU, 0 = leave */
	int dtim;		/* 0 = leave */
	bool hidden;
	bool shared_key;	/* WEP shared-key authentication */
	int inactivity_s;	/* 0 = leave the core's default */
	const u8 *ssid;
	int ssid_len;
	u32 wsec;		/* WL_CFG_WSEC_* */
	u32 auth;		/* WL_CFG_AUTH_* */
};

struct wl_cfg_join {
	const u8 *ssid;
	int ssid_len;
	u32 wsec;		/* WL_CFG_WSEC_* */
	u32 auth;		/* WL_CFG_AUTH_* */
	const u8 *pmk;		/* 32 bytes: the core's supplicant does the 4-way handshake */
	const u8 *wpaie;	/* external supplicant: its WPA/RSN IE (pmk == NULL) */
	int wpaie_len;
	bool shared_key;
};

/* one BSS from a scan or the current association */
struct wl_cfg_bss {
	u8 bssid[6];
	int channel;
	u16 capability;
	u16 beacon_period;
	s16 rssi;		/* dBm */
	const u8 *ies;
	u32 ies_len;
};

/* a core event, decoded (any context) */
struct wl_cfg_event {
	u32 type, status, reason, flags;
	int bsscfgidx;		/* which BSS of the core it is about */
	u8 addr[6];
	const u8 *data;
	u32 len;
};
/* event types wl_cfg80211.c handles (same numbers as the core's WLC_E_*) */
#define WL_CFG_E_SET_SSID	0
#define WL_CFG_E_DEAUTH		5
#define WL_CFG_E_DEAUTH_IND	6
#define WL_CFG_E_ASSOC_IND	8
#define WL_CFG_E_REASSOC_IND	10
#define WL_CFG_E_DISASSOC	11
#define WL_CFG_E_DISASSOC_IND	12
#define WL_CFG_E_LINK		16
#define WL_CFG_E_SCAN_COMPLETE	26
#define WL_CFG_E_PSK_SUP	46
#define WL_CFG_EVENT_LINK_UP	0x01	/* flags of WL_CFG_E_LINK */
#define WL_CFG_SUP_KEYED	6	/* status of WL_CFG_E_PSK_SUP: 4-way and group key done */

enum wl_cfg_cipher { WL_CFG_CIPHER_WEP40, WL_CFG_CIPHER_WEP104, WL_CFG_CIPHER_TKIP,
		     WL_CFG_CIPHER_CCMP };

/*
 * Provided by wl_linux_414.c. Process context only (they allocate and take the perimeter lock).
 * All return 0 or a negative errno. A struct wl_if names the BSS an operation is for; radio-wide
 * operations take any (normally the primary).
 */
struct wl_if *wl_cfg_primary(struct wl_info *wl);
struct net_device *wl_cfg_if_dev(struct wl_if *ifp);
bool wl_cfg_if_has_bss(struct wl_if *ifp);

/* net_device lifetime, RTNL held: allocate (then set ieee80211_ptr), register; destroy */
struct net_device *wl_cfg_netdev_alloc(struct wl_if *ifp, const char *name,
				       unsigned char name_assign_type, const u8 *mac);
int wl_cfg_netdev_register(struct wl_if *ifp, struct net_device *dev);	/* frees dev on error */
void wl_cfg_netdev_destroy(struct wl_if *ifp);
/* a BSS's own address (cur_etheraddr) */
int wl_cfg_if_set_mac(struct wl_if *ifp, const u8 *mac);
/* hand a (down) net_device over to another BSS (interface type change), with that BSS's address */
int wl_cfg_netdev_move(struct wl_if *from, struct wl_if *to, const u8 *mac);
/* a secondary BSS: allocated once, its core bsscfg created on first use */
struct wl_if *wl_cfg_if_alloc(struct wl_info *wl);
int wl_cfg_if_bss_create(struct wl_if *ifp, int idx, const u8 *mac);
void wl_cfg_if_free(struct wl_if *ifp);

/* radio mode: primary BSS as STA or AP; apsta = primary STA plus AP BSSes (the cfg80211 build) */
int wl_cfg_set_mode(struct wl_info *wl, bool primary_sta, bool apsta);

int wl_cfg_ioctl(struct wl_if *ifp, int cmd, void *buf, int len);
int wl_cfg_iovar_getint(struct wl_if *ifp, const char *name, int *val);
int wl_cfg_iovar_setint(struct wl_if *ifp, const char *name, int val);
/*
 * A radio-wide int iovar: nothing if it already has the value, else set (with the radio down and up
 * around it if the core does not take it while up); cycle: bring the radio down and up afterwards
 * too, for settings the core applies only when it comes up (qtxpower).
 */
int wl_cfg_radio_setint(struct wl_info *wl, const char *name, int val, bool cycle);
int wl_cfg_channel(struct wl_if *ifp, int *chan);
/* current chanspec: control and centre channel, width in MHz (20 or 40) */
int wl_cfg_chanspec(struct wl_if *ifp, int *ctl, int *center, int *width);
int wl_cfg_valid_channels(struct wl_if *ifp, u32 *chans, int max);	/* returns count */
int wl_cfg_assoclist(struct wl_if *ifp, u8 (*macs)[6], int max);	/* returns count */
int wl_cfg_sta_info(struct wl_if *ifp, const u8 *mac, struct wl_cfg_sta *st);
int wl_cfg_noise(struct wl_if *ifp, int *dbm);
/* the core's country (its channel list, power limits and Country element); -EINVAL if unknown */
int wl_cfg_set_country(struct wl_if *ifp, const char *cc);
int wl_cfg_board_maxpower(struct wl_if *ifp, int *qdbm);	/* maxp2ga0/1, quarter-dBm */

int wl_cfg_start_ap(struct wl_if *ifp, const struct wl_cfg_ap *ap);
int wl_cfg_stop_ap(struct wl_if *ifp);
/* mac == NULL: group/default key at idx; otherwise the station's pairwise key */
int wl_cfg_set_key(struct wl_if *ifp, int idx, const u8 *mac, enum wl_cfg_cipher cipher,
		   const u8 *key, int key_len, bool primary);
int wl_cfg_del_key(struct wl_if *ifp, int idx, const u8 *mac);
int wl_cfg_key_seq(struct wl_if *ifp, int idx, u8 seq[6]);
int wl_cfg_authorize(struct wl_if *ifp, const u8 *mac, bool authorized);
int wl_cfg_deauth(struct wl_if *ifp, const u8 *mac, int reason);
/* the AP's MAC filter: mode WL_CFG_ACL_*, n addresses (the core's maclist/macmode) */
#define WL_CFG_ACL_OFF		0
#define WL_CFG_ACL_DENY		1	/* deny the listed stations */
#define WL_CFG_ACL_ALLOW	2	/* allow only the listed stations */
int wl_cfg_set_acl(struct wl_if *ifp, int mode, const u8 (*macs)[6], int n);

/* station side */
int wl_cfg_join(struct wl_if *ifp, const struct wl_cfg_join *j);
int wl_cfg_disassoc(struct wl_if *ifp);
int wl_cfg_bssid(struct wl_if *ifp, u8 bssid[6]);	/* -ENOTCONN if not associated */
int wl_cfg_cur_bss(struct wl_if *ifp, void (*cb)(void *ctx, const struct wl_cfg_bss *), void *ctx);
/* association request/response IEs of the current association, kmalloc'ed (caller frees) */
int wl_cfg_assoc_ies(struct wl_if *ifp, u8 **req, u32 *req_len, u8 **resp, u32 *resp_len);
int wl_cfg_scan(struct wl_if *ifp, const u8 *ssid, int ssid_len);
int wl_cfg_scan_results(struct wl_if *ifp, void (*cb)(void *ctx, const struct wl_cfg_bss *),
			void *ctx);

#ifdef WL_CFG80211
struct wl_cfg *wl_cfg80211_attach(struct wl_info *wl, struct net_device *dev, struct device *parent);
/* stage 1, before the core detaches: destroy net_devices, unregister the wiphy */
void wl_cfg80211_detach(struct wl_cfg *cfg);
/* stage 2, after the core detached */
void wl_cfg80211_free(struct wl_cfg *cfg);
const char *wl_cfg80211_name(struct wl_cfg *cfg);
/* the core's SSID changed (WLC_SET_SSID from userspace); process context */
void wl_cfg80211_ssid_changed(struct wl_cfg *cfg, struct wl_if *ifp, const u8 *ssid, int len);
/* a core event for BSS ifp; softirq context */
void wl_cfg80211_event(struct wl_cfg *cfg, struct wl_if *ifp, const struct wl_cfg_event *ev);
/* net_device going down (ndo_stop, RTNL held): end what cfg80211 expects done by then (scans) */
void wl_cfg80211_if_down(struct wl_cfg *cfg, struct net_device *dev);
#else
static inline struct wl_cfg *wl_cfg80211_attach(struct wl_info *wl, struct net_device *dev,
						struct device *parent) { return NULL; }
static inline void wl_cfg80211_detach(struct wl_cfg *cfg) { }
static inline void wl_cfg80211_free(struct wl_cfg *cfg) { }
static inline const char *wl_cfg80211_name(struct wl_cfg *cfg) { return ""; }
static inline void wl_cfg80211_ssid_changed(struct wl_cfg *cfg, struct wl_if *ifp, const u8 *ssid,
					    int len) { }
static inline void wl_cfg80211_event(struct wl_cfg *cfg, struct wl_if *ifp,
				     const struct wl_cfg_event *ev) { }
#endif

#endif
