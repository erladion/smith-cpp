#pragma once

#include "core/History.h"

#include <QString>
#include <QStringList>

namespace Smith {

// Servers and models differ; this is under what the hosted ones take, and a screenshot is far smaller.
constexpr qint64 MAX_IMAGE_BYTES = 5 * 1024 * 1024;
// Pictures in one message.
constexpr int MAX_IMAGES = 8;

// What kind of image the bytes are, going by how they start: "image/png", "image/jpeg",
// "image/gif" or "image/webp"; empty if none of these.
QString imageType(const QByteArray& data);
// "212 kB", "6.0 MB".
QString sizeText(qint64 bytes);

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
