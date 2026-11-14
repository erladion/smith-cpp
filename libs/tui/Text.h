#pragma once

#include "tui/Style.h"

#include <QStringList>

namespace Smith::Tui {

// Removes escape sequences and control characters, so text from the model or
// from tool output can't move the cursor, recolor the screen or hide part of
// an approval prompt. Keeps newlines; tabs become four spaces.
QString sanitize(const QString& text);

// User-perceived characters: an emoji with modifiers or a letter with accents is one.
QStringList graphemes(const QString& text);

// Columns one grapheme takes in a terminal: 0, 1 or 2.
int graphemeWidth(const QString& grapheme);

// Columns a string takes. Assumes sanitized text without newlines.
int textWidth(const QString& text);
int lineWidth(const Line& line);

// Word-wraps a line to `width` columns. Newlines in the text start new lines;
// spaces at a wrap point are dropped; words longer than the width are split.
QList<Line> wrap(const Line& line, int width);

// Wraps `content`, starting the first line with `first` and indenting the rest with `rest`.
QList<Line> prefixed(const Line& first, const Line& rest, const Line& content, int width);

// Cuts a line to `width` columns, ending with "…" when something was cut. Newlines become spaces.
Line truncate(const Line& line, int width);

// Pads a line with spaces to exactly `width` columns (after truncating if needed).
Line fit(const Line& line, int width);

} // namespace Smith::Tui
