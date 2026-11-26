#
# Broadcom wl 5.110.27.20012 for the ASUS RT-N12 D1 (BCM5357C0) on OpenWrt 19.07 / Linux 4.14.
#
# The closed wlc core is NOT part of this package: it is cut out of ASUS's own firmware download at
# build time (scripts/extract.sh), like b43-fwcutter does for b43 firmware. Everything stored here is
# ISC (Broadcom shared code and headers), GPL-2.0 (wl_glue, scripts) or new code from this port.
# Never distribute the resulting ipks or images: they contain the Broadcom core.
#
# Copyright (c) 2026 Andrew Druchenko
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=broadcom-wl5110
PKG_VERSION:=5.110.27.20012
PKG_RELEASE:=1
PKG_LICENSE:=Proprietary (core, extracted at build time) ISC GPL-2.0
PKG_MAINTAINER:=
PKG_CONFIG_DEPENDS:=CONFIG_BRCMWL5110_CFG80211

BRCMWL5110_FW:=$(call qstrip,$(CONFIG_BRCMWL5110_FIRMWARE))
ifeq ($(BRCMWL5110_FW),)
  # official ASUS download, 3.0.0.4.374.813 (2013-09-27); its wl.ko .text is identical to 374_168
  PKG_SOURCE:=FW_RT_N12_D1_3004374813.zip
  PKG_SOURCE_URL:=https://dlcdnets.asus.com/pub/ASUS/wireless/RT-N12_D1/
  PKG_HASH:=15884a068204a9080d80578967928d593bd3d32a452595eecbebf26e3587c764
  BRCMWL5110_FW:=$(DL_DIR)/$(PKG_SOURCE)
endif

# LZMA SquashFS extractor for the ASUS root filesystem (GPL-2.0); built for the host in Build/Prepare
SASQUATCH_VER:=b34aa5ef67068a2137ac04e2272915bd45388260
SASQUATCH_FILE:=sasquatch-$(SASQUATCH_VER).tar.xz

include $(INCLUDE_DIR)/package.mk

# keep the extracted stock wl/uClibc binaries byte-identical (no sstrip); kernel modules are still
# stripped, by STRIP_KMOD
STRIP:=:

define Download/sasquatch
  FILE:=$(SASQUATCH_FILE)
  URL:=https://github.com/onekey-sec/sasquatch.git
  PROTO:=git
  VERSION:=$(SASQUATCH_VER)
  SUBDIR:=sasquatch-$(SASQUATCH_VER)
  MIRROR_HASH:=b2eb4f2309360461152457ac95ab67c2e8885f3e7b827dffa69be24bf156c37d
endef
$(eval $(call Download,sasquatch))

define KernelPackage/brcm-wl5110
  SUBMENU:=Wireless Drivers
  TITLE:=Broadcom wl 5.110 driver for BCM5357 (ASUS RT-N12 D1)
  DEPENDS:=@TARGET_brcm47xx_mips74k @!PACKAGE_kmod-brcm-wl +BRCMWL5110_CFG80211:kmod-cfg80211
  FILES:=$(PKG_BUILD_DIR)/wl_glue/wl_glue.ko $(PKG_BUILD_DIR)/wl/wl.ko
  # cfg80211 first: backports leaves no Module.symvers, so wl.ko's modinfo can't name it as a dependency
  AUTOLOAD:=$(call AutoProbe,$(if $(CONFIG_BRCMWL5110_CFG80211),cfg80211) wl_glue wl)
endef

define KernelPackage/brcm-wl5110/install
	$(if $(CONFIG_BRCMWL5110_CFG80211), \
		$(INSTALL_DIR) $(1)/etc/uci-defaults && \
		$(INSTALL_BIN) ./files/wl5110-cfg80211.uci-defaults $(1)/etc/uci-defaults/90-wl5110-cfg80211)
endef

define KernelPackage/brcm-wl5110/description
 The proprietary Broadcom wl 5.110.27.20012 wireless core from ASUS RT-N12 D1
 firmware, unlinked from its Linux 2.6.22 glue and re-hosted on Linux 4.14 with
 new glue. The core is extracted from the ASUS firmware at build time.
 AP mode only (one SSID; open, WPA2-PSK, WPA/WPA2-PSK mixed).
endef

define KernelPackage/brcm-wl5110/config
  if PACKAGE_kmod-brcm-wl5110
	config BRCMWL5110_FIRMWARE
		string "Use this ASUS firmware file instead of downloading it"
		default ""
		help
		  Path to an RT-N12 D1 firmware image to extract the wl core from:
		  the ASUS .zip or .trx, or a dump of your router's flash (mtd1 or the
		  whole 8 MB). Leave empty to download ASUS 3.0.0.4.374.813.

	config BRCMWL5110_CFG80211
		bool "Register the radio with cfg80211 (experimental)"
		default n
		help
		  Work in progress: the radio becomes a normal OpenWrt "mac80211"
		  wifi-device on interface wlan0, run by netifd and OpenWrt's own
		  hostapd (wpad), and shown in LuCI. The brcmwl5110 package (wlc,
		  nas) is then not needed. Pulls in kmod-cfg80211 and iw.
  endif
endef

define Package/brcmwl5110-utils
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=WirelessAPPS
  TITLE:=Stock Broadcom wl tool for the 5.110 driver (optional diagnostics)
  DEPENDS:=+kmod-brcm-wl5110
endef

define Package/brcmwl5110-utils/description
 Optional. The stock ASUS/Broadcom 'wl' utility (counters, sta_info, scan and
 other diagnostics) and the three uClibc libraries it needs,
 extracted from the same firmware image. Installed under /usr/lib/brcm-wl (the
 loader as /lib/ld-uClibc.so.0) and run through the /usr/sbin/wl wrapper.
endef

define Package/brcmwl5110
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=WirelessAPPS
  TITLE:=/sbin/wifi support for the Broadcom 5.110 driver (type brcmwl), with wlc
  DEPENDS:=+kmod-brcm-wl5110 +nas
  CONFLICTS:=wlc
endef

define Package/brcmwl5110/description
 Lets /etc/config/wireless (wifi-device type 'brcmwl') drive the 5.110 driver:
 AP mode, SSID, channel, hidden SSID, open/WPA2-PSK/mixed via nas, bridged into
 a netifd network. Includes wlc (GPL, from OpenWrt's broadcom-wl, extended for
 5.110 with mpc/isup/rate/ver/autho_sta_list), so no ASUS userspace is needed.
endef

define Package/brcmwl5110/conffiles
/etc/config/wireless
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
	$(TAR) -C $(PKG_BUILD_DIR) -xJf $(DL_DIR)/$(SASQUATCH_FILE)
	+$(MAKE) -C $(PKG_BUILD_DIR)/sasquatch-$(SASQUATCH_VER)/squashfs-tools \
		CC="$(HOSTCC)" CXX="$(HOSTCXX)" \
		XZ_SUPPORT=0 LZO_SUPPORT=0 LZ4_SUPPORT=0 ZSTD_SUPPORT=0 XATTR_SUPPORT=0 \
		sasquatch
	sh ./scripts/extract.sh "$(BRCMWL5110_FW)" $(PKG_BUILD_DIR)/blob \
		$(PKG_BUILD_DIR)/sasquatch-$(SASQUATCH_VER)/squashfs-tools/sasquatch \
		$(TARGET_CROSS)objcopy
	$(CP) $(PKG_BUILD_DIR)/blob/wlc_core.o_shipped $(PKG_BUILD_DIR)/wl/
endef

ifdef CONFIG_BRCMWL5110_CFG80211
  # build against OpenWrt's cfg80211 (mac80211 backports), not the kernel's own headers
  WL_CFG80211_MAKE:=WL_CFG80211=y \
	NOSTDINC_FLAGS="-I$(STAGING_DIR)/usr/include/mac80211-backport/uapi \
		-I$(STAGING_DIR)/usr/include/mac80211-backport \
		-I$(STAGING_DIR)/usr/include/mac80211/uapi \
		-I$(STAGING_DIR)/usr/include/mac80211 \
		-include backport/autoconf.h -include backport/backport.h" \
	KBUILD_EXTRA_SYMBOLS="$(PKG_BUILD_DIR)/wl_glue/Module.symvers \
		$(wildcard $(KERNEL_BUILD_DIR)/backports-*/Module.symvers)"
else
  WL_CFG80211_MAKE:=KBUILD_EXTRA_SYMBOLS=$(PKG_BUILD_DIR)/wl_glue/Module.symvers
endif

define Build/Compile
	+$(KERNEL_MAKE) M=$(PKG_BUILD_DIR)/wl_glue modules
	+$(KERNEL_MAKE) M=$(PKG_BUILD_DIR)/wl $(WL_CFG80211_MAKE) modules
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wno-pointer-sign \
		-I$(PKG_BUILD_DIR)/wlc -I$(PKG_BUILD_DIR)/wl/include \
		-o $(PKG_BUILD_DIR)/wlc/wlc $(PKG_BUILD_DIR)/wlc/wlc.c $(PKG_BUILD_DIR)/wlc/wlutils.c
endef

define Package/brcmwl5110-utils/install
	$(INSTALL_DIR) $(1)/usr/lib/brcm-wl $(1)/usr/sbin $(1)/lib
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/blob/usr/wl $(1)/usr/lib/brcm-wl/wl
	# the stock wl's ELF interpreter is hard-coded as /lib/ld-uClibc.so.0 (no clash with musl's ld-musl)
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/blob/usr/ld-uClibc.so.0 $(1)/lib/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/blob/usr/libc.so.0 $(1)/usr/lib/brcm-wl/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/blob/usr/libgcc_s.so.1 $(1)/usr/lib/brcm-wl/
	$(INSTALL_BIN) ./files/wl.wrapper $(1)/usr/sbin/wl
endef

define Package/brcmwl5110/install
	$(INSTALL_DIR) $(1)/lib/wifi $(1)/etc/uci-defaults $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/wlc/wlc $(1)/usr/sbin/wlc
	$(INSTALL_DATA) ./files/brcmwl.sh $(1)/lib/wifi/brcmwl.sh
	# the cfg80211 build is set up by its own defaults (type mac80211, wlan0); keep these for wl0 only
	$(if $(CONFIG_BRCMWL5110_CFG80211),,$(INSTALL_BIN) ./files/brcmwl.uci-defaults $(1)/etc/uci-defaults/90-brcmwl)
endef

$(eval $(call KernelPackage,brcm-wl5110))
$(eval $(call BuildPackage,brcmwl5110-utils))
$(eval $(call BuildPackage,brcmwl5110))
