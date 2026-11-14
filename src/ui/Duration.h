#pragma once

#include <QString>

namespace Smith::Ui {

// "40ms", "4.2s", "42s", "1m 05s", "1h 02m".
QString formatDuration(qint64 ms);

// The line shown when a turn ends, e.g. "Done in 1m 23s · 7 tool calls".
QString turnSummary(const QString& reason, qint64 ms, int toolCalls);

} // namespace Smith::Ui
