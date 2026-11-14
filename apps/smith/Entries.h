#pragma once

#include "frontend/Entry.h"
#include "tui/Style.h"

#include <QStringList>

#include <optional>

namespace Smith::Ui {

using Tui::Color;
using Tui::Line;
using Tui::Span;
using Tui::Style;

// The lines for an entry, starting with a blank separator line (except the banner and quotes).
QList<Line> entryLines(const Entry& entry, int width);

// The checklist under a header: done steps dim, the current one bold.
QList<Line> todoLines(const TodoList& todos, int width);

// A diff as colored lines with line numbers; shows at most `maxLines`.
QList<Line> diffView(const Diff& diff, int width, int maxLines);

// A rounded box around content lines, as wide as its widest line, or the full width with `fill`.
QList<Line> box(const QList<Line>& content, Color color, int width, bool fill = false);

} // namespace Smith::Ui
