#include "Backend.h"

#include "core/Errors.h"
#include "core/Jobs.h"
#include "core/Reasoning.h"
#include "frontend/Activity.h"

#include <QCoreApplication>
#include <QJSEngine>
#include <QQmlEngine>

namespace Smith::Desktop {

Backend::Backend(const LaunchOptions& launch, QObject* parent) : QObject(parent), m_launch(launch) {
  try {
    const Config config = loadConfig();
    m_theme = startingTheme(config.theme, &m_themeWarnings);
    m_codeStream = config.codeStream;
  } catch (const std::exception&) {
    // start() reports a broken config file; until then, the default look.
  }
  m_palette = Palette::forTheme(m_theme);
  m_transcript.setPalette(m_palette);

  m_controller.setAppCommands([this](const QString& command, const QString& arg) { return runAppCommand(command, arg); });
  connect(&m_controller, &Controller::changed, this, &Backend::changed);
  connect(&m_controller, &Controller::entryAdded, this, [this](const Entry& entry) { m_transcript.append(entry); });
  connect(&m_controller, &Controller::startedOver, this, [this](const QList<Entry>& entries) {
    m_transcript.clear();
    for (const Entry& entry : entries) {
      m_transcript.append(entry);
    }
  });
  connect(&m_controller, &Controller::agentEvent, this, &Backend::onAgentEvent);
  connect(&m_controller, &Controller::turnEnded, this, [this]() { m_transcript.finishLive(); });
  connect(&m_controller, &Controller::inputRestored, this, &Backend::restoreInput);
  connect(&m_controller, &Controller::inputAppended, this, &Backend::appendInput);
  connect(&m_controller, &Controller::pickerRequested, this, &Backend::onPickerRequested);
  connect(&m_controller, &Controller::themeChanged, this, &Backend::onThemeChanged);
  connect(&m_controller, &Controller::modelsChanged, this, &Backend::changed);
  connect(&m_controller, &Controller::sessionsChanged, this, &Backend::refreshSessions);
  connect(&m_controller, &Controller::projectChanged, this, [this]() {
    // The project may be another repository, or another worktree of this one.
    refreshWorktrees();
    Q_EMIT changed();
  });
  connect(&m_controller, &Controller::redPill, this, &Backend::redPill);
  connect(&m_controller, &Controller::quitRequested, this, &Backend::quit);
  connect(&m_controller.prompts(), &Prompts::changed, this, &Backend::promptChanged);
  connect(&m_controller.fileIndex(), &FileIndex::updated, this, [this]() {
    m_filesListed++;
    Q_EMIT filesChanged();
  });
}

Backend::~Backend() {
  quit();
}

Backend* Backend::create(QQmlEngine*, QJSEngine*) {
  QQmlEngine::setObjectOwnership(s_instance, QQmlEngine::CppOwnership);
  return s_instance;
}

void Backend::setInstance(Backend* backend) {
  s_instance = backend;
}

QStringList Backend::spinnerFrames() const {
  return Smith::spinnerFrames(m_theme.base);
}

QVariantList Backend::themeChoices() const {
  QVariantList out;
  for (const ThemeSpec& theme : listThemes().themes) {
    out << QVariantMap{{"value", theme.name}, {"label", theme.name}, {"hint", themeHint(theme)}};
  }
  return out;
}

QStringList Backend::reasoningLevels() const {
  return REASONING_LEVELS;
}

QString Backend::version() const {
  return SMITH_VERSION;
}

// ---- startup ----

void Backend::launch(std::function<void()> work, std::function<void()> finished) {
  m_controller.launch(std::move(work), std::move(finished));
}

void Backend::background(const QString& activity, std::function<std::function<void()>()> work) {
  m_controller.runTask(activity, std::move(work));
}

void Backend::start() {
  if (m_state == "ready") {
    return;
  }
  m_state = "starting";
  m_startupError.clear();
  Q_EMIT changed();
  auto runtime = std::make_shared<std::shared_ptr<Runtime>>();
  auto error = std::make_shared<QString>();
  const LaunchOptions options = m_launch;
  launch(
      [runtime, error, options, closing = m_controller.turns().closing()]() {
        try {
          *runtime = Runtime::start(options, closing);
        } catch (const std::exception& failure) {
          *error = errorMessage(failure);
        }
      },
      [this, runtime, error]() {
        if (!error->isEmpty()) {
          m_state = "failed";
          m_startupError = *error;
          Q_EMIT changed();
          return;
        }
        m_pRuntime = *runtime;
        ready();
      });
}

void Backend::ready() {
  ControllerOptions options = controllerOptions(m_pRuntime);
  options.theme = m_theme;
  options.warnings << m_themeWarnings;
  m_controller.attach(options);
  m_state = "ready";
  m_controller.start();
  Q_EMIT changed();
}

void Backend::notice(NoticeLevel level, const QString& text) {
  m_controller.notice(level, text);
}

// ---- agent events ----

void Backend::onAgentEvent(const AgentEvent& event, const std::optional<Entry>& entry) {
  if (m_closed) {
    return;
  }
  if (const auto* delta = std::get_if<Event::Delta>(&event)) {
    m_controller.turns().setActivity(delta->kind == BlockKind::Text ? "Writing…" : "Thinking…");
    m_transcript.appendLive(delta->kind, delta->text);
  } else if (std::holds_alternative<Event::ResponseEnd>(event) || std::holds_alternative<Event::ToolPrepare>(event)) {
    m_transcript.finishLive();
  }
  if (entry) {
    m_transcript.append(*entry);
  }
}

void Backend::onPickerRequested(const QString& kind, const QString& arg) {
  if (kind == "sessions") {
    setAllSessions(arg == "all");
  }
  Q_EMIT openPicker(kind == "open" ? QString("folder") : kind);
}

void Backend::onThemeChanged() {
  m_theme = m_controller.theme();
  m_palette = Palette::forTheme(m_theme);
  m_transcript.setPalette(m_palette);
  Q_EMIT themeChanged();
}

// ---- approvals and questions ----

void Backend::decide(const QString& decision) {
  Prompts& prompts = m_controller.prompts();
  if (!prompts.approval()) {
    return;
  }
  if (decision == "allow") {
    prompts.decide(ApprovalDecision::Allow);
  } else if (decision == "always") {
    prompts.decide(ApprovalDecision::AllowAlways);
  } else if (decision == "similar") {
    prompts.decide(ApprovalDecision::AllowSimilar);
  } else {
    prompts.decide(ApprovalDecision::Deny);
    if (decision == "interrupt") {
      m_controller.interrupt();
    }
  }
}

void Backend::answerOption(int index) {
  m_controller.prompts().answer(Answer{Answer::Kind::Option, index, {}});
}

void Backend::answerText(const QString& text) {
  if (!text.trimmed().isEmpty()) {
    m_controller.prompts().answer(Answer{Answer::Kind::Text, 0, text.trimmed()});
  }
}

void Backend::interrupt() {
  m_controller.interrupt();
}

// ---- messages ----

void Backend::submit(const QString& value) {
  if (m_state == "ready") {
    m_controller.submit(value);
  }
}

void Backend::queueNext(const QString& value) {
  m_controller.queueNext(value);
}

void Backend::editNext(int index) {
  if (index >= 0 && index < nextTasks().size()) {
    Q_EMIT appendInput(m_controller.takeNext(index));
  }
}

void Backend::dropNext(int index) {
  if (index >= 0 && index < nextTasks().size()) {
    m_controller.takeNext(index);
  }
}

// ---- shutdown ----

void Backend::quit() {
  if (m_closed) {
    return;
  }
  m_closed = true;
  m_controller.close();
  if (m_pRuntime) {
    m_pRuntime->shutdown();
  }
  QCoreApplication::quit();
}

} // namespace Smith::Desktop
