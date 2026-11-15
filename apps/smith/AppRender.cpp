#include "App.h"

#include "tui/Markdown.h"
#include "tui/Text.h"
#include "frontend/Duration.h"
#include "View.h"

#include <QRegularExpression>

namespace Smith::Ui {

using Tui::DIM;
using Tui::fg;
using Tui::Line;
using Tui::prefixed;
using Tui::REASONING;
using Tui::Span;
using Tui::Style;
using Tui::truncate;
using Tui::wrap;

namespace {

// Columns and rows of the rain shown while the conversation is compacted (matrix theme).
constexpr int STRIP_COLS = 22;
constexpr int STRIP_ROWS = 3;
// Spinner frames per step of the status line's code stream.
constexpr int STREAM_EVERY = 4;

} // namespace

// ---- drawing ----

void App::requestRender() {
  if (m_closed || m_renderTimer.isActive()) {
    return;
  }
  m_renderTimer.start();
}

void App::render() {
  if (m_closed || m_pFullRain) {
    return;
  }
  // The animations only need a timer while one is on screen.
  if (!m_turns.compacting()) {
    m_pStrip.reset();
  }
  const bool animating = (!m_turns.activity().isEmpty() && !prompting()) || streaming();
  if (animating && !m_ticker.isActive()) {
    m_ticker.start();
  } else if (!animating && m_ticker.isActive()) {
    m_ticker.stop();
  }
  m_renderer.draw(liveLines());
}

QList<Line> App::liveLines() {
  const int w = width();
  QList<Line> lines;
  if (m_live && !m_live->text.isEmpty()) {
    lines.append(Line{});
    if (m_live->kind == BlockKind::Text) {
      lines.append(Tui::markdownLines(m_live->text, w));
    } else {
      lines.append(wrap(Line{Span{m_live->text, REASONING}}, w));
    }
  }
  const QString activity = m_turns.activity();
  if (!activity.isEmpty() && !prompting()) {
    const QString elapsed = m_turns.elapsed() ? " · " + formatDuration(*m_turns.elapsed()) : QString();
    if (m_turns.compacting() && matrix() && Tui::colorsEnabled()) {
      // A strip of rain, with the activity beside its middle row.
      if (!m_pStrip) {
        Tui::Rain::Options options;
        options.frames = std::numeric_limits<double>::infinity();
        m_pStrip = std::make_unique<Tui::Rain>(STRIP_COLS, STRIP_ROWS, options);
        // Already raining when it appears.
        for (int i = 0; i < 12; i++) {
          m_pStrip->step();
        }
      }
      QList<Line> rain = m_pStrip->lines();
      rain[1].append(Span{"  Purging old memories…" + elapsed, DIM});
      lines.append(Line{});
      for (const Line& line : rain) {
        lines.append(truncate(line, w));
      }
    } else {
      const Spinner& spinner = spinnerFor(m_options.theme);
      lines.append(Line{});
      lines.append(truncate(Line{Span{spinner.frames[m_frame % spinner.frames.size()], fg(spinner.color)}, Span{" " + activity + elapsed, DIM}}, w));
    }
    // The step the agent is on, from its checklist.
    const TodoList todos = m_pAgent->todos();
    for (const Todo& todo : todos) {
      if (todo.status == TodoStatus::InProgress && m_turns.busy()) {
        const int done = todos.size() - unfinished(todos).size();
        lines.append(truncate(Line{Span{QString("  ▶ %1/%2 ").arg(done + 1).arg(todos.size()), fg(Color::Yellow)}, Span{todo.content, DIM}}, w));
        break;
      }
    }
  }
  if (const ApprovalRequest* approval = m_prompts.approval()) {
    lines.append(Line{});
    lines.append(approvalBox(*approval, w, m_terminal.rows()));
  } else if (const Question* question = m_prompts.question()) {
    lines.append(Line{});
    lines.append(questionBox(*question, m_questionSelected, w));
    const QList<Line> answer = prefixed(Line{Span{"› ", fg(Color::Cyan)}}, Line{Span{"  ", {}}}, m_editor.render("Your answer"), w - 4);
    lines.append(box(answer, Color::Gray, w, true));
  } else if (m_picker) {
    lines.append(Line{});
    lines.append(box(m_picker->picker->lines(), Color::Cyan, w));
  } else if (m_textPrompt && !m_turns.busy()) {
    lines.append(Line{});
    lines.append(truncate(Line{Span{m_textPrompt->title, Style{.bold = true}}, Span{"  enter to confirm · esc cancels", DIM}}, w));
    const QList<Line> prompt = prefixed(Line{Span{"› ", fg(Color::Yellow)}}, Line{Span{"  ", {}}}, m_editor.render({}), w - 4);
    lines.append(box(prompt, Color::Yellow, w, true));
  } else if (!m_turns.busy() || agentRunning()) {
    const QStringList queued = m_pAgent->queuedMessages();
    if (!queued.isEmpty()) {
      lines.append(Line{});
      for (const QString& text : queued) {
        lines.append(prefixed(Line{Span{"› ", DIM}}, Line{Span{"  ", {}}}, Line{Span{text, DIM}}, w));
      }
      lines.append(Line{Span{"  queued: the agent gets this at its next step", Style{.dim = true, .italic = true}}});
    }
    const QString placeholder = m_turns.busy() ? "Type to add to what the agent is doing (enter queues it)" : "Ask for something, or /help";
    const QList<Line> prompt = prefixed(Line{Span{"› ", fg(Color::Cyan)}}, Line{Span{"  ", {}}}, m_editor.render(placeholder), w - 4);
    lines.append(Line{});
    lines.append(box(prompt, Color::Gray, w, true));
    lines.append(m_suggestions.lines(currentSuggestions(), w));
  }
  Status status;
  status.endpoint = m_pServer->name();
  status.model = m_pAgent->model();
  status.reasoning = m_pAgent->reasoning();
  status.tokens = m_turns.tokens();
  status.limit = m_turns.limit();
  status.sandboxActive = m_options.sandbox->active();
  status.sandboxType = m_options.sandbox->type();
  status.network = m_options.sandbox->network();
  status.busy = m_turns.busy();
  status.queueing = agentRunning();
  status.jobs = m_options.jobs ? m_options.jobs->running() : 0;
  status.worktreeBranch = m_project.worktreeBranch;
  if (streaming()) {
    const int frame = m_frame;
    status.filler = [frame](int n) { return Tui::codeStream(frame / STREAM_EVERY, n); };
  }
  lines.append(statusLine(status, w));
  return lines;
}

// ---- agent events ----

void App::onEvent(const AgentEvent& event) {
  if (m_closed) {
    return;
  }
  const std::optional<Entry> entry = m_turns.apply(event);
  if (const auto* delta = std::get_if<Event::Delta>(&event)) {
    appendDelta(delta->kind, delta->text);
  } else if (entry || std::holds_alternative<Event::ResponseEnd>(event) || std::holds_alternative<Event::ToolPrepare>(event)) {
    // What streamed so far goes above whatever comes next.
    flushLive();
  }
  if (entry) {
    print(*entry);
  }
  requestRender();
}

void App::appendDelta(BlockKind kind, const QString& text) {
  // Streamed text stays in the live region until a paragraph is complete;
  // then it is printed for good, so the part being redrawn stays small.
  if (m_live && m_live->kind != kind) {
    flushLive();
  }
  QString next;
  if (m_live) {
    next = m_live->text + text;
  } else {
    static const QRegularExpression leading(R"(^\s+)");
    next = QString(text).remove(leading);
  }
  const int cut = Tui::safeSplit(next);
  if (cut > 0) {
    QString done = next.left(cut);
    while (!done.isEmpty() && done.back().isSpace()) {
      done.chop(1);
    }
    print(Entry::simple(kind == BlockKind::Text ? Entry::Kind::Text : Entry::Kind::Reasoning, done));
    next = next.mid(cut);
    while (next.startsWith('\n')) {
      next.remove(0, 1);
    }
  }
  m_live = Live{kind, next};
}

void App::flushLive() {
  if (m_live && !m_live->text.trimmed().isEmpty()) {
    QString text = m_live->text;
    while (!text.isEmpty() && text.back().isSpace()) {
      text.chop(1);
    }
    print(Entry::simple(m_live->kind == BlockKind::Text ? Entry::Kind::Text : Entry::Kind::Reasoning, text));
  }
  m_live.reset();
}

} // namespace Smith::Ui
