# M100 Lab Narrative

This part of the project exists because the `Phomemo M100` is useful, cheap, and frustrating: it is a BLE label printer with no simple open protocol story for ESP32 projects.

The lab firmware in this repo is the focused answer to that problem.

## What This Work Was Trying To Solve

The goal was not "print anything to any thermal printer." The goal was narrower and more useful:

- identify the right BLE services and characteristics for the M100
- build repeatable scan, connect, read, subscribe, and write workflows
- move from raw command poking to a known-good print path
- reuse the proven print path in the main application instead of keeping printer support trapped inside a lab harness

## Core Findings

### 1. A focused lab console is far better than guessing inside the main app

The `xiao-lab` target exists because BLE printer work becomes much easier once you can:

- scan deliberately
- fix a target MAC
- connect on demand
- inspect characteristics
- subscribe to notification traffic
- send exact write payloads and repeat them

That workflow is documented in the lab target and implemented in [ble_smoke_test.cpp](C:\Users\patri\OneDrive\Documents\ESP32\Pack%20Rat\src\ble_smoke_test.cpp).

### 2. The stable print path matters more than theoretical protocol completeness

The useful outcome was not exhaustive protocol coverage. It was finding a raster-send path that the M100 actually accepted reliably enough to print labels.

The practical pattern here is:

- establish a clean printer connection
- send the expected raster payload sequence
- prefer the known-good path over "smarter" speculative rewrites

That lesson shows up repeatedly in the repo: once a print path works, it is safer to route more features through it than to keep inventing new send variants.

### 3. Status and battery probes can interfere with printing if they are treated casually

One of the recurring issues in this project was that status polling, battery probing, and active print sessions all want to use the same BLE link.

The practical lesson:

- do not assume "status check" is harmless
- separate "can I reach the printer?" from "should I actively query it right now?"
- treat print-session stability as more important than opportunistic battery updates

### 4. "Working enough" is a valid open-source milestone

This repo does not pretend to be a complete M100 SDK. It is a working reference that shows how to:

- discover a printer
- connect to it from ESP32 hardware
- render label content to monochrome raster
- print through a practical BLE path

That is enough to be useful to other projects.

## How To Reuse This In Another Project

If your real goal is "my project needs BLE label printing," start here:

1. Keep a printer-lab target separate from your main app.
2. Prove the raster send path in the lab first.
3. Once you trust the send path, reuse it in the main app.
4. Treat printer status, battery, and passive metadata as optional extras.

If you skip step 1, you usually end up debugging BLE transport, rendering, and UI state all at once.

## Relevant Files

- [src/ble_smoke_test.cpp](C:\Users\patri\OneDrive\Documents\ESP32\Pack%20Rat\src\ble_smoke_test.cpp)
- [src/main.cpp](C:\Users\patri\OneDrive\Documents\ESP32\Pack%20Rat\src\main.cpp)

## Build Targets

- `xiao-lab`: focused M100 reverse-engineering console
- `xiao-mainline`: main application using the proven printer path
- `wroom-mainline`: alternate mainline board target

## Serial Workflow Snapshot

```text
scan 5 active
target <printer-mac> public
connect
probe
read ff01
subscribe ff03
preset battery
preset status
```

## If You Are Replicating This Elsewhere

Port the printer work in this order:

- target selection and connection logic
- characteristic discovery and subscription
- known-good raster print path
- only then: battery, status, paper, or convenience helpers

That order matters more than elegance.
