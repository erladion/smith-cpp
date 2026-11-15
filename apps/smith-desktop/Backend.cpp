#include "Backend.h"

#include "core/Errors.h"
#include "core/Images.h"
#include "core/Jobs.h"
#include "core/Reasoning.h"
#include "tui/Rain.h"
#include "frontend/Duration.h"
#include "frontend/Greetings.h"
#include "frontend/Replay.h"
#include "frontend/Activity.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJSEngine>
#include <QQmlEngine>

namespace Smith::Desktop {

Backend::Backend(const LaunchOptions& launch, QObject* parent) : QObject(parent), m_launch(launch) {
  try {
    const Config config = loadConfig();
    m_theme = config.theme;
    m_codeStream = config.codeStream;
  } catch (const std::exception&) {
    // start() reports a broken config file; until then, the default look.
  }
  m_palette = Palette::forTheme(m_theme);
  m_transcript.setPalette(m_palette);

  // The agent calls these from its own threads.
  m_hooks.onEvent = [this](const AgentEvent& event) { QMetaObject::invokeMethod(this, [this, event]() { onEvent(event); }, Qt::QueuedConnection); };
  m_hooks.approve = [this](const ApprovalRequest& request) { return m_prompts.approve(request); };
  m_hooks.ask = [this](const Question& question) { return m_prompts.ask(question); };
  m_prompts.refuseWhen([this]() { return m_turns.interrupted(); });
  connect(&m_prompts, &Prompts::changed, this, &Backend::promptChanged);
  connect(&m_turns, &TurnRunner::changed, this, &Backend::changed);
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
  return Smith::spinnerFrames(m_theme);
}

QStringList Backend::reasoningLevels() const {
  return REASONING_LEVELS;
}

QString Backend::version() const {
  return SMITH_VERSION;
}

// ---- startup ----

void Backend::launch(std::function<void()> work, std::function<void()> finished) {
  m_turns.launch(std::move(work), std::move(finished));
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
      [runtime, error, options, closing = m_turns.closing()]() {
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
  m_pServer = m_pRuntime->server();
  m_project = m_pRuntime->project();
  m_sessionControl = m_pRuntime->sessions();
  m_pAgent = m_pRuntime->createAgent(m_hooks);
  connect(m_pRuntime->jobs().get(), &BackgroundJobs::changed, this, &Backend::changed, Qt::QueuedConnection);
  m_state = "ready";
  const Messages resumed = m_pRuntime->resumedHistory();
  printBanner(!resumed.isEmpty(), m_pRuntime->resumedLastUsed());
  for (const QString& warning : m_pRuntime->warnings()) {
    notice(NoticeLevel::Warn, warning);
  }
  for (const Entry& entry : historyToEntries(resumed, m_project.tools)) {
    m_transcript.append(entry);
  }
  refreshModels();
  refreshSessions();
  refreshWorktrees();
  Q_EMIT changed();
}

void Backend::printBanner(bool resumed, const std::optional<QDateTime>& lastUsed) {
  GreetingContext context;
  context.resumed = resumed;
  context.lastUsed = lastUsed;
  if (!resumed) {
    // A folder no other session has worked in gets its own welcome.
    const QString current = m_sessionControl.currentId();
    context.newFolder = true;
    for (const SessionInfo& info : m_sessionControl.list(false)) {
      if (info.id != current) {
        context.newFolder = false;
      }
    }
  }
  Entry banner;
  banner.kind = Entry::Kind::Banner;
  banner.model = m_pAgent->model();
  banner.server = m_pServer->label();
  banner.cwd = m_project.cwd;
  const QString name = m_sessionControl.currentName();
  banner.session = name.isEmpty() ? m_sessionControl.currentId() : name + " · " + m_sessionControl.currentId();
  banner.resumed = resumed;
  banner.sandboxActive = m_pRuntime->sandbox()->active();
  banner.sandboxDescription = m_pRuntime->sandbox()->describe(m_project.cwd);
  banner.instructions = m_project.instructions;
  for (const Skill& skill : m_project.skills) {
    banner.skills << skill.name;
  }
  if (matrix()) {
    banner.greeting = greeting(context);
  }
  m_transcript.append(banner);
}

void Backend::notice(NoticeLevel level, const QString& text) {
  m_transcript.append(Entry::notice(level, text));
}

// ---- agent events ----

void Backend::onEvent(const AgentEvent& event) {
  if (m_closed) {
    return;
  }
  const std::optional<Entry> entry = m_turns.apply(event);
  if (const auto* delta = std::get_if<Event::Delta>(&event)) {
    m_turns.setActivity(delta->kind == BlockKind::Text ? "Writing…" : "Thinking…");
    m_transcript.appendLive(delta->kind, delta->text);
  } else if (std::holds_alternative<Event::ResponseEnd>(event) || std::holds_alternative<Event::ToolPrepare>(event)) {
    m_transcript.finishLive();
  }
  if (entry) {
    m_transcript.append(*entry);
  }
  Q_EMIT changed();
}

void Backend::decide(const QString& decision) {
  if (!m_prompts.approval()) {
    return;
  }
  if (decision == "allow") {
    m_prompts.decide(ApprovalDecision::Allow);
  } else if (decision == "always") {
    m_prompts.decide(ApprovalDecision::AllowAlways);
  } else if (decision == "similar") {
    m_prompts.decide(ApprovalDecision::AllowSimilar);
  } else {
    m_prompts.decide(ApprovalDecision::Deny);
    if (decision == "interrupt") {
      m_turns.interrupt();
    }
  }
}

void Backend::answerOption(int index) {
  m_prompts.answer(Answer{Answer::Kind::Option, index, {}});
}

void Backend::answerText(const QString& text) {
  if (!text.trimmed().isEmpty()) {
    m_prompts.answer(Answer{Answer::Kind::Text, 0, text.trimmed()});
  }
}

void Backend::dismissQuestion() {
  m_prompts.answer(Answer{});
}

void Backend::interrupt() {
  m_prompts.dismiss();
  m_turns.interrupt();
}

// ---- turns ----

void Backend::submit(const QString& value) {
  const QString text = value.trimmed();
  if (text.isEmpty() || m_state != "ready") {
    return;
  }
  if (m_turns.busy()) {
    if (text.startsWith('/')) {
      notice(NoticeLevel::Warn, "Commands work once the agent is done. Your command is still in the input box.");
      Q_EMIT restoreInput(value);
    } else if (agentRunning()) {
      // Delivered at the agent's next step.
      m_pAgent->queue(text);
    } else {
      Q_EMIT restoreInput(value);
    }
    Q_EMIT changed();
    return;
  }
  if (text.startsWith('/')) {
    runCommand(text);
    return;
  }
  m_transcript.append(Entry::simple(Entry::Kind::User, text));
  // Image files the message names go along with it.
  const ImageRefs refs = imagesIn(text, m_project.cwd);
  if (!refs.attached.isEmpty()) {
    notice(NoticeLevel::Info, "Attached " + refs.attached.join(", ") + ".");
  }
  for (const QString& problem : refs.problems) {
    notice(NoticeLevel::Warn, problem);
  }
  const std::shared_ptr<Agent> agent = m_pAgent;
  const QList<Image> images = refs.images;
  runTurn(
      "Thinking…", [agent, text, images](const CancelPtr& cancel) { agent->send(text, cancel, images); },
      [this]() {
        const TurnStats& turn = m_turns.lastTurn();
        m_transcript.append(Entry::simple(Entry::Kind::Meta, turnSummary(turn.reason, turn.elapsedMs, turn.toolCalls)));
        if (turn.reason == "interrupted" && matrix()) {
          m_transcript.append(Entry::simple(Entry::Kind::Quote, INTERRUPTED));
        }
        refreshSessions();
      },
      true);
}

void Backend::runTurn(const QString& activity, std::function<void(const CancelPtr&)> work, std::function<void()> then, bool timed) {
  m_turns.runTurn(
      activity, m_pServer->baseUrl(), std::move(work),
      [this, then](const QString& error) {
        m_transcript.finishLive();
        if (!error.isEmpty()) {
          notice(NoticeLevel::Error, error);
        }
        // Messages the agent never got (interrupted, error, step limit) go back into the input.
        const QStringList undelivered = m_pAgent->takeQueued();
        if (!undelivered.isEmpty()) {
          notice(NoticeLevel::Warn, "Your queued message wasn't delivered, so it's back in the input box.");
          Q_EMIT restoreInput(undelivered.join("\n\n"));
        }
        if (then) {
          then();
        }
      },
      timed);
}

void Backend::background(const QString& activity, std::function<std::function<void()>()> work) {
  m_turns.runTask(activity, m_pServer ? m_pServer->baseUrl() : QString(), std::move(work),
                  [this](const QString& error) { notice(NoticeLevel::Error, error); });
}

// ---- shutdown ----

void Backend::quit() {
  if (m_closed) {
    return;
  }
  m_closed = true;
  m_turns.interrupt();
  m_prompts.close();
  m_turns.close();
  if (m_pRuntime) {
    m_pRuntime->shutdown();
  }
  QCoreApplication::quit();
}

} // namespace Smith::Desktop
