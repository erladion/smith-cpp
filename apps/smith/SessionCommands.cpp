#include "SessionCommands.h"

#include "core/Dirs.h"
#include "core/Errors.h"
#include "core/Tool.h"
#include "App.h"
#include "frontend/Duration.h"
#include "frontend/Folders.h"
#include "frontend/Replay.h"

#include <QFileInfo>
#include <QLocale>
#include <QMap>
#include <QRegularExpression>

#include <cmath>

namespace Smith::Ui {

SessionCommands::SessionCommands(App& app) : m_app(app) {}

void SessionCommands::openProjectPicker() {
  const QString cwd = m_app.project().cwd;
  QList<Tui::PickerItem> items{{"browse:" + cwd, "Browse folders…", "starting from the current project"}};
  for (const RecentProject& project : m_app.opts().controller.sessions.recentProjects()) {
    if (project.cwd == cwd || !pathExists(project.cwd)) {
      continue;
    }
    // The folder's name stays readable even when its full path is long.
    const QString label = QFileInfo(project.cwd).fileName().isEmpty() ? project.cwd : QFileInfo(project.cwd).fileName();
    items.append({"use:" + project.cwd, label,
                  QString("%1 · %2 · %3").arg(plural(project.sessions, "session"), ago(project.updated), tildify(QFileInfo(project.cwd).path()))});
  }
  m_app.openPicker(std::make_unique<Tui::Picker>("Open a project", items), [this](const QString& value) { pickFolder(value); });
}

void SessionCommands::browseFolder(const QString& dir) {
  QMap<QString, int> sessionCounts;
  for (const RecentProject& project : m_app.opts().controller.sessions.recentProjects()) {
    sessionCounts.insert(project.cwd, project.sessions);
  }
  auto describe = [&](const QString& folder) {
    QStringList parts;
    if (isGitRepo(folder)) {
      parts << "git";
    }
    if (sessionCounts.value(folder)) {
      parts << plural(sessionCounts.value(folder), "session");
    }
    return parts.join(" · ");
  };
  const QString parent = QFileInfo(dir).path();
  const FolderListing listing = listFolders(dir);
  QString useHint = describe(dir);
  if (useHint.isEmpty() && dir == m_app.project().cwd) {
    useHint = "the current project";
  }
  QList<Tui::PickerItem> items{{"use:" + dir, "✓ Use this folder", useHint}};
  if (parent != dir) {
    items.append({"browse:" + parent, "..", "up one level"});
  }
  for (const QString& name : listing.names) {
    const QString path = dir == "/" ? "/" + name : dir + "/" + name;
    items.append({"browse:" + path, name + "/", describe(path)});
  }
  if (!listing.error.isEmpty()) {
    items.append({"browse:" + dir, "(" + listing.error + ")", {}});
  }
  Tui::PickerOptions options;
  options.back = parent != dir ? "browse:" + parent : QString();
  options.help = "type to filter · enter opens · backspace goes up · esc cancels";
  m_app.openPicker(std::make_unique<Tui::Picker>(tildify(dir), items, QString(), options), [this](const QString& value) { pickFolder(value); });
}

void SessionCommands::pickFolder(const QString& value) {
  if (value.startsWith("browse:")) {
    browseFolder(value.mid(7));
  } else if (value.startsWith("use:")) {
    m_app.controller().newSession(value.mid(4));
  }
}

void SessionCommands::openSessionPicker(bool everywhere) {
  const QString current = m_app.opts().controller.sessions.currentId();
  const QList<SessionInfo> sessions = m_app.opts().controller.sessions.list(everywhere);
  if (sessions.isEmpty() || (sessions.size() == 1 && sessions.first().id == current)) {
    const QString where = everywhere ? QString() : QString(" for this folder; /sessions all shows every folder's");
    m_app.print(Entry::notice(NoticeLevel::Info, QString("There are no other saved sessions%1.").arg(where)));
    return;
  }
  QMap<QString, SessionInfo> byId;
  QList<Tui::PickerItem> items;
  for (const SessionInfo& session : sessions) {
    byId.insert(session.id, session);
    QStringList hint;
    if (everywhere) {
      hint << shortPath(session.cwd, 32);
    }
    hint << ago(session.updated) << plural(session.turns, "message") << (session.endpoint.isEmpty() ? QString() : session.endpoint + "/") + session.model;
    items.append({session.id, session.name, hint.join(" · ")});
  }
  const QString title = everywhere ? "Switch session (all folders)" : "Switch session (this folder; /sessions all for every folder)";
  Tui::PickerOptions options;
  options.action = Tui::PickerAction{'d', "delete", [this, byId](const Tui::PickerItem& item) {
                                       if (item.value == m_app.opts().controller.sessions.currentId()) {
                                         return Tui::PickerAction::Check{{}, "That's the session you're in; switch to another one first."};
                                       }
                                       const int turns = byId.value(item.value).turns;
                                       return Tui::PickerAction::Check{
                                           QString("Delete \"%1\" (%2) and its tool log?").arg(item.label, plural(turns, "message")), {}};
                                     }};
  m_app.openPicker(
      std::make_unique<Tui::Picker>(title, items, current, options),
      [this, byId](const QString& id) { m_app.controller().openSession(id, byId.value(id).cwd); },
      [this, byId, everywhere](const QString& id) {
        m_app.controller().deleteSession(id, byId.contains(id) ? byId.value(id).name : id);
        openSessionPicker(everywhere);
      });
}

} // namespace Smith::Ui
