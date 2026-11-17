#pragma once

#include "core/Config.h"
#include "core/Jobs.h"
#include "core/Sandbox.h"
#include "core/Themes.h"
#include "frontend/FileIndex.h"
#include "frontend/Frontend.h"
#include "frontend/Prompts.h"
#include "frontend/Turns.h"

#include <QObject>

#include <functional>
#include <memory>
#include <optional>

namespace Smith {

class Runtime;

// What a controller works with: the runtime's parts, as functions so a test can give its own.
struct ControllerOptions {
  // Creates the agent for the current project; with `current`, carrying over those settings.
  std::function<std::shared_ptr<Agent>(const AgentHooks&, const std::optional<AgentSettings>&)> createAgent;
  ProjectView project;
  // Makes another folder the project. Throws if it isn't a folder.
  std::function<ProjectView(const QString& folder)> switchProject;
  ServerPtr server;
  // Endpoints from the config file, for /endpoint.
  QList<Endpoint> endpoints;
  QString configFile;
  // The model last used on a server, if remembered.
  std::function<QString(const QString& baseUrl)> rememberedModel;
  std::shared_ptr<Sandbox> sandbox;
  SessionControl sessions;
  // Remembers the endpoint, model, reasoning level and theme for next time.
  std::function<void(const PreferenceUpdate&)> savePreferences;
  // Commands the agent started in the background.
  std::shared_ptr<BackgroundJobs> jobs;
  // Conversation restored from a saved session, shown at the start.
  Messages resumed;
  std::optional<QDateTime> resumedLastUsed;
  // Problems found at startup, shown after the banner.
  QStringList warnings;
  ThemeSpec theme;
};

// The options for a started runtime.
ControllerOptions controllerOptions(const std::shared_ptr<Runtime>& runtime);

/*
 * Everything a frontend does that isn't drawing: sending messages and
 * running turns, the queues, plan mode, the slash commands, switching model,
 * endpoint, theme, session and project. The terminal app and the desktop app
 * each show what it reports through its signals, and open their own pickers
 * when it asks for one. Lives on the UI thread.
 *
 * It can be made before there is a runtime (the desktop app starts one in
 * the background); `attach` then gives it one. Until then there's no agent,
 * and the operations do nothing.
 */
class Controller : public QObject {
  Q_OBJECT

public:
  explicit Controller(QObject* pParent = nullptr);
  ~Controller() override;

  void attach(ControllerOptions options);
  bool attached() const { return m_pAgent != nullptr; }

  // Text for /help: the commands and the app's keys.
  void setHelp(std::function<QString()> help) { m_help = std::move(help); }
  // Commands only the app can carry out (worktrees, the rain); true if it took `command`.
  void setAppCommands(std::function<bool(const QString& command, const QString& arg)> run) { m_appCommands = std::move(run); }
  // Added to "Theme: x." when the theme changes, e.g. that it suits a light terminal.
  void setThemeNote(std::function<QString(const ThemeSpec&)> note) { m_themeNote = std::move(note); }

  const ControllerOptions& options() const { return m_options; }
  const std::shared_ptr<Agent>& agent() const { return m_pAgent; }
  const ServerPtr& server() const { return m_pServer; }
  const ProjectView& project() const { return m_project; }
  const ThemeSpec& theme() const { return m_options.theme; }
  bool matrix() const { return m_options.theme.base == Theme::Matrix; }
  TurnRunner& turns() { return m_turns; }
  const TurnRunner& turns() const { return m_turns; }
  Prompts& prompts() { return m_prompts; }
  const Prompts& prompts() const { return m_prompts; }
  // The project's files, for @ completion; `fileIndex` says when they've been listed.
  QStringList files() { return m_fileIndex.files(m_project.cwd); }
  FileIndex& fileIndex() { return m_fileIndex; }
  // The server's models, as last listed (refreshModels).
  const QList<ModelInfo>& models() const { return m_models; }

  // The banner, warnings and resumed conversation: the transcript's start.
  void start();
  // The banner for the transcript's start.
  Entry banner(bool resumed, const std::optional<QDateTime>& lastUsed) const;

  void print(const Entry& entry);
  void notice(NoticeLevel level, const QString& text);

  // ---- messages ----

  // A message for the agent, or a command. While the agent works, a message
  // is given to it at its next step, and a command is refused.
  void submit(const QString& value);
  // While the agent works: `value` starts as a turn of its own once this one is done.
  void queueNext(const QString& value);
  // A queued task back (the last when -1), to change it.
  QString takeNext(int index = -1);
  // Stops the turn under way, refusing what it was asking.
  void interrupt();
  // After a turn in plan mode: plan mode off, and the agent starts on the plan.
  void carryOutPlan();

  // Slow work on a worker thread with `activity` showing; what it returns runs here, and an exception is shown instead.
  void runTask(const QString& activity, std::function<std::function<void()>()> work);
  // A thread of its own for `work`, outside `busy`; `finished` then runs here.
  void launch(std::function<void()> work, std::function<void()> finished = {});

  // ---- commands ----

  void runCommand(const QString& text);
  void setPlanning(bool on);
  void setNetwork(bool on);
  // /theme <name>: switches now, and remembers it. The current one again re-reads its file.
  void setTheme(const QString& name);
  // /theme: says what's wrong with theme files that can't be used, then asks for the picker.
  void pickTheme();
  void showChanges();
  void undo();
  void compact();
  void stopJob(const QString& id);

  // ---- what the agent talks to ----

  void setModel(const QString& id);
  void setReasoning(const QString& level);
  // Connects to a configured endpoint, on `preferredModel` if it serves it, else the model used there last.
  void setEndpoint(const QString& name, const QString& preferredModel = {});
  // Lists the server's models again; `fresh` asks the server rather than its cache.
  void refreshModels(bool fresh = false);

  // ---- sessions and projects ----

  // Starts a new session, in another folder if one is given; `extra` is shown after the notice.
  void newSession(const QString& folder = {}, const QList<Entry>& extra = {});
  // Continues the newest session in `folder`, or starts one there if it has none.
  void resumeIn(const QString& folder, const QList<Entry>& extra = {});
  // Switches to a saved session, moving to its folder when that's another one.
  void openSession(const QString& id, const QString& folder, const QList<Entry>& extra = {});
  void deleteSession(const QString& id, const QString& name);
  // /name: says the current name without one.
  void nameSession(const QString& name);

  // Stops what's running; nothing is reported afterwards.
  void close();

Q_SIGNALS:
  // Work started or ended, or something shown in a status line changed.
  void changed();
  void entryAdded(const Entry& entry);
  // The transcript starts again with these, the banner first.
  void startedOver(const QList<Entry>& entries);
  // From the agent: what it adds to the transcript, if anything. Streamed text is the app's to show.
  void agentEvent(const AgentEvent& event, const std::optional<Entry>& entry);
  // A turn ended: text still streaming is complete.
  void turnEnded();
  // Text for the input box: put in place of what's there, or after it.
  void inputRestored(const QString& text);
  void inputAppended(const QString& text);
  // The app's picker for `kind`: "model", "endpoint", "reasoning", "theme",
  // "sessions" (`arg` "all" for every folder), "open", "skills", "jobs", "plan".
  void pickerRequested(const QString& kind, const QString& arg);
  void themeChanged();
  void modelsChanged();
  // The saved sessions may have changed (a turn, a new or deleted session).
  void sessionsChanged();
  // The project or session changed: what the app lists for it is stale.
  void projectChanged();
  void redPill();
  void quitRequested();

private:
  void onEvent(const AgentEvent& event);
  void runTurn(const QString& activity, std::function<void(const CancelPtr&)> work, std::function<void()> then = {}, bool timed = false);
  void sendMessage(const QString& text);
  void afterUserTurn(bool planning);
  void startOver(bool resumed, const std::optional<QDateTime>& lastUsed, const QList<Entry>& entries);
  // Makes `folder` the project with a fresh agent; notes to show, or nullopt (and an error shown) if it isn't a folder.
  std::optional<QList<Entry>> enterProject(const QString& folder);
  void switchModel(const QString& id);
  bool busy() const { return !attached() || m_turns.busy(); }

  ControllerOptions m_options;
  std::shared_ptr<Agent> m_pAgent;
  ServerPtr m_pServer;
  ProjectView m_project;
  AgentHooks m_hooks;
  Prompts m_prompts;
  TurnRunner m_turns;
  FileIndex m_fileIndex;
  QList<ModelInfo> m_models;
  std::function<QString()> m_help;
  std::function<bool(const QString&, const QString&)> m_appCommands;
  std::function<QString(const ThemeSpec&)> m_themeNote;
  bool m_closed = false;
};

} // namespace Smith
