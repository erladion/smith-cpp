#pragma once

#include <QList>
#include <QString>

#include <array>
#include <optional>

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

// A 24-bit color, 0xRRGGBB.
using Rgb = quint32;

// Exact shades for the colors, indexed by Color (None: text without a color
// of its own). An unset one is the terminal's own shade.
struct Palette {
  std::array<std::optional<Rgb>, 9> shades;
};
void setPalette(const Palette& palette);

// How many colors the terminal shows. Palette shades need 256 or more;
// with the 16 basic ones the terminal's own shades stay.
enum class ColorDepth { Basic, Indexed256, TrueColor };
// From COLORTERM, Windows Terminal's WT_SESSION and TERM.
ColorDepth detectColorDepth();
void setColorDepth(ColorDepth depth);

// The nearest of the 256 indexed colors (the 6×6×6 cube or the grays) to `rgb`.
int nearestIndexed(Rgb rgb);

// Renders a line as terminal output. Span text is sanitized, so it can't inject escape codes.
QString toAnsi(const Line& line);

} // namespace Smith::Tui
