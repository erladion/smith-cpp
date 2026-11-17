#include "App.h"
#include "TerminalTheme.h"

#include "core/Errors.h"
#include "core/Jobs.h"
#include "core/Tool.h"
#include "frontend/Commands.h"
#include "Connection.h"
#include "SessionCommands.h"
#include "WorktreeCommands.h"
#include "frontend/Worktrees.h"

namespace Smith::Ui {

using Tui::Key;

App::App(TuiOptions options, QObject* parent)
    : QObject(parent), m_options(std::move(options)),
      m_renderer(Tui::Screen{[this](const QString& text) { m_terminal.write(text); }, [this]() { return m_terminal.columns(); },
                             [this]() { return m_terminal.rows(); }}) {
  Tui::setPalette(terminalPalette(m_options.controller.theme));

  m_controller.setHelp(&commandHelp);
  m_controller.setAppCommands([this](const QString& command, const QString& arg) { return runAppCommand(command, arg); });
  m_controller.setThemeNote([](const ThemeSpec& theme) {
    QString note;
    if (theme.base == Theme::Light) {
      note = " It suits a terminal with a light background.";
    }
    if (Tui::detectColorDepth() == Tui::ColorDepth::Basic) {
      note += " This terminal shows 16 colors, so its own shades stay.";
    }
    return note;
  });
  connect(&m_controller, &Controller::changed, this, &App::requestRender);
  connect(&m_controller, &Controller::entryAdded, this, &App::print);
  connect(&m_controller, &Controller::startedOver, this, &App::startOver);
  connect(&m_controller, &Controller::agentEvent, this, &App::onAgentEvent);
  connect(&m_controller, &Controller::turnEnded, this, &App::flushLive);
  connect(&m_controller, &Controller::inputRestored, this, [this](const QString& text) { m_editor.setValue(text); });
  connect(&m_controller, &Controller::inputAppended, this, [this](const QString& text) {
    const QString typing = m_editor.value().trimmed();
    m_editor.setValue(typing.isEmpty() ? text : typing + "\n\n" + text);
  });
  connect(&m_controller, &Controller::pickerRequested, this, &App::openPickerFor);
  connect(&m_controller, &Controller::themeChanged, this, &App::applyTheme);
  connect(&m_controller, &Controller::modelsChanged, this, [this]() {
    QStringList ids;
    for (const ModelInfo& model : m_controller.models()) {
      ids << model.id;
    }
    m_suggestions.models = ids;
  });
  connect(&m_controller, &Controller::projectChanged, this, &App::refreshTasks);
  connect(&m_controller, &Controller::redPill, this, &App::playRedPill);
  connect(&m_controller, &Controller::quitRequested, this, &App::exitApp);
  connect(&m_controller.fileIndex(), &FileIndex::updated, this, &App::requestRender);
  m_suggestions.files = [this]() { return m_controller.files(); };

  Prompts& prompts = m_controller.prompts();
  connect(&prompts, &Prompts::questionAsked, this, [this]() {
    // Whatever was typed meanwhile (to queue) stays for after; the answer starts empty.
    m_stashed = m_editor.value();
    m_editor.setValue({});
    m_questionSelected = 0;
  });
  connect(&prompts, &Prompts::changed, this, &App::requestRender);

  m_controller.attach(m_options.controller);
  m_pConnection = std::make_unique<ConnectionCommands>(*this);
  m_pSessions = std::make_unique<SessionCommands>(*this);
  m_pWorktrees = std::make_unique<WorktreeCommands>(*this);

  m_renderTimer.setSingleShot(true);
  m_renderTimer.setInterval(16);
  connect(&m_renderTimer, &QTimer::timeout, this, &App::render);
  m_ticker.setInterval(80);
  connect(&m_ticker, &QTimer::timeout, this, [this]() {
    m_frame++;
    if (m_pStrip) {
      m_pStrip->step();
    }
    requestRender();
  });
  m_resizeTimer.setSingleShot(true);
  m_resizeTimer.setInterval(100);
  connect(&m_resizeTimer, &QTimer::timeout, this, &App::reprint);
  m_rainTimer.setInterval(33);
  connect(&m_rainTimer, &QTimer::timeout, this, [this]() {
    if (!m_pFullRain) {
      return;
    }
    m_pFullRain->step();
    m_terminal.write("\x1b[?2026h" + m_pFullRain->render() + "\x1b[?2026l");
    if (m_pFullRain->done()) {
      stopRain();
    }
  });

  connect(&m_terminal, &Tui::Terminal::input, this, &App::onInput);
  connect(&m_terminal, &Tui::Terminal::resized, this, &App::onResize);
  connect(&m_terminal, &Tui::Terminal::terminate, this, &App::exitApp);
}

App::~App() {
  exitApp();
}

void App::run() {
  m_terminal.enableRaw();
  m_terminal.setReading(true);
  auto start = [this]() {
    // Hide the real cursor (we draw our own), turn on bracketed paste, and ask
    // for the kitty keyboard protocol so Shift+Enter is distinguishable from
    // Enter. Terminals that don't know a request ignore it.
    m_terminal.write("\x1b[?25l\x1b[?2004h\x1b[>1u");
    m_controller.start();
    render();
  };
  if (matrix() && Tui::colorsEnabled()) {
    Tui::Rain::Options rain;
    rain.title = "AGENT SMITH";
    rain.color = rainColor();
    playRain(rain, start);
  } else {
    start();
  }
}

// ---- for the command modules ----

void App::print(const Entry& entry) {
  m_transcript.append(entry);
  m_printedWidth = width();
  m_renderer.print(entryLines(entry, width()));
  requestRender();
}

void App::startOver(const QList<Entry>& entries) {
  m_renderer.reset();
  m_transcript.clear();
  m_live.reset();
  for (const Entry& entry : entries) {
    print(entry);
  }
}

void App::refreshTasks() {
  const QString cwd = m_controller.project().cwd;
  m_controller.launch([this, cwd]() {
    QStringList tasks;
    try {
      for (const Worktree& worktree : listWorktrees(cwd)) {
        if (!worktree.task.isEmpty()) {
          tasks << worktree.task;
        }
      }
    } catch (const std::exception&) {
      // Not a repository: nothing to complete.
    }
    QMetaObject::invokeMethod(this, [this, tasks]() { m_suggestions.tasks = tasks; }, Qt::QueuedConnection);
  });
}

void App::promptText(const QString& title, const QString& initial, std::function<void(const QString&)> onSubmit) {
  m_textPrompt = TextPrompt{title, std::move(onSubmit), m_editor.value()};
  m_editor.setValue(initial);
  m_suggestions.reset();
  requestRender();
}

void App::openPicker(std::unique_ptr<Tui::Picker> picker, std::function<void(const QString&)> onPick, std::function<void(const QString&)> onAction) {
  m_picker = OpenPicker{std::move(picker), std::move(onPick), std::move(onAction)};
  requestRender();
}

void App::withActivity(const QString& activity, std::function<std::function<void()>()> work) {
  m_controller.runTask(activity, std::move(work));
}

// ---- terminal plumbing ----

void App::onInput(const QString& data) {
  if (m_pFullRain) {
    // Any key ends the rain, and is used up doing so.
    stopRain();
    return;
  }
  for (const Key& key : m_keys.feed(data)) {
    if (m_closed) {
      return;
    }
    onKey(key);
  }
  requestRender();
}

void App::onResize() {
  if (m_pFullRain) {
    stopRain();
    return;
  }
  // Wait for dragging to settle, then reprint once.
  m_resizeTimer.start();
}

void App::reprint() {
  if (m_closed || m_pFullRain) {
    return;
  }
  if (width() != m_printedWidth) {
    m_renderer.reset();
    m_printedWidth = width();
    for (const Entry& entry : m_transcript) {
      m_renderer.print(entryLines(entry, width()));
    }
  }
  render();
}

void App::restoreTerminal() {
  m_terminal.write("\x1b[<u\x1b[?2004l\x1b[?25h");
  m_terminal.restore();
}

void App::exitApp() {
  if (m_closed) {
    return;
  }
  m_closed = true;
  m_ticker.stop();
  m_renderTimer.stop();
  m_resizeTimer.stop();
  m_rainTimer.stop();
  if (m_pFullRain) {
    m_terminal.write("\x1b[0m\x1b[?1049l");
    m_pFullRain.reset();
  }
  m_renderer.finish();
  restoreTerminal();
  m_terminal.setReading(false);
  m_controller.close();
  Q_EMIT finished();
}

bool App::matrix() const {
  return m_controller.matrix();
}

Tui::Color App::rainColor() const {
  return matrix() ? Tui::Color::Green : Tui::Color::Blue;
}

bool App::streaming() const {
  return matrix() && m_options.codeStream && Tui::colorsEnabled() && !m_controller.turns().busy();
}

int App::width() const {
  return std::max(20, m_terminal.columns());
}

bool App::prompting() const {
  return m_controller.prompts().waiting();
}

bool App::agentRunning() const {
  return m_controller.turns().agentRunning();
}

// ---- the rain ----

void App::playRain(const Tui::Rain::Options& options, std::function<void()> then) {
  const int cols = m_terminal.columns();
  const int rows = m_terminal.rows();
  if (cols < 30 || rows < 10) {
    then();
    return;
  }
  m_pFullRain = std::make_unique<Tui::Rain>(cols, rows, options);
  m_afterRain = std::move(then);
  m_terminal.write("\x1b[?1049h\x1b[?25l\x1b[2J");
  m_rainTimer.start();
}

void App::stopRain() {
  if (!m_pFullRain) {
    return;
  }
  m_rainTimer.stop();
  m_pFullRain.reset();
  // Leave the alternate screen: the normal screen and its scrollback come back as they were.
  m_terminal.write("\x1b[0m\x1b[?1049l");
  const auto then = std::move(m_afterRain);
  m_afterRain = {};
  if (then) {
    then();
  }
}

} // namespace Smith::Ui
