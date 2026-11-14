#pragma once

#include "Entries.h"

#include <optional>

namespace Smith::Ui {

class App;
class ConnectionCommands;

// /new, /sessions, /name and /open: which conversation, in which folder.
class SessionCommands {
public:
  SessionCommands(App& app, ConnectionCommands& connection);

  // Starts a new session, in another folder if one is given.
  void newSession(const QString& folder = {});
  void nameSession(const QString& name);
  // /open: recent project folders, and a way into the folder browser.
  void openProjectPicker();
  void openSessionPicker(bool everywhere);

private:
  // Makes `folder` the project with a fresh agent; notes to show, or nullopt if it isn't a folder.
  std::optional<QList<Entry>> enterProject(const QString& folder);
  void browseFolder(const QString& dir);
  void pickFolder(const QString& value);
  void deleteSession(const QString& id, bool everywhere, const QString& name);
  void switchSession(const QString& id, const QString& folder);

  App& m_app;
  ConnectionCommands& m_connection;
};

} // namespace Smith::Ui
