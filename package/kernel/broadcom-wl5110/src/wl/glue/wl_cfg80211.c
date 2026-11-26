// SPDX-License-Identifier: ISC
/*
 * cfg80211 front end for the Broadcom wl 5.110 core (experimental, Phase D).
 *
 * The radio is one wiphy with up to two interfaces (vifs), each one BSS of the core:
 *  - vif 0, the core's primary bsscfg: an AP, or a STA;
 *  - vif 1, a secondary bsscfg created on demand: an AP next to a STA on vif 0 (AP+STA, "repeater";
 *    the core's apsta mode). It shares the radio, so it runs on the STA's channel.
 * Net_devices are created and destroyed with the interfaces (add/del_virtual_intf), so OpenWrt's
 * mac80211.sh can name them as it likes (wlan0, wlan0-1, ...).
 *
 * AP: the core keeps doing 802.11 association, beacons and probe responses itself ("AP SME in the
 * driver", as in brcmfmac); hostapd configures the BSS with start_ap, runs the WPA handshakes over
 * the data path and installs keys and station authorization, which map onto the ioctls nas uses.
 * STA: driver SME. wpa_supplicant's connect hands over the PSK (4-way handshake offload), and the
 * core's own supplicant does the handshakes; its events become connect/port-authorized/disconnect
 * results. Scans use the core's scan engine.
 *
 * Everything Broadcom-specific stays in wl_linux_414.c behind the helpers in wl_cfg80211.h.
 *
 * Copyright (c) 2026 Andrew Druchenko
 *
 * Permission to use, copy, modify, and/or distribute this software for any purpose with or without
 * fee is hereby granted, provided that the above copyright notice and this permission notice appear
 * in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/slab.h>
#include <linux/rtnetlink.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>

#include "wl_cfg80211.h"

#define WL_CFG_MAX_STA		32
#define WL_QDBM_NO_LIMIT	127	/* qtxpower: no user limit set, real power is board-limited */
#define WL_CFG_NVIF		2

struct wl_cfg;

struct wl_cfg_vif {
	struct wl_cfg *cfg;
	struct wl_if *ifp;		/* the core BSS; vif 1's is allocated on first use */
	struct wireless_dev wdev;
	int idx;
	bool in_use;			/* handed out by add_virtual_intf */
	/* AP */
	u8 sta_macs[WL_CFG_MAX_STA][ETH_ALEN];	/* dump_station snapshot, taken at idx 0 */
	int n_sta;
	/* STA (vif 0 only) */
	bool connecting, connected, keyed, psk_offload;
	u8 bssid[ETH_ALEN];
};

struct wl_cfg_qev {
	struct list_head list;
	struct wl_cfg_vif *vif;
	struct wl_cfg_event ev;		/* data not kept */
};

struct wl_cfg {
	struct wl_info *wl;
	struct wiphy *wiphy;
	struct mac_address addrs[WL_CFG_NVIF];	/* wiphy->addresses */
	struct wl_cfg_vif vif[WL_CFG_NVIF];
	struct ieee80211_supported_band band;	/* per device: channels get disabled per country */
	struct cfg80211_scan_request *scan_req;
	/* core events that need process context (STA, scan), handled in order under RTNL */
	spinlock_t evlock;
	struct list_head evq;
	struct work_struct evwork;
	bool dying;
};

#define CHAN2G(_ch, _f) { .band = NL80211_BAND_2GHZ, .center_freq = (_f), .hw_value = (_ch), \
			  .max_power = 20 }

static const struct ieee80211_channel wl_cfg_channels_2g[] = {
	CHAN2G(1, 2412), CHAN2G(2, 2417), CHAN2G(3, 2422), CHAN2G(4, 2427), CHAN2G(5, 2432),
	CHAN2G(6, 2437), CHAN2G(7, 2442), CHAN2G(8, 2447), CHAN2G(9, 2452), CHAN2G(10, 2457),
	CHAN2G(11, 2462), CHAN2G(12, 2467), CHAN2G(13, 2472), CHAN2G(14, 2484),
};

static struct ieee80211_rate wl_cfg_rates[] = {
	{ .bitrate = 10, .hw_value = 2 },
	{ .bitrate = 20, .hw_value = 4, .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 55, .hw_value = 11, .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 110, .hw_value = 22, .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 60, .hw_value = 12 }, { .bitrate = 90, .hw_value = 18 },
	{ .bitrate = 120, .hw_value = 24 }, { .bitrate = 180, .hw_value = 36 },
	{ .bitrate = 240, .hw_value = 48 }, { .bitrate = 360, .hw_value = 72 },
	{ .bitrate = 480, .hw_value = 96 }, { .bitrate = 540, .hw_value = 108 },
};

/*
 * BCM5357C0: 2x2 802.11n. The chip can do 40 MHz, but start_ap does not set the core's bandwidth
 * yet, and hostapd needs a 20/40 coexistence scan before it uses HT40. So only 20 MHz is
 * advertised; otherwise LuCI offers HT40 and hostapd then fails to start.
 */
static const struct ieee80211_sta_ht_cap wl_cfg_ht_cap = {
	.ht_supported = true,
	.cap = IEEE80211_HT_CAP_SGI_20,
	.ampdu_factor = IEEE80211_HT_MAX_AMPDU_64K,
	.ampdu_density = IEEE80211_HT_MPDU_DENSITY_8,
	.mcs = {
		.rx_mask = { 0xff, 0xff },
		.rx_highest = cpu_to_le16(144),
		.tx_params = IEEE80211_HT_MCS_TX_DEFINED,
	},
};

/*
 * The core enforces its own country rules (it reports country XU, channels 1-13) and power limits,
 * so cfg80211 gets one permissive custom rule and the channels the core doesn't allow are disabled.
 * Without this, cfg80211's world domain would mark channels 12-13 no-IR.
 */
static const struct ieee80211_regdomain wl_cfg_regd = {
	.n_reg_rules = 1,
	.alpha2 = "99",
	.reg_rules = { REG_RULE(2412 - 10, 2484 + 10, 40, 0, 20, 0) },
};

static const u32 wl_cfg_ciphers[] = {
	WLAN_CIPHER_SUITE_WEP40, WLAN_CIPHER_SUITE_WEP104,
	WLAN_CIPHER_SUITE_TKIP, WLAN_CIPHER_SUITE_CCMP,
};

/* one STA plus one AP on the same channel (the core's apsta), or either alone */
static const struct ieee80211_iface_limit wl_cfg_limits[] = {
	{ .max = 1, .types = BIT(NL80211_IFTYPE_STATION) },
	{ .max = 1, .types = BIT(NL80211_IFTYPE_AP) },
};

static const struct ieee80211_iface_combination wl_cfg_combinations[] = {
	{
		.limits = wl_cfg_limits,
		.n_limits = ARRAY_SIZE(wl_cfg_limits),
		.max_interfaces = 2,
		.num_different_channels = 1,
	},
};

/* hostapd and wpa_supplicant register for action frames; the core handles those it supports */
static const struct ieee80211_txrx_stypes wl_cfg_mgmt_stypes[NUM_NL80211_IFTYPES] = {
	[NL80211_IFTYPE_AP] = {
		.tx = 0xffff,
		.rx = BIT(IEEE80211_STYPE_ACTION >> 4),
	},
	[NL80211_IFTYPE_STATION] = {
		.tx = 0xffff,
		.rx = BIT(IEEE80211_STYPE_ACTION >> 4),
	},
};

static struct wl_cfg *wl_cfg_of(struct wiphy *wiphy)
{
	return wiphy_priv(wiphy);
}

static struct wl_if *wl_cfg_pri(struct wiphy *wiphy)
{
	return wl_cfg_of(wiphy)->vif[0].ifp;
}

static struct wl_cfg_vif *wl_vif_of_wdev(struct wireless_dev *wdev)
{
	return container_of(wdev, struct wl_cfg_vif, wdev);
}

static struct wl_cfg_vif *wl_vif_of_dev(struct net_device *dev)
{
	return wl_vif_of_wdev(dev->ieee80211_ptr);
}

static struct wl_if *wl_ifp_of_dev(struct net_device *dev)
{
	return wl_vif_of_dev(dev)->ifp;
}

/* ------------------------------------------------------------------ radio-wide ops */

static int wl_cfg_get_channel(struct wiphy *wiphy, struct wireless_dev *wdev,
			      struct cfg80211_chan_def *chandef)
{
	struct ieee80211_channel *chan;
	int ch, err = wl_cfg_channel(wl_cfg_pri(wiphy), &ch);

	if (err)
		return err;
	chan = ieee80211_get_channel(wiphy, ieee80211_channel_to_frequency(ch, NL80211_BAND_2GHZ));
	if (!chan)
		return -EINVAL;
	cfg80211_chandef_create(chandef, chan, NL80211_CHAN_HT20);
	return 0;
}

/*
 * Effective TX power: the user limit (qtxpower, quarter-dBm; 127 = none) or the board maximum,
 * whichever is lower. The core lowers it further per rate and per country; it has no query for that.
 */
static int wl_cfg_get_tx_power(struct wiphy *wiphy, struct wireless_dev *wdev, int *dbm)
{
	struct wl_if *ifp = wl_cfg_pri(wiphy);
	int user, board, err = wl_cfg_iovar_getint(ifp, "qtxpower", &user);

	if (err)
		return err;
	user &= 0xff;		/* top bit is the override flag */
	if (wl_cfg_board_maxpower(ifp, &board)) {
		if (user >= WL_QDBM_NO_LIMIT)
			return -ENODATA;
		board = user;
	}
	*dbm = min(user, board) / 4;
	return 0;
}

/* LuCI's "Transmit Power" / iw ... set txpower: sets the user limit, never the override flag */
static int wl_cfg_set_tx_power(struct wiphy *wiphy, struct wireless_dev *wdev,
			       enum nl80211_tx_power_setting type, int mbm)
{
	int qdbm = WL_QDBM_NO_LIMIT;

	if (type != NL80211_TX_POWER_AUTOMATIC) {
		if (mbm < 0)
			return -EINVAL;
		qdbm = min(mbm * 4 / 100, WL_QDBM_NO_LIMIT);
	}
	return wl_cfg_iovar_setint(wl_cfg_pri(wiphy), "qtxpower", qdbm);
}

static int wl_cfg_set_wiphy_params(struct wiphy *wiphy, u32 changed)
{
	struct wl_if *ifp = wl_cfg_pri(wiphy);
	int err = 0;

	if (changed & WIPHY_PARAM_RTS_THRESHOLD)
		err = wl_cfg_iovar_setint(ifp, "rtsthresh",
					  wiphy->rts_threshold == (u32)-1 ? 2347 : wiphy->rts_threshold);
	if (!err && (changed & WIPHY_PARAM_FRAG_THRESHOLD))
		err = wl_cfg_iovar_setint(ifp, "fragthresh",
					  wiphy->frag_threshold == (u32)-1 ? 2346 : wiphy->frag_threshold);
	return err;
}

static int wl_cfg_dump_survey(struct wiphy *wiphy, struct net_device *dev, int idx,
			      struct survey_info *survey)
{
	struct wl_if *ifp = wl_cfg_pri(wiphy);
	int ch, noise;

	if (idx != 0 || wl_cfg_channel(ifp, &ch))
		return -ENOENT;
	survey->channel = ieee80211_get_channel(wiphy,
					ieee80211_channel_to_frequency(ch, NL80211_BAND_2GHZ));
	if (!survey->channel)
		return -ENOENT;
	survey->filled = SURVEY_INFO_IN_USE;
	if (!wl_cfg_noise(ifp, &noise) && noise < 0) {
		survey->filled |= SURVEY_INFO_NOISE_DBM;
		survey->noise = noise;
	}
	return 0;
}

/* ------------------------------------------------------------------ stations */

static void wl_cfg_fill_sinfo(struct station_info *sinfo, const struct wl_cfg_sta *st)
{
	sinfo->filled = BIT_ULL(NL80211_STA_INFO_INACTIVE_TIME) |
			BIT_ULL(NL80211_STA_INFO_CONNECTED_TIME) |
			BIT_ULL(NL80211_STA_INFO_STA_FLAGS);
	sinfo->inactive_time = st->idle_s * 1000;
	sinfo->connected_time = st->connected_s;
	sinfo->sta_flags.mask = BIT(NL80211_STA_FLAG_AUTHORIZED) | BIT(NL80211_STA_FLAG_AUTHENTICATED) |
				BIT(NL80211_STA_FLAG_ASSOCIATED) | BIT(NL80211_STA_FLAG_WME);
	sinfo->sta_flags.set = ((st->flags & WL_CFG_STA_AUTHO) ? BIT(NL80211_STA_FLAG_AUTHORIZED) : 0) |
			       ((st->flags & WL_CFG_STA_AUTHE) ? BIT(NL80211_STA_FLAG_AUTHENTICATED) : 0) |
			       ((st->flags & WL_CFG_STA_ASSOC) ? BIT(NL80211_STA_FLAG_ASSOCIATED) : 0) |
			       ((st->flags & WL_CFG_STA_WME) ? BIT(NL80211_STA_FLAG_WME) : 0);
	if (st->tx_pkts || st->rx_pkts) {
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_TX_PACKETS) |
				 BIT_ULL(NL80211_STA_INFO_TX_FAILED) |
				 BIT_ULL(NL80211_STA_INFO_RX_PACKETS);
		sinfo->tx_packets = st->tx_pkts;
		sinfo->tx_failed = st->tx_failures;
		sinfo->rx_packets = st->rx_pkts;
	}
	if (st->tx_rate_kbps) {
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_TX_BITRATE);
		sinfo->txrate.legacy = st->tx_rate_kbps / 100;	/* units of 100 kbit/s */
	}
	if (st->rx_rate_kbps) {
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_RX_BITRATE);
		sinfo->rxrate.legacy = st->rx_rate_kbps / 100;
	}
	if (st->rssi < 0) {
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_SIGNAL) | BIT_ULL(NL80211_STA_INFO_SIGNAL_AVG);
		sinfo->signal = sinfo->signal_avg = st->rssi;
	}
}

static int wl_cfg_get_station(struct wiphy *wiphy, struct net_device *dev, const u8 *mac,
			      struct station_info *sinfo)
{
	struct wl_cfg_vif *vif = wl_vif_of_dev(dev);
	struct wl_cfg_sta st;
	int err;

	if (vif->wdev.iftype == NL80211_IFTYPE_STATION &&
	    (!vif->connected || !ether_addr_equal(mac, vif->bssid)))
		return -ENOENT;
	err = wl_cfg_sta_info(vif->ifp, mac, &st);
	if (err)
		return err;
	if (vif->wdev.iftype == NL80211_IFTYPE_AP && !(st.flags & WL_CFG_STA_ASSOC))
		return -ENOENT;
	wl_cfg_fill_sinfo(sinfo, &st);
	return 0;
}

static int wl_cfg_dump_station(struct wiphy *wiphy, struct net_device *dev, int idx, u8 *mac,
			       struct station_info *sinfo)
{
	struct wl_cfg_vif *vif = wl_vif_of_dev(dev);
	struct wl_cfg_sta st;
	int n;

	if (vif->wdev.iftype == NL80211_IFTYPE_STATION) {
		if (idx || !vif->connected || wl_cfg_sta_info(vif->ifp, vif->bssid, &st))
			return -ENOENT;
		memcpy(mac, vif->bssid, ETH_ALEN);
		wl_cfg_fill_sinfo(sinfo, &st);
		return 0;
	}
	if (idx == 0) {	/* new dump: snapshot the association list */
		n = wl_cfg_assoclist(vif->ifp, vif->sta_macs, WL_CFG_MAX_STA);
		vif->n_sta = n < 0 ? 0 : n;
	}
	for (; idx < vif->n_sta; idx++) {	/* skip stations that left since the snapshot */
		if (wl_cfg_sta_info(vif->ifp, vif->sta_macs[idx], &st))
			continue;
		memcpy(mac, vif->sta_macs[idx], ETH_ALEN);
		wl_cfg_fill_sinfo(sinfo, &st);
		return 0;
	}
	return -ENOENT;
}

/* ------------------------------------------------------------------ security mapping */

static int wl_cfg_crypto(const struct cfg80211_crypto_settings *c, bool privacy, u32 *wsec,
			 u32 *auth)
{
	int i;
	u32 v = c->wpa_versions;

	*wsec = *auth = 0;
	for (i = 0; i < c->n_ciphers_pairwise; i++) {
		switch (c->ciphers_pairwise[i]) {
		case WLAN_CIPHER_SUITE_CCMP: *wsec |= WL_CFG_WSEC_AES; break;
		case WLAN_CIPHER_SUITE_TKIP: *wsec |= WL_CFG_WSEC_TKIP; break;
		case WLAN_CIPHER_SUITE_WEP40:
		case WLAN_CIPHER_SUITE_WEP104: *wsec |= WL_CFG_WSEC_WEP; break;
		default: return -EOPNOTSUPP;
		}
	}
	switch (c->cipher_group) {
	case 0: break;
	case WLAN_CIPHER_SUITE_CCMP: *wsec |= WL_CFG_WSEC_AES; break;
	case WLAN_CIPHER_SUITE_TKIP: *wsec |= WL_CFG_WSEC_TKIP; break;
	case WLAN_CIPHER_SUITE_WEP40:
	case WLAN_CIPHER_SUITE_WEP104: *wsec |= WL_CFG_WSEC_WEP; break;
	default: return -EOPNOTSUPP;
	}
	for (i = 0; i < c->n_akm_suites; i++) {
		switch (c->akm_suites[i]) {
		case WLAN_AKM_SUITE_PSK:
			*auth |= ((v & NL80211_WPA_VERSION_1) ? WL_CFG_AUTH_WPA_PSK : 0) |
				 ((v & NL80211_WPA_VERSION_2) ? WL_CFG_AUTH_WPA2_PSK : 0);
			break;
		case WLAN_AKM_SUITE_8021X:
			*auth |= ((v & NL80211_WPA_VERSION_1) ? WL_CFG_AUTH_WPA_8021X : 0) |
				 ((v & NL80211_WPA_VERSION_2) ? WL_CFG_AUTH_WPA2_8021X : 0);
			break;
		default:
			return -EOPNOTSUPP;	/* SAE, FT, SHA256 AKMs: not in this core */
		}
	}
	if (privacy && !*auth)
		*wsec |= WL_CFG_WSEC_WEP;
	return 0;
}

static int wl_cfg_key_cipher(u32 suite, enum wl_cfg_cipher *c)
{
	switch (suite) {
	case WLAN_CIPHER_SUITE_WEP40: *c = WL_CFG_CIPHER_WEP40; return 0;
	case WLAN_CIPHER_SUITE_WEP104: *c = WL_CFG_CIPHER_WEP104; return 0;
	case WLAN_CIPHER_SUITE_TKIP: *c = WL_CFG_CIPHER_TKIP; return 0;
	case WLAN_CIPHER_SUITE_CCMP: *c = WL_CFG_CIPHER_CCMP; return 0;
	default: return -EOPNOTSUPP;
	}
}

/* ------------------------------------------------------------------ AP: hostapd */

static int wl_cfg_start_ap_op(struct wiphy *wiphy, struct net_device *dev,
			      struct cfg80211_ap_settings *s)
{
	struct wl_cfg_vif *vif = wl_vif_of_dev(dev);
	struct wl_cfg_ap ap = {
		.beacon_int = s->beacon_interval,
		.dtim = s->dtim_period,
		.hidden = s->hidden_ssid != NL80211_HIDDEN_SSID_NOT_IN_USE,
		.shared_key = s->auth_type == NL80211_AUTHTYPE_SHARED_KEY,
		.inactivity_s = s->inactivity_timeout,
		.ssid = s->ssid,
		.ssid_len = s->ssid_len,
	};
	int err;

	if (!s->chandef.chan)
		return -EINVAL;
	ap.channel = s->chandef.chan->hw_value;
	err = wl_cfg_crypto(&s->crypto, s->privacy, &ap.wsec, &ap.auth);
	if (err)
		return err;
	err = wl_cfg_start_ap(vif->ifp, &ap);
	netdev_info(dev, "start_ap (bss %d): ssid '%.*s' channel %d wsec 0x%x auth 0x%x: %d\n",
		    vif->idx, ap.ssid_len, ap.ssid, ap.channel, ap.wsec, ap.auth, err);
	return err;
}

static int wl_cfg_change_beacon(struct wiphy *wiphy, struct net_device *dev,
				struct cfg80211_beacon_data *info)
{
	return 0;	/* the core builds its own beacon and probe responses */
}

static int wl_cfg_stop_ap_op(struct wiphy *wiphy, struct net_device *dev)
{
	return wl_cfg_stop_ap(wl_ifp_of_dev(dev));
}

static bool wl_cfg_is_sta_addr(const u8 *mac)
{
	return mac && !is_multicast_ether_addr(mac) && !is_zero_ether_addr(mac);
}

static int wl_cfg_add_key_op(struct wiphy *wiphy, struct net_device *dev, u8 idx, bool pairwise,
			     const u8 *mac, struct key_params *p)
{
	enum wl_cfg_cipher c;
	int err = wl_cfg_key_cipher(p->cipher, &c);

	if (err)
		return err;
	if (idx > 3)
		return -EINVAL;
	return wl_cfg_set_key(wl_ifp_of_dev(dev), idx,
			      (pairwise && wl_cfg_is_sta_addr(mac)) ? mac : NULL,
			      c, p->key, p->key_len, !pairwise);
}

static int wl_cfg_del_key_op(struct wiphy *wiphy, struct net_device *dev, u8 idx, bool pairwise,
			     const u8 *mac)
{
	if (idx > 3)
		return -EINVAL;
	/* hostapd treats -ENOENT on delete as success */
	return wl_cfg_del_key(wl_ifp_of_dev(dev), idx,
			      (pairwise && wl_cfg_is_sta_addr(mac)) ? mac : NULL) ? -ENOENT : 0;
}

static int wl_cfg_get_key_op(struct wiphy *wiphy, struct net_device *dev, u8 idx, bool pairwise,
			     const u8 *mac, void *cookie,
			     void (*callback)(void *cookie, struct key_params *))
{
	struct key_params p;
	u8 seq[6];

	if (pairwise || idx > 3)
		return -EOPNOTSUPP;	/* hostapd only asks for the group key's TSC/PN */
	if (wl_cfg_key_seq(wl_ifp_of_dev(dev), idx, seq))
		return -ENOENT;
	memset(&p, 0, sizeof(p));
	p.seq = seq;
	p.seq_len = sizeof(seq);
	callback(cookie, &p);
	return 0;
}

static int wl_cfg_set_default_key(struct wiphy *wiphy, struct net_device *dev, u8 idx,
				  bool unicast, bool multicast)
{
	return 0;	/* group keys are installed as the primary (TX) key by add_key */
}

static int wl_cfg_change_station(struct wiphy *wiphy, struct net_device *dev, const u8 *mac,
				 struct station_parameters *params)
{
	u32 auth = BIT(NL80211_STA_FLAG_AUTHORIZED);

	if (!(params->sta_flags_mask & auth))
		return 0;
	return wl_cfg_authorize(wl_ifp_of_dev(dev), mac, params->sta_flags_set & auth);
}

static int wl_cfg_del_station(struct wiphy *wiphy, struct net_device *dev,
			      struct station_del_parameters *params)
{
	if (!wl_cfg_is_sta_addr(params->mac))
		return 0;	/* hostapd's start-up broadcast deauth: nobody is associated yet */
	return wl_cfg_deauth(wl_ifp_of_dev(dev), params->mac,
			     params->reason_code ?: WLAN_REASON_DEAUTH_LEAVING);
}

static int wl_cfg_set_bss(struct wiphy *wiphy, struct net_device *dev, struct bss_parameters *p)
{
	if (p->ap_isolate >= 0)
		return wl_cfg_iovar_setint(wl_ifp_of_dev(dev), "ap_isolate", p->ap_isolate);
	return 0;	/* preamble, slot time, CTS protection and HT opmode are the core's business */
}

static int wl_cfg_mgmt_tx(struct wiphy *wiphy, struct wireless_dev *wdev,
			  struct cfg80211_mgmt_tx_params *params, u64 *cookie)
{
	return -EOPNOTSUPP;
}

static void wl_cfg_mgmt_frame_register(struct wiphy *wiphy, struct wireless_dev *wdev,
				       u16 frame_type, bool reg)
{
}

/* ------------------------------------------------------------------ scan */

struct wl_cfg_inform_ctx {
	struct wiphy *wiphy;
	int n;
	struct cfg80211_bss *keep;	/* if set to non-NULL by the caller: keep the last one */
	bool want_keep;
};

static void wl_cfg_inform_one(void *ctx, const struct wl_cfg_bss *b)
{
	struct wl_cfg_inform_ctx *c = ctx;
	struct ieee80211_channel *chan;
	struct cfg80211_bss *bss;

	chan = ieee80211_get_channel(c->wiphy,
				     ieee80211_channel_to_frequency(b->channel, NL80211_BAND_2GHZ));
	if (!chan)
		return;
	bss = cfg80211_inform_bss(c->wiphy, chan, CFG80211_BSS_FTYPE_UNKNOWN, b->bssid, 0,
				  b->capability, b->beacon_period, b->ies, b->ies_len,
				  b->rssi * 100, GFP_KERNEL);
	if (bss) {
		c->n++;
		if (c->want_keep) {
			if (c->keep)
				cfg80211_put_bss(c->wiphy, c->keep);
			c->keep = bss;		/* reference handed to cfg80211_connect_bss() */
		} else {
			cfg80211_put_bss(c->wiphy, bss);
		}
	}
}

static int wl_cfg_scan_op(struct wiphy *wiphy, struct cfg80211_scan_request *req)
{
	struct wl_cfg *cfg = wl_cfg_of(wiphy);
	const u8 *ssid = NULL;
	int ssid_len = 0, err;

	if (cfg->scan_req)
		return -EBUSY;
	if (req->n_ssids > 0 && req->ssids[0].ssid_len) {
		ssid = req->ssids[0].ssid;
		ssid_len = req->ssids[0].ssid_len;
	}
	err = wl_cfg_scan(cfg->vif[0].ifp, ssid, ssid_len);
	if (err)
		return -EBUSY;
	cfg->scan_req = req;	/* finished by WLC_E_SCAN_COMPLETE */
	return 0;
}

static void wl_cfg_scan_finish(struct wl_cfg *cfg, bool aborted)
{
	struct cfg80211_scan_info info = { .aborted = aborted };
	struct wl_cfg_inform_ctx ctx = { .wiphy = cfg->wiphy };

	if (!cfg->scan_req)
		return;
	if (!aborted)
		wl_cfg_scan_results(cfg->vif[0].ifp, wl_cfg_inform_one, &ctx);
	cfg80211_scan_done(cfg->scan_req, &info);
	cfg->scan_req = NULL;
}

/* ------------------------------------------------------------------ STA: wpa_supplicant */

static int wl_cfg_connect(struct wiphy *wiphy, struct net_device *dev,
			  struct cfg80211_connect_params *sme)
{
	struct wl_cfg_vif *vif = wl_vif_of_dev(dev);
	struct wl_cfg_join j = {
		.ssid = sme->ssid,
		.ssid_len = sme->ssid_len,
		.shared_key = sme->auth_type == NL80211_AUTHTYPE_SHARED_KEY,
	};
	enum wl_cfg_cipher c;
	int err;

	if (vif->wdev.iftype != NL80211_IFTYPE_STATION)
		return -EOPNOTSUPP;
	err = wl_cfg_crypto(&sme->crypto, sme->privacy, &j.wsec, &j.auth);
	if (err)
		return err;
	if (sme->crypto.psk && (j.auth & (WL_CFG_AUTH_WPA_PSK | WL_CFG_AUTH_WPA2_PSK))) {
		j.pmk = sme->crypto.psk;	/* 4-way handshake offload */
	} else if (j.auth) {
		j.wpaie = sme->ie;		/* external supplicant, keys via add_key */
		j.wpaie_len = sme->ie_len;
	}
	if (sme->key && sme->key_len && !wl_cfg_key_cipher(sme->crypto.ciphers_pairwise[0], &c))
		wl_cfg_set_key(vif->ifp, sme->key_idx, NULL, c, sme->key, sme->key_len, true);
	vif->connecting = true;
	vif->connected = vif->keyed = false;
	vif->psk_offload = j.pmk != NULL;
	eth_zero_addr(vif->bssid);
	err = wl_cfg_join(vif->ifp, &j);
	netdev_info(dev, "connect: wsec 0x%x auth 0x%x%s: %d\n", j.wsec, j.auth,
		    j.pmk ? " (4-way offload)" : "", err);
	if (err)
		vif->connecting = false;
	return err;
}

static int wl_cfg_disconnect(struct wiphy *wiphy, struct net_device *dev, u16 reason_code)
{
	struct wl_cfg_vif *vif = wl_vif_of_dev(dev);

	wl_cfg_disassoc(vif->ifp);
	if (vif->connecting) {
		vif->connecting = false;
		cfg80211_connect_result(dev, NULL, NULL, 0, NULL, 0,
					WLAN_STATUS_UNSPECIFIED_FAILURE, GFP_KERNEL);
	} else if (vif->connected) {
		vif->connected = false;
		cfg80211_disconnected(dev, reason_code, NULL, 0, true, GFP_KERNEL);
	}
	return 0;
}

static void wl_cfg_sta_connected(struct wl_cfg_vif *vif)
{
	struct net_device *dev = vif->wdev.netdev;
	struct wl_cfg_inform_ctx ctx = { .wiphy = vif->cfg->wiphy, .want_keep = true };
	u8 *req, *resp;
	u32 req_len, resp_len;
	int err;

	if (wl_cfg_bssid(vif->ifp, vif->bssid)) {
		netdev_warn(dev, "joined, but no BSSID\n");
		return;
	}
	/* cfg80211 must know the BSS: inform it from the core and pass that entry on directly */
	err = wl_cfg_cur_bss(vif->ifp, wl_cfg_inform_one, &ctx);
	if (err || !ctx.keep)
		netdev_warn(dev, "no BSS info for %pM (%d)\n", vif->bssid, err);
	wl_cfg_assoc_ies(vif->ifp, &req, &req_len, &resp, &resp_len);
	cfg80211_connect_bss(dev, vif->bssid, ctx.keep, req, req_len, resp, resp_len,
			     WLAN_STATUS_SUCCESS, GFP_KERNEL, NL80211_TIMEOUT_UNSPECIFIED);
	kfree(req);
	kfree(resp);
	vif->connecting = false;
	vif->connected = true;
	netdev_info(dev, "connected to %pM\n", vif->bssid);
	if (vif->psk_offload && vif->keyed)
		cfg80211_port_authorized(dev, vif->bssid, GFP_KERNEL);
}

static void wl_cfg_sta_lost(struct wl_cfg_vif *vif, u16 reason)
{
	struct net_device *dev = vif->wdev.netdev;

	if (vif->connecting) {
		vif->connecting = false;
		cfg80211_connect_result(dev, NULL, NULL, 0, NULL, 0,
					WLAN_STATUS_UNSPECIFIED_FAILURE, GFP_KERNEL);
		netdev_info(dev, "connect failed (reason %u)\n", reason);
	} else if (vif->connected) {
		vif->connected = false;
		cfg80211_disconnected(dev, reason, NULL, 0, false, GFP_KERNEL);
		netdev_info(dev, "disconnected (reason %u)\n", reason);
	}
	vif->keyed = false;
}

static void wl_cfg_sta_event(struct wl_cfg_vif *vif, const struct wl_cfg_event *ev)
{
	switch (ev->type) {
	case WL_CFG_E_SET_SSID:
		if (!vif->connecting)
			break;
		if (ev->status == 0)
			wl_cfg_sta_connected(vif);
		else
			wl_cfg_sta_lost(vif, WLAN_REASON_UNSPECIFIED);
		break;
	case WL_CFG_E_PSK_SUP:
		if (ev->status == WL_CFG_SUP_KEYED) {
			vif->keyed = true;
			if (vif->connected && vif->psk_offload)
				cfg80211_port_authorized(vif->wdev.netdev, vif->bssid, GFP_KERNEL);
		} else if (ev->reason) {	/* handshake failed (wrong key, timeout) */
			wl_cfg_disassoc(vif->ifp);
			wl_cfg_sta_lost(vif, WLAN_REASON_4WAY_HANDSHAKE_TIMEOUT);
		}
		break;
	case WL_CFG_E_LINK:
		if (!(ev->flags & WL_CFG_EVENT_LINK_UP) && vif->connected)
			wl_cfg_sta_lost(vif, ev->reason ?: WLAN_REASON_UNSPECIFIED);
		break;
	case WL_CFG_E_DEAUTH_IND:
	case WL_CFG_E_DISASSOC_IND:
		if (vif->connected && ether_addr_equal(ev->addr, vif->bssid))
			wl_cfg_sta_lost(vif, ev->reason ?: WLAN_REASON_UNSPECIFIED);
		break;
	}
}

/* process context: STA results and scan completion, in order, serialized with the ops by RTNL */
static void wl_cfg_evwork(struct work_struct *work)
{
	struct wl_cfg *cfg = container_of(work, struct wl_cfg, evwork);
	struct wl_cfg_qev *q;
	unsigned long flags;

	rtnl_lock();
	for (;;) {
		spin_lock_irqsave(&cfg->evlock, flags);
		q = list_first_entry_or_null(&cfg->evq, struct wl_cfg_qev, list);
		if (q)
			list_del(&q->list);
		spin_unlock_irqrestore(&cfg->evlock, flags);
		if (!q)
			break;
		if (q->ev.type == WL_CFG_E_SCAN_COMPLETE)
			wl_cfg_scan_finish(cfg, q->ev.status != 0);
		else if (q->vif->in_use && q->vif->wdev.netdev &&
			 q->vif->wdev.iftype == NL80211_IFTYPE_STATION)
			wl_cfg_sta_event(q->vif, &q->ev);
		kfree(q);
	}
	rtnl_unlock();
}

/* ------------------------------------------------------------------ interfaces */

/*
 * BSS idx's address. Secondary BSSes need the core's MBSS pattern: locally administered, with the
 * index in the low bits of the last byte (4 BSSes). The same addresses are published in
 * wiphy->addresses, which mac80211.sh uses for additional interfaces (and hostapd then sets).
 */
#define WL_CFG_MBSS_MASK	3

static void wl_cfg_vif_mac(struct wl_cfg *cfg, int idx, u8 *mac)
{
	memcpy(mac, cfg->wiphy->perm_addr, ETH_ALEN);
	if (idx) {
		mac[0] |= 0x02;
		mac[5] = (mac[5] & ~WL_CFG_MBSS_MASK) | ((mac[5] + idx) & WL_CFG_MBSS_MASK);
	}
}

/*
 * One STA and one AP at most. The first interface added becomes vif 0 (the core's primary
 * bsscfg, AP or STA); an AP added while vif 0 is a STA becomes vif 1 (apsta mode).
 */
static struct wireless_dev *wl_cfg_add_virtual_intf(struct wiphy *wiphy, const char *name,
						    unsigned char name_assign_type,
						    enum nl80211_iftype type,
						    struct vif_params *params)
{
	struct wl_cfg *cfg = wl_cfg_of(wiphy);
	struct wl_cfg_vif *vif;
	struct net_device *dev;
	u8 mac[ETH_ALEN];
	int err;

	ASSERT_RTNL();
	if (type != NL80211_IFTYPE_AP && type != NL80211_IFTYPE_STATION)
		return ERR_PTR(-EOPNOTSUPP);
	if (!cfg->vif[0].in_use) {
		vif = &cfg->vif[0];
		err = wl_cfg_set_mode(cfg->wl, type == NL80211_IFTYPE_STATION, false);
	} else if (type == NL80211_IFTYPE_AP &&
		   cfg->vif[0].wdev.iftype == NL80211_IFTYPE_STATION && !cfg->vif[1].in_use) {
		vif = &cfg->vif[1];
		if (!vif->ifp) {
			vif->ifp = wl_cfg_if_alloc(cfg->wl);
			if (!vif->ifp)
				return ERR_PTR(-ENOMEM);
		}
		err = wl_cfg_set_mode(cfg->wl, true, true);
		wl_cfg_vif_mac(cfg, 1, mac);
		err = err ?: wl_cfg_if_bss_create(vif->ifp, 1, mac);
	} else {
		return ERR_PTR(-EBUSY);		/* no second STA, no AP+AP (multiple SSIDs) yet */
	}
	if (err) {
		wiphy_err(wiphy, "cannot set up %s as %s: %d\n", name,
			  type == NL80211_IFTYPE_AP ? "AP" : "STA", err);
		return ERR_PTR(err);
	}
	dev = wl_cfg_if_dev(vif->ifp);
	if (dev && !strcmp(dev->name, name)) {
		/* the interface created at probe, under the requested name: keep it */
		vif->wdev.iftype = type;
	} else {
		if (dev)
			wl_cfg_netdev_destroy(vif->ifp);	/* e.g. the probe-time wlan0 under another name */
		memset(&vif->wdev, 0, sizeof(vif->wdev));
		vif->wdev.wiphy = wiphy;
		vif->wdev.iftype = type;
		wl_cfg_vif_mac(cfg, vif->idx, mac);
		dev = wl_cfg_netdev_alloc(vif->ifp, name, name_assign_type, mac);
		if (IS_ERR(dev))
			return ERR_CAST(dev);
		dev->ieee80211_ptr = &vif->wdev;
		vif->wdev.netdev = dev;
		err = wl_cfg_netdev_register(vif->ifp, dev);
		if (err) {
			vif->wdev.netdev = NULL;
			wiphy_err(wiphy, "cannot create %s: %d\n", name, err);
			return ERR_PTR(err);
		}
	}
	vif->in_use = true;
	vif->connecting = vif->connected = vif->keyed = false;
	return &vif->wdev;
}

static int wl_cfg_del_virtual_intf(struct wiphy *wiphy, struct wireless_dev *wdev)
{
	struct wl_cfg *cfg = wl_cfg_of(wiphy);
	struct wl_cfg_vif *vif = wl_vif_of_wdev(wdev);

	ASSERT_RTNL();
	if (vif->wdev.iftype == NL80211_IFTYPE_STATION && (vif->connected || vif->connecting))
		wl_cfg_disassoc(vif->ifp);
	if (vif->idx == 1)
		wl_cfg_stop_ap(vif->ifp);
	if (cfg->scan_req && cfg->scan_req->wdev == wdev)
		wl_cfg_scan_finish(cfg, true);
	vif->connecting = vif->connected = vif->keyed = false;
	vif->in_use = false;
	/*
	 * wdev.netdev must stay set: nl80211 holds a reference on it during this request and drops it
	 * afterwards through wdev->netdev. The net_device itself is freed once that is done. Nothing
	 * else uses the pointer while in_use is false, and add_virtual_intf rewrites the wdev.
	 */
	wl_cfg_netdev_destroy(vif->ifp);
	return 0;
}

static int wl_cfg_change_virtual_intf(struct wiphy *wiphy, struct net_device *ndev,
				      enum nl80211_iftype type, struct vif_params *params)
{
	struct wl_cfg *cfg = wl_cfg_of(wiphy);
	struct wl_cfg_vif *vif = wl_vif_of_dev(ndev);
	int err;

	if (type == vif->wdev.iftype)
		return 0;
	if (type != NL80211_IFTYPE_AP && type != NL80211_IFTYPE_STATION)
		return -EOPNOTSUPP;
	if (vif->idx != 0 || cfg->vif[1].in_use)
		return -EBUSY;
	err = wl_cfg_set_mode(cfg->wl, type == NL80211_IFTYPE_STATION, false);
	if (!err)
		vif->wdev.iftype = type;
	return err;
}

static const struct cfg80211_ops wl_cfg_ops = {
	.add_virtual_intf	= wl_cfg_add_virtual_intf,
	.del_virtual_intf	= wl_cfg_del_virtual_intf,
	.change_virtual_intf	= wl_cfg_change_virtual_intf,
	.get_channel		= wl_cfg_get_channel,
	.get_tx_power		= wl_cfg_get_tx_power,
	.set_tx_power		= wl_cfg_set_tx_power,
	.set_wiphy_params	= wl_cfg_set_wiphy_params,
	.get_station		= wl_cfg_get_station,
	.dump_station		= wl_cfg_dump_station,
	.dump_survey		= wl_cfg_dump_survey,
	.start_ap		= wl_cfg_start_ap_op,
	.change_beacon		= wl_cfg_change_beacon,
	.stop_ap		= wl_cfg_stop_ap_op,
	.add_key		= wl_cfg_add_key_op,
	.del_key		= wl_cfg_del_key_op,
	.get_key		= wl_cfg_get_key_op,
	.set_default_key	= wl_cfg_set_default_key,
	.change_station		= wl_cfg_change_station,
	.del_station		= wl_cfg_del_station,
	.change_bss		= wl_cfg_set_bss,
	.mgmt_tx		= wl_cfg_mgmt_tx,
	.mgmt_frame_register	= wl_cfg_mgmt_frame_register,
	.scan			= wl_cfg_scan_op,
	.connect		= wl_cfg_connect,
	.disconnect		= wl_cfg_disconnect,
};

/* ------------------------------------------------------------------ notifications from the glue */

static struct wl_cfg_vif *wl_cfg_vif_of_ifp(struct wl_cfg *cfg, struct wl_if *ifp)
{
	int i;

	for (i = 0; i < WL_CFG_NVIF; i++)
		if (cfg->vif[i].ifp == ifp)
			return &cfg->vif[i];
	return NULL;
}

void wl_cfg80211_ssid_changed(struct wl_cfg *cfg, struct wl_if *ifp, const u8 *ssid, int len)
{
	struct wl_cfg_vif *vif;

	if (!cfg || len < 0 || len > IEEE80211_MAX_SSID_LEN)
		return;
	vif = wl_cfg_vif_of_ifp(cfg, ifp);
	if (!vif || !vif->in_use || !vif->wdev.netdev)
		return;
	mutex_lock(&vif->wdev.mtx);
	memcpy(vif->wdev.ssid, ssid, len);
	vif->wdev.ssid_len = len;
	mutex_unlock(&vif->wdev.mtx);
}

static void wl_cfg_queue(struct wl_cfg *cfg, struct wl_cfg_vif *vif, const struct wl_cfg_event *ev)
{
	struct wl_cfg_qev *q = kzalloc(sizeof(*q), GFP_ATOMIC);
	unsigned long flags;

	if (!q)
		return;
	q->vif = vif;
	q->ev = *ev;
	q->ev.data = NULL;
	q->ev.len = 0;
	spin_lock_irqsave(&cfg->evlock, flags);
	if (cfg->dying) {
		spin_unlock_irqrestore(&cfg->evlock, flags);
		kfree(q);
		return;
	}
	list_add_tail(&q->list, &cfg->evq);
	spin_unlock_irqrestore(&cfg->evlock, flags);
	schedule_work(&cfg->evwork);
}

/* softirq context, from wl_sendup() */
void wl_cfg80211_event(struct wl_cfg *cfg, struct wl_if *ifp, const struct wl_cfg_event *ev)
{
	struct wl_cfg_vif *vif;
	struct net_device *dev;

	if (!cfg)
		return;
	if (ev->type == WL_CFG_E_SCAN_COMPLETE) {
		wl_cfg_queue(cfg, &cfg->vif[0], ev);
		return;
	}
	/* the event header says which BSS it is about; the interface it arrived on may not */
	if (ev->bsscfgidx >= 0 && ev->bsscfgidx < WL_CFG_NVIF && cfg->vif[ev->bsscfgidx].ifp)
		vif = &cfg->vif[ev->bsscfgidx];
	else
		vif = wl_cfg_vif_of_ifp(cfg, ifp);
	if (!vif || !vif->in_use)
		return;
	dev = vif->wdev.netdev;
	if (!dev)
		return;
	if (vif->wdev.iftype == NL80211_IFTYPE_STATION) {
		switch (ev->type) {
		case WL_CFG_E_SET_SSID:
		case WL_CFG_E_PSK_SUP:
		case WL_CFG_E_LINK:
		case WL_CFG_E_DEAUTH_IND:
		case WL_CFG_E_DISASSOC_IND:
			wl_cfg_queue(cfg, vif, ev);
		}
		return;
	}
	if (is_zero_ether_addr(ev->addr))
		return;		/* e.g. the core's own DISASSOC when the BSS goes down */
	switch (ev->type) {
	case WL_CFG_E_ASSOC_IND:
	case WL_CFG_E_REASSOC_IND:
		if (ev->status == 0) {
			struct station_info sinfo;

			memset(&sinfo, 0, sizeof(sinfo));
			if (ev->len > 0) {
				sinfo.assoc_req_ies = ev->data;
				sinfo.assoc_req_ies_len = ev->len;
			}
			netdev_info(dev, "station %pM associated (%u IE bytes)\n", ev->addr, ev->len);
			cfg80211_new_sta(dev, ev->addr, &sinfo, GFP_ATOMIC);
		}
		break;
	case WL_CFG_E_DISASSOC_IND:
	case WL_CFG_E_DEAUTH_IND:
	case WL_CFG_E_DISASSOC:
	case WL_CFG_E_DEAUTH:
		netdev_info(dev, "station %pM left\n", ev->addr);
		cfg80211_del_sta(dev, ev->addr, GFP_ATOMIC);
		break;
	}
}

/* ------------------------------------------------------------------ attach / detach */

static void wl_cfg_setup_band(struct wl_cfg *cfg, struct ieee80211_channel *chans)
{
	u32 valid[32];
	int i, j, n = wl_cfg_valid_channels(cfg->vif[0].ifp, valid, ARRAY_SIZE(valid));

	memcpy(chans, wl_cfg_channels_2g, sizeof(wl_cfg_channels_2g));
	for (i = 0; n > 0 && i < ARRAY_SIZE(wl_cfg_channels_2g); i++) {
		for (j = 0; j < n && valid[j] != chans[i].hw_value; j++)
			;
		if (j == n)
			chans[i].flags |= IEEE80211_CHAN_DISABLED;
	}
	cfg->band.band = NL80211_BAND_2GHZ;
	cfg->band.channels = chans;
	cfg->band.n_channels = ARRAY_SIZE(wl_cfg_channels_2g);
	cfg->band.bitrates = wl_cfg_rates;
	cfg->band.n_bitrates = ARRAY_SIZE(wl_cfg_rates);
	cfg->band.ht_cap = wl_cfg_ht_cap;
}

struct wl_cfg *wl_cfg80211_attach(struct wl_info *wl, struct net_device *dev, struct device *parent)
{
	struct wiphy *wiphy;
	struct wl_cfg *cfg;
	struct ieee80211_channel *chans;
	int i, err;

	/* channels live behind the wl_cfg in the same allocation */
	wiphy = wiphy_new(&wl_cfg_ops, sizeof(struct wl_cfg) + sizeof(wl_cfg_channels_2g));
	if (!wiphy)
		return NULL;
	cfg = wiphy_priv(wiphy);
	cfg->wl = wl;
	cfg->wiphy = wiphy;
	for (i = 0; i < WL_CFG_NVIF; i++) {
		cfg->vif[i].cfg = cfg;
		cfg->vif[i].idx = i;
	}
	cfg->vif[0].ifp = wl_cfg_primary(wl);
	spin_lock_init(&cfg->evlock);
	INIT_LIST_HEAD(&cfg->evq);
	INIT_WORK(&cfg->evwork, wl_cfg_evwork);
	chans = (struct ieee80211_channel *)(cfg + 1);
	wl_cfg_setup_band(cfg, chans);

	set_wiphy_dev(wiphy, parent);
	memcpy(wiphy->perm_addr, dev->dev_addr, ETH_ALEN);
	for (i = 0; i < WL_CFG_NVIF; i++)
		wl_cfg_vif_mac(cfg, i, cfg->addrs[i].addr);
	wiphy->addresses = cfg->addrs;
	wiphy->n_addresses = WL_CFG_NVIF;
	wiphy->interface_modes = BIT(NL80211_IFTYPE_AP) | BIT(NL80211_IFTYPE_STATION);
	wiphy->iface_combinations = wl_cfg_combinations;
	wiphy->n_iface_combinations = ARRAY_SIZE(wl_cfg_combinations);
	wiphy->bands[NL80211_BAND_2GHZ] = &cfg->band;
	wiphy->cipher_suites = wl_cfg_ciphers;
	wiphy->n_cipher_suites = ARRAY_SIZE(wl_cfg_ciphers);
	wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
	wiphy->max_scan_ssids = 1;
	wiphy->rts_threshold = 2347;
	wiphy->frag_threshold = 2346;
	wiphy->regulatory_flags = REGULATORY_CUSTOM_REG | REGULATORY_DISABLE_BEACON_HINTS;
	wiphy_apply_custom_regulatory(wiphy, &wl_cfg_regd);
	/* AP: association and probe responses happen in the core; hostapd only does WPA and keys */
	wiphy->flags |= WIPHY_FLAG_HAVE_AP_SME;
	wiphy->probe_resp_offload = NL80211_PROBE_RESP_OFFLOAD_SUPPORT_WPS |
				    NL80211_PROBE_RESP_OFFLOAD_SUPPORT_WPS2;
	wiphy->features |= NL80211_FEATURE_INACTIVITY_TIMER;	/* scb_activity_time */
	wiphy->mgmt_stypes = wl_cfg_mgmt_stypes;
	/* STA: the core's supplicant does the 4-way handshake with the PSK from connect */
	wiphy_ext_feature_set(wiphy, NL80211_EXT_FEATURE_4WAY_HANDSHAKE_STA_PSK);

	err = wiphy_register(wiphy);
	if (err) {
		pr_err("%s: wiphy_register failed: %d\n", dev->name, err);
		wiphy_free(wiphy);
		return NULL;
	}
	/* the interface created at probe: an AP until OpenWrt says otherwise */
	cfg->vif[0].wdev.wiphy = wiphy;
	cfg->vif[0].wdev.iftype = NL80211_IFTYPE_AP;
	cfg->vif[0].wdev.netdev = dev;
	dev->ieee80211_ptr = &cfg->vif[0].wdev;	/* before register_netdev: cfg80211 picks it up there */
	return cfg;
}

const char *wl_cfg80211_name(struct wl_cfg *cfg)
{
	return wiphy_name(cfg->wiphy);
}

/* stage 1: after the glue unregistered the primary net_device, before the core detaches */
void wl_cfg80211_detach(struct wl_cfg *cfg)
{
	unsigned long flags;
	int i;

	if (!cfg)
		return;
	spin_lock_irqsave(&cfg->evlock, flags);
	cfg->dying = true;
	spin_unlock_irqrestore(&cfg->evlock, flags);
	/* no new events are queued now; cancel_work_sync() is GPL-only, flush_workqueue() is not */
	flush_scheduled_work();
	rtnl_lock();
	if (cfg->scan_req) {
		struct cfg80211_scan_info info = { .aborted = true };

		cfg80211_scan_done(cfg->scan_req, &info);
		cfg->scan_req = NULL;
	}
	for (i = 1; i < WL_CFG_NVIF; i++)
		if (cfg->vif[i].ifp) {
			wl_cfg_netdev_destroy(cfg->vif[i].ifp);
			cfg->vif[i].wdev.netdev = NULL;
		}
	rtnl_unlock();
	wiphy_unregister(cfg->wiphy);
}

/* stage 2: after the core detached (it may call wl_del_if() for vif 1's BSS until then) */
void wl_cfg80211_free(struct wl_cfg *cfg)
{
	struct wl_cfg_qev *q, *n;
	int i;

	if (!cfg)
		return;
	list_for_each_entry_safe(q, n, &cfg->evq, list)
		kfree(q);
	for (i = 1; i < WL_CFG_NVIF; i++)
		wl_cfg_if_free(cfg->vif[i].ifp);
	wiphy_free(cfg->wiphy);
}
