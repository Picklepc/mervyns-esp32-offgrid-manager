#include "settings_store.h"

#include <ArduinoJson.h>
#include <Preferences.h>

#include "app_defaults.h"

namespace {

constexpr char SETTINGS_NAMESPACE[] = "mervyns";
constexpr char LEGACY_SETTINGS_NAMESPACE[] = "packrat";

void settingsDefaults(AppSettings &settings) {
  settings.wifiSsid = DEFAULT_WIFI_SSID;
  settings.wifiPassword = DEFAULT_WIFI_PASSWORD;
  settings.hostname = DEFAULT_HOSTNAME;
  settings.victronMac = DEFAULT_VICTRON_MAC;
  settings.victronBindKey = DEFAULT_VICTRON_BINDKEY;
  settings.printerMac = DEFAULT_PRINTER_MAC;
  settings.imageGenProvider = DEFAULT_IMAGE_GEN_PROVIDER;
  settings.imageGenModel = DEFAULT_IMAGE_GEN_MODEL;
  settings.imageGenUrl = DEFAULT_IMAGE_GEN_URL;
  settings.imageGenToken = DEFAULT_IMAGE_GEN_TOKEN;
  settings.defaultLabelSize = DEFAULT_LABEL_SIZE;
  settings.defaultOrientation = DEFAULT_LABEL_ORIENTATION;
  settings.defaultAppearance = DEFAULT_LABEL_APPEARANCE;
  settings.customLabelSizes = DEFAULT_CUSTOM_LABEL_SIZES;
}

}  // namespace

void settingsLoad(AppSettings &settings) {
  settingsDefaults(settings);
  Preferences prefs;
  bool loadedFromLegacy = false;
  if (!prefs.begin(SETTINGS_NAMESPACE, true)) {
    if (!prefs.begin(LEGACY_SETTINGS_NAMESPACE, true)) {
      return;
    }
    loadedFromLegacy = true;
  }

  settings.wifiSsid = prefs.getString("wifiSsid", settings.wifiSsid);
  settings.wifiPassword = prefs.getString("wifiPass", settings.wifiPassword);
  settings.hostname = prefs.getString("hostname", settings.hostname);
  settings.victronMac = prefs.getString("vicMac", settings.victronMac);
  settings.victronBindKey = prefs.getString("vicKey", settings.victronBindKey);
  settings.printerMac = prefs.getString("prnMac", settings.printerMac);
  settings.imageGenProvider = prefs.getString("imgProv", settings.imageGenProvider);
  settings.imageGenModel = prefs.getString("imgModel", settings.imageGenModel);
  settings.imageGenUrl = prefs.getString("imgUrl", settings.imageGenUrl);
  settings.imageGenToken = prefs.getString("imgTok", settings.imageGenToken);
  settings.defaultLabelSize = prefs.getString("lblSize", settings.defaultLabelSize);
  settings.defaultOrientation = prefs.getString("lblOrient", settings.defaultOrientation);
  settings.defaultAppearance = prefs.getString("lblAppear", settings.defaultAppearance);
  settings.customLabelSizes = prefs.getString("lblCustom", settings.customLabelSizes);
  prefs.end();
  if (loadedFromLegacy) {
    settingsSave(settings);
  }
}

bool settingsSave(const AppSettings &settings) {
  Preferences prefs;
  if (!prefs.begin(SETTINGS_NAMESPACE, false)) {
    return false;
  }
  prefs.putString("wifiSsid", settings.wifiSsid);
  prefs.putString("wifiPass", settings.wifiPassword);
  prefs.putString("hostname", settings.hostname);
  prefs.putString("vicMac", settings.victronMac);
  prefs.putString("vicKey", settings.victronBindKey);
  prefs.putString("prnMac", settings.printerMac);
  prefs.putString("imgProv", settings.imageGenProvider);
  prefs.putString("imgModel", settings.imageGenModel);
  prefs.putString("imgUrl", settings.imageGenUrl);
  prefs.putString("imgTok", settings.imageGenToken);
  prefs.putString("lblSize", settings.defaultLabelSize);
  prefs.putString("lblOrient", settings.defaultOrientation);
  prefs.putString("lblAppear", settings.defaultAppearance);
  prefs.putString("lblCustom", settings.customLabelSizes);
  prefs.end();
  return true;
}
