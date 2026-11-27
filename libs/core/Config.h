#pragma once

#include "core/Sandbox.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace Smith {

// The image used in container mode when the config names none.
inline const QString DEFAULT_IMAGE = QStringLiteral("registry.access.redhat.com/ubi9/ubi:latest");

extern const QString EXAMPLE_CONFIG;

// The sandbox settings, with defaults filled in and ~ expanded.
struct SandboxConfig {
  SandboxType type = SandboxType::Bwrap;
  bool network = false;
  QStringList env;
  std::optional<Limits> limits;
  HomeView home;
  ContainerSettings container;
};

// What goes with requests for a model: an endpoint's, or a "models" entry's.
struct ModelSettings {
  // Fields added to every request, such as sampling settings ("top_k", "presence_penalty").
  QJsonObject params;
  // Whether a response that keeps repeating itself is stopped; unset leaves it to the level above.
  std::optional<bool> loopDetection;
  // How full the context window gets (0.1 to 0.95) before the conversation is compacted; unset leaves it to the level above.
  std::optional<double> compactAt;
};

struct Endpoint {
  QString name;
  QString baseUrl;
  QString apiKey;
  QString model;
  std::optional<int> contextSize;
  ModelSettings settings;
};

// A "models" entry: settings for the models whose names match `pattern`
// (an exact name, or a wildcard like "qwen3*"; case doesn't matter).
struct ModelEntry {
  QString pattern;
  ModelSettings settings;
};

// "matrix" adds digital rain at startup and a katakana spinner; "plain" and
// "light" none of it. Each has its own colors; in the terminal, light is for
// one with a light background.
enum class Theme { Matrix, Plain, Light };

// The themes' names, as the config and /theme take them.
extern const QStringList THEME_NAMES;
QString themeName(Theme theme);
std::optional<Theme> themeNamed(const QString& name);
// What a built-in theme looks like, for pickers.
QString themeHint(Theme theme);

struct Config {
  QString file;
  QList<Endpoint> endpoints;
  QString defaultEndpoint;
  // Model calls allowed per message you send.
  std::optional<int> maxSteps;
  // Whether the model gets the task tool (sub-agents).
  bool subagents = true;
  // Whether tool calls are logged to logs/<session>.jsonl.
  bool toolLog = true;
  // A built-in theme's name, or a file's in the themes folder (see Themes.h).
  QString theme = "matrix";
  // Whether the matrix theme shows the idle stream of glyphs in the status line.
  bool codeStream = true;
  SandboxConfig sandbox;
  // Whether changes to files that run outside the sandbox later always ask, and are reported.
  bool protectRunLater = true;
  // What runs without asking, and what is refused: a tool's name, or "bash:<start of a command>".
  QStringList allow;
  QStringList deny;
  // Off unless the config turns it on, here, for an endpoint, or for a model.
  bool loopDetection = false;
  // Unset: Smith's own default.
  std::optional<double> compactAt;
  // In the file's order.
  QList<ModelEntry> models;
  // Problems worth telling the user about that don't stop the agent.
  QStringList warnings;

  const Endpoint* findEndpoint(const QString& name) const;
  // What applies to `model` on the endpoint named `endpoint` (empty for a server given by URL): the
  // top level, then the endpoint, then the model's entry, each over the one before. loopDetection is always set.
  ModelSettings settingsFor(const QString& endpoint, const QString& model) const;
};

QString configPath();

// Loads the config file. A missing file means no endpoints; an invalid one throws SmithError.
Config loadConfig(const QString& file = configPath());

// "8g", "512m", "1.5GB" or plain bytes, in bytes. Throws for anything else.
qint64 parseSize(const QString& text);

} // namespace Smith
