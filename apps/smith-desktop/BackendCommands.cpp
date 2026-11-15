#include "Backend.h"

#include "core/Jobs.h"
#include "core/Reasoning.h"
#include "core/Tool.h"
#include "frontend/Commands.h"

#include <QLocale>
#include <QRegularExpression>

namespace Smith::Desktop {

namespace {

QString windowText(std::optional<int> contextSize) {
  return contextSize ? QLocale(QLocale::English).toString(*contextSize) + "-token context" : QString("context size unknown");
}

QString helpText() {
  QStringList lines;
  int width = 0;
  for (const CommandSpec& command : commands()) {
    width = std::max(width, int(QString("%1 %2").arg(command.name, command.args).size()));
  }
  for (const CommandSpec& command : commands()) {
    lines << "  " + QString("%1 %2").arg(command.name, command.args).leftJustified(width + 2) + command.description;
  }
  return "Commands (the same as in the terminal; most also have a button or menu):\n" + lines.join('\n') +
         "\nKeys: enter sends · shift+enter adds a new line · esc interrupts the agent · ↑/↓ in an empty box recall earlier input\n"
         "      y / a / n answer an approval · ctrl+n new session · ctrl+o open a folder · ctrl+k pick a model";
}

} // namespace

// ---- commands ----

void Backend::runCommand(const QString& text) {
  static const QRegularExpression whitespace(R"(\s+)");
  QStringList parts = text.split(whitespace, Qt::SkipEmptyParts);
  const QString command = parts.takeFirst();
  const QString arg = parts.join(' ');
  if (command == "/exit" || command == "/quit") {
    quit();
  } else if (command == "/help") {
    notice(NoticeLevel::Info, helpText());
  } else if (command == "/new" || command == "/clear") {
    newSession(arg);
  } else if (command == "/sessions" || command == "/resume") {
    setAllSessions(arg == "all");
    Q_EMIT openPicker("sessions");
  } else if (command == "/name") {
    if (arg.isEmpty()) {
      const QString current = sessionName();
      notice(NoticeLevel::Info, current.isEmpty() ? QString("This session has no name yet. /name <name> gives it one; until then, listings use its first message.")
                                                  : QString("This session is named \"%1\". /name <new name> renames it.").arg(current));
    } else {
      renameSession(arg);
    }
  } else if (command == "/open") {
    Q_EMIT openPicker("folder");
  } else if (command == "/worktree") {
    createWorktree(arg);
  } else if (command == "/worktrees") {
    Q_EMIT openPicker("worktrees");
  } else if (command == "/land") {
    if (const std::optional<Worktree> worktree = findTask(arg)) {
      reviewLanding(worktree->path);
    }
  } else if (command == "/drop") {
    if (const std::optional<Worktree> worktree = findTask(arg)) {
      Q_EMIT confirmDrop(worktree->path);
    }
  } else if (command == "/endpoint") {
    if (arg.isEmpty()) {
      Q_EMIT openPicker("endpoint");
    } else {
      setEndpoint(arg);
    }
  } else if (command == "/model") {
    if (arg.isEmpty()) {
      Q_EMIT openPicker("model");
    } else {
      setModel(arg);
    }
  } else if (command == "/reasoning") {
    if (arg.isEmpty()) {
      Q_EMIT openPicker("reasoning");
    } else {
      setReasoning(arg);
    }
  } else if (command == "/network") {
    if (arg == "on" || arg == "off") {
      setNetwork(arg == "on");
    } else {
      notice(NoticeLevel::Info, QString("Network is %1 for sandboxed commands. Use /network on or /network off.").arg(network() ? "on" : "off"));
    }
  } else if (command == "/permissions") {
    notice(NoticeLevel::Info, managePermissions(*m_pAgent->allowed(), arg));
  } else if (command == "/undo") {
    undo();
  } else if (command == "/skills") {
    Q_EMIT openPicker("skills");
  } else if (command == "/jobs") {
    const QStringList words = arg.split(' ', Qt::SkipEmptyParts);
    if (words.value(0) == "stop" && words.size() > 1) {
      stopJob(words[1]);
    } else {
      Q_EMIT openPicker("jobs");
    }
  } else if (command == "/compact") {
    compact();
  } else if (command == "/redpill") {
    Q_EMIT redPill();
  } else {
    notice(NoticeLevel::Error, QString("Unknown command %1. Type /help for the list.").arg(command));
  }
}

void Backend::setReasoning(const QString& level) {
  if (!m_pAgent || m_turns.busy()) {
    return;
  }
  if (!isReasoningLevel(level)) {
    notice(NoticeLevel::Error, QString("Unknown reasoning level \"%1\". Use one of: %2.").arg(level, REASONING_LEVELS.join(", ")));
    return;
  }
  if (level == m_pAgent->reasoning()) {
    return;
  }
  m_pAgent->setReasoning(level);
  m_pRuntime->savePreferences(PreferenceUpdate{m_pServer, {}, level});
  notice(NoticeLevel::Info, QString("Reasoning: %1 (%2).").arg(level, reasoningHint(level)));
  Q_EMIT changed();
}

void Backend::setModel(const QString& id) {
  if (!m_pAgent || m_turns.busy() || id.isEmpty() || id == m_pAgent->model()) {
    return;
  }
  switchModel(id);
}

void Backend::switchModel(const QString& id) {
  const ServerPtr server = m_pServer;
  background(QString("Checking %1…").arg(id), [this, server, id, closing = m_turns.closing()]() -> std::function<void()> {
    const std::optional<int> contextSize = server->contextSize(id, closing);
    return [this, server, id, contextSize]() {
      m_pAgent->setModel(id, contextSize);
      m_pRuntime->savePreferences(PreferenceUpdate{server, id, {}});
      notice(NoticeLevel::Info, QString("Switched to %1 (%2).").arg(id, windowText(contextSize)));
    };
  });
}

void Backend::setEndpoint(const QString& name) {
  if (!m_pRuntime || m_turns.busy() || name == endpoint()) {
    return;
  }
  switchEndpoint(name);
}

void Backend::switchEndpoint(const QString& name, const QString& preferredModel) {
  const Endpoint* found = m_pRuntime->config().findEndpoint(name);
  if (!found) {
    const QStringList known = endpoints();
    notice(NoticeLevel::Error, QString("Unknown endpoint \"%1\". Configured: %2.").arg(name, known.isEmpty() ? QString("none") : known.join(", ")));
    return;
  }
  const Endpoint chosen = *found;
  background(QString("Connecting to %1…").arg(name), [this, chosen, preferredModel, closing = m_turns.closing()]() -> std::function<void()> {
    const ServerPtr next = Server::fromEndpoint(chosen);
    const QString model = next->chooseModel({preferredModel, Runtime::rememberedModel(next->baseUrl())}, closing);
    const std::optional<int> contextSize = next->contextSize(model, closing);
    return [this, next, model, contextSize]() {
      m_pAgent->setModel(model, contextSize, next);
      m_pServer = next;
      m_models.clear();
      refreshModels();
      m_pRuntime->savePreferences(PreferenceUpdate{next, model, {}});
      notice(NoticeLevel::Info, QString("Connected to %1, using %2 (%3).").arg(next->label(), model, windowText(contextSize)));
    };
  });
}

void Backend::refreshModels() {
  if (!m_pServer) {
    return;
  }
  const ServerPtr server = m_pServer;
  auto models = std::make_shared<QVariantList>();
  launch(
      [server, models, closing = m_turns.closing()]() {
        try {
          for (const ModelInfo& info : server->listModels(true, closing)) {
            models->append(QVariantMap{{"id", info.id}, {"context", info.contextSize.value_or(0)}});
          }
        } catch (const std::exception&) {
          // The picker then offers only the current model.
        }
      },
      [this, server, models]() {
        if (server == m_pServer) {
          m_models = *models;
          Q_EMIT changed();
        }
      });
}

void Backend::setNetwork(bool on) {
  if (!m_pRuntime) {
    return;
  }
  const auto sandbox = m_pRuntime->sandbox();
  if (!sandbox->active()) {
    notice(NoticeLevel::Warn, QString("There is no sandbox, so commands already have full network access (%1).").arg(sandbox->reason()));
    return;
  }
  if (sandbox->network() == on) {
    return;
  }
  sandbox->setNetwork(on);
  const QString restart = sandbox->type() == SandboxType::Container
                              ? " The container was removed for that; the next command starts a new one, without what was installed in the old one."
                              : QString();
  notice(NoticeLevel::Info, QString("Sandboxed commands now have network %1.%2").arg(on ? "on" : "off", restart));
  Q_EMIT changed();
}

void Backend::undo() {
  if (!m_pAgent || m_turns.busy()) {
    return;
  }
  const std::optional<UndoResult> result = m_pAgent->undo();
  if (!result) {
    notice(NoticeLevel::Info, "Nothing to undo.");
    return;
  }
  m_transcript.append(undoNotice(*result, m_project.cwd));
  Q_EMIT restoreInput(result->prompt);
  Q_EMIT changed();
}

void Backend::compact() {
  if (!m_pAgent || m_turns.busy()) {
    return;
  }
  m_turns.setCompacting(true);
  const std::shared_ptr<Agent> agent = m_pAgent;
  runTurn("Compacting…", [agent](const CancelPtr& cancel) { agent->compactNow(cancel); });
}

void Backend::stopJob(const QString& id) {
  if (!m_pRuntime) {
    return;
  }
  const auto jobs = m_pRuntime->jobs();
  if (id == "all") {
    const int stopped = jobs->stopAll();
    notice(NoticeLevel::Info, stopped ? QString("Stopped %1.").arg(plural(stopped, "background job")) : QString("No background jobs are running."));
    Q_EMIT changed();
    return;
  }
  background("Stopping " + id + "…", [this, jobs, id]() -> std::function<void()> {
    const JobStatus status = jobs->stop(id);
    return [this, id, status]() { notice(NoticeLevel::Info, QString("Stopped %1 (%2).").arg(id, describeJob(status))); };
  });
}

} // namespace Smith::Desktop
