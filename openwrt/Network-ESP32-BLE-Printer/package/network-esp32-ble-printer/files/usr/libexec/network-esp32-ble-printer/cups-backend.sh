#!/bin/sh

set -eu

PRINTER_NAME="${MERVYNS_PRINTER_NAME:-Mervyns Label Printer}"
QUEUE_NAME="${MERVYNS_QUEUE_NAME:-Mervyns_Label_Printer}"
BRIDGE_URL="${MERVYNS_BRIDGE_URL:-http://127.0.0.1:8631/api/print/image}"
STATUS_URL="${MERVYNS_STATUS_URL:-http://127.0.0.1:8631/api/printer/ready}"
CONTROL_URL="${MERVYNS_CONTROL_URL:-http://127.0.0.1:8631/api/job/control}"
BACKEND_TIMEOUT="${MERVYNS_BACKEND_TIMEOUT:-8}"
JOB_ARCHIVE_DIR="${MERVYNS_JOB_ARCHIVE_DIR:-/tmp/mervyns-cups-jobs}"
KEEP_FAILED_JOBS="${MERVYNS_KEEP_FAILED_JOBS:-1}"
PURGE_COMPLETED_JOBS="${MERVYNS_PURGE_COMPLETED_JOBS:-1}"
PURGE_COMPLETED_JOBS_DELAY_SEC="${MERVYNS_PURGE_COMPLETED_JOBS_DELAY_SEC:-12}"
if [ $# -eq 0 ]; then
	echo "network-esp32-ble-printer:/default \"$PRINTER_NAME\" \"Mervyns NESPi backend\" \"ESP32 BLE label printer bridge\""
	exit 0
fi

JOB_ID="${1:-0}"
USER_NAME="${2:-unknown}"
JOB_TITLE="${3:-Untitled}"
COPIES="${4:-1}"
OPTIONS="${5:-}"
FILE_PATH="${6:-}"

TMP_FILE="/tmp/mervyns-cups-job-${JOB_ID}.bin"
WORK_FILE="$TMP_FILE"
RESPONSE_FILE="/tmp/mervyns-cups-response-${JOB_ID}.json"
trap 'rm -f "$TMP_FILE" "$RESPONSE_FILE"' EXIT
mkdir -p "$JOB_ARCHIVE_DIR"

on_cancel() {
	job_status "INFO: cancel requested for job ${JOB_ID}"
	post_control_action "cancel" 0
	echo "ERROR: job ${JOB_ID} canceled before completion" >&2
	exit 1
}

trap on_cancel INT TERM

if [ -n "$FILE_PATH" ] && [ -f "$FILE_PATH" ]; then
	cp "$FILE_PATH" "$TMP_FILE"
else
	cat > "$TMP_FILE"
fi

CONTENT_TYPE="application/octet-stream"
case "$OPTIONS" in
	*document-format=image/jpeg*) CONTENT_TYPE="image/jpeg" ;;
	*document-format=image/png*) CONTENT_TYPE="image/png" ;;
	*document-format=application/pdf*) CONTENT_TYPE="application/pdf" ;;
	*document-format=image/urf*) CONTENT_TYPE="application/octet-stream" ;;
	*document-format=image/pwg-raster*) CONTENT_TYPE="application/octet-stream" ;;
esac

detect_magic() {
	if dd if="$1" bs=8 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n' | grep -qi '^89504e470d0a1a0a'; then
		echo "image/png"
		return
	fi
	if dd if="$1" bs=3 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n' | grep -qi '^ffd8ff'; then
		echo "image/jpeg"
		return
	fi
	if dd if="$1" bs=5 count=1 2>/dev/null | tr -d '\000' | grep -q '^%PDF-'; then
		echo "application/pdf"
		return
	fi
	echo "application/octet-stream"
}

log_note() {
	echo "INFO: $*" >&2
}

job_status() {
	echo "$*" >&2
}

archive_job() {
	SUFFIX="$1"
	DEST="${JOB_ARCHIVE_DIR}/job-${JOB_ID}-${SUFFIX}.bin"
	cp "$TMP_FILE" "$DEST" 2>/dev/null || true
	echo "$DEST"
}

post_control_action() {
	ACTION="$1"
	DELAY_SEC="$2"
	BODY="{\"jobId\":\"${JOB_ID}\",\"action\":\"${ACTION}\",\"delaySec\":${DELAY_SEC}}"
	if command -v curl >/dev/null 2>&1; then
		curl -sS -o /dev/null \
			--connect-timeout 2 \
			--max-time 4 \
			-X POST \
			-H "Content-Type: application/json" \
			--data "$BODY" \
			"$CONTROL_URL" >/dev/null 2>&1 || true
	elif command -v wget >/dev/null 2>&1; then
		TMP_JSON="/tmp/mervyns-cups-control-${JOB_ID}.json"
		printf '%s' "$BODY" > "$TMP_JSON"
		wget -qO /dev/null \
			-T 4 \
			--header="Content-Type: application/json" \
			--post-file="$TMP_JSON" \
			"$CONTROL_URL" >/dev/null 2>&1 || true
		rm -f "$TMP_JSON" >/dev/null 2>&1 || true
	fi
}

check_printer_ready() {
	STATUS_CODE="000"
	if command -v curl >/dev/null 2>&1; then
		STATUS_CODE="$(curl -sS -o "$RESPONSE_FILE" -w '%{http_code}' --connect-timeout 2 --max-time 4 "$STATUS_URL" || true)"
	elif command -v wget >/dev/null 2>&1; then
		if wget -qO "$RESPONSE_FILE" -T 4 "$STATUS_URL"; then
			STATUS_CODE="200"
		fi
	fi

	if [ "${STATUS_CODE:-000}" != "200" ]; then
		job_status "STATE: +offline-report"
		echo "ERROR: printer unavailable before job ${JOB_ID}" >&2
		[ -f "$RESPONSE_FILE" ] && cat "$RESPONSE_FILE" >&2 || true
		exit 1
	fi
	job_status "STATE: -offline-report"
}

if [ "$CONTENT_TYPE" = "application/octet-stream" ]; then
	CONTENT_TYPE="$(detect_magic "$TMP_FILE")"
fi

if [ "$CONTENT_TYPE" = "application/pdf" ]; then
	PDF_RENDER="/tmp/mervyns-cups-job-${JOB_ID}.png"
	if command -v pdftoppm >/dev/null 2>&1; then
		pdftoppm -png -singlefile -f 1 -r 203 "$TMP_FILE" "/tmp/mervyns-cups-job-${JOB_ID}" >/dev/null 2>&1 || true
	elif command -v gs >/dev/null 2>&1; then
		gs -q -dSAFER -dBATCH -dNOPAUSE -sDEVICE=pnggray -r203 -dFirstPage=1 -dLastPage=1 -sOutputFile="$PDF_RENDER" "$TMP_FILE" >/dev/null 2>&1 || true
	fi
	if [ -f "$PDF_RENDER" ]; then
		WORK_FILE="$PDF_RENDER"
		CONTENT_TYPE="image/png"
		log_note "converted PDF job ${JOB_ID} to PNG before bridge send"
	else
		echo "ERROR: PDF job ${JOB_ID} could not be converted; pdftoppm/gs unavailable or failed" >&2
		exit 1
	fi
fi

if [ "$CONTENT_TYPE" = "application/octet-stream" ]; then
	if [ "$KEEP_FAILED_JOBS" = "1" ]; then
		SAVED="$(archive_job "unknown-format")"
		log_note "saved unsupported job ${JOB_ID} to ${SAVED}"
	fi
	echo "ERROR: unsupported job format for ${JOB_ID}; options=${OPTIONS}" >&2
	exit 1
fi

log_note "job ${JOB_ID} title='${JOB_TITLE}' content-type=${CONTENT_TYPE} copies=${COPIES}"
log_note "job ${JOB_ID} options='${OPTIONS}'"
check_printer_ready

if command -v curl >/dev/null 2>&1; then
	HTTP_CODE="$(curl -sS -o "${RESPONSE_FILE}" -w '%{http_code}' \
		--connect-timeout 3 \
		--max-time "${BACKEND_TIMEOUT}" \
		-X POST \
		-H "Content-Type: ${CONTENT_TYPE}" \
		-H "X-Filename: ${JOB_TITLE}" \
		--data-binary "@${WORK_FILE}" \
		"$BRIDGE_URL" || true)"
elif command -v wget >/dev/null 2>&1; then
	if wget -qO "$RESPONSE_FILE" \
		-T "$BACKEND_TIMEOUT" \
		--header="Content-Type: ${CONTENT_TYPE}" \
		--header="X-Filename: ${JOB_TITLE}" \
		--post-file="$WORK_FILE" \
		"$BRIDGE_URL"; then
		HTTP_CODE="200"
	else
		HTTP_CODE="500"
	fi
else
	echo "ERROR: neither curl nor wget is available for backend transport" >&2
	exit 1
fi

if [ "${HTTP_CODE:-500}" -lt 200 ] || [ "${HTTP_CODE:-500}" -ge 300 ]; then
	echo "ERROR: bridge returned HTTP ${HTTP_CODE:-500}" >&2
	[ -f "$RESPONSE_FILE" ] && cat "$RESPONSE_FILE" >&2 || true
	if [ "$KEEP_FAILED_JOBS" = "1" ]; then
		SAVED="$(archive_job "failed")"
		log_note "saved failed job ${JOB_ID} to ${SAVED}"
	fi
	exit 1
fi

job_status "STATE: +connecting-to-device"
job_status "STATE: -connecting-to-device"
job_status "INFO: forwarded job ${JOB_ID} (${JOB_TITLE}) for ${USER_NAME}; copies=${COPIES}"
job_status "PAGE: 1 1"
job_status "ATTR: job-impressions=1"
job_status "ATTR: job-impressions-completed=1"
job_status "ATTR: job-media-progress=100"
job_status "INFO: job ${JOB_ID} completed successfully"
if [ "$PURGE_COMPLETED_JOBS" = "1" ]; then
	sleep "$PURGE_COMPLETED_JOBS_DELAY_SEC"
	if command -v cancel >/dev/null 2>&1; then
		cancel -x "$JOB_ID" >/dev/null 2>&1 || true
		cancel -a "$QUEUE_NAME" >/dev/null 2>&1 || true
	fi
	job_status "INFO: job ${JOB_ID} intentionally ended in canceled state for client spool cleanup"
fi
exit 0
