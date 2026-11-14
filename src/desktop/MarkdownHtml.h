#pragma once

#include "desktop/Palette.h"

#include <QString>

namespace Smith::Desktop {

// The Markdown models write, as rich text for a QML Text item. It goes
// through the TUI's Markdown renderer, so both frontends read the same
// subset the same way: headings and code in the code color, quotes and
// code blocks with a gutter, lists with bullets.
QString markdownToHtml(const QString& markdown, const Palette& palette);

// Plain text, escaped for rich text, keeping line breaks and spaces.
QString plainToHtml(const QString& text);

} // namespace Smith::Desktop
