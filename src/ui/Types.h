#pragma once

#include "app/Frontend.h"
#include "core/Config.h"
#include "core/Jobs.h"
#include "core/Sandbox.h"

#include <functional>
#include <memory>
#include <optional>

namespace Smith::Ui {

// The frontend types live in app/Frontend.h; these keep the Ui:: names working.
using Smith::AgentHooks;
using Smith::AgentSettings;
using Smith::OpenedSession;
using Smith::Pending;
using Smith::PreferenceUpdate;
using Smith::ProjectView;
using Smith::RecentProject;
using Smith::SessionControl;

struct TuiOptions {
  // Creates the agent for the current project; with `current`, carrying over those settings.
  std::function<std::shared_ptr<Agent>(const AgentHooks&, const std::optional<AgentSettings>&)> createAgent;
  ProjectView project;
  // Makes another folder the project (/new <folder>). Throws if it isn't a folder.
  std::function<ProjectView(const QString& folder)> switchProject;
  ServerPtr server;
  // Endpoints from the config file, for /endpoint.
  QList<Endpoint> endpoints;
  QString configFile;
  // Problems found while loading the config, shown at startup.
  QStringList warnings;
  // The model last used on a server, if remembered.
  std::function<QString(const QString& baseUrl)> rememberedModel;
  std::shared_ptr<Sandbox> sandbox;
  SessionControl sessions;
  // Conversation restored from a saved session, shown before the prompt.
  Messages resumed;
  std::optional<QDateTime> resumedLastUsed;
  // Remembers the user's endpoint, model and reasoning choices for next time.
  std::function<void(const PreferenceUpdate&)> savePreferences;
  Theme theme = Theme::Matrix;
  // With the matrix theme: the idle stream of glyphs in the status line.
  bool codeStream = true;
  // Commands the agent started in the background (/jobs, the status line).
  std::shared_ptr<BackgroundJobs> jobs;
};

} // namespace Smith::Ui
