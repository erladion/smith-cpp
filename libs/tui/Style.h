#pragma once

#include <QList>
#include <QString>

namespace Smith::Tui {

// The 16 basic colors, so the terminal's own theme decides the actual shades.
enum class Color { None, Red, Green, Yellow, Blue, Magenta, Cyan, White, Gray };

struct Style {
  Color color = Color::None;
  bool bold = false;
  bool dim = false;
  bool italic = false;
  bool underline = false;
  bool inverse = false;

  bool operator==(const Style& other) const = default;
};

// A run of text in one style. Text is plain: styling lives only in `style`.
struct Span {
  QString text;
  Style style;
};

// One screen line.
using Line = QList<Span>;

inline const Style DIM{.dim = true};
inline const Style REASONING{.dim = true, .italic = true};

inline Style fg(Color color) {
  return Style{.color = color};
}

// Turns colors off (NO_COLOR); bold, dim, italic and inverse stay.
void setColors(bool enabled);
bool colorsEnabled();

// Renders a line as terminal output. Span text is sanitized, so it can't inject escape codes.
QString toAnsi(const Line& line);

} // namespace Smith::Tui
