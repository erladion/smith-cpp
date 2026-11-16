#include "TerminalTheme.h"

namespace Smith::Ui {

namespace {

// In Color's order: None (text), Red, Green, Yellow, Blue, Magenta, Cyan, White, Gray.
Tui::Palette palette(std::optional<Tui::Rgb> text, Tui::Rgb red, Tui::Rgb green, Tui::Rgb yellow, Tui::Rgb blue, Tui::Rgb magenta, Tui::Rgb cyan,
                     Tui::Rgb white, Tui::Rgb gray) {
  return Tui::Palette{{text, red, green, yellow, blue, magenta, cyan, white, gray}};
}

} // namespace

Tui::Palette terminalPalette(Theme theme) {
  switch (theme) {
  case Theme::Matrix:
    return palette(std::nullopt, 0xff5c5c, 0x00ff41, 0xe8c46a, 0x5fd7c8, 0xc792ea, 0x00ff41, 0xeafff0, 0x767676);
  case Theme::Plain:
    return palette(std::nullopt, 0xf07178, 0x7ec699, 0xe5c07b, 0x61afef, 0xc678dd, 0x56b6c2, 0xdcdfe4, 0x8b9098);
  case Theme::Light:
    // Darker shades that read on white; "white" becomes the text color, since white would vanish.
    return palette(std::nullopt, 0xcf222e, 0x1a7f37, 0x9a6700, 0x0969da, 0x8250df, 0x0b7285, 0x1f2328, 0x6e7781);
  }
  return {};
}

Tui::Palette terminalPalette(const ThemeSpec& theme) {
  Tui::Palette out = terminalPalette(theme.base);
  for (int i = 0; i < TERMINAL_COLORS.size(); i++) {
    const QString& name = TERMINAL_COLORS[i];
    const QString follows = DERIVED_TERMINAL_COLORS.value(name);
    if (theme.terminal.contains(name)) {
      out.shades[size_t(i)] = theme.terminal.value(name);
    } else if (!follows.isEmpty() && theme.colors.contains(follows)) {
      out.shades[size_t(i)] = theme.colors.value(follows);
    }
  }
  return out;
}

} // namespace Smith::Ui
