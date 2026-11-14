#pragma once

#include <QList>
#include <QString>

namespace Smith {

// A line in a diff, with its line number in the old or new file.
struct DiffLine {
  enum class Kind { Context, Remove, Add, Gap };
  Kind kind = Kind::Context;
  QString text;
  int oldLine = 0;
  int newLine = 0;
  // For Gap: unchanged lines left out between hunks.
  int count = 0;
};

using Diff = QList<DiffLine>;

// Line diff as hunks with `context` unchanged lines around each change (Myers, like git's default).
Diff diffLines(const QString& before, const QString& after, int context = 2);

struct DiffStats {
  int added = 0;
  int removed = 0;
};

DiffStats diffStats(const Diff& diff);

} // namespace Smith
