#pragma once

#include <QJsonObject>
#include <QMutex>
#include <QString>
#include <QStringList>

#include <functional>

namespace Smith {

// The start of a shell command that a rule for "commands like this one" would
// cover: "npm test" for "npm test -- --watch". Empty when no such rule is
// offered: for a command that chains or substitutes others (; | && $( ...),
// where a harmless start says nothing about the rest, and for programs that
// only run another (sudo, env, bash -c).
QString commandPrefix(const QString& command);
// Whether a rule for commands starting with `prefix` covers a command: the
// command is that, or goes on with more arguments, and does nothing else.
bool commandMatches(const QString& command, const QString& prefix);

// What runs without asking. A rule is a tool's name ("edit_file"), or for bash
// the start of a command ("bash:npm test"). Rules come from the config file,
// and from the user's answers, remembered for the project. Shared with
// sub-agents, so a grant applies to both; used from several threads.
class AllowedTools {
public:
  // Rules from the config file. Those in `deny` are refused without asking.
  void setConfigured(const QStringList& allow, const QStringList& deny);
  // Rules the user allowed earlier. `onChange` then gets all of them after each change, to save them.
  void setRemembered(const QStringList& rules, std::function<void(const QStringList&)> onChange = {});

  // Whether the whole tool is allowed.
  bool contains(const QString& tool) const;
  // Whether a call runs without asking. A command that would run with no
  // sandbox isn't covered by a rule for all of bash, only by one for commands like it.
  bool allows(const QString& tool, const QJsonObject& input, bool unsandboxed) const;
  // Whether the config file refuses a call.
  bool denies(const QString& tool, const QJsonObject& input) const;
  // Whether what the user allows is kept for later sessions.
  bool remembers() const;

  void insert(const QString& rule);
  // Forgets what the user allowed; the config file's rules stay.
  void forget();

  QStringList configured() const;
  QStringList denied() const;
  QStringList remembered() const;

private:
  mutable QMutex m_mutex;
  QStringList m_configured;
  QStringList m_denied;
  QStringList m_remembered;
  std::function<void(const QStringList&)> m_onChange;
};

} // namespace Smith
