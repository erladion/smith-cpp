#include "Connection.h"

#include "core/Reasoning.h"
#include "App.h"

#include <QLocale>

namespace Smith::Ui {

ConnectionCommands::ConnectionCommands(App& app) : m_app(app) {}

void ConnectionCommands::openEndpointPicker() {
  const QList<Endpoint>& endpoints = m_app.opts().controller.endpoints;
  if (endpoints.isEmpty()) {
    m_app.print(Entry::notice(NoticeLevel::Info, QString("No endpoints are configured. Add them to %1, for example:\n%2")
                                                     .arg(m_app.opts().controller.configFile, EXAMPLE_CONFIG)));
    return;
  }
  QList<Tui::PickerItem> items;
  for (const Endpoint& endpoint : endpoints) {
    items.append({endpoint.name, endpoint.name, endpoint.model.isEmpty() ? endpoint.baseUrl : endpoint.baseUrl + " · " + endpoint.model});
  }
  const QString current = m_app.server()->name();
  m_app.openPicker(std::make_unique<Tui::Picker>("Select an endpoint", items, current), [this, current](const QString& name) {
    if (name != current) {
      m_app.controller().setEndpoint(name);
    }
  });
}

void ConnectionCommands::openModelPicker() {
  const ServerPtr server = m_app.server();
  m_app.withActivity("Loading models…", [this, server, closing = m_app.controller().turns().closing()]() -> std::function<void()> {
    const QList<ModelInfo> models = server->listModels(true, closing);
    return [this, models]() {
      if (models.isEmpty()) {
        m_app.print(Entry::notice(NoticeLevel::Warn, "The server lists no models. Use /model <name> to set one directly."));
        return;
      }
      QList<Tui::PickerItem> items;
      for (const ModelInfo& model : models) {
        items.append({model.id, model.id, model.contextSize ? QLocale(QLocale::English).toString(*model.contextSize) + " ctx" : QString()});
      }
      m_app.openPicker(std::make_unique<Tui::Picker>("Select a model", items, m_app.agent()->model()),
                       [this](const QString& id) { m_app.controller().setModel(id); });
    };
  });
}

void ConnectionCommands::openReasoningPicker() {
  QList<Tui::PickerItem> items;
  for (const QString& level : REASONING_LEVELS) {
    items.append({level, level, reasoningHint(level)});
  }
  m_app.openPicker(std::make_unique<Tui::Picker>("Select reasoning level", items, m_app.agent()->reasoning()),
                   [this](const QString& level) { m_app.controller().setReasoning(level); });
}

} // namespace Smith::Ui
