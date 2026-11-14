#pragma once

#include "core/Config.h"

#include <QStringList>

namespace Smith {

// The spinner's frames: half-width katakana with the matrix theme, braille dots otherwise.
QStringList spinnerFrames(Theme theme);

// What the spinner says for the tools in flight ("bash ls -la" each): one tool, or how many run in parallel.
QString runningLabel(const QStringList& running);

} // namespace Smith
