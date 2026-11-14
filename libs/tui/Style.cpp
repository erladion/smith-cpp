#include "tui/Style.h"

#include "tui/Text.h"

#include <QStringList>

#include <atomic>

namespace Smith::Tui {

namespace {

std::atomic<bool> g_colors{qEnvironmentVariableIsEmpty("NO_COLOR")};

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
  if (style.color != Color::None && g_colors) {
    codes << QString::number(colorCode(style.color));
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
