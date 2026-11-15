#pragma once

#include "core/History.h"

#include <QString>
#include <QStringList>

namespace Smith {

struct ImageRefs {
  QList<Image> images;
  // What was attached, to tell the user: "shot.png (212 kB)".
  QStringList attached;
  // Image files named in the text that couldn't be attached, and why.
  QStringList problems;
};

// The image files a message names, read for sending along with it: a path to
// a PNG, JPEG, GIF or WebP file, as typed, quoted, with escaped spaces (as a
// terminal pastes a dropped file) or as a file:// address. Relative paths are
// from `cwd`. The text itself is sent unchanged.
ImageRefs imagesIn(const QString& text, const QString& cwd);

} // namespace Smith
