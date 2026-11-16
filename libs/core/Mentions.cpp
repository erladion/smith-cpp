#include "core/Mentions.h"

#include "core/Dirs.h"
#include "core/Images.h"
#include "core/Tool.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace Smith {

namespace {

constexpr int MAX_FILES = 10;
// Per file: the start of a longer one is sent.
constexpr int MAX_LINES = 2000;
constexpr int MAX_CHARS = 60000;
constexpr int MAX_LINE_CHARS = 1000;
// For all of them together, so a message can't swamp the context.
constexpr int MAX_TOTAL_CHARS = 150000;
constexpr qint64 MAX_BYTES = 8 * 1024 * 1024;

} // namespace

Attachments filesIn(const QString& text, const QString& cwd) {
  static const QRegularExpression mention(R"re((?:^|(?<=\s))@(?:"([^"\n]+)"|([^\s"]+)))re");
  static const QRegularExpression trailing(R"([,.;:!?)\]]+$)");
  static const QRegularExpression image(R"(\.(png|jpe?g|gif|webp)$)", QRegularExpression::CaseInsensitiveOption);
  Attachments out;
  QSet<QString> seen;
  int total = 0;
  auto found = mention.globalMatch(text);
  while (found.hasNext()) {
    const QRegularExpressionMatch match = found.next();
    QString path = match.captured(1);
    if (path.isEmpty()) {
      // Punctuation after a name typed in a sentence isn't part of it.
      path = match.captured(2).remove(trailing);
    }
    if (path.isEmpty() || image.match(path).hasMatch()) {
      continue;
    }
    const QString file = QDir::cleanPath(QDir(cwd).absoluteFilePath(expandHome(path)));
    const QFileInfo info(file);
    if (seen.contains(file)) {
      continue;
    }
    seen.insert(file);
    if (!info.exists()) {
      if (path.contains('/') || path.contains('.')) {
        out.problems << QString("There's no file %1 to attach.").arg(path);
      }
      continue;
    }
    if (info.isDir()) {
      out.problems << QString("%1 is a folder; mention the files in it to attach them.").arg(path);
      continue;
    }
    const QString shown = file.startsWith(cwd + "/") ? file.mid(cwd.size() + 1) : tildify(file);
    if (out.files.size() >= MAX_FILES) {
      out.problems << QString("%1 isn't attached: a message takes up to %2 files.").arg(shown).arg(MAX_FILES);
      continue;
    }
    if (total >= MAX_TOTAL_CHARS) {
      out.problems << QString("%1 isn't attached: the files before it are already as much as a message takes.").arg(shown);
      continue;
    }
    QFile handle(file);
    if (info.size() > MAX_BYTES || !handle.open(QIODevice::ReadOnly)) {
      out.problems << QString("%1 couldn't be read%2.").arg(shown, info.size() > MAX_BYTES ? QString(": it's too big") : QString());
      continue;
    }
    const QByteArray bytes = handle.readAll();
    if (bytes.contains('\0')) {
      out.problems << QString("%1 looks like a binary file, so it isn't attached.").arg(shown);
      continue;
    }
    QStringList lines = QString::fromUtf8(bytes).split('\n');
    if (!lines.isEmpty() && lines.last().isEmpty()) {
      lines.removeLast();
    }
    const int budget = std::min(MAX_CHARS, MAX_TOTAL_CHARS - total);
    QStringList body;
    int chars = 0;
    for (int i = 0; i < lines.size() && i < MAX_LINES; i++) {
      const QString& line = lines[i];
      const QString numbered = QString("%1\t%2").arg(i + 1).arg(line.size() > MAX_LINE_CHARS ? line.left(MAX_LINE_CHARS) + "… [line truncated]" : line);
      if (chars + numbered.size() > budget && !body.isEmpty()) {
        break;
      }
      body << numbered;
      chars += numbered.size() + 1;
    }
    AttachedFile attached;
    attached.shown = shown;
    attached.cut = body.size() < lines.size();
    attached.content = body.join('\n');
    if (attached.cut) {
      attached.content += QString("\n[Lines 1-%1 of %2]").arg(body.size()).arg(lines.size());
    }
    if (lines.isEmpty()) {
      attached.content = "(empty)";
    }
    total += chars;
    out.files.append(attached);
    out.attached << (attached.cut ? QString("%1 (the first %2 of %3 lines)").arg(shown).arg(body.size()).arg(lines.size())
                                  : QString("%1 (%2)").arg(shown, plural(lines.size(), "line")));
  }
  return out;
}

Attachments attachmentsIn(const QString& text, const QString& cwd) {
  Attachments out = filesIn(text, cwd);
  const ImageRefs images = imagesIn(text, cwd);
  out.images = images.images;
  out.attached << images.attached;
  out.problems << images.problems;
  return out;
}

} // namespace Smith
