#include "Entries.h"

#include "core/Tool.h"
#include "tui/Markdown.h"
#include "tui/Text.h"

namespace Smith::Ui {

using Tui::DIM;
using Tui::fg;
using Tui::lineWidth;
using Tui::prefixed;
using Tui::REASONING;
using Tui::truncate;
using Tui::wrap;

namespace {

constexpr int PREVIEW_LINES = 4;
constexpr int DIFF_LINES = 14;

Color noticeColor(NoticeLevel level) {
  switch (level) {
  case NoticeLevel::Info:
    return Color::Blue;
  case NoticeLevel::Warn:
    return Color::Yellow;
  case NoticeLevel::Error:
    return Color::Red;
  }
  return Color::Blue;
}

QList<Line> withGap(const QList<Line>& lines) {
  return QList<Line>{Line{}} + lines;
}

QList<Line> bannerLines(const Entry& banner, int width) {
  Line sandbox;
  if (banner.sandboxActive) {
    sandbox.append(Span{"✓ " + banner.sandboxDescription, fg(Color::Green)});
  } else {
    sandbox.append(Span{"⚠ shell commands are " + banner.sandboxDescription, fg(Color::Red)});
  }
  QList<Line> content{Line{Span{"Agent Smith", Style{.color = Color::Cyan, .bold = true}}}};
  if (!banner.greeting.isEmpty()) {
    content.append(Line{Span{banner.greeting, Style{.color = Color::Green, .italic = true}}});
    content.append(Line{});
  }
  content.append(Line{Span{"model   " + banner.model, DIM}});
  content.append(Line{Span{"server  " + banner.server, DIM}});
  content.append(Line{Span{"cwd     " + banner.cwd, DIM}});
  content.append(Line{Span{"session " + banner.session + (banner.resumed ? " (resumed)" : ""), DIM}});
  if (!banner.instructions.isEmpty()) {
    content.append(Line{Span{"rules   " + banner.instructions.join(", "), DIM}});
  }
  if (!banner.skills.isEmpty()) {
    content.append(Line{Span{"skills  " + banner.skills.join(", "), DIM}});
  }
  if (!banner.tools.isEmpty()) {
    content.append(Line{Span{"tools   " + banner.tools.join(", "), DIM}});
  }
  content.append(sandbox);
  content.append(Line{Span{"/help for commands · esc interrupts · ctrl+c quits", DIM}});
  return box(content, Color::Cyan, width);
}

QList<Line> toolLines(const Entry& tool, int width) {
  const Line header = truncate(
      Line{
          Span{"● ", fg(tool.isError ? Color::Red : Color::Green)},
          Span{tool.name, Style{.bold = true}},
          Span{tool.summary.isEmpty() ? QString() : " " + tool.summary, DIM},
      },
      width);
  QList<Line> out{header};
  if (tool.diff && !tool.isError) {
    for (const Line& line : diffView(*tool.diff, width, DIFF_LINES)) {
      out.append(Line{Span{"  ", {}}} + line);
    }
    return out;
  }
  QString output = tool.output;
  while (output.endsWith('\n')) {
    output.chop(1);
  }
  const QStringList lines = output.split('\n');
  const Style style = tool.isError ? fg(Color::Red) : DIM;
  for (int i = 0; i < std::min(int(lines.size()), PREVIEW_LINES); i++) {
    out.append(truncate(Line{Span{i == 0 ? "  └ " : "    ", DIM}, Span{lines[i], style}}, width));
  }
  const int hidden = lines.size() - PREVIEW_LINES;
  if (hidden > 0) {
    out.append(Line{Span{"    … " + plural(hidden, "more line"), DIM}});
  }
  return out;
}

} // namespace

QList<Line> entryLines(const Entry& entry, int width) {
  switch (entry.kind) {
  case Entry::Kind::Banner:
    return bannerLines(entry, width);
  case Entry::Kind::User:
    return withGap(prefixed(Line{Span{"› ", Style{.color = Color::Cyan, .bold = true}}}, Line{Span{"  ", {}}}, Line{Span{entry.text, {}}}, width));
  case Entry::Kind::Text:
    return withGap(Tui::markdownLines(entry.text, width));
  case Entry::Kind::Reasoning:
    return withGap(wrap(Line{Span{entry.text, REASONING}}, width));
  case Entry::Kind::Notice:
    return withGap(wrap(Line{Span{entry.text, fg(noticeColor(entry.level))}}, width));
  case Entry::Kind::Tool:
    return withGap(toolLines(entry, width));
  case Entry::Kind::Meta:
    return withGap(wrap(Line{Span{entry.text, DIM}}, width));
  case Entry::Kind::Quote:
    return wrap(Line{Span{entry.text, Style{.color = Color::Green, .italic = true}}}, width);
  case Entry::Kind::Todos:
    return withGap(todoLines(entry.todos, width));
  }
  return {};
}

QList<Line> todoLines(const TodoList& todos, int width) {
  QList<Line> out{Line{
      Span{"● ", fg(Color::Green)},
      Span{"Todos", Style{.bold = true}},
      Span{todos.isEmpty() ? QString(" cleared") : " " + todoProgress(todos), DIM},
  }};
  for (const Todo& todo : todos) {
    Span mark;
    Style style;
    if (todo.status == TodoStatus::Completed) {
      mark = Span{"  ✔ ", fg(Color::Green)};
      style = DIM;
    } else if (todo.status == TodoStatus::InProgress) {
      mark = Span{"  ▶ ", fg(Color::Yellow)};
      style = Style{.bold = true};
    } else {
      mark = Span{"  ○ ", DIM};
    }
    out.append(prefixed(Line{mark}, Line{Span{"    ", {}}}, Line{Span{todo.content, style}}, width));
  }
  return out;
}

QList<Line> diffView(const Diff& diff, int width, int maxLines) {
  int largest = 1;
  for (const DiffLine& line : diff) {
    largest = std::max(largest, line.kind == DiffLine::Kind::Remove ? line.oldLine : line.kind == DiffLine::Kind::Gap ? 0 : line.newLine);
  }
  const int numWidth = QString::number(largest).size();
  auto number = [&](int n) { return Span{QString::number(n).rightJustified(numWidth) + " ", DIM}; };
  QList<Line> lines;
  for (const DiffLine& line : diff) {
    switch (line.kind) {
    case DiffLine::Kind::Gap:
      lines.append(Line{Span{QString(numWidth, ' ') + "   ⋯ " + plural(line.count, "unchanged line"), DIM}});
      break;
    case DiffLine::Kind::Add:
      lines.append(Line{number(line.newLine), Span{"+ ", fg(Color::Green)}, Span{line.text, fg(Color::Green)}});
      break;
    case DiffLine::Kind::Remove:
      lines.append(Line{number(line.oldLine), Span{"- ", fg(Color::Red)}, Span{line.text, fg(Color::Red)}});
      break;
    case DiffLine::Kind::Context:
      lines.append(Line{number(line.newLine), Span{"  ", {}}, Span{line.text, DIM}});
      break;
    }
  }
  QList<Line> shown;
  for (int i = 0; i < std::min(int(lines.size()), maxLines); i++) {
    shown.append(truncate(lines[i], width - 2));
  }
  if (lines.size() > maxLines) {
    shown.append(Line{Span{QString("… %1 more diff lines").arg(lines.size() - maxLines), DIM}});
  }
  return shown;
}

QList<Line> box(const QList<Line>& content, Color color, int width, bool fill) {
  const int max = std::max(1, width - 4);
  int widest = 1;
  for (const Line& line : content) {
    widest = std::max(widest, lineWidth(line));
  }
  const int inner = fill ? max : std::min(max, widest);
  const Style border = fg(color);
  QString rule;
  for (int i = 0; i < inner + 2; i++) {
    rule += QChar(0x2500);
  }
  QList<Line> out{Line{Span{"╭" + rule + "╮", border}}};
  for (const Line& line : content) {
    const Line cut = truncate(line, inner);
    out.append(Line{Span{"│ ", border}} + cut + Line{Span{QString(inner - lineWidth(cut), ' '), {}}, Span{" │", border}});
  }
  out.append(Line{Span{"╰" + rule + "╯", border}});
  return out;
}

} // namespace Smith::Ui
