#pragma once

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

namespace Smith {

// Skills are folders with a SKILL.md: a short header naming the skill and
// saying when to use it, then the instructions. Only names and descriptions
// go into the system prompt; the model loads a skill's instructions on
// demand with the `skill` tool. Same format as Claude and OpenCode use.
struct Skill {
  QString name;
  QString description;
  // The skill's folder; its other files are relative to this.
  QString dir;
  // Instructions: SKILL.md without the header.
  QString body;
};

// Folders searched for skills, highest priority first.
QStringList skillRoots(const QString& projectRoot);

struct LoadedSkills {
  QList<Skill> skills;
  QStringList warnings;
};

// Finds skills under `roots`. A name found earlier wins, so project skills override personal ones.
LoadedSkills loadSkills(const QStringList& roots);

struct Frontmatter {
  QMap<QString, QString> data;
  QString body;
};

// Reads the `---` header at the top of a Markdown file: `key: value`, quoted
// values, `|` and `>` blocks, and plain values continued on indented lines.
Frontmatter parseFrontmatter(const QString& text);

} // namespace Smith
