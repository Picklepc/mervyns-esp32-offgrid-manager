# Network-ESP32-BLE-Printer

This directory is the OpenWrt side of the project: a bridge that tries to make a BLE-only ESP32 printer backend behave more like a normal LAN printer appliance.

It is not finished, but it is far enough along to be useful as a reference.

## What This Work Was Trying To Solve

The ESP32 side already knew how to talk to the M100 printer. That still leaves a real usability gap:

- phones, laptops, and tablets expect network printers
- the printer is actually BLE-only
- the ESP32 is not a full print server

So the OpenWrt layer was used as the "normal network" face of the system.

## Design Shape

The ESP32 exposes:

- `GET /api/bridge/info`
- `GET /api/bridge/printer/status`
- `POST /api/bridge/job/start`
- `POST /api/bridge/job/chunk`
- `POST /api/bridge/job/finish`
- `POST /api/bridge/job/cancel`

The OpenWrt package builds on top of that with:

- UCI config
- `procd` service wiring
- Python bridge daemon
- upload and raster front-end on the OpenWrt host
- spool/cache directories
- CUPS/Docker scaffolding for a more normal network-printer experience

## What Worked

The custom bridge path worked well enough to prove the model:

- OpenWrt can poll ESP stock metadata
- OpenWrt can accept image jobs
- OpenWrt can fit jobs to rectangular or circular label stock
- OpenWrt can convert to monochrome raster
- OpenWrt can stream those jobs back to the ESP bridge

That is already a useful pattern for any project that needs to put a richer network layer in front of an embedded BLE-only device.

## What Did Not Become Fully Clean

The hard part is not the rasterization. The hard part is printer semantics:

- discovery across different client types
- spooling
- job completion state
- cancellation behavior
- driverless printing expectations

That is why the OpenWrt work is described here as incomplete but workable progress.

## Practical Lessons

### 1. Separate the private bridge from the public printer face

This repo moved toward:

- internal bridge on `8631`
- public printer/CUPS side on `631`

That split is the right architectural direction even if the current queue behavior still needs work.

### 2. A stronger host matters

The NESPi was the better target than a lighter router because this layer needs:

- raster conversion
- image normalization
- spooling
- service management
- eventually, better IPP/CUPS behavior

### 3. "Make it discoverable" and "make it behave like a real printer" are different problems

The repo got through discovery and job submission much earlier than it got through spool behavior and client expectations.

That distinction is worth preserving for anyone reusing this work.

## Reusing This In Another Project

If you want to put OpenWrt in front of an embedded device:

1. define a clean private device API first
2. make the OpenWrt side talk only to that API
3. keep the public printer-facing layer separate from the device-facing layer
4. expect job lifecycle handling to take longer than discovery

## Current Host Assumptions

The scaffold is biased toward the NESPi host:

- `host_profile=nespi`
- `data_root=/mnt/sda1/network-esp32-ble-printer`

If the SSD exists, spool/cache data uses it.
If not, the bridge falls back to `/tmp/network-esp32-ble-printer`.

## Direct Deployment

From Windows PowerShell:

```powershell
cd "C:\Users\patri\OneDrive\Documents\ESP32\Pack Rat"
.\openwrt\Network-ESP32-BLE-Printer\scripts\deploy-to-megamind.ps1
```

That path is more important than a polished package feed right now because it keeps iteration fast.

## Relevant Files

- `package/network-esp32-ble-printer/Makefile`
- `package/network-esp32-ble-printer/files/etc/config/network-esp32-ble-printer`
- `package/network-esp32-ble-printer/files/etc/init.d/network-esp32-ble-printer`
- `package/network-esp32-ble-printer/files/usr/libexec/network-esp32-ble-printer/bridge.py`
- `package/network-esp32-ble-printer/files/usr/libexec/network-esp32-ble-printer/bridge.sh`
- `package/network-esp32-ble-printer/files/usr/libexec/network-esp32-ble-printer/cups-backend.sh`
- `package/network-esp32-ble-printer/files/usr/libexec/network-esp32-ble-printer/setup-cups-docker.sh`
- `scripts/install-network-esp32-ble-printer.sh`
- `scripts/deploy-to-megamind.ps1`

## Takeaway

This OpenWrt work is worth sharing because it shows the shape of a useful bridge even before the printer-driver story is perfect.
