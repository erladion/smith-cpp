#include "core/Preferences.h"

#include "core/Dirs.h"
#include "core/Reasoning.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace Smith {

namespace {

QString preferencesFile() {
  return configDir() + "/preferences.json";
}

} // namespace

Preferences loadPreferences() {
  Preferences prefs;
  QFile handle(preferencesFile());
  if (!handle.open(QIODevice::ReadOnly)) {
    return prefs;
  }
  const QJsonDocument document = QJsonDocument::fromJson(handle.readAll());
  if (!document.isObject()) {
    return prefs;
  }
  const QJsonObject raw = document.object();
  const QString reasoning = raw.value("reasoning").toString();
  if (isReasoningLevel(reasoning)) {
    prefs.reasoning = reasoning;
  }
  prefs.endpoint = raw.value("endpoint").toString();
  const QJsonObject models = raw.value("models").toObject();
  for (auto it = models.begin(); it != models.end(); ++it) {
    if (it.value().isString()) {
      prefs.models.insert(it.key(), it.value().toString());
    }
  }
  return prefs;
}

void savePreferences(const PreferenceChange& change) {
  Preferences prefs = loadPreferences();
  if (!change.reasoning.isEmpty()) {
    prefs.reasoning = change.reasoning;
  }
  if (!change.endpoint.isEmpty()) {
    prefs.endpoint = change.endpoint;
  }
  if (!change.modelBaseUrl.isEmpty() && !change.modelId.isEmpty()) {
    prefs.models.insert(change.modelBaseUrl, change.modelId);
  }
  QJsonObject out;
  if (!prefs.reasoning.isEmpty()) {
    out["reasoning"] = prefs.reasoning;
  }
  if (!prefs.endpoint.isEmpty()) {
    out["endpoint"] = prefs.endpoint;
  }
  if (!prefs.models.isEmpty()) {
    QJsonObject models;
    for (auto it = prefs.models.begin(); it != prefs.models.end(); ++it) {
      models[it.key()] = it.value();
    }
    out["models"] = models;
  }
  // Preferences are a convenience; failing to save them shouldn't break anything.
  privateDir(configDir());
  QSaveFile handle(preferencesFile());
  if (!handle.open(QIODevice::WriteOnly)) {
    return;
  }
  handle.write(QJsonDocument(out).toJson(QJsonDocument::Indented));
  if (handle.commit()) {
    privateFile(preferencesFile());
  }
}

} // namespace Smith
