#pragma once

#include <QMap>
#include <QString>

namespace Smith {

// Choices remembered between runs. Models are per server, since each serves its own set.
struct Preferences {
  QString reasoning;
  // Name of the endpoint used last.
  QString endpoint;
  // Base URL → model id.
  QMap<QString, QString> models;
};

Preferences loadPreferences();

struct PreferenceChange {
  QString reasoning;
  QString endpoint;
  QString modelBaseUrl;
  QString modelId;
};

// Merges the non-empty fields into the saved preferences. Failing to save is ignored.
void savePreferences(const PreferenceChange& change);

} // namespace Smith
