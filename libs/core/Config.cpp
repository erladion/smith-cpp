#include "core/Config.h"
#include "compat/Utf8.h"

#include "core/Dirs.h"
#include "core/Errors.h"
#include "core/Json.h"
#include "core/Themes.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>

#include <cmath>
#include <sys/stat.h>

namespace Smith {

const QStringList THEME_NAMES = {"matrix", "plain", "light"};

QString themeName(Theme theme) {
  return THEME_NAMES[int(theme)];
}

std::optional<Theme> themeNamed(const QString& name) {
  const int index = THEME_NAMES.indexOf(name);
  if (index < 0) {
    return std::nullopt;
  }
  return Theme(index);
}

QString themeHint(Theme theme) {
  switch (theme) {
  case Theme::Matrix:
    return "green on black, with digital rain";
  case Theme::Plain:
    return "neutral dark";
  case Theme::Light:
    return "dark on light";
  }
  return {};
}

const QString EXAMPLE_CONFIG = R"({
  "default": "local",
  "maxSteps": 50,
  "endpoints": {
    "local": { "baseURL": "http://localhost:8080/v1" },
    "gpu-box": {
      "baseURL": "http://10.0.0.5:8000/v1",
      "apiKeyEnv": "GPU_BOX_KEY",
      "model": "qwen3-coder-30b",
      "contextSize": 65536
    }
  }
})";

namespace {

const QRegularExpression SIZE(R"(^(\d+(?:\.\d+)?)\s*([kmgt]?)i?b?$)", QRegularExpression::CaseInsensitiveOption);

// Collects problems the way zod reports them: one "✖ message → at path" per problem.
class Checker {
public:
  void problem(const QString& path, const QString& message) {
    m_problems << QString("✖ %1").arg(message) + (path.isEmpty() ? QString() : QString("\n  → at %1").arg(path));
  }

  bool ok() const { return m_problems.isEmpty(); }
  QString text() const { return m_problems.join('\n'); }

  // Unknown keys are mistakes in a hand-written file: report them.
  void strict(const QJsonObject& object, const QStringList& allowed, const QString& path) {
    for (const QString& key : object.keys()) {
      if (!allowed.contains(key)) {
        problem(path, QString("Unrecognized key: \"%1\"").arg(key));
      }
    }
  }

  std::optional<QString> string(const QJsonObject& object, const QString& key, const QString& path, bool nonEmpty = false) {
    if (!object.contains(key)) {
      return std::nullopt;
    }
    const QJsonValue value = object.value(key);
    if (!value.isString()) {
      problem(join(path, key), "Invalid input: expected string");
      return std::nullopt;
    }
    if (nonEmpty && value.toString().isEmpty()) {
      problem(join(path, key), "Too small: expected string to have >=1 characters");
      return std::nullopt;
    }
    return value.toString();
  }

  std::optional<bool> boolean(const QJsonObject& object, const QString& key, const QString& path) {
    if (!object.contains(key)) {
      return std::nullopt;
    }
    if (!object.value(key).isBool()) {
      problem(join(path, key), "Invalid input: expected boolean");
      return std::nullopt;
    }
    return object.value(key).toBool();
  }

  std::optional<double> number(const QJsonObject& object, const QString& key, const QString& path, bool integer, double minimum, double maximum) {
    if (!object.contains(key)) {
      return std::nullopt;
    }
    const QJsonValue value = object.value(key);
    if (!value.isDouble()) {
      problem(join(path, key), "Invalid input: expected number");
      return std::nullopt;
    }
    const double n = value.toDouble();
    if (integer && std::floor(n) != n) {
      problem(join(path, key), "Invalid input: expected int, received number");
      return std::nullopt;
    }
    if (n < minimum || n > maximum) {
      problem(join(path, key), QString("Out of range: expected a number from %1 to %2").arg(minimum).arg(maximum));
      return std::nullopt;
    }
    return n;
  }

  std::optional<QStringList> strings(const QJsonObject& object, const QString& key, const QString& path, bool nonEmpty = false) {
    if (!object.contains(key)) {
      return std::nullopt;
    }
    if (!object.value(key).isArray()) {
      problem(join(path, key), "Invalid input: expected array");
      return std::nullopt;
    }
    QStringList out;
    const QJsonArray items = object.value(key).toArray();
    for (int i = 0; i < items.size(); i++) {
      if (!items[i].isString() || (nonEmpty && items[i].toString().isEmpty())) {
        problem(QString("%1[%2]").arg(join(path, key)).arg(i), "Invalid input: expected a non-empty string");
        continue;
      }
      out << items[i].toString();
    }
    return out;
  }

  std::optional<QJsonObject> object(const QJsonObject& parent, const QString& key, const QString& path) {
    if (!parent.contains(key)) {
      return std::nullopt;
    }
    if (!parent.value(key).isObject()) {
      problem(join(path, key), "Invalid input: expected object");
      return std::nullopt;
    }
    return parent.value(key).toObject();
  }

  static QString join(const QString& path, const QString& key) { return path.isEmpty() ? key : path + "." + key; }

private:
  QStringList m_problems;
};

// Fields Smith writes itself; the config can't replace them.
const QStringList RESERVED_PARAMS = {"model", "messages", "stream", "stream_options", "tools", "parallel_tool_calls"};

// "params" and "loopDetection", on an endpoint or a "models" entry.
ModelSettings modelSettings(const QJsonObject& raw, const QString& path, Checker& check) {
  ModelSettings settings;
  if (const auto params = check.object(raw, "params", path)) {
    for (const QString& key : params->keys()) {
      if (RESERVED_PARAMS.contains(key)) {
        check.problem(path + ".params." + key, "Smith sets this field itself; it can't be in params");
      }
    }
    settings.params = *params;
  }
  settings.loopDetection = check.boolean(raw, "loopDetection", path);
  return settings;
}

// "~/x" or a path relative to the home folder, as an absolute path.
QString homePath(const QString& path) {
  const QString expanded = expandHome(path);
  QString resolved = QDir::cleanPath(QDir(homeDir()).absoluteFilePath(expanded));
  while (resolved.size() > 1 && resolved.endsWith('/')) {
    resolved.chop(1);
  }
  return resolved.isEmpty() ? "/" : resolved;
}

SandboxConfig sandboxConfig(const QJsonObject& raw, Checker& check) {
  const QString at = "sandbox";
  check.strict(raw, {"type", "network", "env", "limits", "home", "container", "projects"}, at);
  SandboxConfig config;
  config.container.image = DEFAULT_IMAGE;

  if (const auto type = check.string(raw, "type", at)) {
    if (*type == "bwrap") {
      config.type = SandboxType::Bwrap;
    } else if (*type == "container") {
      config.type = SandboxType::Container;
    } else {
      check.problem(at + ".type", R"(Invalid option: expected one of "bwrap"|"container")");
    }
  }
  config.network = check.boolean(raw, "network", at).value_or(false);
  if (const auto env = check.strings(raw, "env", at)) {
    static const QRegularExpression name(R"(^[A-Za-z_][A-Za-z0-9_]*(=[\s\S]*)?$)");
    for (const QString& entry : *env) {
      if (!name.match(entry).hasMatch()) {
        check.problem(at + ".env", QString(R"(Invalid string: "NAME" or "NAME=value" (got "%1"))").arg(entry));
      }
    }
    config.env = *env;
  }
  if (const auto limits = check.object(raw, "limits", at)) {
    const QString path = at + ".limits";
    check.strict(*limits, {"memory", "processes", "cpus"}, path);
    Limits parsed;
    if (const auto memory = check.string(*limits, "memory", path)) {
      if (SIZE.match(*memory).hasMatch()) {
        parsed.memory = parseSize(*memory);
      } else {
        check.problem(path + ".memory", R"(Invalid string: a size such as "8g" or "512m")");
      }
    }
    if (const auto processes = check.number(*limits, "processes", path, true, 1, 1e9)) {
      parsed.processes = int(*processes);
    }
    if (const auto cpus = check.number(*limits, "cpus", path, false, 1e-9, 1e6)) {
      parsed.cpus = *cpus;
    }
    config.limits = parsed;
  }
  if (const auto home = check.object(raw, "home", at)) {
    check.strict(*home, {"show", "writable"}, at + ".home");
    for (const QString& dir : check.strings(*home, "show", at + ".home").value_or(QStringList())) {
      config.home.show << homePath(dir);
    }
    for (const QString& dir : check.strings(*home, "writable", at + ".home").value_or(QStringList())) {
      config.home.writable << homePath(dir);
    }
  }
  if (const auto container = check.object(raw, "container", at)) {
    const QString path = at + ".container";
    check.strict(*container, {"runtime", "image", "mounts"}, path);
    if (const auto runtime = check.string(*container, "runtime", path)) {
      if (*runtime == "podman" || *runtime == "docker") {
        config.container.runtime = *runtime;
      } else {
        check.problem(path + ".runtime", R"(Invalid option: expected one of "podman"|"docker")");
      }
    }
    if (const auto image = check.string(*container, "image", path, true)) {
      config.container.image = *image;
    }
    config.container.mounts = check.strings(*container, "mounts", path, true).value_or(QStringList());
  }
  if (const auto projects = check.object(raw, "projects", at)) {
    for (auto it = projects->begin(); it != projects->end(); ++it) {
      const QString path = at + ".projects." + it.key();
      if (!it.value().isObject()) {
        check.problem(path, "Invalid input: expected object");
        continue;
      }
      const QJsonObject project = it.value().toObject();
      check.strict(project, {"image", "mounts"}, path);
      ContainerProject entry;
      entry.folder = homePath(it.key());
      entry.image = check.string(project, "image", path, true).value_or(QString());
      entry.mounts = check.strings(project, "mounts", path, true).value_or(QStringList());
      config.container.projects.append(entry);
    }
  }
  return config;
}

} // namespace

const Endpoint* Config::findEndpoint(const QString& name) const {
  if (name.isEmpty()) {
    return nullptr;
  }
  for (const Endpoint& endpoint : endpoints) {
    if (endpoint.name == name) {
      return &endpoint;
    }
  }
  return nullptr;
}

ModelSettings Config::settingsFor(const QString& endpoint, const QString& model) const {
  ModelSettings out;
  out.loopDetection = loopDetection;
  auto apply = [&out](const ModelSettings& settings) {
    out.params = Json::overlay(out.params, settings.params);
    if (settings.loopDetection) {
      out.loopDetection = settings.loopDetection;
    }
  };
  if (const Endpoint* found = findEndpoint(endpoint)) {
    apply(found->settings);
  }
  // An exact name first, then the first pattern that matches.
  const ModelEntry* chosen = nullptr;
  for (const ModelEntry& entry : models) {
    if (entry.pattern.compare(model, Qt::CaseInsensitive) == 0) {
      chosen = &entry;
      break;
    }
  }
  for (int i = 0; !chosen && i < models.size(); i++) {
    const QRegularExpression pattern(QRegularExpression::wildcardToRegularExpression(models[i].pattern), QRegularExpression::CaseInsensitiveOption);
    if (pattern.match(model).hasMatch()) {
      chosen = &models[i];
    }
  }
  if (chosen) {
    apply(chosen->settings);
  }
  return out;
}

QString configPath() {
  return configDir() + "/config.json";
}

qint64 parseSize(const QString& text) {
  const QRegularExpressionMatch match = SIZE.match(text.trimmed());
  if (!match.hasMatch()) {
    throw SmithError(QString("Not a size: %1").arg(text));
  }
  const int power = QString(" kmgt").indexOf(match.captured(2).isEmpty() ? QString(" ") : match.captured(2).toLower());
  return qint64(std::llround(match.captured(1).toDouble() * std::pow(1024.0, power)));
}

Config loadConfig(const QString& file) {
  Config config;
  config.file = file;
  config.sandbox.container.image = DEFAULT_IMAGE;

  QFile handle(file);
  if (!handle.exists()) {
    return config;
  }
  if (!handle.open(QIODevice::ReadOnly)) {
    throw SmithError(QString("Could not read %1: %2").arg(file, handle.errorString()));
  }
  const QString text = utf8(handle.readAll());
  QJsonValue json;
  try {
    json = Json::parse(text);
  } catch (const std::exception& error) {
    throw SmithError(QString("%1 is not valid JSON: %2").arg(file, QString::fromStdString(error.what())));
  }
  if (!json.isObject()) {
    throw SmithError(QString("Invalid %1:\n✖ Invalid input: expected object").arg(file));
  }
  const QJsonObject root = json.toObject();
  Checker check;
  check.strict(root,
               {"default", "maxSteps", "subagents", "toolLog", "theme", "codeStream", "sandbox", "protectRunLater", "permissions", "loopDetection",
                "models", "endpoints"},
               {});

  config.defaultEndpoint = check.string(root, "default", {}).value_or(QString());
  if (const auto maxSteps = check.number(root, "maxSteps", {}, true, 1, 10000)) {
    config.maxSteps = int(*maxSteps);
  }
  config.subagents = check.boolean(root, "subagents", {}).value_or(true);
  config.toolLog = check.boolean(root, "toolLog", {}).value_or(true);
  if (const auto theme = check.string(root, "theme", {})) {
    // A theme file that can't be used is reported when it's loaded, without stopping Smith.
    if (themeNamed(*theme) || QFile::exists(themesDir() + "/" + *theme + ".json")) {
      config.theme = *theme;
    } else {
      check.problem("theme", QString("Invalid option: expected one of \"%1\", or a file's name in %2").arg(THEME_NAMES.join("\"|\""), tildify(themesDir())));
    }
  }
  config.codeStream = check.boolean(root, "codeStream", {}).value_or(true);
  config.protectRunLater = check.boolean(root, "protectRunLater", {}).value_or(true);
  if (const auto permissions = check.object(root, "permissions", {})) {
    check.strict(*permissions, {"allow", "deny"}, "permissions");
    config.allow = check.strings(*permissions, "allow", "permissions", true).value_or(QStringList());
    config.deny = check.strings(*permissions, "deny", "permissions", true).value_or(QStringList());
  }
  if (const auto sandbox = check.object(root, "sandbox", {})) {
    config.sandbox = sandboxConfig(*sandbox, check);
  }
  config.loopDetection = check.boolean(root, "loopDetection", {}).value_or(false);
  if (const auto models = check.object(root, "models", {})) {
    // In the file's order, so the first pattern that matches is the first written.
    QStringList names = Json::jsonKeyOrder(text, {"models"});
    for (const QString& name : models->keys()) {
      if (!names.contains(name)) {
        names << name;
      }
    }
    for (const QString& name : names) {
      const QString path = "models." + name;
      if (!models->value(name).isObject()) {
        check.problem(path, "Invalid input: expected object");
        continue;
      }
      const QJsonObject raw = models->value(name).toObject();
      check.strict(raw, {"params", "loopDetection"}, path);
      config.models.append(ModelEntry{name, modelSettings(raw, path, check)});
    }
  }

  bool storesKeys = false;
  if (const auto endpoints = check.object(root, "endpoints", {})) {
    // Qt's JSON objects sort their keys; list the endpoints as the file does.
    QStringList names = Json::jsonKeyOrder(text, {"endpoints"});
    for (const QString& name : endpoints->keys()) {
      if (!names.contains(name)) {
        names << name;
      }
    }
    for (const QString& name : names) {
      const auto it = endpoints->find(name);
      if (it == endpoints->end()) {
        continue;
      }
      const QString path = "endpoints." + it.key();
      if (!it.value().isObject()) {
        check.problem(path, "Invalid input: expected object");
        continue;
      }
      const QJsonObject raw = it.value().toObject();
      check.strict(raw, {"baseURL", "apiKey", "apiKeyEnv", "model", "contextSize", "params", "loopDetection"}, path);
      Endpoint endpoint;
      endpoint.name = it.key();
      const auto baseUrl = check.string(raw, "baseURL", path);
      if (!baseUrl) {
        if (!raw.contains("baseURL")) {
          check.problem(path + ".baseURL", "Invalid input: expected string, received undefined");
        }
      } else {
        const QUrl url(*baseUrl, QUrl::StrictMode);
        if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty()) {
          check.problem(path + ".baseURL", "Invalid URL");
        }
        endpoint.baseUrl = *baseUrl;
      }
      const auto apiKey = check.string(raw, "apiKey", path);
      const auto apiKeyEnv = check.string(raw, "apiKeyEnv", path);
      if (apiKey && apiKeyEnv) {
        check.problem(path, "set apiKey or apiKeyEnv, not both");
      }
      if (apiKey) {
        endpoint.apiKey = *apiKey;
        storesKeys = true;
      }
      if (apiKeyEnv) {
        endpoint.apiKey = qEnvironmentVariable(apiKeyEnv->toUtf8().constData());
        if (endpoint.apiKey.isEmpty()) {
          config.warnings << QString("Endpoint \"%1\": the environment variable %2 is not set.").arg(it.key(), *apiKeyEnv);
        }
      }
      endpoint.model = check.string(raw, "model", path).value_or(QString());
      if (const auto contextSize = check.number(raw, "contextSize", path, true, 1, 1e12)) {
        endpoint.contextSize = int(*contextSize);
      }
      endpoint.settings = modelSettings(raw, path, check);
      config.endpoints.append(endpoint);
    }
  }
  if (!check.ok()) {
    throw SmithError(QString("Invalid %1:\n%2").arg(file, check.text()));
  }
  if (!config.defaultEndpoint.isEmpty() && !config.findEndpoint(config.defaultEndpoint)) {
    throw SmithError(QString("%1: the default endpoint \"%2\" is not defined under \"endpoints\".").arg(file, config.defaultEndpoint));
  }
  struct stat info;
  if (storesKeys && ::stat(QFile::encodeName(file).constData(), &info) == 0 && (info.st_mode & 077) != 0) {
    config.warnings << QString("%1 contains API keys but other users can read it. Run: chmod 600 %1").arg(file);
  }
  return config;
}

} // namespace Smith
