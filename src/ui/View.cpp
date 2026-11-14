#include "ui/View.h"

#include "core/Tool.h"
#include "tui/Text.h"
#include "ui/Entries.h"

#include <cmath>

namespace Smith::Ui {

using Tui::DIM;
using Tui::fg;
using Tui::Line;
using Tui::lineWidth;
using Tui::prefixed;
using Tui::Span;
using Tui::Style;
using Tui::truncate;
using Tui::wrap;

const Spinner& spinnerFor(Theme theme) {
  static const Spinner plain{{"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"}, Color::Yellow};
  // Half-width katakana, one column wide like the braille dots.
  static const Spinner matrix = []() {
    Spinner spinner{{}, Color::Green};
    for (const QChar c : QString::fromUtf8("ﾊﾐﾋｰｳｼﾅﾓﾆｻﾜﾂｵﾘｱﾎﾃﾏｹﾒｴｶｷﾑﾕﾗｾﾈｽﾀﾇﾍ")) {
      spinner.frames << QString(c);
    }
    return spinner;
  }();
  return theme == Theme::Matrix ? matrix : plain;
}

QList<Line> approvalBox(const ApprovalRequest& request, int width, int rows) {
  QList<Line> content{Line{Span{QString("Allow %1?").arg(request.tool), Style{.bold = true}}}};
  const QList<Line> summary = wrap(Line{Span{request.summary, {}}}, width - 4);
  content.append(summary.mid(0, 12));
  if (!request.note.isEmpty()) {
    content.append(Line{Span{request.note, fg(Color::Yellow)}});
  }
  if (!request.warning.isEmpty()) {
    for (const QString& line : request.warning.split('\n')) {
      content.append(wrap(Line{Span{"⚠ " + line, Style{.color = Color::Red, .bold = true}}}, width - 4));
    }
  }
  if (summary.size() > 12) {
    content.append(Line{Span{QString("… %1 more lines").arg(summary.size() - 12), DIM}});
  }
  if (request.preview) {
    // Leave room for the rest of the screen, but show as much of the change as fits.
    const int room = std::max(6, rows - int(content.size()) - 10);
    content.append(Line{});
    content.append(diffView(*request.preview, width - 4, room));
    content.append(Line{});
  }
  if (request.hasSandbox) {
    content.append(Line{Span{(request.sandboxActive ? "✓ " : "⚠ ") + request.sandboxDescription,
                             fg(request.sandboxActive ? Color::Green : Color::Red)}});
  }
  content.append(Line{
      Span{"y", fg(Color::Green)},
      Span{" yes · ", DIM},
      Span{"a", fg(Color::Green)},
      Span{QString(" always allow %1 this session · ").arg(request.tool), DIM},
      Span{"n", fg(Color::Red)},
      Span{" no · ", DIM},
      Span{"esc", fg(Color::Red)},
      Span{" no and interrupt", DIM},
  });
  return box(content, Color::Yellow, width, true);
}

QList<Line> questionBox(const Question& question, int selected, int width) {
  QList<Line> content = wrap(Line{Span{question.question, Style{.bold = true}}}, width - 4);
  if (!question.options.isEmpty()) {
    content.append(Line{});
    for (int i = 0; i < question.options.size(); i++) {
      const bool on = i == selected;
      content.append(prefixed(Line{Span{on ? "› " : "  ", fg(Color::Cyan)}, Span{QString("%1. ").arg(i + 1), DIM}}, Line{Span{"     ", {}}},
                              Line{Span{question.options[i], on ? Style{.color = Color::Cyan, .bold = true} : Style{}}}, width - 4));
    }
  }
  content.append(Line{});
  const QString keys = question.options.isEmpty()
                           ? QString("type your answer below · esc dismisses")
                           : QString("↑↓ and enter, or 1-%1, picks · or type your own answer below · esc dismisses").arg(question.options.size());
  content.append(wrap(Line{Span{keys, DIM}}, width - 4));
  return box(content, Color::Cyan, width, true);
}

Line statusLine(const Status& status, int width) {
  auto k = [](qint64 n) { return n >= 1024 ? QString("%1k").arg(std::lround(n / 1024.0)) : QString::number(n); };
  QStringList parts;
  if (!status.endpoint.isEmpty()) {
    parts << status.endpoint;
  }
  parts << status.model << "reasoning " + status.reasoning;
  if (status.tokens) {
    if (status.limit && *status.limit > 0) {
      parts << QString("%1% of %2 context").arg(std::lround(100.0 * *status.tokens / *status.limit)).arg(k(*status.limit));
    } else {
      parts << k(*status.tokens) + " tokens";
    }
  }
  if (status.sandboxActive) {
    parts << QString("%1, net %2").arg(status.sandboxType == SandboxType::Container ? "container" : "sandbox", status.network ? "on" : "off");
  }
  if (status.jobs) {
    parts << plural(status.jobs, "background job");
  }
  if (status.busy) {
    parts << (status.queueing ? "esc to interrupt · enter queues" : "esc to interrupt");
  }
  Line line;
  if (!status.sandboxActive) {
    line.append(Span{"unsandboxed · ", fg(Color::Red)});
  }
  line.append(Span{parts.join(" · "), DIM});
  const int room = std::min(24, width - lineWidth(line) - 3);
  if (status.filler && room >= 6) {
    line.append(Span{QString(width - lineWidth(line) - room, ' '), {}});
    line.append(Span{status.filler(room), Style{.color = Color::Green, .dim = true}});
  }
  return truncate(line, width);
}

QString runningLabel(const QStringList& running) {
  if (running.isEmpty()) {
    return "Thinking…";
  }
  if (running.size() == 1) {
    return running.first();
  }
  QStringList names;
  for (const QString& label : running) {
    names << label.section(' ', 0, 0);
  }
  return QString("%1 tools in parallel: %2").arg(running.size()).arg(names.join(", "));
}

} // namespace Smith::Ui
