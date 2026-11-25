#!/bin/sh
# /lib/wifi/brcmwl.sh - legacy /sbin/wifi driver for the ported Broadcom wl 5.110.27 (ASUS RT-N12 D1).
# Configures the radio with wlc (GPL, from OpenWrt broadcom-wl, extended for 5.110); the stock wl is not needed.
# Driven by /etc/config/wireless; called by `wifi` and at boot by /etc/init.d/network (wifi reload_legacy).
#
#   config wifi-device 'radio0'          config wifi-iface 'default_radio0'
#       option type 'brcmwl'                 option device 'radio0'
#       option channel '6'                   option mode 'ap'          # only AP is supported
#                                            option network 'lan'      # bridged via netifd
#                                            option ssid '...'
#                                            option encryption 'psk2'  # none | psk2 | psk-mixed | psk
#                                            option key '...'          # 8..63 chars
#                                            option hidden '0'
# Only the first wifi-iface of the device is used (no virtual interfaces in this port yet).

append DRIVERS "brcmwl"

BRCMWL_WLC=/usr/sbin/wlc
BRCMWL_IF=wl0
BRCMWL_NASPID=/var/run/nas.$BRCMWL_IF.pid

brcmwl_log() { logger -t brcmwl "$*"; echo "brcmwl: $*"; }
brcmwl_wlc() { $BRCMWL_WLC ifname $BRCMWL_IF "$@"; }

scan_brcmwl() {
	return 0
}

brcmwl_in_ifname() {
	case " $(uci -q get network.$1.ifname) " in
		*" $BRCMWL_IF "*) return 0 ;;
	esac
	return 1
}

# wait (up to ~30 s) until netifd reports the network up, so it doesn't reset wl0 after we start the BSS
brcmwl_wait_network() {
	local n=0
	ubus -t 30 wait_for network.interface.$1 2>/dev/null || return 1
	while [ $n -lt 30 ]; do
		ubus call network.interface.$1 status 2>/dev/null | grep -q '"up": true' && return 0
		sleep 1; n=$((n + 1))
	done
	return 1
}

disable_brcmwl() {
	local device="$1" network vif
	[ -f "$BRCMWL_NASPID" ] && start-stop-daemon -K -q -p "$BRCMWL_NASPID" && rm -f "$BRCMWL_NASPID"
	[ -d /sys/class/net/$BRCMWL_IF ] || return 0
	brcmwl_wlc down >/dev/null 2>&1
	config_get vifs "$device" vifs
	for vif in $vifs; do
		config_get network "$vif" network
		[ -n "$network" ] || continue
		# netifd-managed membership (wl0 listed in network.<net>.ifname): leave it alone
		brcmwl_in_ifname "$network" && continue
		ubus call network.interface.$network remove_device "{\"name\":\"$BRCMWL_IF\"}" >/dev/null 2>&1
	done
	ip link set $BRCMWL_IF down
	return 0
}

enable_brcmwl() {
	local device="$1" channel vifs vif ssid enc key network mode hidden wsec auth restrict

	[ -d /sys/class/net/$BRCMWL_IF ] || { brcmwl_log "$BRCMWL_IF not present (wl.ko not loaded?)"; return 1; }
	config_get channel "$device" channel
	config_get vifs "$device" vifs
	# /sbin/wifi runs scan_wifi more than once, so names can repeat in $vifs: dedupe
	set -- $(for v in $vifs; do echo "$v"; done | awk '!seen[$0]++')
	vif="$1"
	[ -n "$vif" ] || { brcmwl_log "no wifi-iface for $device"; return 1; }
	[ $# -gt 1 ] && brcmwl_log "only the first wifi-iface ($vif) is used"

	config_get mode "$vif" mode
	config_get ssid "$vif" ssid
	config_get enc "$vif" encryption
	config_get key "$vif" key
	config_get network "$vif" network
	config_get_bool hidden "$vif" hidden 0
	config_get_bool vdisabled "$vif" disabled 0
	[ "$vdisabled" = 1 ] && { brcmwl_log "$vif disabled"; return 0; }
	[ "${mode:-ap}" = ap ] || { brcmwl_log "mode '$mode' not supported (only ap)"; return 1; }
	[ -n "$ssid" ] || { brcmwl_log "no ssid set"; return 1; }

	case "${enc:-none}" in
		none)		wsec=0; auth=0 ;;
		psk2*)		wsec=4; auth=128 ;;
		psk-mixed*)	wsec=6; auth=132 ;;
		psk*)		wsec=2; auth=4 ;;
		*)		brcmwl_log "encryption '$enc' not supported"; return 1 ;;
	esac
	if [ $wsec -gt 0 ]; then
		[ ${#key} -ge 8 ] && [ ${#key} -le 63 ] || {
			brcmwl_log "WPA key missing or not 8..63 chars: radio left down"
			return 1
		}
		restrict=1
	else
		restrict=0
	fi

	[ -n "$network" ] && { brcmwl_wait_network "$network" || brcmwl_log "network '$network' not up yet, continuing"; }

	brcmwl_wlc down
	brcmwl_wlc mpc 0 ap 1 infra 1
	[ -n "$channel" ] && [ "$channel" != auto ] && brcmwl_wlc channel "$channel"
	brcmwl_wlc closed "$hidden" wepauth 0 wsec $wsec wpa_auth $auth wsec_restrict $restrict eap_restrict 0
	brcmwl_wlc up
	ip link set $BRCMWL_IF up
	brcmwl_wlc ssid "$ssid" >/dev/null

	# At boot this runs before netifd has created the bridge; an add_device sent then is lost.
	# Preferred: list wl0 in network.<net>.ifname (installer does this for lan) so netifd adds it itself.
	# Fallback for other networks: wait for the netifd interface, then add the device.
	if [ -n "$network" ]; then
		if brcmwl_in_ifname "$network"; then
			# netifd owns membership; re-add only if it got dropped
			[ -e /sys/class/net/br-$network/brif/$BRCMWL_IF ] || \
				ubus call network.interface.$network add_device "{\"name\":\"$BRCMWL_IF\"}"
		else
			ubus call network.interface.$network add_device "{\"name\":\"$BRCMWL_IF\"}"
		fi
	fi

	if [ $wsec -gt 0 ]; then
		start-stop-daemon -S -b -p "$BRCMWL_NASPID" -x /usr/sbin/nas -- -P "$BRCMWL_NASPID" -H 34954 \
			-i $BRCMWL_IF -A -m $auth -w $wsec -s "$ssid" -g 3600 -F -k "$key"
	fi
	set_wifi_up "$vif" "$BRCMWL_IF"
	brcmwl_log "$BRCMWL_IF up: ssid '$ssid' channel ${channel:-default} enc ${enc:-none} network ${network:-none}"
}

detect_brcmwl() {
	[ -d /sys/class/net/$BRCMWL_IF ] || return 0
	config_get type radio0 type
	[ -n "$type" ] && return 0
	cat <<EOF
config wifi-device 'radio0'
	option type 'brcmwl'
	option channel '6'
	option disabled '1'

config wifi-iface 'default_radio0'
	option device 'radio0'
	option network 'lan'
	option mode 'ap'
	option ssid 'OpenWrt'
	option encryption 'psk2'

EOF
}
