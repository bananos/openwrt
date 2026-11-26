// SPDX-License-Identifier: ISC
/*
 * cfg80211 front end for the Broadcom wl 5.110 core (experimental, Phase D).
 *
 * The radio is one wiphy with up to two interfaces (vifs): one STA and one AP. The core runs in its
 * AP+STA mode (apsta) from the first interface on, and each type has its own BSS of the core:
 *  - a STA is the primary bsscfg (0);
 *  - an AP is bsscfg 1, also when it is alone: next to an idle STA. In AP-only mode the core never
 *    finishes a scan; this way hostapd (ACS, 20/40 coexistence) and iw/LuCI can scan while the AP
 *    runs. With a STA connected (repeater) the AP runs on the STA's channel.
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
#include <linux/ctype.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/slab.h>
#include <linux/rtnetlink.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>

#include "wl_cfg80211.h"

#define WL_CFG_MAX_STA		32
#define WL_CFG_MAX_ACL		64	/* MAC filter entries, as the stock wl tool's maclist */
#define WL_QDBM_NO_LIMIT	127	/* qtxpower: no user limit set, real power is board-limited */
/*
 * Interfaces: one STA (on the primary bsscfg) and up to three APs (bsscfgs 1-3), the core's MBSS
 * block of four addresses minus the one whose index is the primary's (see wl_cfg_vif_mac).
 */
#define WL_CFG_NVIF		4
#define WL_CFG_NBSS		4
#define WL_CFG_NADDR		3
#define WL_CFG_MAX_AP		3

struct wl_cfg;

struct wl_cfg_vif {
	struct wl_cfg *cfg;
	struct wl_if *ifp;		/* the core BSS: cfg->bss[0] for a STA, cfg->bss[1] for an AP */
	struct wireless_dev wdev;
	int idx;
	bool in_use;			/* handed out by add_virtual_intf */
	/* AP */
	u8 sta_macs[WL_CFG_MAX_STA][ETH_ALEN];	/* dump_station snapshot, taken at idx 0 */
	int n_sta;
	bool ap_started;
	/* STA */
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
	u8 hw_mac[ETH_ALEN];			/* the radio's own MAC (the idle STA's) */
	struct mac_address addrs[WL_CFG_NADDR];	/* wiphy->addresses */
	struct wl_if *bss[WL_CFG_NBSS];		/* the core's bsscfgs: 0 primary (STA), 1-3 APs */
	bool apsta;				/* the core is in apsta mode, bsscfg 1 exists */
	struct wl_cfg_vif vif[WL_CFG_NVIF];
	struct ieee80211_supported_band band;	/* per device: channels get disabled per country */
	struct cfg80211_scan_request *scan_req;
	/* per-channel load estimate from the last scan (index = channel number), see dump_survey */
	u32 load[15];
#define WL_CFG_LOAD_FULL	1000	/* load of one strong BSS on the channel itself */
	bool load_valid;
	/* core events that need process context (STA, scan), handled in order under RTNL */
	spinlock_t evlock;
	struct list_head evq;
	struct work_struct evwork;
	/* the regulatory country to hand to the core (reg_notifier -> regwork) */
	char reg_alpha2[2];
	struct work_struct regwork;
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
 * BCM5357C0: 2x2 802.11n, 20/40 MHz. start_ap sets the core's chanspec from hostapd's channel
 * definition; hostapd's 20/40 coexistence scan uses the scan op below.
 */
static const struct ieee80211_sta_ht_cap wl_cfg_ht_cap = {
	.ht_supported = true,
	.cap = IEEE80211_HT_CAP_SUP_WIDTH_20_40 | IEEE80211_HT_CAP_SGI_20 |
	       IEEE80211_HT_CAP_SGI_40 | IEEE80211_HT_CAP_DSSSCCK40,
	.ampdu_factor = IEEE80211_HT_MAX_AMPDU_64K,
	.ampdu_density = IEEE80211_HT_MPDU_DENSITY_8,
	.mcs = {
		.rx_mask = { 0xff, 0xff },
		.rx_highest = cpu_to_le16(300),
		.tx_params = IEEE80211_HT_MCS_TX_DEFINED,
	},
};

/*
 * The core enforces its own country rules (by default country XU, channels 1-13) and power limits,
 * so cfg80211 starts from one permissive custom rule and the channels the core doesn't allow are
 * disabled. Without this, cfg80211's world domain would mark channels 12-13 no-IR. A country set
 * later goes to the core too (wl_cfg_reg_notifier).
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

/*
 * One STA and APs on the same channel (the core's apsta): three addresses, so three APs, or a STA and
 * two APs. All APs share the radio's beacon interval.
 */
static const struct ieee80211_iface_limit wl_cfg_limits[] = {
	{ .max = 1, .types = BIT(NL80211_IFTYPE_STATION) },
	{ .max = WL_CFG_MAX_AP, .types = BIT(NL80211_IFTYPE_AP) },
};

static const struct ieee80211_iface_combination wl_cfg_combinations[] = {
	{
		.limits = wl_cfg_limits,
		.n_limits = ARRAY_SIZE(wl_cfg_limits),
		.max_interfaces = WL_CFG_NADDR,
		.num_different_channels = 1,
		.beacon_int_infra_match = true,
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

/* the primary bsscfg: radio-wide settings and scans */
static struct wl_if *wl_cfg_pri(struct wiphy *wiphy)
{
	return wl_cfg_of(wiphy)->bss[0];
}

static struct wl_cfg_vif *wl_cfg_vif_of_type(struct wl_cfg *cfg, enum nl80211_iftype type)
{
	int i;

	for (i = 0; i < WL_CFG_NVIF; i++)
		if (cfg->vif[i].in_use && cfg->vif[i].wdev.iftype == type)
			return &cfg->vif[i];
	return NULL;
}

static int wl_cfg_n_of_type(struct wl_cfg *cfg, enum nl80211_iftype type)
{
	int i, n = 0;

	for (i = 0; i < WL_CFG_NVIF; i++)
		n += cfg->vif[i].in_use && cfg->vif[i].wdev.iftype == type;
	return n;
}

static bool wl_cfg_ap_running(struct wl_cfg *cfg)
{
	int i;

	for (i = 0; i < WL_CFG_NVIF; i++)
		if (cfg->vif[i].in_use && cfg->vif[i].ap_started)
			return true;
	return false;
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
	int ctl, center, width, err = wl_cfg_chanspec(wl_cfg_pri(wiphy), &ctl, &center, &width);

	if (err)
		return err;
	chan = ieee80211_get_channel(wiphy, ieee80211_channel_to_frequency(ctl, NL80211_BAND_2GHZ));
	if (!chan)
		return -EINVAL;
	cfg80211_chandef_create(chandef, chan, width == 40 ?
				(ctl < center ? NL80211_CHAN_HT40PLUS : NL80211_CHAN_HT40MINUS) :
				NL80211_CHAN_HT20);
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
	/* the core applies a new limit when the radio comes up (measured: no change until then) */
	return wl_cfg_radio_setint(wl_cfg_of(wiphy)->wl, "qtxpower", qdbm, true);
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
	struct wl_cfg *cfg = wl_cfg_of(wiphy);
	struct wl_if *ifp = wl_cfg_pri(wiphy);
	struct ieee80211_channel *chan;
	int ch = 0, noise;
	u32 l;

	if (wl_cfg_channel(ifp, &ch))
		ch = 0;
	/* no scan yet: only the current channel, with the noise the core measures there */
	if (!cfg->load_valid) {
		if (idx != 0 || !ch)
			return -ENOENT;
		chan = ieee80211_get_channel(wiphy, ieee80211_channel_to_frequency(ch, NL80211_BAND_2GHZ));
	} else {
		if (idx >= cfg->band.n_channels)
			return -ENOENT;
		chan = &cfg->band.channels[idx];
	}
	if (!chan)
		return -ENOENT;
	survey->channel = chan;
	survey->filled = 0;
	if (chan->hw_value == ch)
		survey->filled |= SURVEY_INFO_IN_USE;
	/*
	 * The core measures noise only where it is; ACS needs a noise floor on every channel, and
	 * the same one everywhere leaves the ranking to the busy time.
	 */
	if (wl_cfg_noise(ifp, &noise) || noise >= 0)
		noise = -92;
	survey->filled |= SURVEY_INFO_NOISE_DBM;
	survey->noise = noise;
	if (cfg->load_valid) {
		/* estimated, see wl_cfg_load_one: 1000 "ms" of channel time, busy saturating with load */
		l = min_t(u32, cfg->load[chan->hw_value], 1000000);
		survey->filled |= SURVEY_INFO_TIME | SURVEY_INFO_TIME_BUSY;
		survey->time = WL_CFG_LOAD_FULL;
		survey->time_busy = WL_CFG_LOAD_FULL * l / (l + WL_CFG_LOAD_FULL);
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

static int wl_cfg_apply_acl(struct wl_cfg_vif *vif, const struct cfg80211_acl_data *acl);

/*
 * hostapd's legacy rate set, from the beacon it built: Supported Rates in the head (after the
 * 24-byte header and 12 fixed bytes), Extended Supported Rates in the tail; basic rates have 0x80.
 * Returns the count (0 if there is nothing to go by: the core keeps its own).
 */
static int wl_cfg_ies_rates(const u8 *ie, size_t len, u8 *rates, int n, int max)
{
	const u8 *end = ie + len;
	int i;

	while (ie + 2 <= end && ie + 2 + ie[1] <= end) {
		if (ie[0] == WLAN_EID_SUPP_RATES || ie[0] == WLAN_EID_EXT_SUPP_RATES)
			for (i = 0; i < ie[1] && n < max; i++)
				if ((ie[2 + i] & 0x7f) != 0x7f)	/* not a BSS membership selector */
					rates[n++] = ie[2 + i];
		ie += 2 + ie[1];
	}
	return n;
}

static int wl_cfg_beacon_rates(const struct cfg80211_beacon_data *b, u8 *rates, int max)
{
	const size_t fixed = 24 + 12;
	int n = 0;

	if (b->head && b->head_len > fixed)
		n = wl_cfg_ies_rates(b->head + fixed, b->head_len - fixed, rates, n, max);
	if (n && b->tail)
		n = wl_cfg_ies_rates(b->tail, b->tail_len, rates, n, max);
	return n;
}

static int wl_cfg_start_ap_op(struct wiphy *wiphy, struct net_device *dev,
			      struct cfg80211_ap_settings *s)
{
	struct wl_cfg_vif *vif = wl_vif_of_dev(dev), *sta;
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
	ap.ht = s->chandef.width != NL80211_CHAN_WIDTH_20_NOHT;	/* LuCI's "Legacy" mode: no HT */
	ap.n_rates = wl_cfg_beacon_rates(&s->beacon, ap.rates, ARRAY_SIZE(ap.rates));
	if (s->chandef.width == NL80211_CHAN_WIDTH_40)
		ap.center_channel = ieee80211_frequency_to_channel(s->chandef.center_freq1);
	err = wl_cfg_crypto(&s->crypto, s->privacy, &ap.wsec, &ap.auth);
	if (err)
		return err;
	/* the radio's channel: a connected STA decides it, else the first AP to start */
	sta = wl_cfg_vif_of_type(vif->cfg, NL80211_IFTYPE_STATION);
	ap.set_radio = !(sta && (sta->connected || sta->connecting)) && !wl_cfg_ap_running(vif->cfg);
	err = wl_cfg_start_ap(vif->ifp, &ap);
	vif->ap_started = !err;
	if (!err && s->acl)
		err = wl_cfg_apply_acl(vif, s->acl);
	netdev_info(dev, "start_ap: channel %d%s%s wsec 0x%x auth 0x%x: %d\n", ap.channel,
		    ap.center_channel ? (ap.center_channel > ap.channel ? " HT40+" : " HT40-") : "",
		    ap.set_radio ? "" : sta && (sta->connected || sta->connecting) ?
		    " (radio follows the STA)" : " (radio already set)", ap.wsec, ap.auth, err);
	return err;
}

/*
 * MAC filter (hostapd's macaddr_acl with accept/deny lists, LuCI's "MAC-Filter"). The core does the
 * association itself, so hostapd hands its list to the driver (ACL offload) and the core's maclist
 * enforces it on (re)association. Stations already associated that the new list excludes are
 * deauthenticated, as hostapd would do itself; the core's DEAUTH event then removes them from
 * cfg80211. The core keeps one list per BSS.
 *
 * Limitation: hostapd (2.9 to 2.11 at least) offloads only its first BSS's list, once per radio
 * (hostapd_set_acl() on iface->bss[0]), and does not check the lists itself for associations the
 * driver reports. So only the first SSID of the radio is filtered: list the SSID that needs a
 * filter first in /etc/config/wireless.
 */
static int wl_cfg_apply_acl(struct wl_cfg_vif *vif, const struct cfg80211_acl_data *acl)
{
	u8 (*macs)[ETH_ALEN], sta[WL_CFG_MAX_STA][ETH_ALEN];
	int i, j, n = acl ? acl->n_acl_entries : 0, mode, nsta, err;

	if (n > WL_CFG_MAX_ACL)
		return -EINVAL;
	mode = !acl ? WL_CFG_ACL_OFF : acl->acl_policy == NL80211_ACL_POLICY_DENY_UNLESS_LISTED ?
	       WL_CFG_ACL_ALLOW : n ? WL_CFG_ACL_DENY : WL_CFG_ACL_OFF;
	macs = kcalloc(max(n, 1), ETH_ALEN, GFP_KERNEL);
	if (!macs)
		return -ENOMEM;
	for (i = 0; i < n; i++)
		memcpy(macs[i], acl->mac_addrs[i].addr, ETH_ALEN);
	err = wl_cfg_set_acl(vif->ifp, mode, (const u8 (*)[6])macs, n);
	if (!err && mode != WL_CFG_ACL_OFF) {
		nsta = wl_cfg_assoclist(vif->ifp, sta, WL_CFG_MAX_STA);
		for (i = 0; i < nsta; i++) {
			for (j = 0; j < n && !ether_addr_equal(sta[i], macs[j]); j++)
				;
			if ((j < n) == (mode == WL_CFG_ACL_DENY))
				wl_cfg_deauth(vif->ifp, sta[i], WLAN_REASON_UNSPECIFIED);
		}
	}
	kfree(macs);
	if (vif->wdev.netdev)
		netdev_info(vif->wdev.netdev, "MAC filter: %s, %d address%s: %d\n",
			    mode == WL_CFG_ACL_ALLOW ? "allow listed" : mode == WL_CFG_ACL_DENY ?
			    "deny listed" : "off", n, n == 1 ? "" : "es", err);
	return err;
}

static int wl_cfg_set_mac_acl(struct wiphy *wiphy, struct net_device *dev,
			      const struct cfg80211_acl_data *acl)
{
	return wl_cfg_apply_acl(wl_vif_of_dev(dev), acl);
}

static int wl_cfg_change_beacon(struct wiphy *wiphy, struct net_device *dev,
				struct cfg80211_beacon_data *info)
{
	return 0;	/* the core builds its own beacon and probe responses */
}

static int wl_cfg_stop_ap_op(struct wiphy *wiphy, struct net_device *dev)
{
	wl_vif_of_dev(dev)->ap_started = false;
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
	/* always on the primary (STA) bsscfg: the core scans there in apsta mode, AP running or not */
	err = wl_cfg_scan(cfg->bss[0], ssid, ssid_len);
	if (err)
		return -EBUSY;
	cfg->scan_req = req;	/* finished by WLC_E_SCAN_COMPLETE */
	return 0;
}

/*
 * Channel load for hostapd's automatic channel selection (ACS). ACS ranks channels by the busy
 * time in a survey of every channel, but this core measures airtime (chanim) only on the channel
 * it is on. So the survey is estimated from the scan results instead, as the core's own channel
 * selection does: every BSS adds load to its 20 MHz channel(s), weighted by its signal, and less
 * to the overlapping channels up to 4 away. A 40 MHz BSS (HT operation IE) loads both halves.
 */
static void wl_cfg_load_add(struct wl_cfg *cfg, int ch, u32 strength)
{
	int k, d;

	for (k = 1; k <= 14; k++) {
		d = k > ch ? k - ch : ch - k;
		if (d < 5)
			cfg->load[k] += strength * (5 - d) / 5;
	}
}

static void wl_cfg_load_one(struct wl_cfg *cfg, const struct wl_cfg_bss *b)
{
	const u8 *ie = b->ies, *end = b->ies + b->ies_len;
	int rssi = b->rssi ? b->rssi : -80, sec = 0;
	u32 s;

	if (b->channel < 1 || b->channel > 14)
		return;
	/* -95 dBm and below: 5%; -45 dBm and above: 100% */
	s = rssi <= -95 ? 50 : rssi >= -45 ? WL_CFG_LOAD_FULL : (rssi + 95) * WL_CFG_LOAD_FULL / 50;
	if (s < 50)
		s = 50;
	while (ie + 2 <= end && ie + 2 + ie[1] <= end) {
		if (ie[0] == WLAN_EID_HT_OPERATION && ie[1] >= 2 && (ie[3] & 0x04)) {
			if ((ie[3] & 0x03) == 1)
				sec = 4;
			else if ((ie[3] & 0x03) == 3)
				sec = -4;
		}
		ie += 2 + ie[1];
	}
	wl_cfg_load_add(cfg, b->channel, s);
	if (sec && b->channel + sec >= 1 && b->channel + sec <= 14)
		wl_cfg_load_add(cfg, b->channel + sec, s);
}

struct wl_cfg_scan_ctx {
	struct wl_cfg_inform_ctx inform;
	struct wl_cfg *cfg;
};

static void wl_cfg_scan_one(void *ctx, const struct wl_cfg_bss *b)
{
	struct wl_cfg_scan_ctx *c = ctx;

	wl_cfg_load_one(c->cfg, b);
	wl_cfg_inform_one(&c->inform, b);
}

static void wl_cfg_scan_finish(struct wl_cfg *cfg, bool aborted)
{
	struct cfg80211_scan_info info = { .aborted = aborted };
	struct wl_cfg_scan_ctx ctx = { .inform = { .wiphy = cfg->wiphy }, .cfg = cfg };

	if (!cfg->scan_req)
		return;
	if (!aborted) {
		memset(cfg->load, 0, sizeof(cfg->load));
		wl_cfg_scan_results(cfg->bss[0], wl_cfg_scan_one, &ctx);
		cfg->load_valid = true;
	}
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
	/* an earlier legacy-mode AP may have left 802.11n off; with an AP running it decides */
	if (!wl_cfg_ap_running(vif->cfg))
		wl_cfg_radio_setint(vif->cfg->wl, "nmode", 1, true);
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
 * Interface addresses. The core matches its secondary BSSes against one base address: the primary's
 * MAC (at the time bsscfg 1 is created, so the real one) with the locally administered bit set and
 * the last byte + 1, with the low 2 bits masked; a BSS's own low 2 bits are its index there
 * (wlc_bsscfg_up, wlc_write_mbss_basemac). So an AP on bsscfg 1 can never use the real MAC.
 * wiphy->addresses are two addresses inside that range; mac80211.sh gives the first one to the
 * first interface (a STA, or a lone AP) and the second one to the next (the AP of a repeater).
 * cfg80211 wants addresses[0] to be the wiphy's perm_addr, so that is the first one too (and the
 * phy's "macaddress" in sysfs, which OpenWrt matches the radio by).
 */
#define WL_CFG_MBSS_MASK	3

static void wl_cfg_vif_mac(struct wl_cfg *cfg, int k, u8 *mac)
{
	u8 base = cfg->hw_mac[5] + 1;

	memcpy(mac, cfg->hw_mac, ETH_ALEN);
	mac[0] |= 0x02;
	mac[5] = (base & ~WL_CFG_MBSS_MASK) | ((base + k) & WL_CFG_MBSS_MASK);
}

/* apsta mode and bsscfg 1, once, before any interface address changes the primary's MAC */
static int wl_cfg_prepare(struct wl_cfg *cfg)
{
	u8 mac[ETH_ALEN];
	int err;

	if (cfg->apsta)
		return 0;
	if (!cfg->bss[1]) {
		cfg->bss[1] = wl_cfg_if_alloc(cfg->wl);
		if (!cfg->bss[1])
			return -ENOMEM;
	}
	err = wl_cfg_set_mode(cfg->wl, true, true);
	wl_cfg_vif_mac(cfg, 0, mac);
	err = err ?: wl_cfg_if_bss_create(cfg->bss[1], 1, mac);
	if (!err)
		cfg->apsta = true;
	return err;
}

/*
 * The default address for a new interface: a STA takes the first one, an AP the first one no other
 * interface has (mac80211.sh and hostapd then set the one it planned, normally the same).
 */
static void wl_cfg_mac_for(struct wl_cfg *cfg, enum nl80211_iftype type, u8 *mac)
{
	int k, i;

	for (k = 0; type == NL80211_IFTYPE_AP && k < WL_CFG_NADDR; k++) {
		for (i = 0; i < WL_CFG_NVIF; i++)
			if (cfg->vif[i].in_use && cfg->vif[i].wdev.netdev &&
			    ether_addr_equal(cfg->vif[i].wdev.netdev->dev_addr, cfg->addrs[k].addr))
				break;
		if (i == WL_CFG_NVIF)
			break;
	}
	wl_cfg_vif_mac(cfg, k < WL_CFG_NADDR ? k : 0, mac);
}

static struct wl_cfg_vif *wl_cfg_vif_of_ifp(struct wl_cfg *cfg, struct wl_if *ifp);

static bool wl_cfg_bss_in_use(struct wl_cfg *cfg, struct wl_if *ifp)
{
	return wl_cfg_vif_of_ifp(cfg, ifp) != NULL;
}

/* the bsscfg for a new interface: the primary for a STA, a free AP bsscfg (created on demand) */
static int wl_cfg_bss_for(struct wl_cfg *cfg, enum nl80211_iftype type, const u8 *mac,
			  struct wl_if **ifpp)
{
	int b, err;

	if (type == NL80211_IFTYPE_STATION) {
		*ifpp = cfg->bss[0];
		return 0;
	}
	for (b = 1; b < WL_CFG_NBSS; b++) {
		if (cfg->bss[b] && wl_cfg_bss_in_use(cfg, cfg->bss[b]))
			continue;
		if (!cfg->bss[b]) {
			cfg->bss[b] = wl_cfg_if_alloc(cfg->wl);
			if (!cfg->bss[b])
				return -ENOMEM;
		}
		err = wl_cfg_if_bss_create(cfg->bss[b], b, mac);
		if (err)
			return err;
		*ifpp = cfg->bss[b];
		return 0;
	}
	return -EBUSY;
}

static struct wireless_dev *wl_cfg_add_virtual_intf(struct wiphy *wiphy, const char *name,
						    unsigned char name_assign_type,
						    enum nl80211_iftype type,
						    struct vif_params *params)
{
	struct wl_cfg *cfg = wl_cfg_of(wiphy);
	struct wl_cfg_vif *vif;
	struct net_device *dev;
	struct wl_if *ifp;
	u8 mac[ETH_ALEN];
	int err;

	ASSERT_RTNL();
	if (type != NL80211_IFTYPE_AP && type != NL80211_IFTYPE_STATION)
		return ERR_PTR(-EOPNOTSUPP);
	/* one address each: the iface combination is only checked when an interface goes up */
	if (wl_cfg_n_of_type(cfg, NL80211_IFTYPE_STATION) + wl_cfg_n_of_type(cfg, NL80211_IFTYPE_AP) >=
	    WL_CFG_NADDR ||
	    (type == NL80211_IFTYPE_STATION ? wl_cfg_vif_of_type(cfg, type) != NULL :
	     wl_cfg_n_of_type(cfg, type) >= WL_CFG_MAX_AP))
		return ERR_PTR(-EBUSY);
	err = wl_cfg_prepare(cfg);
	/*
	 * The first interface: the TX power limit goes back to none, as on mac80211, where it is per
	 * interface. mac80211.sh sets a configured one again after creating the interfaces, and none
	 * for LuCI's "driver default"; the core keeps its limit radio-wide otherwise.
	 */
	if (!err && !wl_cfg_n_of_type(cfg, NL80211_IFTYPE_STATION) &&
	    !wl_cfg_n_of_type(cfg, NL80211_IFTYPE_AP))
		wl_cfg_radio_setint(cfg->wl, "qtxpower", WL_QDBM_NO_LIMIT, false);
	wl_cfg_mac_for(cfg, type, mac);
	err = err ?: wl_cfg_bss_for(cfg, type, mac, &ifp);
	if (err) {
		wiphy_err(wiphy, "cannot set up %s as %s: %d\n", name,
			  type == NL80211_IFTYPE_AP ? "AP" : "STA", err);
		return ERR_PTR(err);
	}
	/* the interface created at probe, on the primary bsscfg and not handed out yet */
	dev = wl_cfg_if_dev(cfg->bss[0]);
	if (!cfg->vif[0].in_use && dev) {
		if (type == NL80211_IFTYPE_STATION && !strcmp(dev->name, name)) {
			vif = &cfg->vif[0];		/* the one OpenWrt asks for: keep it */
			vif->ifp = ifp;
			vif->wdev.iftype = type;
			goto done;
		}
		wl_cfg_netdev_destroy(cfg->bss[0]);
	}
	for (vif = &cfg->vif[0]; vif->in_use; vif++)
		;			/* a free one exists: at most WL_CFG_NADDR are in use */
	memset(&vif->wdev, 0, sizeof(vif->wdev));
	vif->wdev.wiphy = wiphy;
	vif->wdev.iftype = type;
	vif->ifp = ifp;
	dev = wl_cfg_netdev_alloc(ifp, name, name_assign_type, mac);
	if (IS_ERR(dev))
		return ERR_CAST(dev);
	dev->ieee80211_ptr = &vif->wdev;
	vif->wdev.netdev = dev;
	err = wl_cfg_netdev_register(ifp, dev);
	if (err) {
		vif->wdev.netdev = NULL;
		wiphy_err(wiphy, "cannot create %s: %d\n", name, err);
		return ERR_PTR(err);
	}
done:
	vif->in_use = true;
	vif->ap_started = false;
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
	if (vif->wdev.iftype == NL80211_IFTYPE_AP)
		wl_cfg_stop_ap(vif->ifp);
	if (cfg->scan_req && cfg->scan_req->wdev == wdev)
		wl_cfg_scan_finish(cfg, true);
	vif->connecting = vif->connected = vif->keyed = false;
	vif->ap_started = false;
	vif->in_use = false;
	/*
	 * wdev.netdev must stay set: nl80211 holds a reference on it during this request and drops it
	 * afterwards through wdev->netdev. The net_device itself is freed once that is done. Nothing
	 * else uses the pointer while in_use is false, and add_virtual_intf rewrites the wdev.
	 */
	wl_cfg_netdev_destroy(vif->ifp);
	/* without a STA interface the primary bsscfg is the radio's own again (not an AP's address) */
	if (vif->ifp == cfg->bss[0])
		wl_cfg_if_set_mac(cfg->bss[0], cfg->hw_mac);
	return 0;
}

/* AP <-> STA on an existing (down) interface: it moves to the other bsscfg, with that one's address */
static int wl_cfg_change_virtual_intf(struct wiphy *wiphy, struct net_device *ndev,
				      enum nl80211_iftype type, struct vif_params *params)
{
	struct wl_cfg *cfg = wl_cfg_of(wiphy);
	struct wl_cfg_vif *vif = wl_vif_of_dev(ndev);
	struct wl_if *to;
	u8 mac[ETH_ALEN];
	int err;

	ASSERT_RTNL();
	if (type == vif->wdev.iftype)
		return 0;
	if (type != NL80211_IFTYPE_AP && type != NL80211_IFTYPE_STATION)
		return -EOPNOTSUPP;
	if ((type == NL80211_IFTYPE_STATION ? wl_cfg_vif_of_type(cfg, type) != NULL :
	     wl_cfg_n_of_type(cfg, type) >= WL_CFG_MAX_AP) || netif_running(ndev))
		return -EBUSY;
	err = wl_cfg_prepare(cfg);
	wl_cfg_mac_for(cfg, type, mac);
	err = err ?: wl_cfg_bss_for(cfg, type, mac, &to);
	if (err)
		return err;
	if (vif->wdev.iftype == NL80211_IFTYPE_AP)
		wl_cfg_stop_ap(vif->ifp);
	else if (vif->connected || vif->connecting)
		wl_cfg_disassoc(vif->ifp);
	vif->ap_started = false;
	err = wl_cfg_netdev_move(vif->ifp, to, mac);
	if (err)
		return err;
	if (vif->ifp == cfg->bss[0])		/* as in del_virtual_intf */
		wl_cfg_if_set_mac(cfg->bss[0], cfg->hw_mac);
	vif->ifp = to;
	vif->wdev.iftype = type;
	vif->in_use = true;
	vif->connecting = vif->connected = vif->keyed = false;
	return 0;
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
	.set_mac_acl		= wl_cfg_set_mac_acl,
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

/* the interface in use on a bsscfg (the probe-time one may still point at bsscfg 0, unused) */
static struct wl_cfg_vif *wl_cfg_vif_of_ifp(struct wl_cfg *cfg, struct wl_if *ifp)
{
	int i;

	for (i = 0; i < WL_CFG_NVIF; i++)
		if (cfg->vif[i].in_use && cfg->vif[i].ifp == ifp)
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
void wl_cfg80211_if_down(struct wl_cfg *cfg, struct net_device *dev)
{
	ASSERT_RTNL();
	/* cfg80211 warns, and frees the request, if a scan is still running when its netdev is down */
	if (cfg && cfg->scan_req && cfg->scan_req->wdev == dev->ieee80211_ptr)
		wl_cfg_scan_finish(cfg, true);
}

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
	if (ev->bsscfgidx >= 0 && ev->bsscfgidx < WL_CFG_NBSS && cfg->bss[ev->bsscfgidx])
		vif = wl_cfg_vif_of_ifp(cfg, cfg->bss[ev->bsscfgidx]);
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

/* ------------------------------------------------------------------ regulatory */

/*
 * The core has its own country tables: channels, power limits per rate and band edge, and the
 * Country element it puts in its beacons. cfg80211's country (iw reg set, hostapd's country_code,
 * the AP a STA connects to) is handed to it; the world domain becomes the core's "XU". cfg80211 goes
 * on applying the regulatory database to the channel list, and a channel the core does not allow
 * in that country is disabled on top. The notifier only queues: it runs inside cfg80211's
 * regulatory processing, the core calls sleep.
 */
static void wl_cfg_reg_notifier(struct wiphy *wiphy, struct regulatory_request *req)
{
	struct wl_cfg *cfg = wl_cfg_of(wiphy);
	unsigned long flags;

	spin_lock_irqsave(&cfg->evlock, flags);
	if (!cfg->dying) {
		memcpy(cfg->reg_alpha2, req->alpha2, 2);
		schedule_work(&cfg->regwork);
	}
	spin_unlock_irqrestore(&cfg->evlock, flags);
}

static void wl_cfg_regwork(struct work_struct *work)
{
	struct wl_cfg *cfg = container_of(work, struct wl_cfg, regwork);
	char cc[3] = { cfg->reg_alpha2[0], cfg->reg_alpha2[1], 0 };
	u32 valid[32];
	int i, j, n, err;

	if (!isalpha(cc[0]) || !isalpha(cc[1]))	/* "00" world, "98"/"99" intersected/custom */
		strcpy(cc, "XU");
	rtnl_lock();
	if (cfg->dying)
		goto out;
	err = wl_cfg_set_country(cfg->bss[0], cc);
	n = wl_cfg_valid_channels(cfg->bss[0], valid, ARRAY_SIZE(valid));
	for (i = 0; n > 0 && i < cfg->band.n_channels; i++) {
		struct ieee80211_channel *ch = &cfg->band.channels[i];

		for (j = 0; j < n && valid[j] != ch->hw_value; j++)
			;
		if (j == n)
			ch->flags |= IEEE80211_CHAN_DISABLED;
	}
	if (err)
		wiphy_warn(cfg->wiphy, "country %s is not in the core's tables, it keeps its previous one\n",
			   cc);
	else
		wiphy_info(cfg->wiphy, "country %s: %d channels\n", cc, n);
out:
	rtnl_unlock();
}

/* ------------------------------------------------------------------ attach / detach */

static void wl_cfg_setup_band(struct wl_cfg *cfg, struct ieee80211_channel *chans)
{
	u32 valid[32];
	int i, j, n = wl_cfg_valid_channels(cfg->bss[0], valid, ARRAY_SIZE(valid));

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
	cfg->bss[0] = wl_cfg_primary(wl);
	cfg->vif[0].ifp = cfg->bss[0];
	spin_lock_init(&cfg->evlock);
	INIT_LIST_HEAD(&cfg->evq);
	INIT_WORK(&cfg->evwork, wl_cfg_evwork);
	INIT_WORK(&cfg->regwork, wl_cfg_regwork);
	chans = (struct ieee80211_channel *)(cfg + 1);
	wl_cfg_setup_band(cfg, chans);

	set_wiphy_dev(wiphy, parent);
	memcpy(cfg->hw_mac, dev->dev_addr, ETH_ALEN);
	for (i = 0; i < WL_CFG_NADDR; i++)
		wl_cfg_vif_mac(cfg, i, cfg->addrs[i].addr);
	memcpy(wiphy->perm_addr, cfg->addrs[0].addr, ETH_ALEN);
	wiphy->addresses = cfg->addrs;
	wiphy->n_addresses = WL_CFG_NADDR;
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
	wiphy->reg_notifier = wl_cfg_reg_notifier;
	wiphy_apply_custom_regulatory(wiphy, &wl_cfg_regd);
	/* AP: association and probe responses happen in the core; hostapd only does WPA and keys */
	wiphy->flags |= WIPHY_FLAG_HAVE_AP_SME;
	wiphy->probe_resp_offload = NL80211_PROBE_RESP_OFFLOAD_SUPPORT_WPS |
				    NL80211_PROBE_RESP_OFFLOAD_SUPPORT_WPS2;
	wiphy->features |= NL80211_FEATURE_INACTIVITY_TIMER;	/* scb_activity_time */
	wiphy->max_acl_mac_addrs = WL_CFG_MAX_ACL;		/* MAC filter offload */
	wiphy->mgmt_stypes = wl_cfg_mgmt_stypes;
	/* STA: the core's supplicant does the 4-way handshake with the PSK from connect */
	wiphy_ext_feature_set(wiphy, NL80211_EXT_FEATURE_4WAY_HANDSHAKE_STA_PSK);

	err = wiphy_register(wiphy);
	if (err) {
		pr_err("%s: wiphy_register failed: %d\n", dev->name, err);
		wiphy_free(wiphy);
		return NULL;
	}
	/* the interface created at probe, on the primary bsscfg: a STA until OpenWrt says otherwise */
	cfg->vif[0].wdev.wiphy = wiphy;
	cfg->vif[0].wdev.iftype = NL80211_IFTYPE_STATION;
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
	int i, b;

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
	/* the primary's interface is gone already (the glue unregistered it) */
	for (b = 1; b < WL_CFG_NBSS; b++) {
		if (!cfg->bss[b])
			continue;
		wl_cfg_netdev_destroy(cfg->bss[b]);
		for (i = 0; i < WL_CFG_NVIF; i++)
			if (cfg->vif[i].ifp == cfg->bss[b])
				cfg->vif[i].wdev.netdev = NULL;
	}
	rtnl_unlock();
	wiphy_unregister(cfg->wiphy);
}

/* stage 2: after the core detached (it may call wl_del_if() for the AP bsscfgs until then) */
void wl_cfg80211_free(struct wl_cfg *cfg)
{
	struct wl_cfg_qev *q, *n;
	int b;

	if (!cfg)
		return;
	list_for_each_entry_safe(q, n, &cfg->evq, list)
		kfree(q);
	for (b = 1; b < WL_CFG_NBSS; b++)
		wl_cfg_if_free(cfg->bss[b]);
	wiphy_free(cfg->wiphy);
}
