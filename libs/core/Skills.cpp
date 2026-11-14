#include "core/Skills.h"
#include "compat/Utf8.h"

#include "core/Dirs.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSet>

#include <climits>

namespace Smith {

namespace {

constexpr int MAX_DESCRIPTION = 1024;

} // namespace

QStringList skillRoots(const QString& projectRoot) {
  return {
      projectRoot + "/.agents/skills",
      projectRoot + "/.claude/skills",
      configDir() + "/skills",
      homeDir() + "/.claude/skills",
  };
}

LoadedSkills loadSkills(const QStringList& roots) {
  LoadedSkills result;
  QSet<QString> seen;
  for (const QString& root : roots) {
    QDir dir(root);
    if (!dir.exists()) {
      continue;
    }
    QStringList names = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    std::sort(names.begin(), names.end(), [](const QString& a, const QString& b) { return QString::localeAwareCompare(a, b) < 0; });
    for (const QString& name : names) {
      const QString skillDir = root + "/" + name;
      const QString file = skillDir + "/SKILL.md";
      QFile handle(file);
      if (!handle.open(QIODevice::ReadOnly)) {
        continue;
      }
      const Frontmatter parsed = parseFrontmatter(utf8(handle.readAll()));
      QString skillName = parsed.data.value("name").trimmed();
      if (skillName.isEmpty()) {
        skillName = name;
      }
      const QString description = parsed.data.value("description").trimmed();
      if (description.isEmpty()) {
        result.warnings << QString("Skill %1 has no description, so the model couldn't know when to use it; skipped.").arg(file);
        continue;
      }
      if (!seen.contains(skillName)) {
        seen.insert(skillName);
        result.skills.append({skillName, description.left(MAX_DESCRIPTION), skillDir, parsed.body.trimmed()});
      }
    }
  }
  return result;
}

Frontmatter parseFrontmatter(const QString& text) {
  static const QRegularExpression header(R"(^---\r?\n([\s\S]*?)\r?\n---[ \t]*(?:\r?\n|$))");
  const QRegularExpressionMatch match = header.match(text);
  if (!match.hasMatch()) {
    return {{}, text};
  }
  static const QRegularExpression newline(R"(\r?\n)");
  static const QRegularExpression keyLine(R"(^([A-Za-z0-9_-]+):\s*(.*)$)");
  static const QRegularExpression continuation(R"(^\s+\S)");
  static const QRegularExpression blockMarker(R"(^[|>][-+]?$)");
  static const QRegularExpression whitespace(R"(\s+)");
  static const QRegularExpression quoted(R"(^(["'])([\s\S]*)\1$)");

  const QStringList lines = match.captured(1).split(newline);
  Frontmatter result;
  for (int i = 0; i < lines.size(); i++) {
    const QRegularExpressionMatch m = keyLine.match(lines[i]);
    if (!m.hasMatch()) {
      continue;
    }
    const QString key = m.captured(1);
    const QString rawValue = m.captured(2);

    // Indented lines that follow belong to this key.
    QStringList following;
    while (i + 1 < lines.size() && (continuation.match(lines[i + 1]).hasMatch() || lines[i + 1].isEmpty())) {
      following << lines[++i];
    }

    QString value;
    if (blockMarker.match(rawValue).hasMatch()) {
      int indent = INT_MAX;
      for (const QString& line : following) {
        if (!line.trimmed().isEmpty()) {
          int leading = 0;
          while (leading < line.size() && line[leading].isSpace()) {
            leading++;
          }
          indent = std::min(indent, leading);
        }
      }
      QStringList block;
      for (const QString& line : following) {
        block << line.mid(indent == INT_MAX ? 0 : indent);
      }
      value = rawValue.startsWith('|') ? block.join('\n') : block.join(' ').replace(whitespace, " ");
    } else {
      QStringList parts;
      if (!rawValue.isEmpty()) {
        parts << rawValue;
      }
      for (const QString& line : following) {
        if (!line.trimmed().isEmpty()) {
          parts << line.trimmed();
        }
      }
      value = parts.join(' ');
      const QRegularExpressionMatch q = quoted.match(value);
      if (q.hasMatch()) {
        value = q.captured(2);
      }
    }
    result.data.insert(key, value.trimmed());
  }
  result.body = text.mid(match.capturedLength(0));
  return result;
}

} // namespace Smith
