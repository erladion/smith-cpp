#pragma once

#include "core/Themes.h"
#include "tui/Style.h"

namespace Smith::Ui {

// The theme's shades for the terminal's colors: the desktop app's, so both
// look alike. Light is made for a terminal with a light background.
Tui::Palette terminalPalette(Theme theme);
// A theme file's: its base's, with the colors it sets.
Tui::Palette terminalPalette(const ThemeSpec& theme);

} // namespace Smith::Ui
