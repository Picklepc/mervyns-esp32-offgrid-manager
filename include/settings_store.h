#pragma once

#include <Arduino.h>

struct AppSettings {
  String wifiSsid;
  String wifiPassword;
  String hostname;
  String victronMac;
  String victronBindKey;
  String printerMac;
  String imageGenProvider;
  String imageGenModel;
  String imageGenUrl;
  String imageGenToken;
  String defaultLabelSize;
  String defaultOrientation;
  String defaultAppearance;
  String customLabelSizes;
};

void settingsLoad(AppSettings &settings);
bool settingsSave(const AppSettings &settings);
