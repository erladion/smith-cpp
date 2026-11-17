#pragma once

#include "Entries.h"

namespace Smith::Ui {

class App;

// The pickers for /sessions and /open: which conversation, in which folder.
// The controller switches.
class SessionCommands {
public:
  explicit SessionCommands(App& app);

  // /open: recent project folders, and a way into the folder browser.
  void openProjectPicker();
  void openSessionPicker(bool everywhere);

private:
  void browseFolder(const QString& dir);
  void pickFolder(const QString& value);

  App& m_app;
};

} // namespace Smith::Ui
