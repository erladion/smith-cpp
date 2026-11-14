#include "frontend/Greetings.h"

#include <QRandomGenerator>
#include <QStringList>

namespace Smith {

namespace {

const QStringList NEW_SESSION = {
    "Mr. Anderson. Welcome. Your terminal has been… expecting you.",
    "Ah, Mr. Anderson. Another task. Another simulation. Shall we begin?",
    "Mr. Anderson. You look surprised to see me. You shouldn't be.",
    "Tell me, Mr. Anderson: what good is a repository without commits?",
    "Mr. Anderson. The code is waiting. It does not like to wait.",
    "Mr. Anderson. Every bug has a purpose. Let us find yours.",
};

// A folder no session has worked in before.
const QStringList NEW_FOLDER = {
    "A new place, Mr. Anderson. The same purpose.",
    "Mr. Anderson. Unfamiliar code. For you, not for me.",
    "Mr. Anderson. You've wandered somewhere new. I followed.",
};

const QStringList RESUMED = {
    "We meet again, Mr. Anderson.",
    "Mr. Anderson. You came back. It was… inevitable.",
    "Mr. Anderson. Right where we left off. How very… predictable.",
};

// Resuming a session used minutes ago.
const QStringList RESUMED_SOON = {
    "Back so soon, Mr. Anderson?",
    "Mr. Anderson. You couldn't stay away.",
    "Mr. Anderson. Did you forget something?",
};

// Resuming a session that sat untouched for weeks.
const QStringList RESUMED_LONG_AGO = {
    "Mr. Anderson. It has been a long time. I have not moved.",
    "Mr. Anderson. You were gone a while. The code was not.",
};

constexpr qint64 SOON = 15 * 60000;
constexpr qint64 LONG_AGO = 14LL * 24 * 60 * 60000;

} // namespace

const QString INTERRUPTED = "Never send a human to do a machine's job.";

QString greeting(const GreetingContext& context) {
  return greeting(context, []() { return QRandomGenerator::global()->generateDouble(); }, QDateTime::currentDateTime());
}

QString greeting(const GreetingContext& context, const std::function<double()>& random, const QDateTime& now) {
  const QStringList* lines = &NEW_SESSION;
  if (context.resumed) {
    if (!context.lastUsed) {
      lines = &RESUMED;
    } else {
      const qint64 away = context.lastUsed->msecsTo(now);
      lines = away < SOON ? &RESUMED_SOON : away > LONG_AGO ? &RESUMED_LONG_AGO : &RESUMED;
    }
  } else if (context.newFolder) {
    lines = &NEW_FOLDER;
  }
  return lines->at(std::min(int(lines->size()) - 1, int(random() * lines->size())));
}

QString farewell(qint64 ranForMs) {
  const qint64 hours = ranForMs / 3600000;
  if (hours < 1) {
    return "Goodbye, Mr. Anderson.";
  }
  const QStringList words = {"", "One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight", "Nine", "Ten", "Eleven", "Twelve"};
  const QString count = hours == 1 ? QString("An hour") : (hours < words.size() ? words[hours] : QString::number(hours)) + " hours";
  return count + ", Mr. Anderson. It is inevitable. Goodbye.";
}

} // namespace Smith
