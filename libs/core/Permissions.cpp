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
  const QString command = input.value("command").toString().trimmed();
  for (const QString& text : m_denied) {
    const Rule rule = parseRule(text);
    if (rule.tool != tool) {
      continue;
    }
    // Looser than allowing: whatever starts that way is refused, whatever follows it (an argument, a ';').
    const QChar next = command.size() > rule.prefix.size() ? command.at(rule.prefix.size()) : QChar(' ');
    if (rule.prefix.isEmpty() || (command.startsWith(rule.prefix) && !next.isLetterOrNumber() && next != '-' && next != '_')) {
      return true;
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
