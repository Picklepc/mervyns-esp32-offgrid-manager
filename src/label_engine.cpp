#include "label_engine.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>

namespace {

struct FontStyle {
  const char *name;
  const char *stack;
  float widthFactor;
  int weight;
  float letterSpacing;
  float subtitleSpacing;
};

struct ThemeStyle {
  const char *name;
  const char *primaryGlyph;
  const char *secondaryGlyph;
  uint8_t layout;
};

constexpr FontStyle FONT_STYLES[] = {
    {"Office Sans", "Segoe UI, Bahnschrift, Arial, sans-serif", 1.00f, 700, 0.60f, 1.10f},
    {"Storybook Serif", "Georgia, Times New Roman, serif", 1.02f, 700, 0.45f, 1.00f},
    {"Trellis Sans", "Trebuchet MS, Segoe UI, sans-serif", 0.98f, 700, 0.55f, 1.10f},
    {"Garden Ledger", "Palatino Linotype, Book Antiqua, serif", 1.00f, 700, 0.40f, 0.95f},
    {"Market Poster", "Arial Black, Arial, sans-serif", 1.10f, 900, 0.50f, 1.00f},
    {"Tool Bench Mono", "Courier New, Lucida Console, monospace", 1.14f, 700, 0.35f, 0.80f},
    {"Porch Sign", "Verdana, Geneva, sans-serif", 1.02f, 700, 0.52f, 1.00f},
    {"Township Serif", "Cambria, Georgia, serif", 1.00f, 700, 0.44f, 0.90f},
    {"Typewriter Shelf", "Lucida Sans Typewriter, Courier New, monospace", 1.10f, 700, 0.35f, 0.80f},
    {"Workshop Gothic", "Century Gothic, Trebuchet MS, Arial, sans-serif", 0.96f, 700, 0.70f, 1.20f},
    {"Farmhouse Serif", "Constantia, Georgia, serif", 1.01f, 700, 0.46f, 0.96f},
    {"Stencil Crate", "Impact, Haettenschweiler, Arial Narrow Bold, sans-serif", 1.09f, 800, 0.48f, 0.96f},
    {"School Poster", "Tahoma, Verdana, sans-serif", 0.99f, 700, 0.58f, 1.06f},
    {"Ledger Mono", "Consolas, Lucida Console, monospace", 1.08f, 700, 0.32f, 0.74f},
    {"Botanical Serif", "Garamond, Baskerville, serif", 0.98f, 700, 0.42f, 0.92f},
};

constexpr ThemeStyle THEME_STYLES[] = {
    {"Wildflower Shelf", "&#127800;", "&#127807;", 0},
    {"Garden Bed", "&#129716;", "&#127807;", 1},
    {"Chicken Yard", "&#128020;", "&#129370;", 2},
    {"Air Mail", "&#9992;", "&#9729;", 3},
    {"Stable Tack", "&#128052;", "&#11088;", 2},
    {"Barn Quilt", "&#10022;", "&#10023;", 4},
    {"Atomic Lab", "&#9883;", "&#128300;", 3},
    {"Bee Orchard", "&#128029;", "&#127827;", 0},
    {"Butterfly Patch", "&#129419;", "&#127804;", 1},
    {"Seed Packet", "&#127793;", "&#127803;", 4},
    {"Honey Harvest", "&#127855;", "&#128029;", 1},
    {"Country Fair", "&#128668;", "&#127806;", 2},
    {"Night Garden", "&#127769;", "&#10024;", 0},
    {"Maker Basket", "&#9986;", "&#129525;", 4},
    {"Star Sticker", "&#10029;", "&#10022;", 5},
    {"Barn Sign", "&#127960;", "&#10022;", 6},
    {"Hen Sticker", "&#128020;", "&#129370;", 6},
    {"Bee Sticker", "&#128029;", "&#127855;", 6},
    {"Tractor Sticker", "&#128668;", "&#127806;", 6},
    {"Compost Sticker", "&#128169;", "&#127793;", 6},
};

String collapseSpaces(const String &input) {
  String out;
  bool lastSpace = false;
  for (size_t i = 0; i < input.length(); ++i) {
    char c = input[i];
    if (c == '\r' || c == '\n' || c == '\t' || c == '_' || c == '/' || c == '\\') {
      c = ' ';
    }
    if (c == ' ') {
      if (!lastSpace) {
        out += c;
      }
      lastSpace = true;
    } else {
      out += c;
      lastSpace = false;
    }
  }
  out.trim();
  return out;
}

String normalizeLabelInput(String text) {
  text.trim();
  text = collapseSpaces(text);
  text.replace("&", " AND ");
  text.replace("+", " PLUS ");
  text = collapseSpaces(text);
  text.toUpperCase();
  while (text.startsWith("THE ")) text.remove(0, 4);
  while (text.startsWith("A ")) text.remove(0, 2);
  while (text.startsWith("AN ")) text.remove(0, 3);
  return collapseSpaces(text.length() ? text : "UNNAMED BIN");
}

std::vector<String> splitWords(const String &text) {
  std::vector<String> words;
  String current;
  for (size_t i = 0; i < text.length(); ++i) {
    const char c = text[i];
    if (c == ' ') {
      if (current.length()) {
        words.push_back(current);
        current = "";
      }
    } else {
      current += c;
    }
  }
  if (current.length()) {
    words.push_back(current);
  }
  return words;
}

String svgEscape(String value) {
  value.replace("&", "&amp;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  value.replace("\"", "&quot;");
  return value;
}

float estimateLineWidth(const String &line, float fontSize, float widthFactor) {
  int weightedChars = 0;
  for (size_t i = 0; i < line.length(); ++i) {
    const char c = line[i];
    if (c == 'I' || c == '1' || c == 'L' || c == ' ') {
      weightedChars += 6;
    } else if (c == 'W' || c == 'M' || c == 'N' || c == '8' || c == 'B') {
      weightedChars += 12;
    } else {
      weightedChars += 9;
    }
  }
  return ((weightedChars / 10.0f) * fontSize) * widthFactor;
}

bool fitsBlock(const std::vector<String> &lines, float fontSize, float widthFactor, int availableWidth, int availableHeight) {
  if (lines.empty()) {
    return true;
  }
  const float lineGap = std::max(4.0f, fontSize * 0.22f);
  const float totalHeight = lines.size() * fontSize + (lines.size() - 1) * lineGap;
  if (totalHeight > availableHeight) {
    return false;
  }
  for (const auto &line : lines) {
    if (estimateLineWidth(line, fontSize, widthFactor) > availableWidth) {
      return false;
    }
  }
  return true;
}

std::vector<String> buildLinesForFont(const std::vector<String> &words, float fontSize, float widthFactor, int availableWidth, size_t maxLines) {
  std::vector<String> lines;
  String current;

  for (const auto &word : words) {
    String working = word;
    while (working.length()) {
      String candidate = current.length() ? current + " " + working : working;
      if (estimateLineWidth(candidate, fontSize, widthFactor) <= availableWidth || !current.length()) {
        current = candidate;
        break;
      }

      lines.push_back(current);
      current = "";
      if (lines.size() >= maxLines) {
        lines.back() += " " + working;
        working = "";
      }
    }
  }

  if (current.length()) {
    lines.push_back(current);
  }

  if (lines.empty()) {
    lines.push_back("UNNAMED BIN");
  }

  while (lines.size() > maxLines) {
    lines[maxLines - 1] += " " + lines.back();
    lines.pop_back();
  }

  return lines;
}

bool isCircleLabelKey(const String &key) {
  return key.equalsIgnoreCase("50-circle");
}

uint32_t hashLabelSeed(const LabelSpec &spec, const String &normalized) {
  uint32_t hash = spec.variantSeed ? spec.variantSeed : static_cast<uint32_t>(millis());
  auto mixByte = [&](uint8_t b) {
    hash ^= b;
    hash *= 16777619u;
    hash = (hash << 7) | (hash >> 25);
  };

  const String inputs[] = {normalized, spec.shelfHint, spec.sizeKey, spec.orientation, spec.appearance};
  for (const auto &item : inputs) {
    for (size_t i = 0; i < item.length(); ++i) {
      mixByte(static_cast<uint8_t>(item[i]));
    }
    mixByte(0x5f);
  }

  return hash ? hash : 0x51f15e7u;
}

String themedGlyph(int x, int y, int size, const char *glyph, float opacity, int rotationDeg) {
  String svg;
  svg.reserve(180);
  svg += "<text x='";
  svg += String(x);
  svg += "' y='";
  svg += String(y);
  svg += "' font-size='";
  svg += String(size);
  svg += "' text-anchor='middle' dominant-baseline='middle' opacity='";
  svg += String(opacity, 2);
  svg += "'";
  if (rotationDeg != 0) {
    svg += " transform='rotate(";
    svg += String(rotationDeg);
    svg += " ";
    svg += String(x);
    svg += " ";
    svg += String(y);
    svg += ")'";
  }
  svg += ">";
  svg += glyph;
  svg += "</text>";
  return svg;
}

String buildStarSticker(int centerX, int centerY, int outerRadius, int innerRadius, float opacity) {
  String points;
  for (int i = 0; i < 10; ++i) {
    const float angle = (-90.0f + i * 36.0f) * 3.14159265f / 180.0f;
    const int radius = (i % 2 == 0) ? outerRadius : innerRadius;
    const int x = centerX + static_cast<int>(std::round(std::cos(angle) * radius));
    const int y = centerY + static_cast<int>(std::round(std::sin(angle) * radius));
    if (i) {
      points += " ";
    }
    points += String(x);
    points += ",";
    points += String(y);
  }

  String svg;
  svg.reserve(220);
  svg += "<polygon points='";
  svg += points;
  svg += "' fill='currentColor' opacity='";
  svg += String(opacity, 2);
  svg += "'/>";
  return svg;
}

String buildOverlayTheme(const ThemeStyle &theme, const LabelPreset &preset) {
  const int minSide = std::min(preset.widthPx, preset.heightPx);
  const int centerX = preset.widthPx / 2;
  const int centerY = preset.heightPx / 2;
  const int big = std::max(72, minSide / 2);
  const int small = std::max(14, minSide / 12);
  const int edge = std::max(small + 24, minSide / 4);

  String svg;
  svg.reserve(900);
  svg += "<g font-family='Segoe UI Emoji, Apple Color Emoji, Noto Color Emoji, Segoe UI Symbol, sans-serif'>";
  if (theme.layout == 5) {
    svg += buildStarSticker(centerX, centerY, big / 2, std::max(18, big / 4), 0.24f);
  } else {
    svg += themedGlyph(centerX, centerY + 8, big, theme.primaryGlyph, 0.28f, 0);
  }
  svg += themedGlyph(edge, edge, small, theme.secondaryGlyph, 0.54f, -10);
  svg += themedGlyph(preset.widthPx - edge, edge, small, theme.secondaryGlyph, 0.54f, 10);
  svg += themedGlyph(edge, preset.heightPx - edge, small, theme.secondaryGlyph, 0.54f, -10);
  svg += themedGlyph(preset.widthPx - edge, preset.heightPx - edge, small, theme.secondaryGlyph, 0.54f, 10);
  svg += "</g>";
  return svg;
}

String buildThemeDecorations(const ThemeStyle &theme, const LabelPreset &preset) {
  if (theme.layout >= 5) {
    return buildOverlayTheme(theme, preset);
  }

  const int minSide = std::min(preset.widthPx, preset.heightPx);
  const int icon = std::max(14, minSide / 11);
  const int edge = std::max(icon + 16, minSide / 5);
  const int small = std::max(11, icon - 7);
  const int centerX = preset.widthPx / 2;
  const int centerY = preset.heightPx / 2;

  String svg;
  svg.reserve(2200);
  svg += "<g font-family='Segoe UI Emoji, Apple Color Emoji, Noto Color Emoji, Segoe UI Symbol, sans-serif'>";

  switch (theme.layout) {
    case 0:
      svg += themedGlyph(edge, edge, icon, theme.primaryGlyph, 0.86f, -12);
      svg += themedGlyph(preset.widthPx - edge, edge, icon, theme.primaryGlyph, 0.86f, 12);
      svg += themedGlyph(edge, preset.heightPx - edge, icon, theme.primaryGlyph, 0.86f, 10);
      svg += themedGlyph(preset.widthPx - edge, preset.heightPx - edge, icon, theme.primaryGlyph, 0.86f, -10);
      svg += themedGlyph(centerX - icon, edge + 10, small, theme.secondaryGlyph, 0.52f, -6);
      svg += themedGlyph(centerX, edge + 4, small, theme.secondaryGlyph, 0.52f, 0);
      svg += themedGlyph(centerX + icon, edge + 10, small, theme.secondaryGlyph, 0.52f, 6);
      break;
    case 1:
      svg += themedGlyph(centerX, edge + 4, icon + 1, theme.primaryGlyph, 0.84f, 0);
      svg += themedGlyph(centerX, preset.heightPx - edge - 4, icon + 1, theme.primaryGlyph, 0.84f, 0);
      svg += themedGlyph(edge + 4, edge + 4, small, theme.secondaryGlyph, 0.58f, -18);
      svg += themedGlyph(preset.widthPx - edge - 4, edge + 4, small, theme.secondaryGlyph, 0.58f, 18);
      svg += themedGlyph(edge + 4, preset.heightPx - edge - 4, small, theme.secondaryGlyph, 0.58f, -18);
      svg += themedGlyph(preset.widthPx - edge - 4, preset.heightPx - edge - 4, small, theme.secondaryGlyph, 0.58f, 18);
      break;
    case 2:
      svg += themedGlyph(edge + 4, centerY, icon + 2, theme.primaryGlyph, 0.84f, -10);
      svg += themedGlyph(preset.widthPx - edge - 4, centerY, icon + 2, theme.primaryGlyph, 0.84f, 10);
      svg += themedGlyph(edge + 8, edge + 8, small, theme.secondaryGlyph, 0.52f, -15);
      svg += themedGlyph(preset.widthPx - edge - 8, edge + 8, small, theme.secondaryGlyph, 0.52f, 15);
      svg += themedGlyph(edge + 8, preset.heightPx - edge - 8, small, theme.secondaryGlyph, 0.52f, -15);
      svg += themedGlyph(preset.widthPx - edge - 8, preset.heightPx - edge - 8, small, theme.secondaryGlyph, 0.52f, 15);
      break;
    case 3:
      svg += themedGlyph(edge + 8, edge + 8, small, theme.secondaryGlyph, 0.56f, -20);
      svg += themedGlyph(centerX, edge + 8, icon - 1, theme.primaryGlyph, 0.82f, 0);
      svg += themedGlyph(preset.widthPx - edge - 8, centerY - 6, icon + 1, theme.primaryGlyph, 0.82f, 14);
      svg += themedGlyph(centerX - 8, preset.heightPx - edge - 8, small, theme.secondaryGlyph, 0.56f, 18);
      svg += themedGlyph(edge + 10, preset.heightPx - edge - 8, small, theme.secondaryGlyph, 0.46f, -14);
      break;
    default:
      svg += themedGlyph(edge + 6, edge + 6, small, theme.secondaryGlyph, 0.50f, -10);
      svg += themedGlyph(centerX - icon, edge + 6, small, theme.primaryGlyph, 0.76f, -6);
      svg += themedGlyph(centerX, edge + 2, icon - 1, theme.primaryGlyph, 0.88f, 0);
      svg += themedGlyph(centerX + icon, edge + 6, small, theme.primaryGlyph, 0.76f, 6);
      svg += themedGlyph(centerX - icon, preset.heightPx - edge - 6, small, theme.secondaryGlyph, 0.56f, -6);
      svg += themedGlyph(centerX, preset.heightPx - edge - 2, icon - 1, theme.secondaryGlyph, 0.74f, 0);
      svg += themedGlyph(centerX + icon, preset.heightPx - edge - 6, small, theme.secondaryGlyph, 0.56f, 6);
      break;
  }

  svg += "</g>";
  return svg;
}

}  // namespace

LabelPreset labelPresetNativeByKey(const String &key) {
  LabelPreset preset;
  preset.key = key.length() ? key : "50x30";
  bool parsed = true;

  if (preset.key == "40x30") {
    preset.widthPx = 320;
    preset.heightPx = 240;
  } else if (preset.key == "50x30") {
    preset.widthPx = 400;
    preset.heightPx = 240;
  } else if (preset.key == "50x80") {
    preset.widthPx = 400;
    preset.heightPx = 640;
  } else if (preset.key == "50x50") {
    preset.widthPx = 400;
    preset.heightPx = 400;
  } else if (isCircleLabelKey(preset.key)) {
    preset.widthPx = 400;
    preset.heightPx = 400;
  } else if (preset.key == "60x40") {
    preset.widthPx = 480;
    preset.heightPx = 320;
  } else {
    parsed = false;
    const int xIndex = preset.key.indexOf('x');
    if (xIndex > 0 && xIndex < (preset.key.length() - 1)) {
      String widthPart = preset.key.substring(0, xIndex);
      String heightPart = preset.key.substring(xIndex + 1);
      widthPart.trim();
      heightPart.trim();
      bool widthOk = widthPart.length() > 0;
      bool heightOk = heightPart.length() > 0;
      for (size_t i = 0; i < widthPart.length() && widthOk; ++i) {
        widthOk = std::isdigit(static_cast<unsigned char>(widthPart[i]));
      }
      for (size_t i = 0; i < heightPart.length() && heightOk; ++i) {
        heightOk = std::isdigit(static_cast<unsigned char>(heightPart[i]));
      }
      if (widthOk && heightOk) {
        const int widthMm = widthPart.toInt();
        const int heightMm = heightPart.toInt();
        if (widthMm >= 20 && widthMm <= 120 && heightMm >= 20 && heightMm <= 120) {
          preset.widthPx = widthMm * 8;
          preset.heightPx = heightMm * 8;
          parsed = true;
        }
      }
    }
    if (!parsed) {
      preset.key = "50x30";
      preset.widthPx = 400;
      preset.heightPx = 240;
    }
  }

  return preset;
}

LabelPreset labelPresetByKey(const String &key, const String &orientation) {
  LabelPreset preset = labelPresetNativeByKey(key);
  const bool nativePortrait = preset.heightPx > preset.widthPx;
  const bool wantPortrait = orientation == "portrait";
  if (wantPortrait != nativePortrait) {
    std::swap(preset.widthPx, preset.heightPx);
  }
  return preset;
}

std::vector<String> labelPresetKeys() {
  return {"40x30", "50x30", "50x50", "50-circle", "60x40", "50x80"};
}

LabelRender renderLabel(const LabelSpec &spec) {
  LabelRender render;
  const LabelPreset preset = labelPresetByKey(spec.sizeKey, spec.orientation);
  const bool darkMode = spec.appearance == "dark";
  const bool circleLabel = isCircleLabelKey(spec.sizeKey);
  const String normalized = normalizeLabelInput(spec.text);
  const String subtitle = spec.shelfHint.length() ? normalizeLabelInput(spec.shelfHint) : "";
  const std::vector<String> words = splitWords(normalized);
  const size_t maxLines = preset.heightPx >= preset.widthPx ? 5 : 4;
  const uint32_t seed = hashLabelSeed(spec, normalized);
  const uint32_t fontSeed = (seed ^ 0x9e3779b9u) + ((seed << 6) | (seed >> 26));
  const uint32_t themeSeed = (seed * 1103515245u) + 12345u;

  const bool creativeStandard = spec.creativeMix && !spec.wild;
  const bool plainThisTime = !creativeStandard || ((seed & 0x3u) == 0u);
  const FontStyle &font = FONT_STYLES[fontSeed % (sizeof(FONT_STYLES) / sizeof(FONT_STYLES[0]))];
  const ThemeStyle &theme = THEME_STYLES[themeSeed % (sizeof(THEME_STYLES) / sizeof(THEME_STYLES[0]))];

  const int sideInset = circleLabel ? (plainThisTime ? 98 : 114) : (plainThisTime ? 42 : 56);
  const int topInset = plainThisTime ? 18 : 28;
  const int bottomInset = subtitle.length()
      ? (plainThisTime ? 76 : 92)
      : (plainThisTime ? 44 : 58);
  const int availableWidth = preset.widthPx - sideInset;
  const int availableHeight = preset.heightPx - bottomInset - topInset;

  float bestFont = std::max(18.0f, preset.heightPx * 0.12f);
  std::vector<String> bestLines;

  for (float fontSize = std::min(preset.heightPx * 0.28f, preset.widthPx * 0.21f); fontSize >= 18.0f; fontSize -= 2.0f) {
    std::vector<String> lines = buildLinesForFont(words, fontSize, font.widthFactor, availableWidth, maxLines);
    if (fitsBlock(lines, fontSize, font.widthFactor, availableWidth, availableHeight)) {
      bestFont = fontSize;
      bestLines = lines;
      break;
    }
    if (bestLines.empty()) {
      bestLines = lines;
    }
  }

  if (bestLines.empty()) {
    bestLines.push_back("UNNAMED BIN");
  }

  const int fontSize = static_cast<int>(std::round(bestFont));
  const int lineGap = std::max(4, static_cast<int>(std::round(bestFont * 0.22f)));
  const int subtitleSize = std::max(11, fontSize / 3);
  const int blockHeight = static_cast<int>(bestLines.size()) * fontSize + (static_cast<int>(bestLines.size()) - 1) * lineGap;
  const int bottomReserve = subtitle.length() ? subtitleSize + (plainThisTime ? 18 : 24) : (plainThisTime ? 8 : 16);
  const int startY = topInset + std::max(0, (preset.heightPx - topInset - bottomReserve - blockHeight) / 2) + fontSize;

  render.normalized = normalized;
  render.widthPx = preset.widthPx;
  render.heightPx = preset.heightPx;
  render.prompt = "";
  render.mode = plainThisTime ? "standardized" : "creative-standard";
  render.explanation = plainThisTime
      ? String("Plain standard label selected for this pass with ") + font.name + ". About one in four Create Label clicks stay clean and minimal."
      : String("Creative standard variant: ") + theme.name + " with " + font.name + ".";

  const char *paper = darkMode ? "#101214" : "#ffffff";
  const char *ink = darkMode ? "#ffffff" : "#000000";

  String svg;
  svg.reserve(5200);
  svg += "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 ";
  svg += String(preset.widthPx);
  svg += " ";
  svg += String(preset.heightPx);
  svg += "' width='";
  svg += String(preset.widthPx);
  svg += "' height='";
  svg += String(preset.heightPx);
  svg += "'>";
  svg += "<rect width='100%' height='100%' fill='";
  svg += paper;
  svg += "'/>";
  if (!plainThisTime) {
    svg += buildThemeDecorations(theme, preset);
  }
  svg += "<g font-family='";
  svg += font.stack;
  svg += "' text-anchor='middle' fill='";
  svg += ink;
  svg += "'>";

  for (size_t i = 0; i < bestLines.size(); ++i) {
    const int y = startY + static_cast<int>(i) * (fontSize + lineGap);
    svg += "<text x='";
    svg += String(preset.widthPx / 2);
    svg += "' y='";
    svg += String(y);
    svg += "' font-size='";
    svg += String(fontSize);
    svg += "' font-weight='";
    svg += String(font.weight);
    svg += "' letter-spacing='";
    svg += String(font.letterSpacing, 2);
    svg += "'>";
    svg += svgEscape(bestLines[i]);
    svg += "</text>";
  }

  if (subtitle.length()) {
    svg += "<text x='";
    svg += String(preset.widthPx / 2);
    svg += "' y='";
    svg += String(preset.heightPx - 12);
    svg += "' font-family='";
    svg += font.stack;
    svg += "' font-size='";
    svg += String(subtitleSize);
    svg += "' letter-spacing='";
    svg += String(font.subtitleSpacing, 2);
    svg += "' opacity='0.84'>";
    svg += svgEscape(subtitle);
    svg += "</text>";
  }

  svg += "</g></svg>";
  render.svg = svg;
  return render;
}
