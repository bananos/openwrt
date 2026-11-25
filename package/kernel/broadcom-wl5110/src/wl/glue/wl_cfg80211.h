/*
 * Interface between the wl 5.110 glue (wl_linux_414.c, which knows the Broadcom structures) and
 * its cfg80211 front end (wl_cfg80211.c, which knows cfg80211). ISC, see wl_cfg80211.c.
 */
#ifndef WL_CFG80211_H
#define WL_CFG80211_H

#include <linux/types.h>

struct wl_info;
struct wl_cfg;
struct net_device;
struct device;

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

/* Provided by wl_linux_414.c. Process context only (they allocate and take the perimeter lock).
 * All return 0 or a negative errno. */
int wl_cfg_ioctl(struct wl_info *wl, int cmd, void *buf, int len);
int wl_cfg_iovar_getint(struct wl_info *wl, const char *name, int *val);
int wl_cfg_iovar_setint(struct wl_info *wl, const char *name, int val);
int wl_cfg_channel(struct wl_info *wl, int *chan);
int wl_cfg_valid_channels(struct wl_info *wl, u32 *chans, int max);	/* returns count */
int wl_cfg_assoclist(struct wl_info *wl, u8 (*macs)[6], int max);	/* returns count */
int wl_cfg_sta_info(struct wl_info *wl, const u8 *mac, struct wl_cfg_sta *st);
int wl_cfg_noise(struct wl_info *wl, int *dbm);
int wl_cfg_ssid(struct wl_info *wl, u8 *ssid, int *len);

/* AP settings from cfg80211 start_ap, in neutral terms (wl_linux_414.c maps them to wsec/wpa_auth) */
#define WL_CFG_WSEC_WEP		0x1
#define WL_CFG_WSEC_TKIP	0x2
#define WL_CFG_WSEC_AES		0x4

#define WL_CFG_AUTH_WPA_PSK	0x1
#define WL_CFG_AUTH_WPA2_PSK	0x2
#define WL_CFG_AUTH_WPA_8021X	0x4
#define WL_CFG_AUTH_WPA2_8021X	0x8

struct wl_cfg_ap {
	int channel;
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

enum wl_cfg_cipher { WL_CFG_CIPHER_WEP40, WL_CFG_CIPHER_WEP104, WL_CFG_CIPHER_TKIP,
		     WL_CFG_CIPHER_CCMP };

int wl_cfg_start_ap(struct wl_info *wl, const struct wl_cfg_ap *ap);
int wl_cfg_stop_ap(struct wl_info *wl);
/* mac == NULL: group/default key at idx; otherwise the station's pairwise key */
int wl_cfg_set_key(struct wl_info *wl, int idx, const u8 *mac, enum wl_cfg_cipher cipher,
		   const u8 *key, int key_len, bool primary);
int wl_cfg_del_key(struct wl_info *wl, int idx, const u8 *mac);
int wl_cfg_key_seq(struct wl_info *wl, int idx, u8 seq[6]);
int wl_cfg_authorize(struct wl_info *wl, const u8 *mac, bool authorized);
int wl_cfg_deauth(struct wl_info *wl, const u8 *mac, int reason);

#ifdef WL_CFG80211
struct wl_cfg *wl_cfg80211_attach(struct wl_info *wl, struct net_device *dev, struct device *parent);
void wl_cfg80211_detach(struct wl_cfg *cfg);
const char *wl_cfg80211_name(struct wl_cfg *cfg);
/* the core's SSID changed (WLC_SET_SSID from userspace); process context */
void wl_cfg80211_ssid_changed(struct wl_cfg *cfg, const u8 *ssid, int len);
/* a station (re)associated (ies = its association-request IEs) or left; softirq context */
void wl_cfg80211_sta_assoc(struct wl_cfg *cfg, const u8 *mac, const u8 *ies, int ies_len);
void wl_cfg80211_sta_left(struct wl_cfg *cfg, const u8 *mac);
#else
static inline struct wl_cfg *wl_cfg80211_attach(struct wl_info *wl, struct net_device *dev,
						struct device *parent) { return NULL; }
static inline void wl_cfg80211_detach(struct wl_cfg *cfg) { }
static inline const char *wl_cfg80211_name(struct wl_cfg *cfg) { return ""; }
static inline void wl_cfg80211_ssid_changed(struct wl_cfg *cfg, const u8 *ssid, int len) { }
static inline void wl_cfg80211_sta_assoc(struct wl_cfg *cfg, const u8 *mac, const u8 *ies,
					 int ies_len) { }
static inline void wl_cfg80211_sta_left(struct wl_cfg *cfg, const u8 *mac) { }
#endif

#endif
