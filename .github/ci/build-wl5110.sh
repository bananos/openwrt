#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Andrew Druchenko
# Compile the broadcom-wl5110 packages with the official OpenWrt 19.07.10 brcm47xx/mips74k SDK.
#
#   [DL=<download dir>] .github/ci/build-wl5110.sh <workdir> [sdk.tar.xz]
#
# Downloads the SDK unless a tarball is given, and the ASUS firmware through the package's own
# download step. Builds twice: the default (wlc/nas) packages, then kmod-brcm-wl5110 with the
# cfg80211 option, which also builds OpenWrt's mac80211 package (backports) from this tree. It only
# checks that the ipks are produced and then deletes them: the packages contain the Broadcom core
# and must never be published (no CI artifacts).
set -eu

SDK_URL=https://downloads.openwrt.org/releases/19.07.10/targets/brcm47xx/mips74k/openwrt-sdk-19.07.10-brcm47xx-mips74k_gcc-7.5.0_musl.Linux-x86_64.tar.xz
SDK_SHA256=c79e7a6afb780ff7214ed6d514b57c1fcc56a35065fb193931918b16b388e578

TREE=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$1; SDK_TAR=${2:-}
mkdir -p "$WORK"; cd "$WORK"

if [ -z "$SDK_TAR" ]; then
	SDK_TAR=$PWD/sdk.tar.xz
	[ -f "$SDK_TAR" ] || wget -q -O "$SDK_TAR" "$SDK_URL"
fi
echo "$SDK_SHA256  $SDK_TAR" | sha256sum -c -

rm -rf sdk; mkdir sdk
tar -C sdk --strip-components=1 -xJf "$SDK_TAR"
cd sdk
# optional: reuse an existing download directory (ASUS firmware, sasquatch, backports)
if [ -n "${DL:-}" ]; then rm -rf dl; ln -s "$DL" dl; fi

# the SDK's package scan does not follow symlinks, so copy the packages in
cp -r "$TREE/package/kernel/broadcom-wl5110" "$TREE/package/kernel/mac80211" package/
# host prerequisites (python2, ncurses) are only needed for menuconfig and other packages
mkdir -p staging_dir/host; touch staging_dir/host/.prereq-build

make defconfig >/dev/null
cat >> .config <<CFG
CONFIG_PACKAGE_kmod-brcm-wl5110=m
CONFIG_PACKAGE_brcmwl5110=m
CONFIG_PACKAGE_brcmwl5110-utils=m
CONFIG_BRCMWL5110_FIRMWARE=""
# CONFIG_BRCMWL5110_CFG80211 is not set
# its firmware packaging step needs Python 2, and it isn't needed here
# CONFIG_PACKAGE_kmod-b43 is not set
CFG
make defconfig >/dev/null

KMOD=bin/targets/brcm47xx/mips74k/packages/kmod-brcm-wl5110_*.ipk
rc=0
check() {
	for p in "$@"; do
		if [ -f "$p" ]; then ls -l "$p"; else echo "missing: $p"; rc=1; fi
	done
}

# 1. default: wlc + nas
make package/broadcom-wl5110/compile -j"$(nproc)" V=s
check $KMOD bin/packages/mipsel_74kc/base/brcmwl5110_*.ipk \
	bin/packages/mipsel_74kc/base/brcmwl5110-utils_*.ipk

# 2. cfg80211 (hostapd, netifd's mac80211 handler)
sed -i 's/^# CONFIG_BRCMWL5110_CFG80211 is not set$/CONFIG_BRCMWL5110_CFG80211=y/' .config
make defconfig >/dev/null
rm -f $KMOD
make package/broadcom-wl5110/compile -j"$(nproc)" V=s
check $KMOD
for p in $KMOD; do
	[ -f "$p" ] || continue
	tar -xzOf "$p" ./control.tar.gz | tar -xzO ./control | grep -q '^Depends:.*kmod-cfg80211' ||
		{ echo "cfg80211 build does not depend on kmod-cfg80211"; rc=1; }
	tar -xzOf "$p" ./data.tar.gz | tar -tz | grep -q 'uci-defaults/90-wl5110-cfg80211' ||
		{ echo "cfg80211 build lacks its uci-defaults"; rc=1; }
done

# never leave the built (Broadcom-containing) packages or the extracted core behind
rm -rf bin build_dir/target-*/broadcom-wl5110-*
exit $rc
