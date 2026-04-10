#!/usr/bin/env python3

import base64
import io
import json
import os
import pathlib
import socket
import struct
import subprocess
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from PIL import Image, ImageDraw, ImageOps


SERVICE_NAME = "Network-ESP32-BLE-Printer"
SERVICE_VERSION = "0.2.2"
CONFIG_NAME = "network-esp32-ble-printer"
RUNTIME_ROOT = pathlib.Path("/tmp/network-esp32-ble-printer")
STATE_FILE = RUNTIME_ROOT / "bridge-info.json"
STATUS_FILE = RUNTIME_ROOT / "status"
PRINTER_STATUS_FILE = RUNTIME_ROOT / "printer-status.json"
LOG_FILE = RUNTIME_ROOT / "bridge.log"

JOB_LOCK = threading.Lock()
JOB_COUNTER = 0
LAST_CUPS_STOCK = ""
PRINTER_STATUS_TTL_SEC = 10

IPP_PRINT_JOB = 0x0002
IPP_VALIDATE_JOB = 0x0004
IPP_GET_JOB_ATTRIBUTES = 0x0009
IPP_GET_JOBS = 0x000A
IPP_GET_PRINTER_ATTRIBUTES = 0x000B

IPP_TAG_OPERATION = 0x01
IPP_TAG_JOB = 0x02
IPP_TAG_END = 0x03
IPP_TAG_PRINTER = 0x04

IPP_TAG_CHARSET = 0x47
IPP_TAG_LANGUAGE = 0x48
IPP_TAG_URI = 0x45
IPP_TAG_NAME = 0x42
IPP_TAG_TEXT = 0x41
IPP_TAG_KEYWORD = 0x44
IPP_TAG_INTEGER = 0x21
IPP_TAG_BOOLEAN = 0x22
IPP_TAG_ENUM = 0x23
IPP_TAG_MIMETYPE = 0x49

SUPPORTED_DOC_FORMATS = (
    "image/urf",
    "image/pwg-raster",
    "image/jpeg",
    "image/png",
    "application/octet-stream",
)

SUPPORTED_OPERATIONS = (
    IPP_PRINT_JOB,
    IPP_VALIDATE_JOB,
    IPP_GET_JOB_ATTRIBUTES,
    IPP_GET_JOBS,
    IPP_GET_PRINTER_ATTRIBUTES,
)


def log(message: str) -> None:
    stamp = time.strftime("%Y-%m-%d %H:%M:%S")
    line = f"{stamp} {message}"
    LOG_FILE.parent.mkdir(parents=True, exist_ok=True)
    with LOG_FILE.open("a", encoding="utf-8") as handle:
        handle.write(line + "\n")
    try:
        subprocess.run(["logger", "-t", SERVICE_NAME, "--", message], check=False)
    except Exception:
        pass


def uci_get(option: str, default: str = "") -> str:
    try:
        result = subprocess.run(
            ["uci", "-q", "get", f"{CONFIG_NAME}.main.{option}"],
            capture_output=True,
            text=True,
            check=False,
        )
        value = result.stdout.strip()
        return value if value else default
    except Exception:
        return default


def resolve_data_root() -> pathlib.Path:
    configured = uci_get("data_root", "")
    if configured and pathlib.Path(configured).is_dir():
        return pathlib.Path(configured)
    return RUNTIME_ROOT


def ensure_runtime() -> tuple[pathlib.Path, pathlib.Path]:
    data_root = resolve_data_root()
    spool_dir = pathlib.Path(uci_get("spool_dir", "")) if uci_get("spool_dir", "") else data_root / "spool"
    cache_dir = pathlib.Path(uci_get("cache_dir", "")) if uci_get("cache_dir", "") else data_root / "cache"
    RUNTIME_ROOT.mkdir(parents=True, exist_ok=True)
    spool_dir.mkdir(parents=True, exist_ok=True)
    cache_dir.mkdir(parents=True, exist_ok=True)
    return spool_dir, cache_dir


def cups_root() -> pathlib.Path:
    configured = uci_get("cups_data_root", "/mnt/sda1/network-esp32-ble-printer/cups")
    root = pathlib.Path(configured) if configured else pathlib.Path("/tmp/network-esp32-ble-printer/cups")
    root.mkdir(parents=True, exist_ok=True)
    return root


def esp_url(path: str) -> str:
    base = uci_get("esp_url", "http://192.168.1.4").rstrip("/")
    return f"{base}{path}"


def fetch_json(url: str, method: str = "GET", data: bytes | None = None, headers: dict | None = None) -> dict:
    timeout = float(uci_get("esp_timeout_sec", "5") or "5")
    request = urllib.request.Request(url, data=data, method=method)
    for key, value in (headers or {}).items():
        request.add_header(key, value)
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def fetch_bridge_info() -> dict:
    return fetch_json(esp_url("/api/bridge/info"))


def fetch_printer_status() -> dict:
    return fetch_json(esp_url("/api/bridge/printer/status"))


def cache_bridge_info(info: dict) -> None:
    RUNTIME_ROOT.mkdir(parents=True, exist_ok=True)
    STATE_FILE.write_text(json.dumps(info), encoding="utf-8")
    STATUS_FILE.write_text("online", encoding="utf-8")


def cache_printer_status(status: dict) -> None:
    RUNTIME_ROOT.mkdir(parents=True, exist_ok=True)
    payload = dict(status)
    payload["_cachedAt"] = time.time()
    PRINTER_STATUS_FILE.write_text(json.dumps(payload), encoding="utf-8")


def parse_stock_mm(bridge_info: dict) -> tuple[int, int]:
    label = str(bridge_info.get("defaultLabelSize", "") or "")
    if label.endswith("-circle"):
        base = label.split("-", 1)[0]
        if base.isdigit():
            mm = int(base)
            return mm, mm
    if "x" in label:
        left, right = label.split("x", 1)
        if left.isdigit() and right.isdigit():
            return int(left), int(right)
    stock_w = int(bridge_info.get("stockWidthPx", 400) or 400)
    stock_h = int(bridge_info.get("stockHeightPx", 240) or 240)
    return max(20, round(stock_w / 8)), max(20, round(stock_h / 8))


def points_from_mm(mm: int) -> int:
    return max(1, int(round((mm / 25.4) * 72.0)))


def write_cups_ppd(bridge_info: dict) -> pathlib.Path:
    width_mm, height_mm = parse_stock_mm(bridge_info)
    width_pt = points_from_mm(width_mm)
    height_pt = points_from_mm(height_mm)
    left = max(4, int(round(width_pt * 0.02)))
    right = width_pt - left
    top_margin = max(6, int(round(height_pt * 0.04)))
    bottom_margin = max(8, int(round(height_pt * 0.05)))
    top = height_pt - top_margin
    bottom = bottom_margin
    ppd = f"""*PPD-Adobe: "4.3"
*FormatVersion: "4.3"
*FileVersion: "1.0"
*LanguageVersion: English
*LanguageEncoding: ISOLatin1
*PCFileName: "MERVYNS.PPD"
*Manufacturer: "Mervyns"
*Product: "(Network-ESP32-BLE-Printer)"
*ModelName: "Mervyns Label Printer"
*ShortNickName: "Mervyns Label Printer"
*NickName: "Mervyns Label Printer"
*PSVersion: "(3010.000) 0"
*LanguageLevel: "3"
*ColorDevice: False
*DefaultColorSpace: Gray
*FileSystem: False
*Throughput: "1"
*LandscapeOrientation: Plus90
*VariablePaperSize: False
*DefaultResolution: 203dpi
*OpenUI *PageSize/Media Size: PickOne
*DefaultPageSize: LabelStock
*PageSize LabelStock/Label Stock: "<</PageSize[{width_pt} {height_pt}]>>setpagedevice"
*CloseUI: *PageSize
*OpenUI *PageRegion/Page Region: PickOne
*DefaultPageRegion: LabelStock
*PageRegion LabelStock/Label Stock: "<</PageSize[{width_pt} {height_pt}]>>setpagedevice"
*CloseUI: *PageRegion
*DefaultImageableArea: LabelStock
*ImageableArea LabelStock: "{left} {bottom} {right} {top}"
*DefaultPaperDimension: LabelStock
*PaperDimension LabelStock: "{width_pt} {height_pt}"
*OpenUI *Resolution/Resolution: PickOne
*DefaultResolution: 203dpi
*Resolution 203dpi/203 DPI: "<</HWResolution[203 203]>>setpagedevice"
*CloseUI: *Resolution
"""
    ppd_path = cups_root() / "mervyns.ppd"
    ppd_path.write_text(ppd, encoding="latin-1")
    return ppd_path


def sync_cups_queue(bridge_info: dict) -> None:
    global LAST_CUPS_STOCK
    if uci_get("cups_enabled", "1") != "1":
        return
    stock_key = f"{bridge_info.get('defaultLabelSize','')}|{bridge_info.get('stockWidthPx','')}|{bridge_info.get('stockHeightPx','')}"
    write_cups_ppd(bridge_info)
    if stock_key == LAST_CUPS_STOCK:
        return
    LAST_CUPS_STOCK = stock_key
    log(f"observed stock change {bridge_info.get('defaultLabelSize', 'unknown')} (leaving CUPS queue stable)")


def get_bridge_info(force_refresh: bool = True) -> dict:
    if force_refresh:
        try:
            info = fetch_bridge_info()
            cache_bridge_info(info)
            sync_cups_queue(info)
            return info
        except Exception as exc:
            log(f"bridge info fetch failed: {exc}")
            STATUS_FILE.write_text("offline", encoding="utf-8")
    if STATE_FILE.exists():
        return json.loads(STATE_FILE.read_text(encoding="utf-8"))
    return {"message": "No bridge info cached yet."}


def get_printer_status(force_refresh: bool = False) -> dict:
    ttl = max(2, int(uci_get("printer_status_ttl_sec", str(PRINTER_STATUS_TTL_SEC)) or str(PRINTER_STATUS_TTL_SEC)))
    if not force_refresh and PRINTER_STATUS_FILE.exists():
        try:
            cached = json.loads(PRINTER_STATUS_FILE.read_text(encoding="utf-8"))
            cached_at = float(cached.get("_cachedAt", 0) or 0)
            if cached_at and (time.time() - cached_at) < ttl:
                return cached
        except Exception:
            pass
    try:
        status = fetch_printer_status()
        cache_printer_status(status)
        return status
    except Exception as exc:
        log(f"printer status fetch failed: {exc}")
    if PRINTER_STATUS_FILE.exists():
        try:
            return json.loads(PRINTER_STATUS_FILE.read_text(encoding="utf-8"))
        except Exception:
            pass
    return {
        "reachable": False,
        "batteryKnown": False,
        "batteryPercent": -1,
        "lastSeenMs": -1,
        "message": "Printer status unavailable.",
    }


def printer_ready_status(force_refresh: bool = False) -> tuple[bool, dict]:
    status = get_printer_status(force_refresh=force_refresh)
    reachable = bool(status.get("reachable", False))
    last_seen_ms = int(status.get("lastSeenMs", -1) or -1)
    stale_limit_ms = max(5000, int(uci_get("printer_ready_stale_ms", "20000") or "20000"))
    stale = last_seen_ms >= 0 and last_seen_ms > stale_limit_ms
    # A live status query proving the printer is reachable is enough to allow
    # the job, even if the last battery notify is old. The battery timestamp is
    # useful telemetry, but it should not block printing once the device answers.
    available = reachable
    status["available"] = available
    status["stale"] = stale
    if available and stale and status.get("message"):
        status["message"] = f"{status['message']} Battery telemetry is stale, but the printer is responding."
    elif not available and not status.get("message"):
        status["message"] = "Printer is unavailable."
    return available, status


def control_cups_job(job_id: str, delay_sec: int = 0) -> None:
    container = uci_get("cups_container_name", "mervyns-cups")
    queue_name = "Mervyns_Label_Printer"
    if not container or not job_id:
        return

    def runner() -> None:
        if delay_sec > 0:
            time.sleep(delay_sec)
        try:
            result = subprocess.run(
                [
                    "docker", "exec", container, "sh", "-lc",
                    f"cancel -x {job_id} >/dev/null 2>&1 || true; cancel -a {queue_name} >/dev/null 2>&1 || true"
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            log(f"issued cups cleanup for job {job_id} via {container} rc={result.returncode}")
        except Exception as exc:
            log(f"failed to issue cups cleanup for job {job_id}: {exc}")

    threading.Thread(target=runner, daemon=True).start()


def nonwhite_bbox(image: Image.Image, threshold: int = 245) -> tuple[int, int, int, int] | None:
    gray = image.convert("L")
    mask = gray.point(lambda px: 255 if px < threshold else 0, mode="L")
    return mask.getbbox()


def fit_source_to_canvas(
    source: Image.Image,
    width: int,
    height: int,
    margin_left: int = 0,
    margin_top: int = 0,
    margin_right: int = 0,
    margin_bottom: int = 0,
    scale_factor: float = 1.0,
) -> Image.Image:
    source = ImageOps.exif_transpose(source).convert("L")
    draw_width = max(1, width - margin_left - margin_right)
    draw_height = max(1, height - margin_top - margin_bottom)
    best = None
    for rotate in (False, True):
        candidate = source.rotate(90, expand=True) if rotate else source
        scale = min(draw_width / candidate.width, draw_height / candidate.height)
        draw_w = max(1, int(candidate.width * scale))
        draw_h = max(1, int(candidate.height * scale))
        limiting_axis = "width" if (draw_width / candidate.width) <= (draw_height / candidate.height) else "height"
        bbox = nonwhite_bbox(candidate)
        if bbox:
            content_cover_w = max(0.0, min(1.0, (bbox[2] - bbox[0]) / max(1, candidate.width)))
            content_cover_h = max(0.0, min(1.0, (bbox[3] - bbox[1]) / max(1, candidate.height)))
        else:
            content_cover_w = 1.0
            content_cover_h = 1.0
        if not best or (draw_w * draw_h) > best[0]:
            best = (draw_w * draw_h, candidate, draw_w, draw_h, limiting_axis, content_cover_w, content_cover_h, bbox)

    _, chosen, draw_w, draw_h, limiting_axis, content_cover_w, content_cover_h, chosen_bbox = best
    adaptive_scale = scale_factor
    if limiting_axis == "height":
        if content_cover_h >= 0.94:
            adaptive_scale *= 0.88
        elif content_cover_h >= 0.85:
            adaptive_scale *= 0.92
        elif content_cover_h >= 0.72:
            adaptive_scale *= 0.96
    else:
        if content_cover_w >= 0.94:
            adaptive_scale *= 0.90
        elif content_cover_w >= 0.85:
            adaptive_scale *= 0.94
        elif content_cover_w >= 0.72:
            adaptive_scale *= 0.97
    adaptive_scale *= 0.90
    if adaptive_scale < 1.0:
        draw_w = max(1, int(draw_w * adaptive_scale))
        draw_h = max(1, int(draw_h * adaptive_scale))
    canvas = Image.new("L", (width, height), 255)
    x = margin_left + ((draw_width - draw_w) // 2)
    y = margin_top + ((draw_height - draw_h) // 2)
    if chosen_bbox:
        bbox_left, bbox_top, bbox_right, bbox_bottom = chosen_bbox
        bbox_cx = ((bbox_left + bbox_right) / 2.0) / max(1, chosen.width)
        bbox_cy = ((bbox_top + bbox_bottom) / 2.0) / max(1, chosen.height)
        content_center_x = bbox_cx * draw_w
        content_center_y = bbox_cy * draw_h
        target_center_x = margin_left + (draw_width / 2.0)
        target_center_y = margin_top + (draw_height / 2.0)
        x = int(round(target_center_x - content_center_x))
        y = int(round(target_center_y - content_center_y))
        x = max(margin_left, min(x, margin_left + draw_width - draw_w))
        y = max(margin_top, min(y, margin_top + draw_height - draw_h))
    vertical_bias_percent = max(-20, min(20, int(uci_get("system_vertical_bias_percent", "-4") or "-4")))
    if vertical_bias_percent and height > 0:
        y += int(round((vertical_bias_percent / 100.0) * height))
        y = max(margin_top, min(y, margin_top + draw_height - draw_h))
    resized = chosen.resize((draw_w, draw_h), Image.Resampling.LANCZOS)
    canvas.paste(resized, (x, y))
    return canvas


def apply_circle_mask(image: Image.Image) -> Image.Image:
    image = image.convert("L")
    mask = Image.new("L", image.size, 0)
    draw = ImageDraw.Draw(mask)
    inset = 8
    draw.ellipse((inset, inset, image.width - inset, image.height - inset), fill=255)
    white = Image.new("L", image.size, 255)
    white.paste(image, mask=mask)
    return white


def trim_white_border(image: Image.Image, threshold: int = 245, padding: int = 4) -> Image.Image:
    bbox = nonwhite_bbox(image, threshold=threshold)
    if not bbox:
        return image
    left = max(0, bbox[0] - padding)
    top = max(0, bbox[1] - padding)
    right = min(image.width, bbox[2] + padding)
    bottom = min(image.height, bbox[3] + padding)
    if right <= left or bottom <= top:
        return image
    return image.crop((left, top, right, bottom))


def render_job_image(raw_bytes: bytes, bridge_info: dict, cache_dir: pathlib.Path) -> tuple[bytes, int]:
    source = Image.open(io.BytesIO(raw_bytes))
    trim_white = uci_get("trim_white_border", "0").strip().lower() in ("1", "true", "yes", "on")
    if trim_white:
        source = trim_white_border(source)
    target_width = int(bridge_info.get("targetPrintWidthPx", 384) or 384)
    shape = bridge_info.get("shape", "rect")
    fit_mode = bridge_info.get("fitMode", "rect-contain")
    system_scale_percent = max(70, min(100, int(uci_get("system_scale_percent", "100") or "100")))

    if shape == "circle" or fit_mode == "circle-inscribed":
        target_height = target_width
    else:
        job_width = max(1, int(bridge_info.get("jobWidthPx", 400) or 400))
        job_height = max(1, int(bridge_info.get("jobHeightPx", 240) or 240))
        target_height = max(8, int(round((job_height / job_width) * target_width)))

    if shape == "circle" or fit_mode == "circle-inscribed":
        margin_left = margin_top = margin_right = margin_bottom = 18
        scale_factor = 0.92
    else:
        # For rectangular stock, rely on the adaptive fit calculation alone and
        # do not inject any extra canvas margins.
        margin_left = margin_top = margin_right = margin_bottom = 0
        scale_factor = system_scale_percent / 100.0

    canvas = fit_source_to_canvas(
        source,
        target_width,
        target_height,
        margin_left=margin_left,
        margin_top=margin_top,
        margin_right=margin_right,
        margin_bottom=margin_bottom,
        scale_factor=scale_factor,
    )
    canvas = ImageOps.autocontrast(canvas, cutoff=1)
    if shape == "circle" or fit_mode == "circle-inscribed":
        canvas = apply_circle_mask(canvas)

    dither_mode = getattr(Image, "Dither", None)
    floyd = dither_mode.FLOYDSTEINBERG if dither_mode else Image.FLOYDSTEINBERG
    bw = canvas.convert("1", dither=floyd)
    cache_dir.mkdir(parents=True, exist_ok=True)
    bw.convert("L").save(cache_dir / "latest-preview.png", format="PNG")

    width_bytes = (bw.width + 7) // 8
    raster = bytearray(width_bytes * bw.height)
    pixels = bw.load()
    for y in range(bw.height):
        for x in range(bw.width):
            if pixels[x, y] == 0:
                raster[y * width_bytes + (x >> 3)] |= 0x80 >> (x & 7)
    return bytes(raster), bw.height


def send_job_to_esp(raster: bytes, height: int, bridge_info: dict) -> dict:
    width_bytes = int(bridge_info.get("targetPrintWidthPx", 384) or 384) // 8
    start_body = json.dumps({
        "widthBytes": width_bytes,
        "height": height,
        "totalBytes": len(raster),
    }).encode("utf-8")
    fetch_json(
        esp_url("/api/bridge/job/start"),
        method="POST",
        data=start_body,
        headers={"Content-Type": "application/json"},
    )
    chunk_bytes = int(bridge_info.get("chunkBytes", 1536) or 1536)
    for offset in range(0, len(raster), chunk_bytes):
        chunk = raster[offset:offset + chunk_bytes]
        payload = base64.b64encode(chunk)
        fetch_json(
            esp_url("/api/bridge/job/chunk"),
            method="POST",
            data=payload,
            headers={"Content-Type": "text/plain"},
        )
    return fetch_json(esp_url("/api/bridge/job/finish"), method="POST", data=b"")


def require_printer_ready(force_refresh: bool = False) -> dict:
    available, status = printer_ready_status(force_refresh=force_refresh)
    if not available:
        raise RuntimeError(status.get("message", "Printer is unavailable."))
    return status


def parse_ipp_attributes(payload: bytes) -> tuple[int, int, dict[str, list[tuple[int, bytes]]], bytes]:
    if len(payload) < 8:
        raise ValueError("IPP payload too short")
    version = struct.unpack(">H", payload[:2])[0]
    operation = struct.unpack(">H", payload[2:4])[0]
    request_id = struct.unpack(">I", payload[4:8])[0]
    pos = 8
    attrs: dict[str, list[tuple[int, bytes]]] = {}
    current_name = ""
    while pos < len(payload):
        tag = payload[pos]
        pos += 1
        if tag == IPP_TAG_END:
            return version, request_id, attrs, payload[pos:]
        if tag in (IPP_TAG_OPERATION, IPP_TAG_JOB, IPP_TAG_PRINTER):
            current_name = ""
            continue
        if pos + 4 > len(payload):
            break
        name_len = struct.unpack(">H", payload[pos:pos + 2])[0]
        pos += 2
        name = payload[pos:pos + name_len].decode("utf-8", errors="ignore") if name_len else current_name
        pos += name_len
        value_len = struct.unpack(">H", payload[pos:pos + 2])[0]
        pos += 2
        value = payload[pos:pos + value_len]
        pos += value_len
        current_name = name
        attrs.setdefault(name, []).append((tag, value))
    return version, request_id, attrs, b""


def ipp_attr_bytes(tag: int, name: str, value: bytes) -> bytes:
    name_bytes = name.encode("utf-8")
    return bytes([tag]) + struct.pack(">H", len(name_bytes)) + name_bytes + struct.pack(">H", len(value)) + value


def ipp_attr_int(tag: int, name: str, value: int) -> bytes:
    return ipp_attr_bytes(tag, name, struct.pack(">i", value))


def ipp_attr_bool(name: str, value: bool) -> bytes:
    return ipp_attr_bytes(IPP_TAG_BOOLEAN, name, b"\x01" if value else b"\x00")


def ipp_attr_text(tag: int, name: str, value: str) -> bytes:
    return ipp_attr_bytes(tag, name, value.encode("utf-8"))


def build_ipp_response(status_code: int, request_id: int, attributes: list[bytes], group_tag: int = IPP_TAG_PRINTER) -> bytes:
    body = bytearray()
    body += struct.pack(">H", 0x0200)
    body += struct.pack(">H", status_code)
    body += struct.pack(">I", request_id)
    body.append(IPP_TAG_OPERATION)
    body += ipp_attr_text(IPP_TAG_CHARSET, "attributes-charset", "utf-8")
    body += ipp_attr_text(IPP_TAG_LANGUAGE, "attributes-natural-language", "en-us")
    body.append(group_tag)
    for item in attributes:
        body += item
    body.append(IPP_TAG_END)
    return bytes(body)


def build_printer_attributes(request_id: int, bridge_info: dict) -> bytes:
    printer_name = uci_get("printer_name", "Mervyns Label Printer")
    listen_port = int(uci_get("bridge_port", uci_get("listen_port", "8631")) or "8631")
    hostname = socket.gethostname() or "openwrt"
    printer_uri = f"ipp://{hostname}:{listen_port}{uci_get('ipp_path', '/ipp/print')}"
    media_name = bridge_info.get("defaultLabelSize", "custom")
    available, printer = printer_ready_status(force_refresh=False)
    printer_state = 3 if available else 5
    printer_reason = "none" if available else "offline"
    attrs = [
        ipp_attr_text(IPP_TAG_URI, "printer-uri-supported", printer_uri),
        ipp_attr_text(IPP_TAG_URI, "uri-supported", printer_uri),
        ipp_attr_text(IPP_TAG_NAME, "printer-name", printer_name),
        ipp_attr_text(IPP_TAG_TEXT, "printer-info", "Mervyns NESPi bridge to ESP32 BLE label printer"),
        ipp_attr_text(IPP_TAG_TEXT, "printer-make-and-model", "Network-ESP32-BLE-Printer"),
        ipp_attr_int(IPP_TAG_ENUM, "printer-state", printer_state),
        ipp_attr_text(IPP_TAG_KEYWORD, "printer-state-reasons", printer_reason),
        ipp_attr_bool("printer-is-accepting-jobs", available),
        ipp_attr_text(IPP_TAG_KEYWORD, "ipp-versions-supported", "1.1"),
        ipp_attr_text(IPP_TAG_KEYWORD, "ipp-versions-supported", "2.0"),
        ipp_attr_text(IPP_TAG_KEYWORD, "ipp-features-supported", "ipp-everywhere"),
        ipp_attr_text(IPP_TAG_CHARSET, "charset-supported", "utf-8"),
        ipp_attr_text(IPP_TAG_LANGUAGE, "generated-natural-language-supported", "en-us"),
        ipp_attr_text(IPP_TAG_LANGUAGE, "natural-language-configured", "en-us"),
        ipp_attr_text(IPP_TAG_KEYWORD, "uri-authentication-supported", "none"),
        ipp_attr_text(IPP_TAG_KEYWORD, "uri-security-supported", "none"),
        ipp_attr_text(IPP_TAG_MIMETYPE, "document-format-default", "image/jpeg"),
        ipp_attr_text(IPP_TAG_KEYWORD, "media-default", media_name),
        ipp_attr_text(IPP_TAG_KEYWORD, "media-supported", media_name),
        ipp_attr_text(IPP_TAG_KEYWORD, "pdl-override-supported", "not-attempted"),
        ipp_attr_text(IPP_TAG_KEYWORD, "compression-supported", "none"),
        ipp_attr_bool("color-supported", False),
        ipp_attr_bool("multiple-document-jobs-supported", False),
        ipp_attr_int(IPP_TAG_INTEGER, "copies-supported", 1),
        ipp_attr_int(IPP_TAG_INTEGER, "queued-job-count", 0),
        ipp_attr_int(IPP_TAG_INTEGER, "printer-up-time", max(1, int(time.monotonic()))),
    ]
    for fmt in SUPPORTED_DOC_FORMATS:
        attrs.append(ipp_attr_text(IPP_TAG_MIMETYPE, "document-format-supported", fmt))
    for op in SUPPORTED_OPERATIONS:
        attrs.append(ipp_attr_int(IPP_TAG_ENUM, "operations-supported", op))
    return build_ipp_response(0x0000, request_id, attrs, group_tag=IPP_TAG_PRINTER)


def build_ipp_simple_ok(request_id: int) -> bytes:
    return build_ipp_response(0x0000, request_id, [])


def handle_ipp_request(payload: bytes) -> tuple[int, bytes]:
    global JOB_COUNTER
    version, request_id, attrs, document = parse_ipp_attributes(payload)
    operation = struct.unpack(">H", payload[2:4])[0]
    bridge_info = get_bridge_info(force_refresh=True)

    if operation == IPP_GET_PRINTER_ATTRIBUTES:
        return HTTPStatus.OK, build_printer_attributes(request_id, bridge_info)

    if operation == IPP_GET_JOBS:
        return HTTPStatus.OK, build_ipp_response(0x0000, request_id, [], group_tag=IPP_TAG_JOB)

    if operation == IPP_GET_JOB_ATTRIBUTES:
        attrs = [
            ipp_attr_int(IPP_TAG_INTEGER, "job-id", max(JOB_COUNTER, 1)),
            ipp_attr_int(IPP_TAG_ENUM, "job-state", 9),
            ipp_attr_text(IPP_TAG_KEYWORD, "job-state-reasons", "none"),
        ]
        return HTTPStatus.OK, build_ipp_response(0x0000, request_id, attrs, group_tag=IPP_TAG_JOB)

    if operation == IPP_VALIDATE_JOB:
        return HTTPStatus.OK, build_ipp_simple_ok(request_id)

    if operation != IPP_PRINT_JOB:
        return HTTPStatus.BAD_REQUEST, build_ipp_response(0x0501, request_id, [])

    if not document:
        return HTTPStatus.BAD_REQUEST, build_ipp_response(0x0406, request_id, [])

    doc_format = "application/octet-stream"
    if "document-format" in attrs and attrs["document-format"]:
        doc_format = attrs["document-format"][0][1].decode("utf-8", errors="ignore")
    if doc_format not in SUPPORTED_DOC_FORMATS:
        log(f"ipp print-job unsupported document-format={doc_format}")
        return HTTPStatus.BAD_REQUEST, build_ipp_response(0x040B, request_id, [])

    if not JOB_LOCK.acquire(blocking=False):
        return HTTPStatus.CONFLICT, build_ipp_response(0x0506, request_id, [])

    try:
        require_printer_ready(force_refresh=True)
        _, cache_dir = ensure_runtime()
        raster, height = render_job_image(document, bridge_info, cache_dir)
        send_job_to_esp(raster, height, bridge_info)
        JOB_COUNTER += 1
        attrs = [
            ipp_attr_int(IPP_TAG_INTEGER, "job-id", JOB_COUNTER),
            ipp_attr_int(IPP_TAG_ENUM, "job-state", 9),
            ipp_attr_text(IPP_TAG_KEYWORD, "job-state-reasons", "none"),
        ]
        return HTTPStatus.OK, build_ipp_response(0x0000, request_id, attrs, group_tag=IPP_TAG_JOB)
    finally:
        JOB_LOCK.release()


class BridgeHandler(BaseHTTPRequestHandler):
    server_version = f"Network-ESP32-BLE-Printer/{SERVICE_VERSION}"

    def _send_json(self, payload: dict, status: int = 200) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_html(self, html: str, status: int = 200) -> None:
        body = html.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_ipp(self, payload: bytes, status: int = 200) -> None:
        self.send_response(status)
        self.send_header("Content-Type", "application/ipp")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self) -> None:
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/":
            info = get_bridge_info(force_refresh=False)
            printer_name = uci_get("printer_name", "Mervyns Label Printer")
            html = f"""<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>{printer_name}</title><style>body{{font-family:sans-serif;background:#f4f1e8;color:#2a2218;margin:0;padding:24px}}.wrap{{max-width:880px;margin:0 auto}}.panel{{background:#fff;border-radius:18px;padding:20px;box-shadow:0 14px 30px rgba(0,0,0,.08)}}button{{padding:12px 16px;border-radius:999px;border:0;background:#2e57aa;color:#fff;font-weight:700;cursor:pointer}}input{{display:block;margin:16px 0}}pre{{white-space:pre-wrap;background:#f6f3ea;padding:14px;border-radius:12px}}</style></head><body><div class="wrap"><div class="panel"><h1>{printer_name}</h1><p>NESPi bridge for the ESP32 BLE label printer.</p><input id="file" type="file" accept="image/png,image/jpeg,image/webp"><button id="send">Print Image</button><p id="status">Waiting for a file.</p><pre>{json.dumps(info, indent=2)}</pre></div></div><script>document.getElementById('send').onclick=async()=>{{const input=document.getElementById('file');if(!input.files.length){{document.getElementById('status').textContent='Pick an image first.';return}}document.getElementById('status').textContent='Sending image to printer...';const r=await fetch('/api/print/image',{{method:'POST',headers:{{'Content-Type':input.files[0].type||'application/octet-stream','X-Filename':input.files[0].name}},body:input.files[0]}});const data=await r.json();document.getElementById('status').textContent=data.message||JSON.stringify(data)}};</script></body></html>"""
            self._send_html(html)
            return
        if parsed.path == "/api/info":
            self._send_json(get_bridge_info(force_refresh=True))
            return
        if parsed.path == "/api/status":
            payload = {
                "bridge": get_bridge_info(force_refresh=False),
                "printer": get_printer_status(force_refresh=False),
                "status": STATUS_FILE.read_text(encoding="utf-8").strip() if STATUS_FILE.exists() else "unknown",
            }
            self._send_json(payload)
            return
        if parsed.path == "/api/printer/ready":
            available, printer = printer_ready_status(force_refresh=True)
            self._send_json(printer, status=200 if available else 503)
            return
        if parsed.path == "/api/job/control":
            self._send_json({"message": "Use POST."}, status=405)
            return
        self._send_json({"message": "Not found."}, status=404)

    def do_HEAD(self) -> None:
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == uci_get("ipp_path", "/ipp/print"):
            self.send_response(200)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_response(404)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_OPTIONS(self) -> None:
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == uci_get("ipp_path", "/ipp/print"):
            self.send_response(200)
            self.send_header("Allow", "OPTIONS, HEAD, POST")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_response(404)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_POST(self) -> None:
        parsed = urllib.parse.urlparse(self.path)
        ipp_path = uci_get("ipp_path", "/ipp/print")
        if parsed.path == ipp_path:
            length = int(self.headers.get("Content-Length", "0") or "0")
            payload = self.rfile.read(length) if length > 0 else b""
            try:
                status, response = handle_ipp_request(payload)
            except Exception as exc:
                log(f"ipp request failed: {type(exc).__name__}: {exc}")
                response = build_ipp_response(0x0500, 1, [])
                status = HTTPStatus.INTERNAL_SERVER_ERROR
            self._send_ipp(response, status=status)
            return

        if parsed.path != "/api/print/image":
            if parsed.path == "/api/job/control":
                length = int(self.headers.get("Content-Length", "0") or "0")
                payload = self.rfile.read(length) if length > 0 else b"{}"
                try:
                    data = json.loads(payload.decode("utf-8"))
                except Exception:
                    self._send_json({"message": "Invalid JSON."}, status=400)
                    return
                job_id = str(data.get("jobId", "") or "").strip()
                action = str(data.get("action", "") or "").strip().lower()
                delay_sec = int(data.get("delaySec", 0) or 0)
                if not job_id or action not in ("complete", "cancel"):
                    self._send_json({"message": "jobId and action are required."}, status=400)
                    return
                control_cups_job(job_id, max(0, delay_sec))
                self._send_json({"message": f"Queued {action} cleanup for job {job_id}.", "jobId": job_id})
                return
            self._send_json({"message": "Not found."}, status=404)
            return

        length = int(self.headers.get("Content-Length", "0") or "0")
        if length <= 0:
            self._send_json({"message": "Image body was empty."}, status=400)
            return
        raw = self.rfile.read(length)
        ensure_runtime()
        spool_dir, cache_dir = ensure_runtime()
        job_name = self.headers.get("X-Filename", f"job-{int(time.time())}.bin")
        safe_name = os.path.basename(job_name) or f"job-{int(time.time())}.bin"
        (spool_dir / safe_name).write_bytes(raw)

        if not JOB_LOCK.acquire(blocking=False):
            self._send_json({"message": "Another print job is already active."}, status=409)
            return
        try:
            bridge_info = get_bridge_info(force_refresh=True)
            require_printer_ready(force_refresh=True)
            raster, height = render_job_image(raw, bridge_info, cache_dir)
            result = send_job_to_esp(raster, height, bridge_info)
            payload = {
                "message": result.get("message", "Image sent to ESP printer bridge."),
                "heightPx": height,
                "bytes": len(raster),
                "stock": bridge_info.get("defaultLabelSize"),
                "shape": bridge_info.get("shape"),
            }
            log(f"printed {safe_name} stock={payload['stock']} shape={payload['shape']} height={height} bytes={len(raster)}")
            self._send_json(payload)
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="ignore")
            log(f"ESP bridge HTTP error: {exc.code} {detail}")
            self._send_json({"message": f"ESP bridge returned HTTP {exc.code}.", "detail": detail}, status=502)
        except Exception as exc:
            log(f"print failed: {exc}")
            self._send_json({"message": f"Print failed: {exc}"}, status=500)
        finally:
            JOB_LOCK.release()


def run_poll_once() -> int:
    ensure_runtime()
    info = get_bridge_info(force_refresh=True)
    printer_ok, printer = printer_ready_status(force_refresh=False)
    log(
        "profile=%s stock=%s orientation=%s shape=%s stock=%sx%s job=%sx%s fit=%s circle_diameter=%s printer=%s battery=%s lastSeenMs=%s"
        % (
            uci_get("host_profile", "generic"),
            info.get("defaultLabelSize", "?"),
            info.get("defaultOrientation", "?"),
            info.get("shape", "?"),
            info.get("stockWidthPx", "?"),
            info.get("stockHeightPx", "?"),
            info.get("jobWidthPx", "?"),
            info.get("jobHeightPx", "?"),
            info.get("fitMode", "?"),
            info.get("largestCircleDiameterPx", "?"),
            "ready" if printer_ok else "offline",
            printer.get("batteryPercent", "?"),
            printer.get("lastSeenMs", "?"),
        )
    )
    return 0


def poll_forever() -> None:
    while True:
        try:
            run_poll_once()
        except Exception as exc:
            log(f"poll loop failed: {exc}")
        interval = max(5, int(uci_get("poll_interval", "30") or "30"))
        time.sleep(interval)


def run_daemon() -> int:
    ensure_runtime()
    run_poll_once()
    thread = threading.Thread(target=poll_forever, daemon=True)
    thread.start()
    host = uci_get("listen_host", "0.0.0.0")
    port = int(uci_get("bridge_port", uci_get("listen_port", "8631")) or "8631")
    log(f"starting bridge version {SERVICE_VERSION} on {host}:{port}")
    server = ThreadingHTTPServer((host, port), BridgeHandler)
    log(f"daemon listening on {host}:{port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


def main() -> int:
    import sys

    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "daemon":
        return run_daemon()
    if cmd == "poll":
        return run_poll_once()
    if cmd == "status":
        print(json.dumps(get_bridge_info(force_refresh=False)))
        return 0
    print("usage: bridge.py daemon|poll|status")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
