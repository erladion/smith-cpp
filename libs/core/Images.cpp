#include "core/Images.h"

#include "core/Dirs.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

namespace Smith {

namespace {

// Servers and models differ; this is under what the hosted ones take, and a screenshot is far smaller.
constexpr qint64 MAX_IMAGE_BYTES = 5 * 1024 * 1024;
constexpr int MAX_IMAGES = 8;

// What kind of image the bytes are, going by how they start; empty if none of these.
QString imageType(const QByteArray& data) {
  if (data.startsWith("\x89PNG\r\n\x1a\n")) {
    return "image/png";
  }
  if (data.startsWith("\xFF\xD8\xFF")) {
    return "image/jpeg";
  }
  if (data.startsWith("GIF87a") || data.startsWith("GIF89a")) {
    return "image/gif";
  }
  if (data.startsWith("RIFF") && data.mid(8, 4) == "WEBP") {
    return "image/webp";
  }
  return {};
}

QString sizeText(qint64 bytes) {
  return bytes < 1024 * 1024 ? QString("%1 kB").arg(std::max<qint64>(1, bytes / 1024)) : QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
}

} // namespace

ImageRefs imagesIn(const QString& text, const QString& cwd) {
  // Quoted, or a run of non-spaces in which a backslash keeps the next character.
  static const QRegularExpression word(R"re('([^'\n]+)'|"([^"\n]+)"|((?:\\.|[^\s'"\\])+))re");
  static const QRegularExpression extension(R"(\.(png|jpe?g|gif|webp)$)", QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression escaped(R"(\\(.))");
  ImageRefs refs;
  QSet<QString> seen;
  auto found = word.globalMatch(text);
  while (found.hasNext()) {
    const QRegularExpressionMatch match = found.next();
    QString path = !match.captured(1).isEmpty() ? match.captured(1) : !match.captured(2).isEmpty() ? match.captured(2) : match.captured(3).replace(escaped, "\\1");
    // Punctuation after a name typed in a sentence isn't part of it.
    static const QRegularExpression trailing(R"([,.;:!?)\]]+$)");
    if (match.captured(1).isEmpty() && match.captured(2).isEmpty()) {
      path.remove(trailing);
    }
    if (!extension.match(path).hasMatch()) {
      continue;
    }
    if (path.startsWith("file://")) {
      path = QUrl(path).toLocalFile();
    }
    const QString file = QDir::cleanPath(QDir(cwd).absoluteFilePath(expandHome(path)));
    const QFileInfo info(file);
    if (!info.isFile() || seen.contains(file)) {
      continue;
    }
    seen.insert(file);
    const QString name = info.fileName();
    if (info.size() > MAX_IMAGE_BYTES) {
      refs.problems << QString("%1 is %2; images over %3 aren't attached.").arg(name, sizeText(info.size()), sizeText(MAX_IMAGE_BYTES));
      continue;
    }
    if (refs.images.size() >= MAX_IMAGES) {
      refs.problems << QString("%1 isn't attached: a message takes up to %2 images.").arg(name).arg(MAX_IMAGES);
      continue;
    }
    QFile handle(file);
    if (!handle.open(QIODevice::ReadOnly)) {
      refs.problems << QString("%1 couldn't be read.").arg(name);
      continue;
    }
    const QByteArray data = handle.readAll();
    const QString type = imageType(data);
    if (type.isEmpty()) {
      // Named like an image, but isn't one of these: left as the text it is.
      continue;
    }
    refs.images.append(Image{name, type, data});
    refs.attached << QString("%1 (%2)").arg(name, sizeText(data.size()));
  }
  return refs;
}

} // namespace Smith
