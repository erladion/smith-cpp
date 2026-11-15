#pragma once

#include "core/Cancel.h"
#include "core/Config.h"

#include <QList>
#include <QMutex>
#include <QString>

#include <memory>
#include <optional>

namespace Smith {

struct ModelInfo {
  QString id;
  // Context window, when the listing includes it (vLLM's max_model_len).
  std::optional<int> contextSize;
};

struct ServerOptions {
  // Endpoint name from the config file; empty for an ad-hoc --base-url.
  QString name;
  // Model to use when no other choice applies.
  QString model;
  // Context window to use instead of asking the server.
  std::optional<int> contextSize;
};

// One OpenAI-compatible server: where it is, its key, and what we can learn
// about it. All calls block; use them from a worker thread in the TUI.
class Server {
public:
  Server(const QString& baseUrl, const QString& apiKey, const ServerOptions& options = {});

  // A configured endpoint. Endpoints without a key get none: global keys from
  // the environment are never sent to a named server they weren't meant for.
  static std::shared_ptr<Server> fromEndpoint(const Endpoint& endpoint, const QString& apiKeyOverride = {});

  QString baseUrl() const { return m_baseUrl; }
  QString apiKey() const { return m_apiKey; }
  QString name() const { return m_options.name; }
  // "name (url)" for configured endpoints, the URL otherwise.
  QString label() const;
  // The URL for an API path, e.g. "/chat/completions".
  QString url(const QString& path) const;

  // Models the server advertises. Cached; pass refresh to ask again. Throws on failure.
  QList<ModelInfo> listModels(bool refresh = false, const CancelPtr& cancel = {});
  // The cached listing, if there is one, without asking.
  std::optional<QList<ModelInfo>> cachedModels() const;

  // Picks a model: the first of `preferred` the server lists, then the
  // endpoint's configured model, then the first model listed.
  QString chooseModel(const QStringList& preferred, const CancelPtr& cancel = {});

  // The context window for a model, if known: configured, then the model
  // listing (vLLM), then llama.cpp's /props.
  std::optional<int> contextSize(const QString& model, const CancelPtr& cancel = {});

private:
  QString m_baseUrl;
  QString m_apiKey;
  ServerOptions m_options;
  mutable QMutex m_mutex;
  std::optional<QList<ModelInfo>> m_models;
};

using ServerPtr = std::shared_ptr<Server>;

} // namespace Smith
