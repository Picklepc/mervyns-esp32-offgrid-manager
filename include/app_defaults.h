#pragma once

#include <Arduino.h>

// Build defaults used on first boot before /settings.json exists.
static constexpr char DEFAULT_WIFI_SSID[] = "YOUR_CONTAINER_WIFI";
static constexpr char DEFAULT_WIFI_PASSWORD[] = "YOUR_WIFI_PASSWORD";
static constexpr char DEFAULT_HOSTNAME[] = "mervyns";
static constexpr char DEFAULT_VICTRON_MAC[] = "AA:BB:CC:DD:EE:FF";
static constexpr char DEFAULT_VICTRON_BINDKEY[] = "00112233445566778899AABBCCDDEEFF";
static constexpr char DEFAULT_PRINTER_MAC[] = "";
static constexpr char DEFAULT_IMAGE_GEN_PROVIDER[] = "openai";
static constexpr char DEFAULT_IMAGE_GEN_MODEL[] = "gpt-image-1-mini";
static constexpr char DEFAULT_HF_IMAGE_MODEL[] = "stabilityai/stable-diffusion-xl-base-1.0";
static constexpr char DEFAULT_IMAGE_GEN_URL[] = "";
static constexpr char DEFAULT_IMAGE_GEN_TOKEN[] = "";
static constexpr char DEFAULT_LABEL_SIZE[] = "50x80";
static constexpr char DEFAULT_LABEL_ORIENTATION[] = "portrait";
static constexpr char DEFAULT_LABEL_APPEARANCE[] = "light";
static constexpr char DEFAULT_CUSTOM_LABEL_SIZES[] = "";
static constexpr char DEFAULT_SETUP_AP_SUFFIX[] = "-Setup";

static constexpr uint32_t HISTORY_SAMPLE_PERIOD_MS = 60UL * 1000UL;
static constexpr size_t MAX_HISTORY_SAMPLES = 24UL * 60UL * 7UL;

static constexpr float BATTERY_OK_VOLTS = 13.2f;
static constexpr float BATTERY_LOW_VOLTS = 12.8f;
static constexpr float BATTERY_CRITICAL_VOLTS = 12.2f;
