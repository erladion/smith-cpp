#pragma once

#include "tui/Style.h"

namespace Smith::Tui {

// Renders the Markdown that models commonly write: **bold**, *italic*,
// `code`, headings, bullet and numbered lists, quotes, rules, fenced code
// blocks and links. Anything else shows as plain text.
QList<Line> markdownLines(const QString& text, int width);

// Inline Markdown in one line of text, on top of an optional base style.
Line inlineSpans(const QString& text, const Style& base = {});

// Where streamed Markdown can be split so the first part can be printed for
// good: the last blank line that isn't inside a code block. -1 if none.
int safeSplit(const QString& text);

} // namespace Smith::Tui
