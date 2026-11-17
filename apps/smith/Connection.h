#pragma once

#include <QString>

namespace Smith::Ui {

class App;

// The pickers for /endpoint, /model and /reasoning; the controller switches.
class ConnectionCommands {
public:
  explicit ConnectionCommands(App& app);

  void openEndpointPicker();
  void openModelPicker();
  void openReasoningPicker();

private:
  App& m_app;
};

} // namespace Smith::Ui
