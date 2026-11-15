#include "Backend.h"

#include "core/Dirs.h"
#include "core/Errors.h"
#include "core/Tool.h"
#include "frontend/Duration.h"
#include "frontend/Folders.h"
#include "frontend/Replay.h"

#include <QDateTime>
#include <QRegularExpression>
#include <QUrl>

namespace Smith::Desktop {

// ---- sessions and projects ----

void Backend::refreshSessions() {
  m_sessions.clear();
  if (m_sessionControl.list) {
    const QString current = m_sessionControl.currentId();
    for (const SessionInfo& info : m_sessionControl.list(m_allSessions)) {
      QStringList hint{ago(info.updated), plural(info.turns, "message")};
      m_sessions.append(QVariantMap{
          {"id", info.id},
          {"name", info.name},
          {"hint", hint.join(" · ")},
          {"model", (info.endpoint.isEmpty() ? QString() : info.endpoint + "/") + info.model},
          {"folder", shortPath(info.cwd, 40)},
          {"cwd", info.cwd},
          {"current", info.id == current},
      });
    }
  }
  Q_EMIT sessionsChanged();
}

void Backend::startOver(bool resumed, const std::optional<QDateTime>& lastUsed, const QList<Entry>& entries) {
  QList<Entry> all = entries;
  // Background jobs belong to the conversation that started them.
  const int stopped = m_pRuntime->jobs()->clear();
  if (stopped) {
    all.prepend(Entry::notice(NoticeLevel::Info, QString("Stopped %1 from the previous session.").arg(plural(stopped, "background job"))));
  }
  m_transcript.clear();
  m_turns.forgetContext();
  printBanner(resumed, lastUsed);
  for (const Entry& entry : all) {
    m_transcript.append(entry);
  }
  refreshSessions();
  // The project may be another repository, or another worktree of this one.
  refreshWorktrees();
  Q_EMIT changed();
}

std::optional<QList<Entry>> Backend::enterProject(const QString& folder) {
  ProjectView next;
  try {
    next = m_pRuntime->switchProject(folder);
  } catch (const std::exception& error) {
    notice(NoticeLevel::Error, errorMessage(error));
    return std::nullopt;
  }
  const AgentSettings current{m_pServer, m_pAgent->model(), m_pAgent->reasoning(), m_pAgent->contextLimit()};
  m_project = next;
  m_pAgent = m_pRuntime->createAgent(m_hooks, current);
  QList<Entry> notes;
  for (const QString& warning : next.warnings) {
    notes.append(Entry::notice(NoticeLevel::Warn, warning));
  }
  if (next.cwd == homeDir() || next.cwd == "/") {
    notes.append(Entry::notice(NoticeLevel::Warn, QString("The project is now %1: the agent can change anything under it.").arg(next.cwd)));
  }
  return notes;
}

void Backend::newSession(const QString& folder) {
  if (!m_pAgent || m_turns.busy()) {
    return;
  }
  startNewSession(folder, {});
}

void Backend::startNewSession(const QString& folder, const QList<Entry>& extra) {
  const QString previous = sessionId();
  QList<Entry> notes;
  if (!folder.isEmpty()) {
    const std::optional<QList<Entry>> entered = enterProject(folder);
    if (!entered) {
      return;
    }
    notes = *entered;
  } else {
    m_pAgent->loadConversation({});
  }
  m_sessionControl.startNew(m_pAgent->model(), m_pServer->name());
  const QString where = folder.isEmpty() ? QString() : " in " + tildify(m_project.cwd);
  notes.prepend(Entry::notice(NoticeLevel::Info, QString("Started a new session%1. The previous one is %2.").arg(where, previous)));
  startOver(false, std::nullopt, notes + extra);
}

void Backend::resumeIn(const QString& folder, const QList<Entry>& extra) {
  for (const SessionInfo& session : m_sessionControl.list(true)) {
    if (session.cwd == folder) {
      if (session.id != sessionId()) {
        openSessionIn(session.id, folder, extra);
      }
      return;
    }
  }
  startNewSession(folder, extra);
}

void Backend::openProject(const QString& folder) {
  const QUrl url(folder);
  newSession(url.isLocalFile() ? url.toLocalFile() : folder);
}

void Backend::openSession(const QString& id) {
  if (!m_pAgent || m_turns.busy() || id == sessionId()) {
    return;
  }
  QString folder;
  for (const QVariant& session : m_sessions) {
    if (session.toMap().value("id").toString() == id) {
      folder = session.toMap().value("cwd").toString();
    }
  }
  openSessionIn(id, folder, {});
}

void Backend::openSessionIn(const QString& id, const QString& folder, const QList<Entry>& extra) {
  QList<Entry> notes;
  if (!folder.isEmpty() && folder != m_project.cwd) {
    if (!pathExists(folder)) {
      notice(NoticeLevel::Error, QString("That session's folder, %1, doesn't exist anymore.").arg(folder));
      return;
    }
    const std::optional<QList<Entry>> entered = enterProject(folder);
    if (!entered) {
      return;
    }
    notes.append(Entry::notice(NoticeLevel::Info, QString("Now working in %1.").arg(tildify(folder))));
    notes.append(*entered);
  }
  OpenedSession opened;
  try {
    opened = m_sessionControl.open(id);
  } catch (const std::exception& error) {
    notice(NoticeLevel::Error, QString("Could not open session %1: %2").arg(id, errorMessage(error)));
    return;
  }
  m_pAgent->loadConversation(opened.messages);
  startOver(true, opened.lastUsed, notes + extra + historyToEntries(opened.messages, m_project.tools));

  // Continue on the endpoint and model the session last used, when they're available.
  if (!opened.endpoint.isEmpty() && opened.endpoint != endpoint() && m_pRuntime->config().findEndpoint(opened.endpoint)) {
    switchEndpoint(opened.endpoint, opened.model);
  } else if (!opened.model.isEmpty() && opened.model != m_pAgent->model()) {
    const ServerPtr server = m_pServer;
    const QString model = opened.model;
    background(QString("Checking %1…").arg(model), [this, server, model]() -> std::function<void()> {
      bool served = false;
      try {
        for (const ModelInfo& info : server->listModels()) {
          served = served || info.id == model;
        }
      } catch (const std::exception&) {
        served = false;
      }
      return [this, served, model, server]() {
        if (served) {
          switchModel(model);
        } else {
          notice(NoticeLevel::Warn,
                 QString("This session used %1, which %2 doesn't serve; continuing with %3.").arg(model, server->label(), m_pAgent->model()));
        }
      };
    });
  }
}

void Backend::deleteSession(const QString& id) {
  if (!m_sessionControl.remove) {
    return;
  }
  QString name = id;
  for (const QVariant& session : m_sessions) {
    if (session.toMap().value("id").toString() == id) {
      name = session.toMap().value("name").toString();
    }
  }
  try {
    m_sessionControl.remove(id);
  } catch (const std::exception& error) {
    notice(NoticeLevel::Error, "Could not delete the session: " + errorMessage(error));
    return;
  }
  notice(NoticeLevel::Info, QString("Deleted the session \"%1\" (%2) and its tool log.").arg(name, id));
  refreshSessions();
}

void Backend::renameSession(const QString& name) {
  if (!m_sessionControl.rename || name.trimmed().isEmpty()) {
    return;
  }
  m_sessionControl.rename(name.trimmed());
  static const QRegularExpression whitespace(R"(\s)");
  const QString resume = whitespace.match(name.trimmed()).hasMatch() ? QString("smith --resume \"%1\"").arg(name.trimmed()) : "smith --resume " + name.trimmed();
  notice(NoticeLevel::Info, QString("Named this session \"%1\". Resume it from anywhere with: %2").arg(name.trimmed(), resume));
  refreshSessions();
}

} // namespace Smith::Desktop
