#!/bin/sh

set -eu

TMP_ROOT="${1:-/tmp/network-esp32-ble-printer}"
SERVICE_NAME="network-esp32-ble-printer"
LIB_DIR="/usr/libexec/network-esp32-ble-printer"
CONFIG_PATH="/etc/config/network-esp32-ble-printer"
INIT_PATH="/etc/init.d/network-esp32-ble-printer"
SYSUPGRADE_CONF="/etc/sysupgrade.conf"
AVAHI_SERVICE_DIR="/etc/avahi/services"
AVAHI_SERVICE_PATH="$AVAHI_SERVICE_DIR/network-esp32-ble-printer.service"

echo "[install] using temp root: $TMP_ROOT"

mkdir -p "$LIB_DIR"

cp "$TMP_ROOT/files/etc/config/network-esp32-ble-printer" "$CONFIG_PATH"
cp "$TMP_ROOT/files/etc/init.d/network-esp32-ble-printer" "$INIT_PATH"
cp "$TMP_ROOT/files/usr/libexec/network-esp32-ble-printer/bridge.py" "$LIB_DIR/bridge.py"
cp "$TMP_ROOT/files/usr/libexec/network-esp32-ble-printer/bridge.sh" "$LIB_DIR/bridge.sh"
cp "$TMP_ROOT/files/usr/libexec/network-esp32-ble-printer/cups-backend.sh" "$LIB_DIR/cups-backend.sh"
cp "$TMP_ROOT/files/usr/libexec/network-esp32-ble-printer/setup-cups-docker.sh" "$LIB_DIR/setup-cups-docker.sh"

echo "[install] refreshing package deps (best effort)"
opkg update || echo "[install] warning: opkg update failed, continuing"
opkg install python3 python3-pillow avahi-dbus-daemon avahi-utils docker dockerd docker-compose luci-app-dockerman || echo "[install] warning: package install failed, continuing"

CURRENT_PORT="$(uci -q get network-esp32-ble-printer.main.listen_port 2>/dev/null || true)"
BRIDGE_PORT="$(uci -q get network-esp32-ble-printer.main.bridge_port 2>/dev/null || true)"
[ -n "$BRIDGE_PORT" ] || uci -q set network-esp32-ble-printer.main.bridge_port='8631'
PUBLIC_PORT="$(uci -q get network-esp32-ble-printer.main.public_ipp_port 2>/dev/null || true)"
[ -n "$PUBLIC_PORT" ] || uci -q set network-esp32-ble-printer.main.public_ipp_port='631'
CURRENT_IMAGE="$(uci -q get network-esp32-ble-printer.main.cups_image 2>/dev/null || true)"
if [ -z "$CURRENT_IMAGE" ] || [ "$CURRENT_IMAGE" = "ghcr.io/linuxserver/cupsd:latest" ]; then
	uci -q set network-esp32-ble-printer.main.cups_image='olbat/cupsd:latest'
fi
if [ -z "$CURRENT_PORT" ] || [ "$CURRENT_PORT" = "631" ]; then
	uci -q set network-esp32-ble-printer.main.listen_port='8631'
fi
uci -q set network-esp32-ble-printer.main.web_path='/'
uci -q set network-esp32-ble-printer.main.ipp_path='/ipp/print'
uci -q set network-esp32-ble-printer.main.advertise_service='1'
uci -q set network-esp32-ble-printer.main.cups_enabled='1'
[ -n "$(uci -q get network-esp32-ble-printer.main.printer_status_ttl_sec 2>/dev/null || true)" ] || uci -q set network-esp32-ble-printer.main.printer_status_ttl_sec='10'
[ -n "$(uci -q get network-esp32-ble-printer.main.printer_ready_stale_ms 2>/dev/null || true)" ] || uci -q set network-esp32-ble-printer.main.printer_ready_stale_ms='20000'
uci commit network-esp32-ble-printer

chmod 755 "$INIT_PATH"
chmod 755 "$LIB_DIR/bridge.py"
chmod 755 "$LIB_DIR/bridge.sh"
chmod 755 "$LIB_DIR/cups-backend.sh"
chmod 755 "$LIB_DIR/setup-cups-docker.sh"

BRIDGE_VERSION="$(grep -m1 '^SERVICE_VERSION = ' "$LIB_DIR/bridge.py" | cut -d'\"' -f2)"
echo "[install] bridge version: ${BRIDGE_VERSION:-unknown}"

touch "$SYSUPGRADE_CONF"
grep -qxF "$CONFIG_PATH" "$SYSUPGRADE_CONF" || echo "$CONFIG_PATH" >> "$SYSUPGRADE_CONF"
grep -qxF "$INIT_PATH" "$SYSUPGRADE_CONF" || echo "$INIT_PATH" >> "$SYSUPGRADE_CONF"
grep -qxF "$LIB_DIR/bridge.py" "$SYSUPGRADE_CONF" || echo "$LIB_DIR/bridge.py" >> "$SYSUPGRADE_CONF"
grep -qxF "$LIB_DIR/bridge.sh" "$SYSUPGRADE_CONF" || echo "$LIB_DIR/bridge.sh" >> "$SYSUPGRADE_CONF"
grep -qxF "$LIB_DIR/cups-backend.sh" "$SYSUPGRADE_CONF" || echo "$LIB_DIR/cups-backend.sh" >> "$SYSUPGRADE_CONF"
grep -qxF "$LIB_DIR/setup-cups-docker.sh" "$SYSUPGRADE_CONF" || echo "$LIB_DIR/setup-cups-docker.sh" >> "$SYSUPGRADE_CONF"

if [ "$(uci -q get network-esp32-ble-printer.main.advertise_service 2>/dev/null || echo 1)" = "1" ]; then
	mkdir -p "$AVAHI_SERVICE_DIR"
	PRINTER_NAME="$(uci -q get network-esp32-ble-printer.main.printer_name 2>/dev/null || echo 'Mervyns Label Printer')"
	LISTEN_PORT="$(uci -q get network-esp32-ble-printer.main.public_ipp_port 2>/dev/null || echo 631)"
	IPP_PATH="$(uci -q get network-esp32-ble-printer.main.ipp_path 2>/dev/null || echo /ipp/print)"
	HOSTNAME="$(uci -q get system.@system[0].hostname 2>/dev/null || echo megamind)"
	cat > "$AVAHI_SERVICE_PATH" <<EOF
<?xml version="1.0" standalone='no'?><!--*-nxml-*-->
<!DOCTYPE service-group SYSTEM "avahi-service.dtd">
<service-group>
  <name replace-wildcards="yes">$PRINTER_NAME</name>
  <service>
    <type>_ipp._tcp</type>
    <subtype>_universal._sub._ipp._tcp</subtype>
    <port>$LISTEN_PORT</port>
    <txt-record>txtvers=1</txt-record>
    <txt-record>qtotal=1</txt-record>
    <txt-record>rp=${IPP_PATH#/}</txt-record>
    <txt-record>ty=$PRINTER_NAME</txt-record>
    <txt-record>note=NESPi bridge to ESP32 BLE label printer</txt-record>
    <txt-record>product=(Network-ESP32-BLE-Printer)</txt-record>
    <txt-record>adminurl=http://$HOSTNAME:$LISTEN_PORT/</txt-record>
    <txt-record>priority=0</txt-record>
    <txt-record>Copies=F</txt-record>
    <txt-record>Color=F</txt-record>
    <txt-record>Duplex=F</txt-record>
    <txt-record>Transparent=T</txt-record>
    <txt-record>Binary=T</txt-record>
    <txt-record>printer-type=0x80104E</txt-record>
    <txt-record>printer-state=3</txt-record>
    <txt-record>pdl=application/pdf,image/urf,image/pwg-raster,image/png,image/jpeg,application/octet-stream</txt-record>
    <txt-record>URF=CP1,DM1,IS1,MT1-2-8-9-10-11,OB10,PQ3,RS384,SRGB24,W8</txt-record>
  </service>
  <service>
    <type>_printer._tcp</type>
    <port>0</port>
  </service>
</service-group>
EOF
	grep -qxF "$AVAHI_SERVICE_PATH" "$SYSUPGRADE_CONF" || echo "$AVAHI_SERVICE_PATH" >> "$SYSUPGRADE_CONF"
	/etc/init.d/dbus enable 2>/dev/null || true
	/etc/init.d/dbus start 2>/dev/null || true
	/etc/init.d/avahi-daemon enable 2>/dev/null || true
	/etc/init.d/avahi-daemon restart 2>/dev/null || true
fi

/etc/init.d/dockerd enable 2>/dev/null || true
/etc/init.d/dockerd start 2>/dev/null || true

/etc/init.d/"$SERVICE_NAME" enable
/etc/init.d/"$SERVICE_NAME" restart

echo "[install] completed"
echo "[install] bridge UI: http://$(uci -q get system.@system[0].hostname 2>/dev/null || echo openwrt):$(uci -q get network-esp32-ble-printer.main.bridge_port 2>/dev/null || echo 8631)/"
echo "[install] public printer port: $(uci -q get network-esp32-ble-printer.main.public_ipp_port 2>/dev/null || echo 631)"
