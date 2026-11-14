#include "ui/Duration.h"

#include "core/Tool.h"

#include <cmath>

namespace Smith::Ui {

QString formatDuration(qint64 ms) {
  if (ms < 1000) {
    return QString("%1ms").arg(std::max<qint64>(1, ms));
  }
  const double seconds = ms / 1000.0;
  if (seconds < 10) {
    return QString::number(seconds, 'f', 1) + "s";
  }
  if (seconds < 60) {
    return QString("%1s").arg(qint64(seconds));
  }
  const qint64 minutes = qint64(seconds / 60);
  if (minutes < 60) {
    return QString("%1m %2s").arg(minutes).arg(qint64(std::fmod(seconds, 60)), 2, 10, QChar('0'));
  }
  return QString("%1h %2m").arg(minutes / 60).arg(minutes % 60, 2, 10, QChar('0'));
}

QString turnSummary(const QString& reason, qint64 ms, int toolCalls) {
  const QString took = formatDuration(ms);
  QString head;
  if (reason == "stop") {
    head = "Done in " + took;
  } else if (reason == "interrupted") {
    head = "Interrupted after " + took;
  } else if (reason == "max_steps") {
    head = "Stopped at the step limit after " + took;
  } else if (reason == "length") {
    head = "Cut off after " + took;
  } else {
    head = "Stopped after " + took;
  }
  return toolCalls > 0 ? head + " · " + plural(toolCalls, "tool call") : head;
}

} // namespace Smith::Ui
