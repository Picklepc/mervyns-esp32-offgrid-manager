include $(TOPDIR)/rules.mk

PKG_NAME:=network-esp32-ble-printer
PKG_VERSION:=0.2.0
PKG_RELEASE:=1

PKG_LICENSE:=MIT
PKG_MAINTAINER:=OpenAI Codex

include $(INCLUDE_DIR)/package.mk

define Package/network-esp32-ble-printer
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Network-ESP32-BLE-Printer bridge for Mervyns
  DEPENDS:=+python3 +python3-pillow +procd
endef

define Package/network-esp32-ble-printer/description
  OpenWrt image bridge for Mervyns. Polls ESP32 label stock metadata, accepts
  image jobs over HTTP, rasterizes them for the saved label stock, and streams
  the final monochrome job to the ESP32 BLE printer bridge.
endef

define Build/Compile
endef

define Package/network-esp32-ble-printer/install
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/etc/config/network-esp32-ble-printer $(1)/etc/config/network-esp32-ble-printer
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/etc/init.d/network-esp32-ble-printer $(1)/etc/init.d/network-esp32-ble-printer
	$(INSTALL_DIR) $(1)/usr/libexec/network-esp32-ble-printer
	$(INSTALL_BIN) ./files/usr/libexec/network-esp32-ble-printer/bridge.sh $(1)/usr/libexec/network-esp32-ble-printer/bridge.sh
	$(INSTALL_BIN) ./files/usr/libexec/network-esp32-ble-printer/bridge.py $(1)/usr/libexec/network-esp32-ble-printer/bridge.py
endef

$(eval $(call BuildPackage,network-esp32-ble-printer))
