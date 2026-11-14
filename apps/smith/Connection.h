#pragma once

#include <QString>

namespace Smith::Ui {

class App;

// /endpoint, /model and /reasoning: what the agent talks to, and how.
class ConnectionCommands {
public:
  explicit ConnectionCommands(App& app);

  void switchModel(const QString& id);
  void switchReasoning(const QString& level);
  // /reasoning <level>, with an error for a level that doesn't exist.
  void setReasoning(const QString& arg);
  // Connects to a configured endpoint, on `preferredModel` if it serves it, else the model used there last.
  void switchEndpoint(const QString& name, const QString& preferredModel = {});

  void openEndpointPicker();
  void openModelPicker();
  void openReasoningPicker();

private:
  App& m_app;
};

} // namespace Smith::Ui
