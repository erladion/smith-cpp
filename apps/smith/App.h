#pragma once

#include "tui/Editor.h"
#include "tui/Keys.h"
#include "tui/Picker.h"
#include "tui/Rain.h"
#include "tui/Renderer.h"
#include "tui/Terminal.h"
#include "Entries.h"
#include "frontend/Prompts.h"
#include "frontend/Turns.h"
#include "Suggestions.h"
#include "Types.h"

#include <QElapsedTimer>
#include <QMap>
#include <QObject>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>

namespace Smith::Ui {

class ConnectionCommands;
class SessionCommands;
class WorktreeCommands;

// How the session on screen started, for the banner's greeting.
struct Start {
  bool resumed = false;
  std::optional<QDateTime> lastUsed;
};

// The interactive terminal UI: a transcript above, a live region below. This
// class runs the screen, the keys and agent turns; the commands that switch
// session, project, endpoint or model are in SessionCommands and
// ConnectionCommands, and work through its public part. Its own code is
// split by what it does: App.cpp (startup, terminal, turns), AppRender.cpp,
// AppInput.cpp and AppCommands.cpp.
class App : public QObject {
  Q_OBJECT

public:
  explicit App(TuiOptions options, QObject* parent = nullptr);
  ~App() override;

  // Starts the UI; `finished` is emitted when the user quits.
  void run();

  // ---- for the command modules ----

  const TuiOptions& opts() const { return m_options; }
  const AgentHooks& hooks() const { return m_hooks; }
  const std::shared_ptr<Agent>& agent() const { return m_pAgent; }
  void setAgent(const std::shared_ptr<Agent>& agent) { m_pAgent = agent; }
  const ServerPtr& server() const { return m_pServer; }
  void setServer(const ServerPtr& server) { m_pServer = server; }
  const ProjectView& project() const { return m_project; }
  void setProject(const ProjectView& project) { m_project = project; }

  // Adds an entry to the transcript.
  void print(const Entry& entry);
  // Clears the screen and starts a fresh transcript.
  void startOver(const Start& start, const QList<Entry>& entries);
  // Shows a picker; `onPick` gets the value of the item chosen (not called on esc).
  void openPicker(std::unique_ptr<Tui::Picker> picker, std::function<void(const QString&)> onPick,
                  std::function<void(const QString&)> onAction = {});
  // Asks for a line of text in the input box, starting from `initial`; `onSubmit`
  // gets it on enter. esc cancels, and the input box gets back what was in it.
  void promptText(const QString& title, const QString& initial, std::function<void(const QString&)> onSubmit);
  // Shows a spinner while `work` runs on a worker thread. It returns what to
  // do next on the UI thread; an exception is printed instead.
  void withActivity(const QString& activity, std::function<std::function<void()>()> work);
  // Fetches the server's model list again, for completing /model.
  void refreshModels();
  // Lists the repository's task worktrees again, for completing /land and /drop.
  void refreshTasks();
  SessionCommands& sessions() { return *m_pSessions; }

Q_SIGNALS:
  void finished();

private:
  struct Live {
    BlockKind kind = BlockKind::Text;
    QString text;
  };

  struct OpenPicker {
    std::unique_ptr<Tui::Picker> picker;
    std::function<void(const QString&)> onPick;
    std::function<void(const QString&)> onAction;
  };

  struct TextPrompt {
    QString title;
    std::function<void(const QString&)> onSubmit;
    // What was in the input box, put back afterwards.
    QString stashed;
  };

  void printBanner(const Start& start);
  void onInput(const QString& data);
  void onResize();
  void reprint();
  void exitApp();
  void restoreTerminal();
  bool matrix() const;
  bool streaming() const;
  int width() const;
  bool prompting() const;
  bool agentRunning() const;

  void requestRender();
  void render();
  QList<Tui::Line> liveLines();

  void onEvent(const AgentEvent& event);
  void appendDelta(BlockKind kind, const QString& text);
  void flushLive();

  // Answers the question on screen and puts back what was in the input box.
  void answer(const Answer& answer);
  void onQuestionKey(const Tui::Key& key);

  QList<Suggestion> currentSuggestions() const;
  void onKey(const Tui::Key& key);
  void queueMessage(const QString& value);
  void submit(const QString& value);
  void runCommand(const QString& text);
  // Runs an agent turn; `then` follows on the UI thread. A timed turn is one the user sent.
  void runBusy(const QString& activity, std::function<void(const CancelPtr&)> work, std::function<void()> then = {}, bool timed = false);

  // Full-screen digital rain until it ends or a key is pressed; then `then`.
  void playRain(const Tui::Rain::Options& options, std::function<void()> then);
  void stopRain();

  void undo();
  void listSkills();
  void manageJobs(const QString& arg);
  void setNetwork(const QString& arg);

  TuiOptions m_options;
  AgentHooks m_hooks;
  std::shared_ptr<Agent> m_pAgent;
  ServerPtr m_pServer;
  ProjectView m_project;
  Tui::Terminal m_terminal;
  Tui::InlineRenderer m_renderer;
  Tui::KeyReader m_keys;
  Tui::LineEditor m_editor;
  SuggestionList m_suggestions;
  std::unique_ptr<ConnectionCommands> m_pConnection;
  std::unique_ptr<SessionCommands> m_pSessions;
  std::unique_ptr<WorktreeCommands> m_pWorktrees;

  // Approvals and questions from the agent, and the turn or task under way.
  Prompts m_prompts;
  TurnRunner m_turns;
  // The option highlighted in the question on screen.
  int m_questionSelected = 0;

  std::optional<Live> m_live;
  std::optional<OpenPicker> m_picker;
  std::optional<TextPrompt> m_textPrompt;
  std::unique_ptr<Tui::Rain> m_pStrip;
  // A full-screen animation has the terminal: no drawing, keys end it.
  std::unique_ptr<Tui::Rain> m_pFullRain;
  std::function<void()> m_afterRain;
  QTimer m_rainTimer;
  // What was in the input box when a question came up, put back after the answer.
  QString m_stashed;

  // Everything printed so far, to reprint at a new width after a resize.
  QList<Entry> m_transcript;
  int m_printedWidth = 0;
  QTimer m_resizeTimer;
  int m_frame = 0;
  QTimer m_ticker;
  QTimer m_renderTimer;
  bool m_closed = false;
};

} // namespace Smith::Ui
