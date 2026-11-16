#include "App.h"

#include "core/Jobs.h"
#include "core/Tool.h"
#include "frontend/Commands.h"
#include "Connection.h"
#include "frontend/Duration.h"
#include "SessionCommands.h"
#include "TerminalTheme.h"
#include "WorktreeCommands.h"

#include <QRegularExpression>

#include <limits>

namespace Smith::Ui {

// ---- commands ----

void App::runCommand(const QString& text) {
  static const QRegularExpression whitespace(R"(\s+)");
  QStringList parts = text.split(whitespace, Qt::SkipEmptyParts);
  const QString command = parts.takeFirst();
  const QString arg = parts.join(' ');
  if (command == "/exit" || command == "/quit") {
    exitApp();
  } else if (command == "/help") {
    print(Entry::notice(NoticeLevel::Info, commandHelp()));
  } else if (command == "/new" || command == "/clear") {
    m_pSessions->newSession(arg);
  } else if (command == "/sessions" || command == "/resume") {
    m_pSessions->openSessionPicker(arg == "all");
  } else if (command == "/name") {
    m_pSessions->nameSession(arg);
  } else if (command == "/open") {
    m_pSessions->openProjectPicker();
  } else if (command == "/worktree") {
    m_pWorktrees->create(arg);
  } else if (command == "/worktrees") {
    m_pWorktrees->openPicker();
  } else if (command == "/land") {
    m_pWorktrees->land(arg);
  } else if (command == "/drop") {
    m_pWorktrees->drop(arg);
  } else if (command == "/endpoint") {
    if (arg.isEmpty()) {
      m_pConnection->openEndpointPicker();
    } else {
      m_pConnection->switchEndpoint(arg);
    }
  } else if (command == "/model") {
    if (arg.isEmpty()) {
      m_pConnection->openModelPicker();
    } else {
      m_pConnection->switchModel(arg);
    }
  } else if (command == "/reasoning") {
    if (arg.isEmpty()) {
      m_pConnection->openReasoningPicker();
    } else {
      m_pConnection->setReasoning(arg);
    }
  } else if (command == "/theme") {
    setTheme(arg);
  } else if (command == "/plan") {
    setPlanning(arg);
  } else if (command == "/diff") {
    showChanges();
  } else if (command == "/network") {
    setNetwork(arg);
  } else if (command == "/permissions") {
    print(Entry::notice(NoticeLevel::Info, managePermissions(*m_pAgent->allowed(), arg)));
  } else if (command == "/undo") {
    undo();
  } else if (command == "/skills") {
    listSkills();
  } else if (command == "/jobs") {
    manageJobs(arg);
  } else if (command == "/compact") {
    m_turns.setCompacting(true);
    const std::shared_ptr<Agent> current = m_pAgent;
    runBusy("Compacting…", [current](const CancelPtr& cancel) { current->compactNow(cancel); });
  } else if (command == "/redpill") {
    // Not listed in /help.
    if (!Tui::colorsEnabled()) {
      print(Entry::notice(NoticeLevel::Info, "The Matrix needs colors, and NO_COLOR is set."));
      return;
    }
    m_ticker.stop();
    Tui::Rain::Options options;
    options.frames = std::numeric_limits<double>::infinity();
    options.color = rainColor();
    playRain(options, [this]() {
      // The window may have been resized meanwhile: reprint if so, then redraw.
      reprint();
    });
  } else {
    print(Entry::notice(NoticeLevel::Error, QString("Unknown command %1. Type /help for the list.").arg(command)));
  }
}

void App::undo() {
  const std::optional<UndoResult> result = m_pAgent->undo();
  if (!result) {
    print(Entry::notice(NoticeLevel::Info, "Nothing to undo."));
    return;
  }
  print(undoNotice(*result, m_project.cwd));
  m_editor.setValue(result->prompt);
}

void App::listSkills() {
  if (m_project.skills.isEmpty()) {
    print(Entry::notice(NoticeLevel::Info, "No skills found. Add folders with a SKILL.md to .agents/skills/ in the project or ~/.config/smith/skills/."));
    return;
  }
  static const QRegularExpression whitespace(R"(\s+)");
  QStringList lines;
  for (const Skill& skill : m_project.skills) {
    lines << skill.name + ": " + QString(skill.description).replace(whitespace, " ");
  }
  print(Entry::notice(NoticeLevel::Info, lines.join('\n')));
}

void App::manageJobs(const QString& arg) {
  const auto jobs = m_options.jobs;
  if (!jobs) {
    print(Entry::notice(NoticeLevel::Info, "Background jobs aren't available here."));
    return;
  }
  const QStringList parts = arg.split(' ', Qt::SkipEmptyParts);
  if (parts.value(0) == "stop" && parts.size() > 1) {
    const QString id = parts[1];
    if (id == "all") {
      const int stopped = jobs->stopAll();
      print(Entry::notice(NoticeLevel::Info, stopped ? QString("Stopped %1.").arg(plural(stopped, "background job")) : QString("No background jobs are running.")));
      return;
    }
    withActivity("Stopping " + id + "…", [this, jobs, id]() -> std::function<void()> {
      const JobStatus status = jobs->stop(id);
      return [this, id, status]() { print(Entry::notice(NoticeLevel::Info, QString("Stopped %1 (%2).").arg(id, describeJob(status)))); };
    });
    return;
  }
  if (!arg.isEmpty()) {
    print(Entry::notice(NoticeLevel::Error, "Use /jobs to list background jobs, /jobs stop <id> or /jobs stop all to stop them."));
    return;
  }
  const QList<JobStatus> list = jobs->list();
  if (list.isEmpty()) {
    print(Entry::notice(NoticeLevel::Info, "No background jobs. The agent starts them with bash's run_in_background."));
    return;
  }
  QStringList lines;
  for (const JobStatus& job : list) {
    lines << QString("%1  %2 started %3 ago  %4")
                 .arg(job.id, describeJob(job).leftJustified(20), formatDuration(job.started.msecsTo(QDateTime::currentDateTime())), job.command);
  }
  print(Entry::notice(NoticeLevel::Info, lines.join('\n') + "\n/jobs stop <id> or /jobs stop all stops them."));
}

void App::setNetwork(const QString& arg) {
  const auto sandbox = m_options.sandbox;
  if (!sandbox->active()) {
    print(Entry::notice(NoticeLevel::Warn, QString("There is no sandbox, so commands already have full network access (%1).").arg(sandbox->reason())));
    return;
  }
  if (arg != "on" && arg != "off") {
    print(Entry::notice(NoticeLevel::Info,
                        QString("Network is %1 for sandboxed commands. Use /network on or /network off.").arg(sandbox->network() ? "on" : "off")));
    return;
  }
  if (sandbox->network() == (arg == "on")) {
    print(Entry::notice(NoticeLevel::Info, QString("Network is already %1 for sandboxed commands.").arg(arg)));
    return;
  }
  sandbox->setNetwork(arg == "on");
  const QString restart = sandbox->type() == SandboxType::Container
                              ? " The container was removed for that; the next command starts a new one, without what was installed in the old one."
                              : QString();
  print(Entry::notice(NoticeLevel::Info, QString("Sandboxed commands now have network %1.%2").arg(arg, restart)));
}

void App::setPlanning(const QString& arg) {
  if (!arg.isEmpty() && arg != "on" && arg != "off") {
    print(Entry::notice(NoticeLevel::Error, "Use /plan to switch plan mode, or /plan on or /plan off."));
    return;
  }
  const bool on = arg.isEmpty() ? !m_pAgent->planning() : arg == "on";
  if (on == m_pAgent->planning()) {
    print(Entry::notice(NoticeLevel::Info, QString("Plan mode is already %1.").arg(arg)));
    return;
  }
  m_pAgent->setPlanning(on);
  print(Entry::notice(NoticeLevel::Info, planModeText(on)));
}

void App::offerPlan() {
  const QList<Tui::PickerItem> items{
      {"go", "Carry out the plan", "plan mode off, and the agent starts"},
      {"keep", "Keep planning", "stay in plan mode and say what to change"},
  };
  Tui::PickerOptions options;
  options.help = "enter picks · esc keeps planning";
  openPicker(std::make_unique<Tui::Picker>("The plan is ready", items, QString(), options), [this](const QString& value) {
    if (value == "go") {
      m_pAgent->setPlanning(false);
      print(Entry::notice(NoticeLevel::Info, planModeText(false)));
      submit(GO_AHEAD);
    }
  });
}

void App::showChanges() {
  for (const Entry& entry : changesEntries(m_pAgent->sessionChanges(), m_project.cwd)) {
    print(entry);
  }
}

void App::setTheme(const QString& arg) {
  // Applied again when it's the current one, so an edited theme file shows.
  const auto apply = [this](const QString& name) {
    QString problem;
    const std::optional<ThemeSpec> theme = findTheme(name, &problem);
    if (!theme) {
      print(Entry::notice(NoticeLevel::Error, problem));
      return;
    }
    m_options.theme = *theme;
    Tui::setPalette(terminalPalette(*theme));
    m_options.savePreferences(PreferenceUpdate{{}, {}, {}, theme->name});
    QString note;
    if (theme->base == Theme::Light) {
      note = " It suits a terminal with a light background.";
    }
    if (Tui::detectColorDepth() == Tui::ColorDepth::Basic) {
      note += " This terminal shows 16 colors, so its own shades stay.";
    }
    print(Entry::notice(NoticeLevel::Info, QString("Theme: %1.%2").arg(theme->name, note)));
    // Everything on screen again, in the new colors.
    m_printedWidth = -1;
    reprint();
  };
  if (!arg.isEmpty()) {
    apply(arg);
    return;
  }
  const ThemeList themes = listThemes();
  for (const QString& problem : themes.problems) {
    print(Entry::notice(NoticeLevel::Warn, problem));
  }
  QList<Tui::PickerItem> items;
  for (const ThemeSpec& theme : themes.themes) {
    items.append({theme.name, theme.name, themeHint(theme)});
  }
  openPicker(std::make_unique<Tui::Picker>("Select theme", items, m_options.theme.name), apply);
}

} // namespace Smith::Ui
