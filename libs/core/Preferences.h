#pragma once

#include <QMap>
#include <QString>
#include <QStringList>

namespace Smith {

// Choices remembered between runs. Models are per server, since each serves its own set.
struct Preferences {
  QString reasoning;
  // The theme picked with /theme; empty until one is.
  QString theme;
  // Name of the endpoint used last.
  QString endpoint;
  // Base URL → model id.
  QMap<QString, QString> models;
  // Project folder → what the user allowed to run there without asking.
  QMap<QString, QStringList> allowed;
};

Preferences loadPreferences();

struct PreferenceChange {
  QString reasoning;
  QString endpoint;
  QString modelBaseUrl;
  QString modelId;
  // With a folder: what is allowed there now (replacing what was; none forgets the folder).
  QString allowedIn;
  QStringList allowed;
  QString theme;
};

// Merges the non-empty fields into the saved preferences. Failing to save is ignored.
void savePreferences(const PreferenceChange& change);

} // namespace Smith
