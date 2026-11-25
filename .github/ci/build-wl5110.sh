#!/bin/sh
# Compile the broadcom-wl5110 packages with the official OpenWrt 19.07.10 brcm47xx/mips74k SDK.
#
#   .github/ci/build-wl5110.sh <workdir> [sdk.tar.xz]
#
# Downloads the SDK unless a tarball is given, and the ASUS firmware through the package's own
# download step. It only checks that the three ipks are produced and then deletes them: the
# packages contain the Broadcom core and must never be published (no CI artifacts).
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

# the SDK's package scan does not follow symlinks, so copy the package in
cp -r "$TREE/package/kernel/broadcom-wl5110" package/
# host prerequisites (python2, ncurses) are only needed for menuconfig and other packages
mkdir -p staging_dir/host; touch staging_dir/host/.prereq-build

make defconfig >/dev/null
cat >> .config <<CFG
CONFIG_PACKAGE_kmod-brcm-wl5110=m
CONFIG_PACKAGE_brcmwl5110=m
CONFIG_PACKAGE_brcmwl5110-utils=m
CONFIG_BRCMWL5110_FIRMWARE=""
CFG
make defconfig >/dev/null

make package/broadcom-wl5110/compile -j"$(nproc)" V=s

rc=0
for p in bin/targets/brcm47xx/mips74k/packages/kmod-brcm-wl5110_*.ipk \
	 bin/packages/mipsel_74kc/base/brcmwl5110_*.ipk \
	 bin/packages/mipsel_74kc/base/brcmwl5110-utils_*.ipk; do
	if [ -f "$p" ]; then ls -l "$p"; else echo "missing: $p"; rc=1; fi
done

# never leave the built (Broadcom-containing) packages or the extracted core behind
rm -rf bin build_dir/target-*/broadcom-wl5110-*
exit $rc
