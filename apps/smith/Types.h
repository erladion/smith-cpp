#pragma once

#include "frontend/Controller.h"

namespace Smith::Ui {

// The frontend types live in frontend/; these keep the Ui:: names working.
using Smith::AgentHooks;
using Smith::AgentSettings;
using Smith::OpenedSession;
using Smith::PreferenceUpdate;
using Smith::ProjectView;
using Smith::RecentProject;
using Smith::SessionControl;

struct TuiOptions {
  // What the controller works with: the runtime's parts, the theme and startup warnings.
  ControllerOptions controller;
  // With the matrix theme: the idle stream of glyphs in the status line.
  bool codeStream = true;
};

} // namespace Smith::Ui
