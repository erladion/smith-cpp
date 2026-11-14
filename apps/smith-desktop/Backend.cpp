#include "Backend.h"

#include "core/Errors.h"
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
  m_hooks.approve = [this](const ApprovalRequest& request) { return approveFromWorker(request); };
  m_hooks.ask = [this](const Question& question) { return askFromWorker(question); };
}

Backend::~Backend() {
  quit();
}

Backend* Backend::create(QQmlEngine*, QJSEngine*) {
  QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
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

QThread* Backend::launch(std::function<void()> work, std::function<void()> finished) {
  QThread* thread = QThread::create(std::move(work));
  m_workers << thread;
  connect(thread, &QThread::finished, this, [this, thread, finished]() {
    m_workers.removeAll(thread);
    thread->deleteLater();
    if (!m_closed && finished) {
      finished();
    }
  });
  thread->start();
  return thread;
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
      [runtime, error, options]() {
        try {
          *runtime = Runtime::start(options);
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
  auto label = [this]() {
    QStringList labels;
    for (const QString& id : m_runningOrder) {
      labels << m_running.value(id);
    }
    return runningLabel(labels);
  };
  switch (event.type) {
  case AgentEvent::Type::Delta:
    setCompacting(false);
    m_activity = event.kind == BlockKind::Text ? "Writing…" : "Thinking…";
    m_transcript.appendLive(event.kind, event.text);
    break;
  case AgentEvent::Type::ResponseEnd:
    m_transcript.finishLive();
    break;
  case AgentEvent::Type::ToolPrepare:
    setCompacting(false);
    m_transcript.finishLive();
    m_activity = "Preparing " + event.name + "…";
    break;
  case AgentEvent::Type::UserMessage:
    m_transcript.append(Entry::simple(Entry::Kind::User, event.text));
    break;
  case AgentEvent::Type::ToolStart:
    setCompacting(false);
    m_running.insert(event.id, event.name + " " + event.summary);
    m_runningOrder.removeAll(event.id);
    m_runningOrder << event.id;
    m_activity = label();
    break;
  case AgentEvent::Type::ToolEnd: {
    setCompacting(false);
    m_turnToolCalls++;
    m_running.remove(event.id);
    m_runningOrder.removeAll(event.id);
    m_activity = label();
    // The checklist itself was shown when it changed.
    if (event.name == TODO_TOOL && !event.isError) {
      break;
    }
    Entry entry;
    entry.kind = Entry::Kind::Tool;
    entry.name = event.name;
    entry.summary = event.summary;
    entry.output = event.output;
    entry.isError = event.isError;
    entry.diff = event.diff;
    m_transcript.append(entry);
    break;
  }
  case AgentEvent::Type::Context:
    m_contextTokens = event.tokens;
    m_contextLimit = event.limit.value_or(0);
    break;
  case AgentEvent::Type::Activity:
    setCompacting(event.compacting);
    // Progress from inside a tool (a sub-agent); say so when several run at once.
    m_activity = m_running.size() > 1 ? QString("%1 in parallel · %2").arg(m_running.size()).arg(event.text) : event.text;
    break;
  case AgentEvent::Type::Notice:
    notice(event.level, event.text);
    break;
  case AgentEvent::Type::Todos:
    if (m_busy) {
      Entry entry;
      entry.kind = Entry::Kind::Todos;
      entry.todos = event.todos;
      m_transcript.append(entry);
    }
    break;
  case AgentEvent::Type::Done:
    m_turnReason = doneReasonName(event.reason);
    break;
  }
  Q_EMIT changed();
}

void Backend::setCompacting(bool on) {
  m_compacting = on;
}

ApprovalDecision Backend::approveFromWorker(const ApprovalRequest& request) {
  auto pending = std::make_shared<Pending<ApprovalDecision>>();
  QMetaObject::invokeMethod(
      this,
      [this, request, pending]() {
        if (m_closed) {
          pending->resolve(ApprovalDecision::Deny);
          return;
        }
        m_approval = PendingApproval{request, pending};
        Q_EMIT promptChanged();
      },
      Qt::QueuedConnection);
  // Once the app has closed, nobody will answer.
  return pending->wait([this]() { return m_closing.load(); }, ApprovalDecision::Deny);
}

Answer Backend::askFromWorker(const Question& question) {
  auto pending = std::make_shared<Pending<Answer>>();
  QMetaObject::invokeMethod(
      this,
      [this, question, pending]() {
        if (m_closed) {
          pending->resolve(Answer{});
          return;
        }
        m_question = PendingQuestion{question, pending};
        Q_EMIT promptChanged();
      },
      Qt::QueuedConnection);
  return pending->wait([this]() { return m_closing.load(); }, Answer{});
}

void Backend::decide(const QString& decision) {
  if (!m_approval) {
    return;
  }
  const auto pending = m_approval->answer;
  m_approval.reset();
  Q_EMIT promptChanged();
  if (decision == "allow") {
    pending->resolve(ApprovalDecision::Allow);
  } else if (decision == "always") {
    pending->resolve(ApprovalDecision::AllowAlways);
  } else {
    pending->resolve(ApprovalDecision::Deny);
    if (decision == "interrupt" && m_pCancel) {
      m_pCancel->cancel();
    }
  }
}

void Backend::answerOption(int index) {
  if (!m_question) {
    return;
  }
  const auto pending = m_question->answer;
  m_question.reset();
  Q_EMIT promptChanged();
  pending->resolve(Answer{Answer::Kind::Option, index, {}});
}

void Backend::answerText(const QString& text) {
  if (!m_question || text.trimmed().isEmpty()) {
    return;
  }
  const auto pending = m_question->answer;
  m_question.reset();
  Q_EMIT promptChanged();
  pending->resolve(Answer{Answer::Kind::Text, 0, text.trimmed()});
}

void Backend::dismissQuestion() {
  if (!m_question) {
    return;
  }
  const auto pending = m_question->answer;
  m_question.reset();
  Q_EMIT promptChanged();
  pending->resolve(Answer{});
}

void Backend::interrupt() {
  if (m_approval) {
    decide("deny");
  }
  if (m_question) {
    dismissQuestion();
  }
  if (m_pCancel) {
    m_pCancel->cancel();
  }
}

// ---- turns ----

void Backend::submit(const QString& value) {
  const QString text = value.trimmed();
  if (text.isEmpty() || m_state != "ready") {
    return;
  }
  if (m_busy) {
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
  m_turnClock.start();
  m_turnStartedAt = QDateTime::currentMSecsSinceEpoch();
  m_turnToolCalls = 0;
  m_turnReason.clear();
  const std::shared_ptr<Agent> agent = m_pAgent;
  runTurn("Thinking…", [agent, text](const CancelPtr& cancel) { agent->send(text, cancel); },
          [this]() {
            m_transcript.append(Entry::simple(Entry::Kind::Meta, turnSummary(m_turnReason, m_turnClock.elapsed(), m_turnToolCalls)));
            if (m_turnReason == "interrupted" && matrix()) {
              m_transcript.append(Entry::simple(Entry::Kind::Quote, INTERRUPTED));
            }
            m_turnStartedAt = 0;
            refreshSessions();
          });
}

void Backend::runTurn(const QString& activity, std::function<void(const CancelPtr&)> work, std::function<void()> then) {
  const CancelPtr cancel = makeCancel();
  m_pCancel = cancel;
  m_busy = true;
  m_activity = activity;
  Q_EMIT changed();
  auto error = std::make_shared<QString>();
  const QString baseUrl = m_pServer->baseUrl();
  launch(
      [work, cancel, error, baseUrl]() {
        try {
          work(cancel);
        } catch (const std::exception& failure) {
          *error = describeError(failure, baseUrl);
        } catch (...) {
          *error = "Unexpected error.";
        }
      },
      [this, error, then]() {
        m_transcript.finishLive();
        if (!error->isEmpty()) {
          notice(NoticeLevel::Error, *error);
        }
        m_pCancel.reset();
        m_activity.clear();
        m_running.clear();
        m_runningOrder.clear();
        setCompacting(false);
        m_busy = false;
        // Messages the agent never got (interrupted, error, step limit) go back into the input.
        const QStringList undelivered = m_pAgent->takeQueued();
        if (!undelivered.isEmpty()) {
          notice(NoticeLevel::Warn, "Your queued message wasn't delivered, so it's back in the input box.");
          Q_EMIT restoreInput(undelivered.join("\n\n"));
        }
        if (then) {
          then();
        }
        Q_EMIT changed();
      });
}

void Backend::background(const QString& activity, std::function<std::function<void()>()> work) {
  m_busy = true;
  m_activity = activity;
  Q_EMIT changed();
  auto next = std::make_shared<std::function<void()>>();
  auto error = std::make_shared<QString>();
  const QString baseUrl = m_pServer ? m_pServer->baseUrl() : QString();
  launch(
      [work, next, error, baseUrl]() {
        try {
          *next = work();
        } catch (const std::exception& failure) {
          *error = describeError(failure, baseUrl);
        }
      },
      [this, next, error]() {
        m_activity.clear();
        m_busy = false;
        if (!error->isEmpty()) {
          notice(NoticeLevel::Error, *error);
        } else if (*next) {
          (*next)();
        }
        Q_EMIT changed();
      });
}

// ---- shutdown ----

void Backend::quit() {
  if (m_closed) {
    return;
  }
  m_closed = true;
  m_closing = true;
  if (m_pCancel) {
    m_pCancel->cancel();
  }
  if (m_approval) {
    m_approval->answer->resolve(ApprovalDecision::Deny);
  }
  if (m_question) {
    m_question->answer->resolve(Answer{});
  }
  // The agent's threads stop at their next check of the cancel token.
  for (QThread* thread : m_workers) {
    thread->wait();
  }
  if (m_pRuntime) {
    m_pRuntime->shutdown();
  }
  QCoreApplication::quit();
}

} // namespace Smith::Desktop
