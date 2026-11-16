#pragma once

#include "core/History.h"

#include <QString>
#include <QStringList>

namespace Smith {

// What a message brings along: the files it mentions with @ and the image
// files it names.
struct Attachments {
  QList<Image> images;
  QList<AttachedFile> files;
  // What was attached, to tell the user: "src/app.cpp (120 lines)", "shot.png (212 kB)".
  QStringList attached;
  // Files named that couldn't be attached, and why.
  QStringList problems;
};

// The files a message mentions as @path or @"path with spaces": relative to
// `cwd`, absolute or with ~/, read for sending along with it. Pictures are
// left to imagesIn. A mention that names nothing that exists, like @here, is
// left alone; one that looks like a path is reported.
Attachments filesIn(const QString& text, const QString& cwd);

// filesIn and imagesIn together.
Attachments attachmentsIn(const QString& text, const QString& cwd);

} // namespace Smith
