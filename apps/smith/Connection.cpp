#include "Connection.h"

#include "core/Reasoning.h"
#include "App.h"

#include <QLocale>

namespace Smith::Ui {

namespace {

QString windowText(std::optional<int> contextSize) {
  return contextSize ? QLocale(QLocale::English).toString(*contextSize) + "-token context" : QString("context size unknown");
}

} // namespace

ConnectionCommands::ConnectionCommands(App& app) : m_app(app) {}

void ConnectionCommands::switchModel(const QString& id) {
  const ServerPtr server = m_app.server();
  m_app.withActivity(QString("Checking %1…").arg(id), [this, server, id]() -> std::function<void()> {
    const std::optional<int> contextSize = server->contextSize(id);
    return [this, server, id, contextSize]() {
      m_app.agent()->setModel(id, contextSize);
      m_app.opts().savePreferences(PreferenceUpdate{server, id, {}});
      m_app.print(Entry::notice(NoticeLevel::Info, QString("Switched to %1 (%2).").arg(id, windowText(contextSize))));
    };
  });
}

void ConnectionCommands::switchReasoning(const QString& level) {
  m_app.agent()->setReasoning(level);
  m_app.opts().savePreferences(PreferenceUpdate{m_app.server(), {}, level});
  m_app.print(Entry::notice(NoticeLevel::Info, QString("Reasoning: %1 (%2).").arg(level, reasoningHint(level))));
}

void ConnectionCommands::setReasoning(const QString& arg) {
  if (isReasoningLevel(arg)) {
    switchReasoning(arg);
    return;
  }
  m_app.print(Entry::notice(NoticeLevel::Error, QString("Unknown reasoning level \"%1\". Use one of: %2.").arg(arg, REASONING_LEVELS.join(", "))));
}

void ConnectionCommands::switchEndpoint(const QString& name, const QString& preferredModel) {
  const Endpoint* endpoint = nullptr;
  QStringList known;
  for (const Endpoint& candidate : m_app.opts().endpoints) {
    known << candidate.name;
    if (candidate.name == name) {
      endpoint = &candidate;
    }
  }
  if (!endpoint) {
    m_app.print(Entry::notice(NoticeLevel::Error,
                              QString("Unknown endpoint \"%1\". Configured: %2.").arg(name, known.isEmpty() ? QString("none") : known.join(", "))));
    return;
  }
  const Endpoint chosen = *endpoint;
  const auto remembered = m_app.opts().rememberedModel;
  m_app.withActivity(QString("Connecting to %1…").arg(name), [this, chosen, preferredModel, remembered]() -> std::function<void()> {
    const ServerPtr next = Server::fromEndpoint(chosen);
    const QString model = next->chooseModel({preferredModel, remembered(next->baseUrl())});
    const std::optional<int> contextSize = next->contextSize(model);
    return [this, next, model, contextSize]() {
      m_app.agent()->setModel(model, contextSize, next);
      m_app.setServer(next);
      m_app.refreshModels();
      m_app.opts().savePreferences(PreferenceUpdate{next, model, {}});
      m_app.print(Entry::notice(NoticeLevel::Info, QString("Connected to %1, using %2 (%3).").arg(next->label(), model, windowText(contextSize))));
    };
  });
}

void ConnectionCommands::openEndpointPicker() {
  const QList<Endpoint>& endpoints = m_app.opts().endpoints;
  if (endpoints.isEmpty()) {
    m_app.print(Entry::notice(NoticeLevel::Info,
                              QString("No endpoints are configured. Add them to %1, for example:\n%2").arg(m_app.opts().configFile, EXAMPLE_CONFIG)));
    return;
  }
  QList<Tui::PickerItem> items;
  for (const Endpoint& endpoint : endpoints) {
    items.append({endpoint.name, endpoint.name, endpoint.model.isEmpty() ? endpoint.baseUrl : endpoint.baseUrl + " · " + endpoint.model});
  }
  const QString current = m_app.server()->name();
  m_app.openPicker(std::make_unique<Tui::Picker>("Select an endpoint", items, current), [this, current](const QString& name) {
    if (name != current) {
      switchEndpoint(name);
    }
  });
}

void ConnectionCommands::openModelPicker() {
  const ServerPtr server = m_app.server();
  m_app.withActivity("Loading models…", [this, server]() -> std::function<void()> {
    const QList<ModelInfo> models = server->listModels(true);
    return [this, models]() {
      if (models.isEmpty()) {
        m_app.print(Entry::notice(NoticeLevel::Warn, "The server lists no models. Use /model <name> to set one directly."));
        return;
      }
      QList<Tui::PickerItem> items;
      for (const ModelInfo& model : models) {
        items.append({model.id, model.id, model.contextSize ? QLocale(QLocale::English).toString(*model.contextSize) + " ctx" : QString()});
      }
      const QString current = m_app.agent()->model();
      m_app.openPicker(std::make_unique<Tui::Picker>("Select a model", items, current), [this, current](const QString& id) {
        if (id != current) {
          switchModel(id);
        }
      });
    };
  });
}

void ConnectionCommands::openReasoningPicker() {
  QList<Tui::PickerItem> items;
  for (const QString& level : REASONING_LEVELS) {
    items.append({level, level, reasoningHint(level)});
  }
  const QString current = m_app.agent()->reasoning();
  m_app.openPicker(std::make_unique<Tui::Picker>("Select reasoning level", items, current), [this, current](const QString& level) {
    if (isReasoningLevel(level) && level != current) {
      switchReasoning(level);
    }
  });
}

} // namespace Smith::Ui
