#pragma once

#include "frontend/Frontend.h"
#include "core/Config.h"
#include "core/Instructions.h"
#include "core/Jobs.h"
#include "core/Sandbox.h"
#include "core/Server.h"
#include "core/Session.h"
#include "core/ToolLog.h"

#include <QCommandLineParser>
#include <QMutex>

#include <memory>
#include <optional>

namespace Smith {

// What the command line chose, for any frontend.
struct LaunchOptions {
  // The folder to work in; empty for the current directory.
  QString folder;
  QString endpoint;
  QString baseUrl;
  QString apiKey;
  QString model;
  QString reasoning;
  QString contextSize;
  QString temperature;
  QString maxSteps;
  QString sandbox;
  bool network = false;
  QString resume;
  bool continueLast = false;
};

// The flags every frontend takes: -e, --base-url, --model, --sandbox, -c, -r and so on.
void addLaunchOptions(QCommandLineParser& parser);
// Reads them back; `folder` is the single positional argument, if given.
LaunchOptions readLaunchOptions(const QCommandLineParser& parser);

// An existing folder, given relative to `base`, absolute, or with ~/. Throws if it isn't one.
QString resolveFolder(const QString& folder, const QString& base);

// Everything a running Smith works with: the config, the server and model
// it starts on, the sandbox, background jobs, the project and the session
// being recorded. The TUI, the desktop app and headless runs all start one.
class Runtime : public std::enable_shared_from_this<Runtime> {
public:
  // Picks the server, model and sandbox, and opens or starts the session.
  // Blocks (it asks the server for its models) until done or `cancel` is set;
  // throws SmithError, or AbortError once cancelled.
  static std::shared_ptr<Runtime> start(const LaunchOptions& options, const CancelPtr& cancel = {});
  ~Runtime();

  const Config& config() const { return m_config; }
  ServerPtr server() const { return m_pServer; }
  QString model() const { return m_model; }
  QString reasoning() const { return m_reasoning; }
  std::optional<int> contextSize() const { return m_contextSize; }
  std::shared_ptr<Sandbox> sandbox() const { return m_pSandbox; }
  std::shared_ptr<BackgroundJobs> jobs() const { return m_pJobs; }
  // Problems worth showing at startup: config, sandbox and project warnings.
  QStringList warnings() const { return m_warnings; }
  // The resumed conversation, if a session was resumed.
  Messages resumedHistory() const { return m_resumed; }
  std::optional<QDateTime> resumedLastUsed() const { return m_resumedLastUsed; }

  std::shared_ptr<Session> session() const;
  QString projectCwd() const;
  // The current project, with instruction paths shortened for display.
  ProjectView project() const;

  // An agent for the current project. Without `current` it continues the
  // resumed conversation with the startup settings; with it, it starts empty
  // with those settings (a new project).
  std::shared_ptr<Agent> createAgent(const AgentHooks& hooks, const std::optional<AgentSettings>& current = std::nullopt);
  // Makes another folder the project. Throws if it isn't a folder.
  ProjectView switchProject(const QString& folder);
  SessionControl sessions();
  // Remembers the endpoint, model and reasoning level for next time, and records the model in the session.
  void savePreferences(const PreferenceUpdate& update);
  // The model last used on a server, if remembered.
  static QString rememberedModel(const QString& baseUrl);

  // Stops background jobs and removes what the sandbox started. Also done on destruction.
  void shutdown();

private:
  struct Project {
    QString cwd;
    QList<InstructionFile> instructions;
    QList<Skill> skills;
    ToolList tools;
    QStringList warnings;
    // For a linked worktree: its root, its branch, and the main repository's .git it uses.
    QString worktreeRoot;
    QString worktreeBranch;
    QString gitCommonDir;
  };

  Runtime() = default;
  Project loadProject(const QString& cwd) const;
  static QStringList visibleFolders(const Project& project);

  Config m_config;
  ServerPtr m_pServer;
  QString m_model;
  QString m_reasoning;
  std::optional<int> m_contextSize;
  std::optional<double> m_temperature;
  int m_maxSteps = 50;
  std::shared_ptr<Sandbox> m_pSandbox;
  std::shared_ptr<BackgroundJobs> m_pJobs;
  std::shared_ptr<ToolLog> m_pToolLog;
  QStringList m_warnings;
  Messages m_resumed;
  std::optional<QDateTime> m_resumedLastUsed;
  bool m_shutDown = false;

  // The project and session change while frontends run; agent threads read them.
  mutable QMutex m_mutex;
  Project m_project;
  std::shared_ptr<Session> m_pSession;
};

} // namespace Smith
