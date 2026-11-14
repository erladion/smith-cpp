#pragma once

#include "core/Sandbox.h"

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

struct Endpoint {
  QString name;
  QString baseUrl;
  QString apiKey;
  QString model;
  std::optional<int> contextSize;
};

// "matrix" adds digital rain at startup and a katakana spinner; "plain" none of it.
enum class Theme { Matrix, Plain };

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
  Theme theme = Theme::Matrix;
  // Whether the matrix theme shows the idle stream of glyphs in the status line.
  bool codeStream = true;
  SandboxConfig sandbox;
  // Whether changes to files that run outside the sandbox later always ask, and are reported.
  bool protectRunLater = true;
  // Problems worth telling the user about that don't stop the agent.
  QStringList warnings;

  const Endpoint* findEndpoint(const QString& name) const;
};

QString configPath();

// Loads the config file. A missing file means no endpoints; an invalid one throws SmithError.
Config loadConfig(const QString& file = configPath());

// "8g", "512m", "1.5GB" or plain bytes, in bytes. Throws for anything else.
qint64 parseSize(const QString& text);

} // namespace Smith
