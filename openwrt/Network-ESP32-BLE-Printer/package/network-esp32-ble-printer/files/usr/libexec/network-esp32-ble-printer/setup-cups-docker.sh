#!/bin/sh

set -eu

CONFIG_NAME="network-esp32-ble-printer"
LIB_DIR="/usr/libexec/network-esp32-ble-printer"
LOGGER_TAG="Network-ESP32-BLE-Printer"

uci_get() {
	uci -q get "${CONFIG_NAME}.main.$1" 2>/dev/null || echo "${2:-}"
}

log() {
	logger -t "$LOGGER_TAG" -- "$1"
	echo "$1"
}

command_exists() {
	command -v "$1" >/dev/null 2>&1
}

ensure_dirs() {
	CUPS_ROOT="$(uci_get cups_data_root "/mnt/sda1/network-esp32-ble-printer/cups")"
	[ -n "$CUPS_ROOT" ] || CUPS_ROOT="/tmp/network-esp32-ble-printer/cups"
	mkdir -p "$CUPS_ROOT/backend" "$CUPS_ROOT/log"
	chmod 755 "$CUPS_ROOT" "$CUPS_ROOT/backend" "$CUPS_ROOT/log"
	echo "$CUPS_ROOT"
}

write_cupsd_conf() {
	CUPS_ROOT="$1"
	PUBLIC_PORT="$(uci_get public_ipp_port "631")"
	cat > "$CUPS_ROOT/cupsd.conf" <<EOF
LogLevel warn
MaxLogSize 0
Listen 0.0.0.0:${PUBLIC_PORT}
Listen /var/run/cups/cups.sock
Browsing On
BrowseLocalProtocols dnssd
DefaultShared Yes
WebInterface Yes
IdleExitTimeout 0
PreserveJobFiles No
PreserveJobHistory No
AutoPurgeJobs Yes
DefaultAuthType None
<Location />
  Order allow,deny
  Allow all
</Location>
<Location /admin>
  Order allow,deny
  Allow all
</Location>
<Location /admin/conf>
  AuthType None
  Require user @SYSTEM
  Order allow,deny
  Allow all
</Location>
<Policy default>
  JobPrivateAccess all
  JobPrivateValues none
  SubscriptionPrivateAccess all
  SubscriptionPrivateValues none
  <Limit All>
    Order allow,deny
    Allow all
  </Limit>
</Policy>
EOF
}

apply_container() {
	CUPS_ENABLED="$(uci_get cups_enabled "1")"
	[ "$CUPS_ENABLED" = "1" ] || exit 0

	if ! command_exists docker; then
		log "docker command not found; skipping CUPS frontend setup"
		exit 0
	fi

	CUPS_ROOT="$(ensure_dirs)"
	CONTAINER_NAME="$(uci_get cups_container_name "mervyns-cups")"
	IMAGE="$(uci_get cups_image "olbat/cupsd:latest")"
	BRIDGE_PORT="$(uci_get bridge_port "8631")"
	PRINTER_NAME="$(uci_get printer_name "Mervyns Label Printer")"
	BACKEND_TIMEOUT="$(uci_get backend_timeout_sec "8")"
	KEEP_FAILED_JOBS="$(uci_get keep_failed_jobs "1")"
	PURGE_COMPLETED_JOBS="$(uci_get purge_completed_jobs "1")"
	PURGE_COMPLETED_JOBS_DELAY_SEC="$(uci_get purge_completed_jobs_delay_sec "12")"
	BACKEND_TARGET="$CUPS_ROOT/backend/network-esp32-ble-printer"

	cp "$LIB_DIR/cups-backend.sh" "$BACKEND_TARGET"
	chmod 755 "$BACKEND_TARGET"
	write_cupsd_conf "$CUPS_ROOT"

	docker rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
	docker run -d \
		--name "$CONTAINER_NAME" \
		--restart unless-stopped \
		--network host \
		-e MERVYNS_BRIDGE_URL="http://127.0.0.1:${BRIDGE_PORT}/api/print/image" \
		-e MERVYNS_STATUS_URL="http://127.0.0.1:${BRIDGE_PORT}/api/printer/ready" \
		-e MERVYNS_CONTROL_URL="http://127.0.0.1:${BRIDGE_PORT}/api/job/control" \
		-e MERVYNS_PRINTER_NAME="$PRINTER_NAME" \
		-e MERVYNS_BACKEND_TIMEOUT="$BACKEND_TIMEOUT" \
		-e MERVYNS_KEEP_FAILED_JOBS="$KEEP_FAILED_JOBS" \
		-e MERVYNS_PURGE_COMPLETED_JOBS="$PURGE_COMPLETED_JOBS" \
		-e MERVYNS_PURGE_COMPLETED_JOBS_DELAY_SEC="$PURGE_COMPLETED_JOBS_DELAY_SEC" \
		-e MERVYNS_JOB_ARCHIVE_DIR="/mervyns-data/jobs" \
		-v /var/run/dbus:/var/run/dbus \
		-v "$CUPS_ROOT/cupsd.conf:/etc/cups/cupsd.conf" \
		-v "$CUPS_ROOT:/mervyns-data" \
		"$IMAGE" >/dev/null 2>&1 || {
			log "failed to start CUPS container ${CONTAINER_NAME} with image ${IMAGE}"
			docker logs "$CONTAINER_NAME" 2>/dev/null | tail -n 40 | while IFS= read -r line; do log "cups-log: $line"; done || true
			exit 0
		}

	sleep 6
	docker exec "$CONTAINER_NAME" sh -lc '
		set -eu
		BACKEND_DST="/usr/lib/cups/backend/network-esp32-ble-printer"
		cp /mervyns-data/backend/network-esp32-ble-printer "$BACKEND_DST"
		chmod 755 "$BACKEND_DST"
		lpadmin -x "Mervyns_Label_Printer" >/dev/null 2>&1 || true
		lpadmin -p "Mervyns_Label_Printer" -E \
			-v "network-esp32-ble-printer:/default" \
			-m raw \
			-D "${MERVYNS_PRINTER_NAME:-Mervyns Label Printer}" \
			-o printer-is-shared=true \
			-o job-sheets-default=none \
			-o printer-error-policy=abort-job \
			-o ipp-attribute-fidelity=false
		cupsenable "Mervyns_Label_Printer"
		cupsaccept "Mervyns_Label_Printer"
	' >/dev/null 2>&1 || log "failed to configure CUPS queue in container"

	log "CUPS docker frontend applied via ${CONTAINER_NAME}"
}

stop_container() {
	CONTAINER_NAME="$(uci_get cups_container_name "mervyns-cups")"
	if command_exists docker; then
		docker rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
	fi
}

case "${1:-apply}" in
	apply|start)
		apply_container
		;;
	stop)
		stop_container
		;;
	*)
		echo "usage: $0 [apply|start|stop]" >&2
		exit 1
		;;
esac
