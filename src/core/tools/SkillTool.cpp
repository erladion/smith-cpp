#include "core/tools/Tools.h"

#include "core/Errors.h"
#include "core/Paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace Smith {

namespace {

constexpr int MAX_CHARS = 30000;
constexpr int MAX_LISTED_FILES = 50;

QStringList listSkillFiles(const QString& dir, const QString& prefix = {}) {
  QStringList out;
  const QFileInfoList entries = QDir(dir).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
  for (const QFileInfo& entry : entries) {
    if (out.size() >= MAX_LISTED_FILES) {
      break;
    }
    const QString rel = prefix.isEmpty() ? entry.fileName() : prefix + "/" + entry.fileName();
    if (entry.isDir()) {
      out << listSkillFiles(entry.filePath(), rel);
    } else {
      out << rel;
    }
  }
  return out.mid(0, MAX_LISTED_FILES);
}

} // namespace

ToolDef skillTool(const QList<Skill>& skills) {
  ToolDef tool;
  tool.name = "skill";
  tool.description = "Load a skill: packaged instructions for a kind of task. The available skills are listed in the system prompt. Call this with "
                     "a skill's name before starting a task that matches its description, then follow its instructions. Pass file to read one "
                     "of the skill's other files.";
  tool.schema = Schema::object(
      {
          {"name", Schema::string("The skill's name")},
          {"file", Schema::string("A file inside the skill's folder, relative to it")},
      },
      {"name"});
  tool.readOnly = true;
  tool.summarize = [](const QJsonObject& input) {
    const QString file = input.value("file").toString();
    return file.isEmpty() ? input.value("name").toString() : input.value("name").toString() + ": " + file;
  };
  tool.run = [skills](const QJsonObject& input, const ToolContext&) {
    const QString name = input.value("name").toString();
    const Skill* skill = nullptr;
    QStringList names;
    for (const Skill& candidate : skills) {
      names << candidate.name;
      if (candidate.name == name) {
        skill = &candidate;
      }
    }
    if (!skill) {
      return ToolResult::error(QString("No skill named \"%1\". Available: %2.").arg(name, names.join(", ")));
    }
    const QString file = input.value("file").toString();
    if (!file.isEmpty()) {
      const QString target = resolveInRoot(skill->dir, file);
      QFile handle(target);
      if (!handle.open(QIODevice::ReadOnly)) {
        throw SmithError(QString("ENOENT: no such file or directory, open '%1'").arg(target), "ENOENT");
      }
      return ToolResult{truncateText(QString::fromUtf8(handle.readAll()), MAX_CHARS)};
    }
    QStringList others = listSkillFiles(skill->dir);
    others.removeAll("SKILL.md");
    QString listing;
    if (!others.isEmpty()) {
      QStringList lines;
      for (const QString& other : others) {
        lines << "- " + other;
      }
      listing = "\n\nOther files in this skill (read them with file, or run scripts from " + skill->dir + "):\n" + lines.join('\n');
    }
    return ToolResult{"Skill folder: " + skill->dir + "\n\n" + truncateText(skill->body, MAX_CHARS) + listing};
  };
  return tool;
}

} // namespace Smith
