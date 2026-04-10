# Victron BLE Narrative

The Victron part of this project is intentionally narrower than a full dashboard stack. It focuses on `Instant Readout` over BLE and a live local page instead of long-lived storage and cloud sync.

## What This Work Was Trying To Solve

The target use case was simple:

- show current SmartSolar state locally
- use the existing Victron bind-key based BLE readout
- avoid making the ESP32 into a long-term telemetry database
- keep the power page responsive and readable

This was not an attempt to reproduce all of VictronConnect.

## Core Findings

### 1. Live feed beats fake history on small devices

This repo originally drifted toward retaining more telemetry state than it needed. That added complexity without actually improving the main experience.

The better shape for this project was:

- live packet decode
- compact current-value presentation
- no dependency on deep retained history for the normal page

That made the power page easier to keep stable.

### 2. The bind-key path is useful even if it is not the richest path

The current implementation stays with the existing bind-key workflow because it is practical and already good enough for:

- device state
- error code
- battery voltage/current
- PV power
- load current
- yield today

That is enough for a meaningful local dashboard.

### 3. Trimmed dashboards are easier to trust

The current power page works best when it is honest about what it is:

- a live feed
- not a historian
- not a full Victron clone

That honesty is part of why the page is reusable for other projects.

## How To Reuse This In Another Project

If you want a local ESP32 dashboard for a Victron controller:

1. Start with the live BLE packet path.
2. Expose only the values you can keep fresh.
3. Avoid building your own long-term history unless you truly need it.
4. Make the UI explicit about what is live, cached, or unavailable.

If a future project needs deeper history, that should probably be a separate synchronization layer, not a side effect of the live page.

## Relevant Files

- [src/main.cpp](C:\Users\patri\OneDrive\Documents\ESP32\Pack%20Rat\src\main.cpp)
- [src/web_pages.cpp](C:\Users\patri\OneDrive\Documents\ESP32\Pack%20Rat\src\web_pages.cpp)
- [include/app_defaults.h](C:\Users\patri\OneDrive\Documents\ESP32\Pack%20Rat\include\app_defaults.h)

## Useful Takeaway

For small embedded projects, "live and correct enough" is often better than "historical and fragile."
