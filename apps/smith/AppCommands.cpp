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

// ---- what the controller hands the terminal ----

bool App::runAppCommand(const QString& command, const QString& arg) {
  if (command == "/worktree") {
    m_pWorktrees->create(arg);
  } else if (command == "/worktrees") {
    m_pWorktrees->openPicker();
  } else if (command == "/land") {
    m_pWorktrees->land(arg);
  } else if (command == "/drop") {
    m_pWorktrees->drop(arg);
  } else {
    return false;
  }
  return true;
}

void App::openPickerFor(const QString& kind, const QString& arg) {
  if (kind == "model") {
    m_pConnection->openModelPicker();
  } else if (kind == "endpoint") {
    m_pConnection->openEndpointPicker();
  } else if (kind == "reasoning") {
    m_pConnection->openReasoningPicker();
  } else if (kind == "theme") {
    pickTheme();
  } else if (kind == "sessions") {
    m_pSessions->openSessionPicker(arg == "all");
  } else if (kind == "open") {
    m_pSessions->openProjectPicker();
  } else if (kind == "skills") {
    listSkills();
  } else if (kind == "jobs") {
    listJobs();
  } else if (kind == "plan") {
    offerPlan();
  }
}

void App::listSkills() {
  const ProjectView& project = m_controller.project();
  if (project.skills.isEmpty()) {
    print(Entry::notice(NoticeLevel::Info, "No skills found. Add folders with a SKILL.md to .agents/skills/ in the project or ~/.config/smith/skills/."));
    return;
  }
  static const QRegularExpression whitespace(R"(\s+)");
  QStringList lines;
  for (const Skill& skill : project.skills) {
    lines << skill.name + ": " + QString(skill.description).replace(whitespace, " ");
  }
  print(Entry::notice(NoticeLevel::Info, lines.join('\n')));
}

void App::listJobs() {
  const auto jobs = m_options.controller.jobs;
  const QList<JobStatus> list = jobs ? jobs->list() : QList<JobStatus>();
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

void App::pickTheme() {
  QList<Tui::PickerItem> items;
  for (const ThemeSpec& theme : listThemes().themes) {
    items.append({theme.name, theme.name, themeHint(theme)});
  }
  // Applied again when it's the current one, so an edited theme file shows.
  openPicker(std::make_unique<Tui::Picker>("Select theme", items, m_controller.theme().name),
             [this](const QString& name) { m_controller.setTheme(name); });
}

void App::applyTheme() {
  Tui::setPalette(terminalPalette(m_controller.theme()));
  // Everything on screen again, in the new colors.
  m_printedWidth = -1;
  reprint();
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
      m_controller.carryOutPlan();
    }
  });
}

void App::playRedPill() {
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
}

} // namespace Smith::Ui
