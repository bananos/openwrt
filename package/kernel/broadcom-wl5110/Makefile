#
# Broadcom wl 5.110.27.20012 for the ASUS RT-N12 D1 (BCM5357C0) on OpenWrt 19.07 / Linux 4.14.
#
# The closed wlc core is NOT part of this package: it is cut out of ASUS's own firmware download at
# build time (scripts/extract.sh), like b43-fwcutter does for b43 firmware. Everything stored here is
# ISC (Broadcom shared code and headers), GPL-2.0 (wl_glue, scripts) or new code from this port.
# Never distribute the resulting ipks or images: they contain the Broadcom core.
#
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=broadcom-wl5110
PKG_VERSION:=5.110.27.20012
PKG_RELEASE:=1
PKG_LICENSE:=Proprietary (core, extracted at build time) ISC GPL-2.0
PKG_MAINTAINER:=

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
  DEPENDS:=@TARGET_brcm47xx_mips74k @!PACKAGE_kmod-brcm-wl
  FILES:=$(PKG_BUILD_DIR)/wl_glue/wl_glue.ko $(PKG_BUILD_DIR)/wl/wl.ko
  AUTOLOAD:=$(call AutoProbe,wl_glue wl)
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
  endif
endef

define Package/brcmwl5110-utils
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=WirelessAPPS
  TITLE:=Stock Broadcom wl tool for the 5.110 driver (with its uClibc)
  DEPENDS:=+kmod-brcm-wl5110
endef

define Package/brcmwl5110-utils/description
 The stock ASUS/Broadcom 'wl' utility and the three uClibc libraries it needs,
 extracted from the same firmware image. Installed under /usr/lib/brcm-wl (the
 loader as /lib/ld-uClibc.so.0) and run through the /usr/sbin/wl wrapper.
endef

define Package/brcmwl5110
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=WirelessAPPS
  TITLE:=/sbin/wifi support for the Broadcom 5.110 driver (type brcmwl)
  DEPENDS:=+kmod-brcm-wl5110 +brcmwl5110-utils +nas
  PKGARCH:=all
endef

define Package/brcmwl5110/description
 Lets /etc/config/wireless (wifi-device type 'brcmwl') drive the 5.110 driver:
 AP mode, SSID, channel, hidden SSID, open/WPA2-PSK/mixed via nas, bridged into
 a netifd network.
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

define Build/Compile
	+$(KERNEL_MAKE) M=$(PKG_BUILD_DIR)/wl_glue modules
	+$(KERNEL_MAKE) M=$(PKG_BUILD_DIR)/wl \
		KBUILD_EXTRA_SYMBOLS=$(PKG_BUILD_DIR)/wl_glue/Module.symvers modules
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
	$(INSTALL_DIR) $(1)/lib/wifi $(1)/etc/uci-defaults
	$(INSTALL_DATA) ./files/brcmwl.sh $(1)/lib/wifi/brcmwl.sh
	$(INSTALL_BIN) ./files/brcmwl.uci-defaults $(1)/etc/uci-defaults/90-brcmwl
endef

$(eval $(call KernelPackage,brcm-wl5110))
$(eval $(call BuildPackage,brcmwl5110-utils))
$(eval $(call BuildPackage,brcmwl5110))
