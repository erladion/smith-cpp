#include "Backend.h"

#include "frontend/Commands.h"

#include <QUrl>

namespace Smith::Desktop {

namespace {

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
         "\nKeys: enter sends · shift+enter adds a new line · esc interrupts the agent · shift+tab switches plan mode\n"
         "      tab, while the agent works, queues the next task · ↑/↓ in an empty box recall earlier input\n"
         "      y / a / n answer an approval · ctrl+n new session · ctrl+o open a folder · ctrl+k pick a model";
}

} // namespace

// ---- commands ----

bool Backend::runAppCommand(const QString& command, const QString& arg) {
  if (command == "/help") {
    notice(NoticeLevel::Info, helpText());
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
  } else {
    return false;
  }
  return true;
}

void Backend::pickTheme() {
  m_controller.pickTheme();
}

void Backend::setTheme(const QString& name) {
  m_controller.setTheme(name);
}

void Backend::setReasoning(const QString& level) {
  m_controller.setReasoning(level);
}

void Backend::setModel(const QString& id) {
  m_controller.setModel(id);
}

void Backend::setEndpoint(const QString& name) {
  m_controller.setEndpoint(name);
}

void Backend::refreshModels() {
  m_controller.refreshModels(true);
}

void Backend::setNetwork(bool on) {
  m_controller.setNetwork(on);
}

void Backend::setPlanning(bool on) {
  m_controller.setPlanning(on);
}

void Backend::carryOutPlan() {
  m_controller.carryOutPlan();
}

void Backend::showChanges() {
  m_controller.showChanges();
}

void Backend::undo() {
  m_controller.undo();
}

void Backend::compact() {
  m_controller.compact();
}

void Backend::stopJob(const QString& id) {
  m_controller.stopJob(id);
}

// ---- sessions and projects ----

void Backend::newSession(const QString& folder) {
  m_controller.newSession(folder);
}

void Backend::openProject(const QString& folder) {
  const QUrl url(folder);
  m_controller.newSession(url.isLocalFile() ? url.toLocalFile() : folder);
}

void Backend::openSession(const QString& id) {
  QString folder;
  for (const QVariant& session : m_sessions) {
    if (session.toMap().value("id").toString() == id) {
      folder = session.toMap().value("cwd").toString();
    }
  }
  m_controller.openSession(id, folder);
}

void Backend::deleteSession(const QString& id) {
  QString name = id;
  for (const QVariant& session : m_sessions) {
    if (session.toMap().value("id").toString() == id) {
      name = session.toMap().value("name").toString();
    }
  }
  m_controller.deleteSession(id, name);
}

void Backend::renameSession(const QString& name) {
  if (!name.trimmed().isEmpty()) {
    m_controller.nameSession(name);
  }
}

} // namespace Smith::Desktop
