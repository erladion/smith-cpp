#pragma once

#include "tui/Editor.h"
#include "tui/Keys.h"
#include "tui/Picker.h"
#include "tui/Rain.h"
#include "tui/Renderer.h"
#include "tui/Terminal.h"
#include "Entries.h"
#include "frontend/Controller.h"
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

// The interactive terminal UI: a transcript above, a live region below. What
// happens is the Controller's (messages, turns, commands, sessions); this
// class draws it, reads the keys and opens the pickers it asks for. Its code
// is split by what it does: App.cpp (startup, terminal, the controller's
// signals), AppRender.cpp, AppInput.cpp and AppCommands.cpp; the pickers for
// sessions, projects, endpoints and worktrees are in SessionCommands,
// ConnectionCommands and WorktreeCommands.
class App : public QObject {
  Q_OBJECT

public:
  explicit App(TuiOptions options, QObject* parent = nullptr);
  ~App() override;

  // Starts the UI; `finished` is emitted when the user quits.
  void run();

  // ---- for the command modules ----

  Controller& controller() { return m_controller; }
  const TuiOptions& opts() const { return m_options; }
  const std::shared_ptr<Agent>& agent() const { return m_controller.agent(); }
  const ServerPtr& server() const { return m_controller.server(); }
  const ProjectView& project() const { return m_controller.project(); }

  // Adds an entry to the transcript.
  void print(const Entry& entry);
  // Shows a picker; `onPick` gets the value of the item chosen (not called on esc).
  void openPicker(std::unique_ptr<Tui::Picker> picker, std::function<void(const QString&)> onPick,
                  std::function<void(const QString&)> onAction = {});
  // Asks for a line of text in the input box, starting from `initial`; `onSubmit`
  // gets it on enter. esc cancels, and the input box gets back what was in it.
  void promptText(const QString& title, const QString& initial, std::function<void(const QString&)> onSubmit);
  // Shows a spinner while `work` runs on a worker thread. It returns what to
  // do next on the UI thread; an exception is printed instead.
  void withActivity(const QString& activity, std::function<std::function<void()>()> work);
  // Lists the repository's task worktrees again, for completing /land and /drop.
  void refreshTasks();

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

  // The transcript starts again with `entries` (the controller's startedOver).
  void startOver(const QList<Entry>& entries);
  void onInput(const QString& data);
  void onResize();
  void reprint();
  void exitApp();
  void restoreTerminal();
  bool matrix() const;
  // The rain's color: matrix's green, else the accent's blue (both the theme's shades).
  Tui::Color rainColor() const;
  bool streaming() const;
  int width() const;
  bool prompting() const;
  bool agentRunning() const;

  void requestRender();
  void render();
  QList<Tui::Line> liveLines();

  void onAgentEvent(const AgentEvent& event, const std::optional<Entry>& entry);
  void appendDelta(BlockKind kind, const QString& text);
  void flushLive();

  // Answers the question on screen and puts back what was in the input box.
  void answer(const Answer& answer);
  void onQuestionKey(const Tui::Key& key);

  QList<Suggestion> currentSuggestions() const;
  void onKey(const Tui::Key& key);

  // Full-screen digital rain until it ends or a key is pressed; then `then`.
  void playRain(const Tui::Rain::Options& options, std::function<void()> then);
  void stopRain();

  // What the terminal does for a command the controller hands it: worktrees.
  bool runAppCommand(const QString& command, const QString& arg);
  // The picker the controller asks for.
  void openPickerFor(const QString& kind, const QString& arg);
  void listSkills();
  void listJobs();
  void pickTheme();
  // After a turn in plan mode: carry out the plan, or keep planning.
  void offerPlan();
  void playRedPill();
  void applyTheme();

  TuiOptions m_options;
  Controller m_controller;
  Tui::Terminal m_terminal;
  Tui::InlineRenderer m_renderer;
  Tui::KeyReader m_keys;
  Tui::LineEditor m_editor;
  SuggestionList m_suggestions;
  std::unique_ptr<ConnectionCommands> m_pConnection;
  std::unique_ptr<SessionCommands> m_pSessions;
  std::unique_ptr<WorktreeCommands> m_pWorktrees;

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
