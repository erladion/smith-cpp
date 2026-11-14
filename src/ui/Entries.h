#pragma once

#include "core/Diff.h"
#include "core/Events.h"
#include "core/tools/TodoTool.h"
#include "tui/Style.h"

#include <QStringList>

#include <optional>

namespace Smith::Ui {

using Tui::Color;
using Tui::Line;
using Tui::Span;
using Tui::Style;

// Something printed into the transcript.
struct Entry {
  enum class Kind {
    Banner,
    User,
    // Assistant text, rendered as Markdown.
    Text,
    Reasoning,
    Tool,
    Notice,
    // Quiet information, e.g. how long a turn took.
    Meta,
    // Agent Smith remarking on something, directly under the entry before it.
    Quote,
    // The agent's checklist, after it changed.
    Todos,
  };

  Kind kind = Kind::Notice;
  QString text;
  NoticeLevel level = NoticeLevel::Info;

  // Banner.
  QString model;
  QString server;
  QString cwd;
  QString session;
  bool resumed = false;
  bool sandboxActive = false;
  QString sandboxDescription;
  QStringList instructions;
  QStringList skills;
  QString greeting;

  // Tool.
  QString name;
  QString summary;
  QString output;
  bool isError = false;
  std::optional<Diff> diff;

  TodoList todos;

  static Entry notice(NoticeLevel level, const QString& text);
  static Entry simple(Kind kind, const QString& text);
};

// The lines for an entry, starting with a blank separator line (except the banner and quotes).
QList<Line> entryLines(const Entry& entry, int width);

// The checklist under a header: done steps dim, the current one bold.
QList<Line> todoLines(const TodoList& todos, int width);

// A diff as colored lines with line numbers; shows at most `maxLines`.
QList<Line> diffView(const Diff& diff, int width, int maxLines);

// A rounded box around content lines, as wide as its widest line, or the full width with `fill`.
QList<Line> box(const QList<Line>& content, Color color, int width, bool fill = false);

} // namespace Smith::Ui
