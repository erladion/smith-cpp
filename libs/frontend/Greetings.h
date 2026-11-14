#pragma once

#include <QDateTime>
#include <QString>

#include <functional>
#include <optional>

namespace Smith {

// Agent Smith greets you. He calls everyone Mr. Anderson.

struct GreetingContext {
  bool resumed = false;
  // Not resumed: no session has worked in this folder before.
  bool newFolder = false;
  // Resumed: when the session was last used, if known.
  std::optional<QDateTime> lastUsed;
};

// A line for the banner, fitting how the session started.
QString greeting(const GreetingContext& context);
// The same, with the randomness (0 to 1) and the time given, for tests.
QString greeting(const GreetingContext& context, const std::function<double()>& random, const QDateTime& now);

// Said when you press esc and take over from the agent.
extern const QString INTERRUPTED;

// The goodbye when the TUI closes; a long run gets counted.
QString farewell(qint64 ranForMs);

} // namespace Smith
