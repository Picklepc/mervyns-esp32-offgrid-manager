# Mervyn's

`Mervyn's` is an open-source ESP32 project that ties together four interesting threads:

- a Phomemo `M100` BLE label printer
- lightweight label rendering plus optional AI image generation
- a work-in-progress OpenWrt print bridge
- Victron SmartSolar `Instant Readout` telemetry over BLE

This repo is intentionally being published as a reference project for other people building practical, weird, or "vibe coded" hardware/software hybrids. It is free to use, free to remix, and licensed under MIT.

## Off-Grid Origin Story

This project grew out of an off-grid storage container that used to be the back end of a Mervyn's truck.

That container gradually picked up:

- Used 225w solar panel operating at 48v - $30 on OfferUp 
- solar charging with Victron 75/15 charge controller
- 12v/156wh Motorcycle battery - allows for higher amperage draws, 156wh lasts all night with power to spare. I just had it laying around. 
- 12v RV lights
- 12v camping ceiling fan - $15 on Amazon - I snipped off the remote control and run this at full speed directly from a regular rocker switch. 
- USB-C PD drop-down power offering multiple voltages through trigger cables up to 20v and 65w - the was the most efficient way to bypass DC adapters and buck converters for electronics
- Cudy WR3000 OpenWrt Wi-Fi that averages around 5w of draw 
- a Google Nest speaker through PD Trigger cable fixed at 15v
- a Phomemo M100 label printer that can be recharged with usb-a to usb-c cable
- an ESP32 running the local monitor and management software

That is the real reason this repo exists. The useful discovery was that an ESP32 is a very good low-power "glue computer" for small off-grid installs:

- it can report solar and battery state over wifi
- it can host a local UI instead of depending on cloud dashboards
- it can integrate odd little tools like a BLE label printer
- it can replace proprietary phone-only utilities with something local and hackable

This project is not arguing that every container needs all of this. It is showing that once you already have an off-grid power system, the ESP32 is a surprisingly capable place to centralize monitoring and a few practical utilities simply because you can. This project can expand to all kinds of off-grid shops, sheds, tiny homes, and remote solar wifi access point/repeaters. 

## Why This Repo Exists

This started as a real container-side utility app, then turned into a useful test bed for a few niche but reusable integration problems:

- How do you make a low-cost BLE label printer usable from an ESP32?
- How do you keep a label workflow lightweight, fast, and local, while still allowing optional AI image generation?
- How far can you push an OpenWrt device toward becoming a network printer bridge for a BLE-only printer?
- What is the smallest useful Victron BLE dashboard you can run locally without dragging in full cloud infrastructure?

The result is not a polished product. It is a worked example with findings, tradeoffs, and enough code to reuse in other projects.

## Project Shape

Main firmware targets:

- `xiao-mainline`: primary app for the Seeed Studio XIAO ESP32-S3
- `wroom-mainline`: ESP32 DevKit / WROOM-flavored mainline build
- `xiao-lab`: focused M100 reverse-engineering console

Main hosted pages:

- `/`: label maker and printer workflow
- `/status`: live Victron telemetry page
- `/admin`: provisioning, radio, printer, and firmware tools

## What Works Today

- self-hosted label workflow with standard labels, creative standard variants, AI-generated labels, uploads, export, and direct print
- M100 BLE discovery, targeted connection, and practical printing from the ESP32
- local settings, hostname-based hosting, and fallback setup AP provisioning
- Victron SmartSolar live BLE telemetry with a trimmed, live-only power page
- OpenWrt bridge progress that is incomplete but workable enough to study and build from

## What Is Still Experimental

- OpenWrt printer spooling and job lifecycle behavior
- driverless network-printer behavior across Android, iPhone, Windows, and macOS
- some parts of the BLE status/battery handling around the M100
- portability of the Victron BLE path across boards and long runtimes

## Focused Docs

These docs are the real value of the repo if you want to reuse parts of it:

- [M100 Lab Narrative](docs/m100_lab.md)
- [Image Generation Narrative](docs/image_generation.md)
- [Victron BLE Narrative](docs/victron_ble.md)
- [OpenWrt Bridge Narrative](openwrt/Network-ESP32-BLE-Printer/README.md)

Each one is written as:

- what problem was being solved
- what was learned
- what parts are reusable
- how to adapt the work to another project

## Hardware Used

- Seeed Studio XIAO ESP32-S3
- ESP32 DevKit / WROOM for alternate build testing
- Phomemo M100 BLE label printer
- Victron SmartSolar controller with `Instant Readout via Bluetooth`
- local Wi-Fi network
- optional OpenWrt host for network print bridging

## Build

Main app:

```powershell
pio run -e xiao-mainline
pio run -e xiao-mainline -t upload
pio device monitor -b 115200
```

ESP32 DevKit / WROOM:

```powershell
pio run -e wroom-mainline
pio run -e wroom-mainline -t upload
pio device monitor -b 115200
```

M100 lab console:

```powershell
pio run -e xiao-lab
pio run -e xiao-lab -t upload
pio device monitor -b 115200
```

## Setup Summary

1. Flash a mainline target.
2. Join the fallback AP if the board is not on Wi-Fi yet.
3. Open `/admin`.
4. Save Wi-Fi settings, printer MAC, and any Victron or image-generation settings you want.
5. Use the saved hostname on your LAN.

## Repo Pointers

- [platformio.ini](platformio.ini): build targets
- [src/main.cpp](src/main.cpp): APIs, BLE, Wi-Fi, printer flow, Victron flow
- [src/web_pages.cpp](src/web_pages.cpp): embedded UI
- [src/label_engine.cpp](src/label_engine.cpp): standard and creative standard label rendering
- [src/ble_smoke_test.cpp](src/ble_smoke_test.cpp): M100 reverse-engineering console
- [openwrt/Network-ESP32-BLE-Printer](openwrt/Network-ESP32-BLE-Printer): OpenWrt bridge work

## License

MIT. See [LICENSE](LICENSE).

Use it, fork it, learn from it, strip it for parts, or build something stranger from it.
