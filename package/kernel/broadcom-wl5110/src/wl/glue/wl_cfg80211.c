/*
 * cfg80211 front end for the Broadcom wl 5.110 core (experimental, Phase D).
 *
 * The radio is a wiphy with one AP-mode wireless_dev (wl0). The core keeps doing 802.11
 * association, beacons and probe responses itself ("AP SME in the driver", as in brcmfmac); hostapd
 * configures the BSS with start_ap, runs the WPA 4-way/group handshakes over the normal data path
 * and installs keys and the station's authorization through the ops below, which map one-to-one onto
 * the core ioctls nas uses. State is reported through nl80211 (channel, stations, noise, thresholds).
 * brcmwl.sh/wlc/nas still work alongside, as long as only one of them drives the BSS.
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
#include <net/cfg80211.h>

#include "wl_cfg80211.h"

#define WL_CFG_MAX_STA		32
#define WL_QDBM_NO_LIMIT	127	/* qtxpower: no user limit set, real power is board-limited */

struct wl_cfg {
	struct wl_info *wl;
	struct wireless_dev wdev;
	struct ieee80211_supported_band band;	/* per device: channels get disabled per country */
	u8 sta_macs[WL_CFG_MAX_STA][ETH_ALEN];	/* dump_station snapshot, taken at idx 0 */
	int n_sta;
	bool vif_in_use;			/* handed out by add_virtual_intf */
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

/* BCM5357C0: 2x2 802.11n, 20/40 MHz */
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

static struct wl_cfg *wl_cfg_of(struct wiphy *wiphy)
{
	return wiphy_priv(wiphy);
}

/* ------------------------------------------------------------------ cfg80211 ops */

static int wl_cfg_get_channel(struct wiphy *wiphy, struct wireless_dev *wdev,
			      struct cfg80211_chan_def *chandef)
{
	struct ieee80211_channel *chan;
	int ch, err = wl_cfg_channel(wl_cfg_of(wiphy)->wl, &ch);

	if (err)
		return err;
	chan = ieee80211_get_channel(wiphy, ieee80211_channel_to_frequency(ch, NL80211_BAND_2GHZ));
	if (!chan)
		return -EINVAL;
	cfg80211_chandef_create(chandef, chan, NL80211_CHAN_HT20);
	return 0;
}

static int wl_cfg_get_tx_power(struct wiphy *wiphy, struct wireless_dev *wdev, int *dbm)
{
	int qdbm, err = wl_cfg_iovar_getint(wl_cfg_of(wiphy)->wl, "qtxpower", &qdbm);

	if (err)
		return err;
	qdbm &= 0xff;		/* top bit is the override flag */
	if (qdbm >= WL_QDBM_NO_LIMIT)
		return -ENODATA;	/* no limit set: actual power not known here */
	*dbm = qdbm / 4;
	return 0;
}

static int wl_cfg_set_wiphy_params(struct wiphy *wiphy, u32 changed)
{
	struct wl_info *wl = wl_cfg_of(wiphy)->wl;
	int err = 0;

	if (changed & WIPHY_PARAM_RTS_THRESHOLD)
		err = wl_cfg_iovar_setint(wl, "rtsthresh",
					  wiphy->rts_threshold == (u32)-1 ? 2347 : wiphy->rts_threshold);
	if (!err && (changed & WIPHY_PARAM_FRAG_THRESHOLD))
		err = wl_cfg_iovar_setint(wl, "fragthresh",
					  wiphy->frag_threshold == (u32)-1 ? 2346 : wiphy->frag_threshold);
	return err;
}

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
	struct wl_cfg_sta st;
	int err = wl_cfg_sta_info(wl_cfg_of(wiphy)->wl, mac, &st);

	if (err)
		return err;
	if (!(st.flags & WL_CFG_STA_ASSOC))
		return -ENOENT;
	wl_cfg_fill_sinfo(sinfo, &st);
	return 0;
}

static int wl_cfg_dump_station(struct wiphy *wiphy, struct net_device *dev, int idx, u8 *mac,
			       struct station_info *sinfo)
{
	struct wl_cfg *cfg = wl_cfg_of(wiphy);
	struct wl_cfg_sta st;
	int n;

	if (idx == 0) {	/* new dump: snapshot the association list */
		n = wl_cfg_assoclist(cfg->wl, cfg->sta_macs, WL_CFG_MAX_STA);
		cfg->n_sta = n < 0 ? 0 : n;
	}
	for (; idx < cfg->n_sta; idx++) {	/* skip stations that left since the snapshot */
		if (wl_cfg_sta_info(cfg->wl, cfg->sta_macs[idx], &st))
			continue;
		memcpy(mac, cfg->sta_macs[idx], ETH_ALEN);
		wl_cfg_fill_sinfo(sinfo, &st);
		return 0;
	}
	return -ENOENT;
}

static int wl_cfg_dump_survey(struct wiphy *wiphy, struct net_device *dev, int idx,
			      struct survey_info *survey)
{
	struct wl_info *wl = wl_cfg_of(wiphy)->wl;
	int ch, noise;

	if (idx != 0 || wl_cfg_channel(wl, &ch))
		return -ENOENT;
	survey->channel = ieee80211_get_channel(wiphy,
					ieee80211_channel_to_frequency(ch, NL80211_BAND_2GHZ));
	if (!survey->channel)
		return -ENOENT;
	survey->filled = SURVEY_INFO_IN_USE;
	if (!wl_cfg_noise(wl, &noise) && noise < 0) {
		survey->filled |= SURVEY_INFO_NOISE_DBM;
		survey->noise = noise;
	}
	return 0;
}

/* ------------------------------------------------------------------ AP: hostapd */

static int wl_cfg_crypto_to_ap(const struct cfg80211_crypto_settings *c, bool privacy,
			       struct wl_cfg_ap *ap)
{
	int i;
	u32 v = c->wpa_versions;

	for (i = 0; i < c->n_ciphers_pairwise; i++) {
		switch (c->ciphers_pairwise[i]) {
		case WLAN_CIPHER_SUITE_CCMP: ap->wsec |= WL_CFG_WSEC_AES; break;
		case WLAN_CIPHER_SUITE_TKIP: ap->wsec |= WL_CFG_WSEC_TKIP; break;
		case WLAN_CIPHER_SUITE_WEP40:
		case WLAN_CIPHER_SUITE_WEP104: ap->wsec |= WL_CFG_WSEC_WEP; break;
		default: return -EOPNOTSUPP;
		}
	}
	switch (c->cipher_group) {
	case 0: break;
	case WLAN_CIPHER_SUITE_CCMP: ap->wsec |= WL_CFG_WSEC_AES; break;
	case WLAN_CIPHER_SUITE_TKIP: ap->wsec |= WL_CFG_WSEC_TKIP; break;
	case WLAN_CIPHER_SUITE_WEP40:
	case WLAN_CIPHER_SUITE_WEP104: ap->wsec |= WL_CFG_WSEC_WEP; break;
	default: return -EOPNOTSUPP;
	}
	for (i = 0; i < c->n_akm_suites; i++) {
		switch (c->akm_suites[i]) {
		case WLAN_AKM_SUITE_PSK:
			ap->auth |= ((v & NL80211_WPA_VERSION_1) ? WL_CFG_AUTH_WPA_PSK : 0) |
				    ((v & NL80211_WPA_VERSION_2) ? WL_CFG_AUTH_WPA2_PSK : 0);
			break;
		case WLAN_AKM_SUITE_8021X:
			ap->auth |= ((v & NL80211_WPA_VERSION_1) ? WL_CFG_AUTH_WPA_8021X : 0) |
				    ((v & NL80211_WPA_VERSION_2) ? WL_CFG_AUTH_WPA2_8021X : 0);
			break;
		default:
			return -EOPNOTSUPP;	/* SAE, FT, SHA256 AKMs: not in this core */
		}
	}
	if (privacy && !ap->auth)
		ap->wsec |= WL_CFG_WSEC_WEP;
	return 0;
}

static int wl_cfg_start_ap_op(struct wiphy *wiphy, struct net_device *dev,
			      struct cfg80211_ap_settings *s)
{
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
	err = wl_cfg_crypto_to_ap(&s->crypto, s->privacy, &ap);
	if (err)
		return err;
	if (s->chandef.width != NL80211_CHAN_WIDTH_20_NOHT && s->chandef.width != NL80211_CHAN_WIDTH_20)
		netdev_info(dev, "start_ap: width %d requested, the core picks its own bandwidth\n",
			    s->chandef.width);
	err = wl_cfg_start_ap(wl_cfg_of(wiphy)->wl, &ap);
	netdev_info(dev, "start_ap: ssid '%.*s' channel %d wsec 0x%x auth 0x%x: %d\n",
		    ap.ssid_len, ap.ssid, ap.channel, ap.wsec, ap.auth, err);
	return err;
}

static int wl_cfg_change_beacon(struct wiphy *wiphy, struct net_device *dev,
				struct cfg80211_beacon_data *info)
{
	return 0;	/* the core builds its own beacon and probe responses */
}

static int wl_cfg_stop_ap_op(struct wiphy *wiphy, struct net_device *dev)
{
	return wl_cfg_stop_ap(wl_cfg_of(wiphy)->wl);
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
	return wl_cfg_set_key(wl_cfg_of(wiphy)->wl, idx, (pairwise && wl_cfg_is_sta_addr(mac)) ? mac : NULL,
			      c, p->key, p->key_len, !pairwise);
}

static int wl_cfg_del_key_op(struct wiphy *wiphy, struct net_device *dev, u8 idx, bool pairwise,
			  const u8 *mac)
{
	if (idx > 3)
		return -EINVAL;
	/* hostapd treats -ENOENT on delete as success */
	return wl_cfg_del_key(wl_cfg_of(wiphy)->wl, idx,
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
	if (wl_cfg_key_seq(wl_cfg_of(wiphy)->wl, idx, seq))
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
	return wl_cfg_authorize(wl_cfg_of(wiphy)->wl, mac, params->sta_flags_set & auth);
}

static int wl_cfg_del_station(struct wiphy *wiphy, struct net_device *dev,
			      struct station_del_parameters *params)
{
	if (!wl_cfg_is_sta_addr(params->mac))
		return 0;	/* hostapd's start-up broadcast deauth: nobody is associated yet */
	return wl_cfg_deauth(wl_cfg_of(wiphy)->wl, params->mac,
			     params->reason_code ?: WLAN_REASON_DEAUTH_LEAVING);
}

static int wl_cfg_set_bss(struct wiphy *wiphy, struct net_device *dev, struct bss_parameters *p)
{
	if (p->ap_isolate >= 0)
		return wl_cfg_iovar_setint(wl_cfg_of(wiphy)->wl, "ap_isolate", p->ap_isolate);
	return 0;	/* preamble, slot time, CTS protection and HT opmode are the core's business */
}

/* hostapd registers for action frames; the core handles those it supports itself */
static int wl_cfg_mgmt_tx(struct wiphy *wiphy, struct wireless_dev *wdev,
			  struct cfg80211_mgmt_tx_params *params, u64 *cookie)
{
	return -EOPNOTSUPP;
}

static void wl_cfg_mgmt_frame_register(struct wiphy *wiphy, struct wireless_dev *wdev,
				       u16 frame_type, bool reg)
{
}

static const struct ieee80211_txrx_stypes wl_cfg_mgmt_stypes[NUM_NL80211_IFTYPES] = {
	[NL80211_IFTYPE_AP] = {
		.tx = 0xffff,
		.rx = BIT(IEEE80211_STYPE_ACTION >> 4),
	},
};

/*
 * Station mode comes in D4. hostapd refuses drivers that advertise neither auth/assoc nor connect
 * ("Driver does not support authentication/association or connect commands"), even for AP use.
 */
static int wl_cfg_connect(struct wiphy *wiphy, struct net_device *dev,
			  struct cfg80211_connect_params *sme)
{
	return -EOPNOTSUPP;
}

static int wl_cfg_disconnect(struct wiphy *wiphy, struct net_device *dev, u16 reason_code)
{
	return -EOPNOTSUPP;
}

/*
 * One BSS, one netdev (wlan0), created at probe. OpenWrt's mac80211.sh deletes all interfaces of the
 * phy and then adds one per wifi-iface ("iw phy phyN interface add <ifname> type __ap", ifname
 * defaulting to wlan<N>). Deleting only takes the netdev down; adding hands the same wireless_dev
 * back if the name matches. The netdev stays registered throughout, so the cfg80211 netdev notifier
 * keeps it on the wiphy's list. Renaming isn't possible (dev_change_name() is not exported), so a
 * different name is refused: set "option ifname" in the wifi-iface (the package's defaults do).
 */
static struct wireless_dev *wl_cfg_add_virtual_intf(struct wiphy *wiphy, const char *name,
						    unsigned char name_assign_type,
						    enum nl80211_iftype type,
						    struct vif_params *params)
{
	struct wl_cfg *cfg = wl_cfg_of(wiphy);
	struct net_device *dev = cfg->wdev.netdev;

	ASSERT_RTNL();
	if (type != NL80211_IFTYPE_AP)
		return ERR_PTR(-EOPNOTSUPP);	/* station mode: D4 */
	if (cfg->vif_in_use)
		return ERR_PTR(-EBUSY);		/* no multiple SSIDs (yet) */
	if (strcmp(dev->name, name)) {
		netdev_warn(dev, "cannot create '%s': only '%s' exists; set option ifname '%s'\n",
			    name, dev->name, dev->name);
		return ERR_PTR(-EOPNOTSUPP);
	}
	cfg->vif_in_use = true;
	cfg->wdev.iftype = type;
	return &cfg->wdev;
}

static int wl_cfg_del_virtual_intf(struct wiphy *wiphy, struct wireless_dev *wdev)
{
	struct wl_cfg *cfg = wl_cfg_of(wiphy);

	ASSERT_RTNL();
	if (wdev != &cfg->wdev)
		return -EINVAL;
	if (netif_running(wdev->netdev))
		dev_close(wdev->netdev);
	cfg->vif_in_use = false;
	return 0;
}

static int wl_cfg_change_virtual_intf(struct wiphy *wiphy, struct net_device *ndev,
				      enum nl80211_iftype type, struct vif_params *params)
{
	/* AP only for now; station mode comes in D4 */
	return type == NL80211_IFTYPE_AP ? 0 : -EOPNOTSUPP;
}

static const struct cfg80211_ops wl_cfg_ops = {
	.add_virtual_intf	= wl_cfg_add_virtual_intf,
	.del_virtual_intf	= wl_cfg_del_virtual_intf,
	.change_virtual_intf	= wl_cfg_change_virtual_intf,
	.get_channel		= wl_cfg_get_channel,
	.get_tx_power		= wl_cfg_get_tx_power,
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
	.connect		= wl_cfg_connect,
	.disconnect		= wl_cfg_disconnect,
};

/* ------------------------------------------------------------------ notifications from the glue */

void wl_cfg80211_ssid_changed(struct wl_cfg *cfg, const u8 *ssid, int len)
{
	if (!cfg || len < 0 || len > IEEE80211_MAX_SSID_LEN)
		return;
	mutex_lock(&cfg->wdev.mtx);
	memcpy(cfg->wdev.ssid, ssid, len);
	cfg->wdev.ssid_len = len;
	mutex_unlock(&cfg->wdev.mtx);
}

void wl_cfg80211_sta_assoc(struct wl_cfg *cfg, const u8 *mac, const u8 *ies, int ies_len)
{
	struct station_info sinfo;

	if (!cfg || !cfg->wdev.netdev)
		return;
	memset(&sinfo, 0, sizeof(sinfo));
	if (ies_len > 0) {
		sinfo.assoc_req_ies = ies;
		sinfo.assoc_req_ies_len = ies_len;
	}
	netdev_info(cfg->wdev.netdev, "station %pM associated (%d IE bytes)\n", mac, ies_len);
	cfg80211_new_sta(cfg->wdev.netdev, mac, &sinfo, GFP_ATOMIC);
}

void wl_cfg80211_sta_left(struct wl_cfg *cfg, const u8 *mac)
{
	if (!cfg || !cfg->wdev.netdev)
		return;
	netdev_info(cfg->wdev.netdev, "station %pM left\n", mac);
	cfg80211_del_sta(cfg->wdev.netdev, mac, GFP_ATOMIC);
}

/* ------------------------------------------------------------------ attach / detach */

static void wl_cfg_setup_band(struct wl_cfg *cfg, struct ieee80211_channel *chans)
{
	u32 valid[32];
	int i, j, n = wl_cfg_valid_channels(cfg->wl, valid, ARRAY_SIZE(valid));

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
	int err;

	/* channels live behind the wl_cfg in the same allocation */
	wiphy = wiphy_new(&wl_cfg_ops, sizeof(struct wl_cfg) + sizeof(wl_cfg_channels_2g));
	if (!wiphy)
		return NULL;
	cfg = wiphy_priv(wiphy);
	cfg->wl = wl;
	chans = (struct ieee80211_channel *)(cfg + 1);
	wl_cfg_setup_band(cfg, chans);

	set_wiphy_dev(wiphy, parent);
	memcpy(wiphy->perm_addr, dev->dev_addr, ETH_ALEN);
	wiphy->interface_modes = BIT(NL80211_IFTYPE_AP);
	wiphy->bands[NL80211_BAND_2GHZ] = &cfg->band;
	wiphy->cipher_suites = wl_cfg_ciphers;
	wiphy->n_cipher_suites = ARRAY_SIZE(wl_cfg_ciphers);
	wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
	wiphy->max_scan_ssids = 1;
	wiphy->rts_threshold = 2347;
	wiphy->frag_threshold = 2346;
	wiphy->regulatory_flags = REGULATORY_CUSTOM_REG | REGULATORY_DISABLE_BEACON_HINTS;
	/* association and probe responses happen in the core; hostapd only does WPA and keys */
	wiphy->flags |= WIPHY_FLAG_HAVE_AP_SME;
	wiphy->probe_resp_offload = NL80211_PROBE_RESP_OFFLOAD_SUPPORT_WPS |
				    NL80211_PROBE_RESP_OFFLOAD_SUPPORT_WPS2;
	wiphy->features |= NL80211_FEATURE_INACTIVITY_TIMER;	/* scb_activity_time */
	wiphy->mgmt_stypes = wl_cfg_mgmt_stypes;
	wiphy_apply_custom_regulatory(wiphy, &wl_cfg_regd);

	err = wiphy_register(wiphy);
	if (err) {
		pr_err("%s: wiphy_register failed: %d\n", dev->name, err);
		wiphy_free(wiphy);
		return NULL;
	}
	cfg->wdev.wiphy = wiphy;
	cfg->wdev.iftype = NL80211_IFTYPE_AP;
	cfg->wdev.netdev = dev;
	dev->ieee80211_ptr = &cfg->wdev;	/* before register_netdev: cfg80211 picks it up there */
	return cfg;
}

const char *wl_cfg80211_name(struct wl_cfg *cfg)
{
	return wiphy_name(cfg->wdev.wiphy);
}

/* after unregister_netdev */
void wl_cfg80211_detach(struct wl_cfg *cfg)
{
	struct wiphy *wiphy;

	if (!cfg)
		return;
	wiphy = cfg->wdev.wiphy;
	wiphy_unregister(wiphy);
	wiphy_free(wiphy);
}
