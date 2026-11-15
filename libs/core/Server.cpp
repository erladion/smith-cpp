#include "core/Server.h"
#include "compat/Utf8.h"

#include "core/Errors.h"
#include "core/Http.h"

#include <QJsonArray>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QUrl>

namespace Smith {

Server::Server(const QString& baseUrl, const QString& apiKey, const ServerOptions& options)
    : m_baseUrl(baseUrl), m_apiKey(apiKey), m_options(options) {}

std::shared_ptr<Server> Server::fromEndpoint(const Endpoint& endpoint, const QString& apiKeyOverride) {
  const QString key = !apiKeyOverride.isEmpty() ? apiKeyOverride : (!endpoint.apiKey.isEmpty() ? endpoint.apiKey : QString("no-key"));
  return std::make_shared<Server>(endpoint.baseUrl, key, ServerOptions{endpoint.name, endpoint.model, endpoint.contextSize});
}

QString Server::label() const {
  return m_options.name.isEmpty() ? m_baseUrl : QString("%1 (%2)").arg(m_options.name, m_baseUrl);
}

QString Server::url(const QString& path) const {
  QString base = m_baseUrl;
  while (base.endsWith('/')) {
    base.chop(1);
  }
  return base + path;
}

QList<ModelInfo> Server::listModels(bool refresh, const CancelPtr& cancel) {
  {
    QMutexLocker lock(&m_mutex);
    if (!refresh && m_models) {
      return *m_models;
    }
  }
  // Don't cache failures; the server may come up later.
  const QJsonValue response = Http::getJson(url("/models"), m_apiKey, 60000, cancel);
  QList<ModelInfo> models;
  for (const QJsonValue& value : response.toObject().value("data").toArray()) {
    const QJsonObject model = value.toObject();
    ModelInfo info{model.value("id").toString(), std::nullopt};
    if (model.value("max_model_len").isDouble()) {
      info.contextSize = model.value("max_model_len").toInt();
    }
    models.append(info);
  }
  QMutexLocker lock(&m_mutex);
  m_models = models;
  return models;
}

std::optional<QList<ModelInfo>> Server::cachedModels() const {
  QMutexLocker lock(&m_mutex);
  return m_models;
}

QString Server::chooseModel(const QStringList& preferred, const CancelPtr& cancel) {
  QList<ModelInfo> models;
  try {
    models = listModels(false, cancel);
  } catch (const AbortError&) {
    throw;
  } catch (const ConnectionError& error) {
    if (!m_options.model.isEmpty()) {
      return m_options.model;
    }
    // A connection error explains itself.
    throw SmithError(describeError(error, m_baseUrl));
  } catch (const std::exception& error) {
    // The server may not implement /v1/models; a configured model still works.
    if (!m_options.model.isEmpty()) {
      return m_options.model;
    }
    throw SmithError(QString("Could not list models at %1: %2\nPass --model, or set \"model\" for the endpoint in the config file.")
                         .arg(m_baseUrl, describeError(error, m_baseUrl)));
  }
  QStringList ids;
  for (const ModelInfo& model : models) {
    ids << model.id;
  }
  for (const QString& id : preferred + QStringList{m_options.model}) {
    if (!id.isEmpty() && ids.contains(id)) {
      return id;
    }
  }
  if (!ids.isEmpty()) {
    return ids.first();
  }
  if (!m_options.model.isEmpty()) {
    return m_options.model;
  }
  throw SmithError(QString("%1 lists no models. Pass --model.").arg(m_baseUrl));
}

std::optional<int> Server::contextSize(const QString& model, const CancelPtr& cancel) {
  if (m_options.contextSize) {
    return m_options.contextSize;
  }
  try {
    for (const ModelInfo& info : listModels(false, cancel)) {
      if (info.id == model && info.contextSize) {
        return info.contextSize;
      }
    }
  } catch (const std::exception&) {
    // Fall through to /props.
  }
  QString root = m_baseUrl;
  root.remove(QRegularExpression("/v1/?$"));
  try {
    const QJsonValue props = Http::getJson(QString("%1/props?model=%2").arg(root, utf8(QUrl::toPercentEncoding(model))), m_apiKey, 3000, cancel);
    const QJsonObject object = props.toObject();
    QJsonValue n = object.value("default_generation_settings").toObject().value("n_ctx");
    if (!n.isDouble()) {
      n = object.value("n_ctx");
    }
    if (n.isDouble() && n.toInt() > 0) {
      return n.toInt();
    }
  } catch (const std::exception&) {
    // Unknown; compaction waits for the server to say the prompt is too long.
  }
  return std::nullopt;
}

} // namespace Smith
