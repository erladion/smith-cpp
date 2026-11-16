#include "tui/Style.h"

#include "tui/Text.h"

#include <QStringList>

#include <atomic>
#include <cstdlib>

namespace Smith::Tui {

namespace {

std::atomic<bool> g_colors{qEnvironmentVariableIsEmpty("NO_COLOR")};
// Set and read on the UI thread, which draws.
Palette g_palette;
ColorDepth g_depth = detectColorDepth();

int red(Rgb rgb) {
  return int((rgb >> 16) & 0xff);
}

int green(Rgb rgb) {
  return int((rgb >> 8) & 0xff);
}

int blue(Rgb rgb) {
  return int(rgb & 0xff);
}

int colorCode(Color color) {
  switch (color) {
  case Color::Red:
    return 31;
  case Color::Green:
    return 32;
  case Color::Yellow:
    return 33;
  case Color::Blue:
    return 34;
  case Color::Magenta:
    return 35;
  case Color::Cyan:
    return 36;
  case Color::White:
    return 37;
  case Color::Gray:
    return 90;
  case Color::None:
    break;
  }
  return 0;
}

// The escape code's color part: the palette's shade when the terminal can show it, else the basic color.
QString colorPart(Color color) {
  const std::optional<Rgb> shade = g_palette.shades[size_t(color)];
  if (shade && g_depth == ColorDepth::TrueColor) {
    return QString("38;2;%1;%2;%3").arg(red(*shade)).arg(green(*shade)).arg(blue(*shade));
  }
  if (shade && g_depth == ColorDepth::Indexed256) {
    return QString("38;5;%1").arg(nearestIndexed(*shade));
  }
  return color == Color::None ? QString() : QString::number(colorCode(color));
}

QString sgr(const Style& style) {
  QStringList codes;
  if (style.bold) {
    codes << "1";
  }
  if (style.dim) {
    codes << "2";
  }
  if (style.italic) {
    codes << "3";
  }
  if (style.underline) {
    codes << "4";
  }
  if (style.inverse) {
    codes << "7";
  }
  if (g_colors) {
    const QString color = colorPart(style.color);
    if (!color.isEmpty()) {
      codes << color;
    }
  }
  return codes.isEmpty() ? QString() : "\x1b[" + codes.join(';') + "m";
}

} // namespace

void setColors(bool enabled) {
  g_colors = enabled;
}

bool colorsEnabled() {
  return g_colors;
}

void setPalette(const Palette& palette) {
  g_palette = palette;
}

ColorDepth detectColorDepth() {
  const QString colorTerm = qEnvironmentVariable("COLORTERM").toLower();
  if (colorTerm == "truecolor" || colorTerm == "24bit" || !qEnvironmentVariableIsEmpty("WT_SESSION")) {
    return ColorDepth::TrueColor;
  }
  return qEnvironmentVariable("TERM").contains("256color") ? ColorDepth::Indexed256 : ColorDepth::Basic;
}

void setColorDepth(ColorDepth depth) {
  g_depth = depth;
}

int nearestIndexed(Rgb rgb) {
  static constexpr int LEVELS[] = {0, 95, 135, 175, 215, 255};
  auto distance = [rgb](int r, int g, int b) {
    const int dr = red(rgb) - r;
    const int dg = green(rgb) - g;
    const int db = blue(rgb) - b;
    return dr * dr + dg * dg + db * db;
  };
  auto nearestLevel = [](int value) {
    int best = 0;
    for (int i = 1; i < 6; i++) {
      if (std::abs(LEVELS[i] - value) < std::abs(LEVELS[best] - value)) {
        best = i;
      }
    }
    return best;
  };
  const int r = nearestLevel(red(rgb));
  const int g = nearestLevel(green(rgb));
  const int b = nearestLevel(blue(rgb));
  int best = 16 + 36 * r + 6 * g + b;
  int bestDistance = distance(LEVELS[r], LEVELS[g], LEVELS[b]);
  // The 24 grays, 8 to 238 in steps of 10.
  for (int i = 0; i < 24; i++) {
    const int level = 8 + 10 * i;
    const int d = distance(level, level, level);
    if (d < bestDistance) {
      best = 232 + i;
      bestDistance = d;
    }
  }
  return best;
}

QString toAnsi(const Line& line) {
  QString out;
  for (const Span& span : line) {
    const QString text = sanitize(span.text).replace('\n', ' ');
    const QString open = sgr(span.style);
    out += open.isEmpty() ? text : open + text + "\x1b[0m";
  }
  return out;
}

} // namespace Smith::Tui
