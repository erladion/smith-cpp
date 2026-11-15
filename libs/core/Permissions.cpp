#include "core/Permissions.h"

#include <QMutexLocker>
#include <QRegularExpression>
#include <QSet>

namespace Smith {

namespace {

// Programs that run whatever follows: a rule for them would be a rule for anything.
const QSet<QString> WRAPPERS = {"sudo", "doas", "env", "xargs", "nohup", "time", "timeout", "nice", "exec", "eval", "command", "builtin", "watch"};
// Programs that run code given as an argument: only offered together with the script they run.
const QSet<QString> INTERPRETERS = {"sh", "bash", "zsh", "fish", "dash", "python", "python3", "node", "deno", "bun", "perl", "ruby", "php", "lua"};

// Nothing that chains, substitutes, redirects or continues: one program and its arguments.
bool simpleCommand(const QString& command) {
  static const QRegularExpression shell(R"([;&|<>$`(){}\\\n\r])");
  return !command.isEmpty() && !shell.match(command).hasMatch();
}

// Reading a command line for deny rules: close enough to the shell that a
// command can't slip past one by how it is written.
constexpr int MAX_QUOTE_DEPTH = 4;

// The simple commands in a command line, each as its words, quotes and
// backslashes taken out as the shell does. Quoted text is read as a command
// line too, as `bash -c '...'`, `eval "..."` and "$(...)" run it.
void readCommands(const QString& line, int depth, QList<QStringList>& commands) {
  static const QString SEPARATORS = ";&|()`{}<>\n\r";
  QStringList words;
  QString word;
  bool inWord = false;
  auto endWord = [&]() {
    if (inWord) {
      words << word;
    }
    word.clear();
    inWord = false;
  };
  auto endCommand = [&]() {
    endWord();
    if (!words.isEmpty()) {
      commands << words;
    }
    words.clear();
  };
  auto quoted = [&](const QString& text) {
    word += text;
    inWord = true;
    if (depth < MAX_QUOTE_DEPTH) {
      readCommands(text, depth + 1, commands);
    }
  };
  for (int i = 0; i < line.size(); i++) {
    const QChar ch = line[i];
    if (ch == '\\') {
      if (i + 1 < line.size()) {
        word += line[++i];
        inWord = true;
      }
    } else if (ch == '\'') {
      const int end = line.indexOf('\'', i + 1);
      const int stop = end < 0 ? line.size() : end;
      quoted(line.mid(i + 1, stop - i - 1));
      i = stop;
    } else if (ch == '"') {
      QString text;
      int j = i + 1;
      for (; j < line.size() && line[j] != '"'; j++) {
        if (line[j] == '\\' && j + 1 < line.size()) {
          j++;
        }
        text += line[j];
      }
      quoted(text);
      i = j;
    } else if (ch == '#' && !inWord) {
      // A comment, to the end of the line.
      while (i + 1 < line.size() && line[i + 1] != '\n') {
        i++;
      }
    } else if (ch.isSpace() && ch != '\n' && ch != '\r') {
      endWord();
    } else if (SEPARATORS.contains(ch)) {
      endCommand();
    } else {
      word += ch;
      inWord = true;
    }
  }
  endCommand();
}

// The program a word names: "git" for "/usr/bin/git".
QString programName(const QString& word) {
  return word.mid(word.lastIndexOf('/') + 1);
}

// Whether a simple command runs what a rule's words describe: the program,
// then the rest of the rule's words in order among its arguments ("git -C .
// push" for "git push"). Behind a wrapper (sudo, xargs, find -exec) the
// program can be any of the words.
bool runsRule(const QStringList& words, const QStringList& rule) {
  int start = 0;
  // Variables set for the command: FOO=1 git push.
  while (start < words.size() && words[start].indexOf('=') > 0) {
    start++;
  }
  if (start >= words.size()) {
    return false;
  }
  const bool wrapped = WRAPPERS.contains(programName(words[start])) || words.contains("-exec") || words.contains("-execdir") || words.contains("-ok");
  for (int at = start; at < words.size(); at++) {
    if (words[at] == rule[0] || programName(words[at]) == rule[0]) {
      int next = 1;
      for (int i = at + 1; i < words.size() && next < rule.size(); i++) {
        if (words[i] == rule[next]) {
          next++;
        }
      }
      if (next == rule.size()) {
        return true;
      }
    }
    if (!wrapped) {
      break;
    }
  }
  return false;
}

struct Rule {
  QString tool;
  // For bash: the start of the command; empty for the whole tool.
  QString prefix;
};

Rule parseRule(const QString& rule) {
  const int colon = rule.indexOf(':');
  return colon < 0 ? Rule{rule.trimmed(), {}} : Rule{rule.left(colon).trimmed(), rule.mid(colon + 1).trimmed()};
}

bool covers(const QStringList& rules, const QString& tool, const QJsonObject& input, bool wholeToolCounts) {
  const QString command = input.value("command").toString();
  for (const QString& text : rules) {
    const Rule rule = parseRule(text);
    if (rule.tool != tool) {
      continue;
    }
    if (rule.prefix.isEmpty() ? wholeToolCounts : commandMatches(command, rule.prefix)) {
      return true;
    }
  }
  return false;
}

} // namespace

QString commandPrefix(const QString& command) {
  const QString trimmed = command.trimmed();
  if (!simpleCommand(trimmed)) {
    return {};
  }
  static const QRegularExpression space(R"(\s+)");
  const QStringList words = trimmed.split(space, Qt::SkipEmptyParts);
  const QString first = words.value(0);
  if (first.isEmpty() || first.startsWith('-') || first.contains('=') || first.contains('"') || first.contains('\'') || WRAPPERS.contains(first)) {
    return {};
  }
  // "npm test", "git status": the second word when it names what the program does, not an option or a quoted argument.
  const QString second = words.value(1);
  const bool named = !second.isEmpty() && !second.startsWith('-') && !second.contains('"') && !second.contains('\'');
  if (!named) {
    return INTERPRETERS.contains(first) ? QString() : first;
  }
  return first + " " + second;
}

bool commandMatches(const QString& command, const QString& prefix) {
  const QString trimmed = command.trimmed();
  return !prefix.isEmpty() && simpleCommand(trimmed) && (trimmed == prefix || trimmed.startsWith(prefix + " "));
}

void AllowedTools::setConfigured(const QStringList& allow, const QStringList& deny) {
  QMutexLocker lock(&m_mutex);
  m_configured = allow;
  m_denied = deny;
}

void AllowedTools::setRemembered(const QStringList& rules, std::function<void(const QStringList&)> onChange) {
  QMutexLocker lock(&m_mutex);
  m_remembered = rules;
  m_onChange = std::move(onChange);
}

bool AllowedTools::contains(const QString& tool) const {
  QMutexLocker lock(&m_mutex);
  return m_configured.contains(tool) || m_remembered.contains(tool);
}

bool AllowedTools::allows(const QString& tool, const QJsonObject& input, bool unsandboxed) const {
  QMutexLocker lock(&m_mutex);
  return covers(m_configured, tool, input, !unsandboxed) || covers(m_remembered, tool, input, !unsandboxed);
}

bool AllowedTools::denies(const QString& tool, const QJsonObject& input) const {
  QMutexLocker lock(&m_mutex);
  QList<QStringList> commands;
  readCommands(input.value("command").toString(), 0, commands);
  static const QRegularExpression space(R"(\s+)");
  for (const QString& text : m_denied) {
    const Rule rule = parseRule(text);
    if (rule.tool != tool) {
      continue;
    }
    if (rule.prefix.isEmpty()) {
      return true;
    }
    // Looser than allowing: the command is refused wherever in the line it runs, however it is written.
    const QStringList words = rule.prefix.split(space, Qt::SkipEmptyParts);
    for (const QStringList& command : commands) {
      if (runsRule(command, words)) {
        return true;
      }
    }
  }
  return false;
}

bool AllowedTools::remembers() const {
  QMutexLocker lock(&m_mutex);
  return bool(m_onChange);
}

void AllowedTools::insert(const QString& rule) {
  QStringList all;
  std::function<void(const QStringList&)> changed;
  {
    QMutexLocker lock(&m_mutex);
    if (m_remembered.contains(rule)) {
      return;
    }
    m_remembered << rule;
    all = m_remembered;
    changed = m_onChange;
  }
  if (changed) {
    changed(all);
  }
}

void AllowedTools::forget() {
  std::function<void(const QStringList&)> changed;
  {
    QMutexLocker lock(&m_mutex);
    m_remembered.clear();
    changed = m_onChange;
  }
  if (changed) {
    changed({});
  }
}

QStringList AllowedTools::configured() const {
  QMutexLocker lock(&m_mutex);
  return m_configured;
}

QStringList AllowedTools::denied() const {
  QMutexLocker lock(&m_mutex);
  return m_denied;
}

QStringList AllowedTools::remembered() const {
  QMutexLocker lock(&m_mutex);
  return m_remembered;
}

} // namespace Smith
