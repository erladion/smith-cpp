#pragma once

// What every frontend (the TUI, the desktop app, the headless runner) uses
// to drive agents: the hooks an agent calls back through, and the session
// and project controls a frontend works with.

#include "core/Agent.h"
#include "core/Server.h"
#include "core/Session.h"
#include "core/Skills.h"

#include <QDateTime>
#include <QMutex>
#include <QWaitCondition>

#include <functional>
#include <memory>
#include <optional>

namespace Smith {

// How the frontend talks to an agent: events out, and blocking questions to the user.
struct AgentHooks {
  std::function<void(const AgentEvent&)> onEvent;
  std::function<ApprovalDecision(const ApprovalRequest&)> approve;
  // Questions from ask_user; without it (non-interactive runs) the tool isn't offered.
  std::function<Answer(const Question&)> ask;
};

// Settings carried over when the agent is recreated for another project.
struct AgentSettings {
  ServerPtr server;
  QString model;
  QString reasoning;
  std::optional<int> contextSize;
};

// The folder being worked in, and what was loaded for it.
struct ProjectView {
  QString cwd;
  // Instruction files in use, for the banner.
  QStringList instructions;
  // Skills available, for the banner and /skills.
  QList<Skill> skills;
  ToolList tools;
  // Problems found while loading (e.g. a skill without a description).
  QStringList warnings;
};

struct OpenedSession {
  Messages messages;
  QString model;
  QString endpoint;
  QString cwd;
  std::optional<QDateTime> lastUsed;
};

struct RecentProject {
  QString cwd;
  int sessions = 0;
  QDateTime updated;
};

// Lets a frontend list, open and start sessions; the runtime owns which one is recorded to.
struct SessionControl {
  std::function<QString()> currentId;
  // The name given with /name; empty if none.
  std::function<QString()> currentName;
  // This folder's sessions (or every folder's), newest first.
  std::function<QList<SessionInfo>(bool everywhere)> list;
  // Makes `id` the session being recorded and returns its conversation. Throws on failure.
  std::function<OpenedSession(const QString& id)> open;
  std::function<void(const QString& name)> rename;
  // Deletes a session that isn't the current one, with its tool log. Throws on failure.
  std::function<void(const QString& id)> remove;
  // Starts recording to a new session (its file appears with the first message).
  std::function<void(const QString& model, const QString& endpoint)> startNew;
  // Folders with saved sessions, most recently used first.
  std::function<QList<RecentProject>()> recentProjects;
};

struct PreferenceUpdate {
  ServerPtr server;
  QString model;
  QString reasoning;
};

// A value one thread waits for and another provides: the user's answer to
// an approval or question, given on the UI thread to a waiting agent thread.
template <typename T>
class Pending {
public:
  void resolve(const T& value) {
    QMutexLocker lock(&m_mutex);
    if (!m_value) {
      m_value = value;
      m_ready.wakeAll();
    }
  }

  // Waits for the value; gives `fallback` once `abandoned` says nobody will answer.
  T wait(const std::function<bool()>& abandoned, const T& fallback) {
    QMutexLocker lock(&m_mutex);
    while (!m_value) {
      if (abandoned()) {
        return fallback;
      }
      m_ready.wait(&m_mutex, 100);
    }
    return *m_value;
  }

private:
  QMutex m_mutex;
  QWaitCondition m_ready;
  std::optional<T> m_value;
};

} // namespace Smith
