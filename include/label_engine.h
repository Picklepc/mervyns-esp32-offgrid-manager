#pragma once

#include <Arduino.h>
#include <vector>

struct LabelPreset {
  String key;
  int widthPx;
  int heightPx;
};

struct LabelSpec {
  String text;
  String shelfHint;
  String sizeKey;
  String orientation;
  String appearance;
  bool wild = false;
  bool creativeMix = false;
  uint32_t variantSeed = 0;
};

struct LabelRender {
  String normalized;
  String prompt;
  String svg;
  String explanation;
  String mode = "standardized";
  int widthPx = 384;
  int heightPx = 240;
};

LabelPreset labelPresetByKey(const String &key, const String &orientation);
LabelPreset labelPresetNativeByKey(const String &key);
std::vector<String> labelPresetKeys();
LabelRender renderLabel(const LabelSpec &spec);
