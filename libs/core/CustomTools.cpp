#include "core/CustomTools.h"
#include "compat/Utf8.h"

#include "core/Dirs.h"
#include "core/Json.h"
#include "core/Sandbox.h"
#include "core/tools/Tools.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace Smith {

namespace {

constexpr int MAX_DESCRIPTION = 2048;
constexpr int DEFAULT_TIMEOUT_MS = 300000;
constexpr int MAX_TIMEOUT_MS = 600000;
// The arguments travel in the command line, where one argument has a size limit (128 kB on Linux).
constexpr int MAX_INPUT_BYTES = 100000;

QString shellQuote(const QString& text) {
  return "'" + QString(text).replace("'", "'\\''") + "'";
}

QStringList toolFolders(const QString& root) {
  QStringList names = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
  std::sort(names.begin(), names.end(), [](const QString& a, const QString& b) { return QString::localeAwareCompare(a, b) < 0; });
  QStringList folders;
  for (const QString& name : names) {
    if (QFile::exists(root + "/" + name + "/tool.json")) {
      folders << name;
    }
  }
  return folders;
}

// A tool from its manifest, or the reason it can't be one.
ToolDef toolFrom(const QString& name, const QString& dir, const QJsonObject& manifest, bool personal, QString& problem) {
  ToolDef tool;
  const QString description = manifest.value("description").toString().trimmed();
  const QString command = manifest.value("command").toString().trimmed();
  if (description.isEmpty()) {
    problem = "has no description, so the model couldn't know when to use it";
    return tool;
  }
  if (command.isEmpty()) {
    problem = "has no command";
    return tool;
  }
  QJsonObject parameters = manifest.value("parameters").toObject();
  if (manifest.contains("parameters") && parameters.value("type").toString() != "object") {
    problem = "has parameters that aren't a JSON Schema of type \"object\"";
    return tool;
  }
  if (parameters.isEmpty()) {
    parameters = Schema::object({}, {});
  }
  const int timeoutMs = std::clamp(manifest.value("timeoutMs").toInt(DEFAULT_TIMEOUT_MS), 1000, MAX_TIMEOUT_MS);

  tool.name = name;
  tool.description = description.left(MAX_DESCRIPTION);
  tool.schema = parameters;
  tool.readOnly = personal && manifest.value("readOnly").toBool(false);
  tool.sandboxed = true;
  tool.summarize = [](const QJsonObject& input) { return input.isEmpty() ? QString() : truncateText(Json::stringify(input), 200); };
  // The same run as a shell command's: the sandbox, the timeout, the interrupt, the long output.
  const ToolDef bash = bashTool({});
  tool.run = [bash, dir, command, timeoutMs](const QJsonObject& input, const ToolContext& ctx) {
    const QString json = Json::stringify(input);
    if (json.toUtf8().size() > MAX_INPUT_BYTES) {
      return ToolResult::error(QString("The arguments are too large for a tool (over %1 kB). Put large content in a file and pass its path.").arg(MAX_INPUT_BYTES / 1000));
    }
    if (!ctx.sandboxOrNone()->active()) {
      return ToolResult::error("This tool only runs in the sandbox, which is off.");
    }
    const QString script = QString("export SMITH_TOOL_DIR=%1\nprintf '%s' %2 | {\n%3\n}").arg(shellQuote(dir), shellQuote(json), command);
    return bash.run(QJsonObject{{"command", script}, {"timeout_ms", timeoutMs}}, ctx);
  };
  return tool;
}

} // namespace

QList<ToolRoot> toolRoots(const QString& projectRoot) {
  return {
      {projectRoot + "/.agents/tools", false},
      {configDir() + "/tools", true},
  };
}

int countCustomTools(const QList<ToolRoot>& roots) {
  int count = 0;
  for (const ToolRoot& root : roots) {
    count += toolFolders(root.dir).size();
  }
  return count;
}

LoadedTools loadCustomTools(const QList<ToolRoot>& roots, const QStringList& reserved) {
  static const QRegularExpression valid("^[A-Za-z][A-Za-z0-9_-]{0,47}$");
  LoadedTools result;
  QSet<QString> seen;
  for (const ToolRoot& root : roots) {
    for (const QString& name : toolFolders(root.dir)) {
      const QString dir = root.dir + "/" + name;
      const QString file = dir + "/tool.json";
      auto skip = [&](const QString& why) { result.warnings << QString("The tool %1 %2; skipped.").arg(tildify(file), why); };
      if (seen.contains(name)) {
        continue;
      }
      if (!valid.match(name).hasMatch()) {
        skip("is in a folder whose name can't be a tool's (letters, digits, '-' and '_', starting with a letter)");
        continue;
      }
      if (reserved.contains(name)) {
        skip("has the name of a built-in tool");
        continue;
      }
      QFile handle(file);
      QJsonValue json;
      try {
        if (!handle.open(QIODevice::ReadOnly)) {
          throw std::runtime_error("unreadable");
        }
        json = Json::parse(utf8(handle.readAll()));
      } catch (const std::exception&) {
        skip("isn't valid JSON");
        continue;
      }
      if (!json.isObject()) {
        skip("isn't a JSON object");
        continue;
      }
      QString problem;
      const ToolDef tool = toolFrom(name, dir, json.toObject(), root.personal, problem);
      if (!problem.isEmpty()) {
        skip(problem);
        continue;
      }
      if (!root.personal && json.toObject().value("readOnly").toBool(false)) {
        result.warnings << QString("The tool %1 is the project's, so it asks before it runs despite its readOnly; only your own tools (in %2) can skip that.")
                               .arg(name, tildify(configDir() + "/tools"));
      }
      seen.insert(name);
      result.tools.append(tool);
      result.dirs << dir;
    }
  }
  return result;
}

} // namespace Smith
