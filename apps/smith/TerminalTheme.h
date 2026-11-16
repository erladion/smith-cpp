#pragma once

#include "core/Config.h"
#include "tui/Style.h"

namespace Smith::Ui {

// The theme's shades for the terminal's colors: the desktop app's, so both
// look alike. Light is made for a terminal with a light background.
Tui::Palette terminalPalette(Theme theme);

} // namespace Smith::Ui
