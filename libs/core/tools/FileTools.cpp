#include "core/tools/Tools.h"
#include "compat/Utf8.h"

#include "core/Errors.h"
#include "core/Json.h"
#include "core/Paths.h"
#include "core/Sandbox.h"

#include <QMutex>

namespace Smith {

namespace {

constexpr int MAX_OUTPUT_CHARS = 20000;
constexpr int DEFAULT_LINE_LIMIT = 500;
constexpr int MAX_LINE_CHARS = 1000;
constexpr int MAX_EDITS = 50;

// Reads and writes happen inside the sandbox when it's active, so the file
// tools can reach exactly what shell commands can. The path checks come
// first, for clear errors; the sandbox enforces them.
QByteArray readThrough(const QString& file, const ToolContext& ctx) {
  return ctx.sandboxOrNone()->readBytes(file, ctx.cwd);
}

std::optional<QByteArray> readIfExists(const QString& file, const ToolContext& ctx) {
  try {
    return readThrough(file, ctx);
  } catch (const SmithError& error) {
    if (error.code() == "ENOENT") {
      return std::nullopt;
    }
    throw;
  }
}

// How a file's bytes are read as text, so a change can be written back the
// same way and leave the rest of the file byte for byte as it was.
enum class Encoding {
  Utf8,
  // UTF-8 with a byte order mark, which decoding drops.
  MarkedUtf8,
  // Anything else: one character per byte, so every byte survives. Right for
  // Latin-1; for other encodings the text looks odd but nothing is lost.
  Bytes,
};

const QByteArray MARK("\xEF\xBB\xBF");

struct FileText {
  QString text;
  Encoding encoding = Encoding::Utf8;
};

FileText decode(const QByteArray& bytes) {
  const QString text = utf8(bytes);
  const QByteArray again = text.toUtf8();
  if (again == bytes) {
    return FileText{text, Encoding::Utf8};
  }
  if (bytes.startsWith(MARK) && MARK + again == bytes) {
    return FileText{text, Encoding::MarkedUtf8};
  }
  return FileText{QString::fromLatin1(bytes.constData(), bytes.size()), Encoding::Bytes};
}

// The first character that has no byte of its own, if any.
std::optional<QChar> beyondBytes(const QString& text) {
  for (const QChar ch : text) {
    if (ch.unicode() > 0xFF) {
      return ch;
    }
  }
  return std::nullopt;
}

QByteArray encode(const QString& text, Encoding encoding) {
  switch (encoding) {
  case Encoding::Utf8:
    return text.toUtf8();
  case Encoding::MarkedUtf8:
    return MARK + text.toUtf8();
  case Encoding::Bytes:
    return text.toLatin1();
  }
  return text.toUtf8();
}

// Changes to files are made one at a time, from reading the file to writing
// it back. Sub-agents can edit the same file at once, and a write that started
// from the file as it was would drop the other's change.
QMutex& fileChanges() {
  static QMutex mutex;
  return mutex;
}

const QString NOT_UTF8 = "[This file isn't UTF-8: each byte is shown as one Latin-1 character. edit_file keeps its bytes that way.]";

struct WritePlan {
  QString file;
  QByteArray content;
  bool created = false;
  Diff diff;
};

// Works out a write without doing it, so the approval prompt can show the diff.
WritePlan planWrite(const QJsonObject& input, const ToolContext& ctx) {
  const QString file = resolveForWrite(ctx.cwd, input.value("path").toString(), ctx.sandbox);
  assertWritable(ctx.cwd, file);
  const std::optional<QByteArray> before = readIfExists(file, ctx);
  const QString content = input.value("content").toString();
  const FileText old = before ? decode(*before) : FileText{};
  // A file keeps the encoding it has, where the new text can be written in it.
  const Encoding encoding = old.encoding == Encoding::Bytes && beyondBytes(content) ? Encoding::Utf8 : old.encoding;
  return WritePlan{file, encode(content, encoding), !before.has_value(), diffLines(old.text, content)};
}

struct Replacement {
  QString oldString;
  QString newString;
  bool all = false;
};

// The replacements a call asks for: its own, then those in `edits`.
QList<Replacement> replacements(const QJsonObject& input) {
  QList<Replacement> list{{input.value("old_string").toString(), input.value("new_string").toString(), input.value("replace_all").toBool(false)}};
  for (const QJsonValue& value : input.value("edits").toArray()) {
    const QJsonObject edit = value.toObject();
    list.append({edit.value("old_string").toString(), edit.value("new_string").toString(), edit.value("replace_all").toBool(false)});
  }
  return list;
}

struct EditPlan {
  QString file;
  QByteArray content;
  int replaced = 0;
  Diff diff;
};

// Works out an edit without doing it: every replacement applies, in order, or
// none does. Throws with a message for the model if one can't.
EditPlan planEdit(const QJsonObject& input, const ToolContext& ctx) {
  const QString path = input.value("path").toString();
  const QList<Replacement> edits = replacements(input);
  const QString file = resolveForWrite(ctx.cwd, path, ctx.sandbox);
  assertWritable(ctx.cwd, file);
  const QByteArray bytes = readThrough(file, ctx);
  if (bytes.contains('\0')) {
    throw SmithError(QString("%1 looks like a binary file; edit_file only changes text.").arg(path));
  }
  const FileText read = decode(bytes);
  QString updated = read.text;
  int replaced = 0;
  for (int i = 0; i < edits.size(); i++) {
    const Replacement& edit = edits[i];
    // Which one, when there are several.
    const QString which = edits.size() > 1 ? QString("Edit %1 of %2: ").arg(i + 1).arg(edits.size()) : QString();
    const QString after = edits.size() > 1 && i > 0 ? QString(" (after the edits before it)") : QString();
    if (edit.oldString == edit.newString) {
      throw SmithError(which + "old_string and new_string are identical; nothing to change.");
    }
    const int count = updated.count(edit.oldString);
    if (count == 0) {
      throw SmithError(which + QString("old_string was not found in %1%2. Read the file and copy the text exactly.").arg(path, after));
    }
    if (count > 1 && !edit.all) {
      throw SmithError(which + QString("old_string occurs %1 times in %2%3. Include more surrounding text to make it unique, or set replace_all.")
                                   .arg(QString::number(count), path, after));
    }
    if (read.encoding == Encoding::Bytes) {
      if (const std::optional<QChar> stray = beyondBytes(edit.newString)) {
        throw SmithError(which + QString("%1 isn't UTF-8, so its bytes are kept one character each, and new_string has a character that can't be "
                                         "written that way: '%2'. Use only characters the file already has, or change it with a command.")
                                     .arg(path, QString(*stray)));
      }
    }
    if (edit.all) {
      updated.replace(edit.oldString, edit.newString);
    } else {
      const int at = updated.indexOf(edit.oldString);
      updated = updated.left(at) + edit.newString + updated.mid(at + edit.oldString.size());
    }
    replaced += edit.all ? count : 1;
  }
  return EditPlan{file, encode(updated, read.encoding), replaced, diffLines(read.text, updated)};
}

} // namespace

ToolDef readFileTool() {
  ToolDef tool;
  tool.name = "read_file";
  tool.description = QString("Read a text file. Returns lines prefixed with their 1-based line numbers. Reads up to %1 lines by default; use offset "
                             "and limit for larger files. Files outside the working directory can be read too (the user is asked first).")
                         .arg(DEFAULT_LINE_LIMIT);
  tool.schema = Schema::object(
      {
          {"path", Schema::string("File path, relative to the working directory, absolute, or starting with ~/")},
          {"offset", Schema::integer("First line to read (1-based)", 1)},
          {"limit", Schema::integer("Maximum number of lines to read", 1)},
      },
      {"path"});
  tool.readOnly = true;
  tool.summarize = [](const QJsonObject& input) {
    const QString path = input.value("path").toString();
    if (!input.contains("offset") && !input.contains("limit")) {
      return path;
    }
    const int offset = input.value("offset").toInt(1);
    const int limit = input.value("limit").toInt(DEFAULT_LINE_LIMIT);
    return QString("%1 (lines %2-%3)").arg(path, QString::number(offset), QString::number(offset + limit - 1));
  };
  tool.approvalReason = [](const QJsonObject& input, const ToolContext& ctx) -> std::optional<QString> {
    if (resolveForRead(ctx.cwd, input.value("path").toString(), ctx.sandbox).outside) {
      return OUTSIDE;
    }
    return std::nullopt;
  };
  tool.run = [](const QJsonObject& input, const ToolContext& ctx) {
    const QString path = input.value("path").toString();
    const int offset = input.value("offset").toInt(1);
    const int limit = input.value("limit").toInt(DEFAULT_LINE_LIMIT);
    const QString file = resolveForRead(ctx.cwd, path, ctx.sandbox).path;
    const QByteArray bytes = readThrough(file, ctx);
    if (bytes.contains('\0')) {
      return ToolResult::error(QString("%1 looks like a binary file.").arg(path));
    }
    const FileText read = decode(bytes);
    const QString& text = read.text;
    QStringList lines = text.split('\n');
    if (!lines.isEmpty() && lines.last().isEmpty()) {
      lines.removeLast();
    }
    if (lines.isEmpty()) {
      return ToolResult{QString("%1 is empty.").arg(path)};
    }
    if (offset > lines.size()) {
      return ToolResult::error(QString("%1 has only %2 lines.").arg(path, QString::number(lines.size())));
    }
    const QStringList slice = lines.mid(offset - 1, limit);
    QStringList body;
    for (int i = 0; i < slice.size(); i++) {
      const QString& line = slice[i];
      const QString shown = line.size() > MAX_LINE_CHARS ? line.left(MAX_LINE_CHARS) + "… [line truncated]" : line;
      body << QString("%1\t%2").arg(offset + i).arg(shown);
    }
    const int end = offset + slice.size() - 1;
    QString footer = end < lines.size() ? QString("\n\n[Showing lines %1-%2 of %3]").arg(offset).arg(end).arg(lines.size()) : QString();
    if (read.encoding == Encoding::Bytes) {
      footer += "\n\n" + NOT_UTF8;
    }
    return ToolResult{truncateText(body.join('\n'), MAX_OUTPUT_CHARS) + footer};
  };
  return tool;
}

ToolDef writeFileTool() {
  ToolDef tool;
  tool.name = "write_file";
  tool.description = "Create a file or overwrite it completely. Prefer edit_file for changes to existing files.";
  tool.schema = Schema::object(
      {
          {"path", Schema::string("File path, relative to the working directory. In the sandbox, /tmp works too, for scratch files")},
          {"content", Schema::string("The full file contents")},
      },
      {"path", "content"});
  tool.summarize = [](const QJsonObject& input) {
    return QString("%1 (%2 lines)").arg(input.value("path").toString()).arg(input.value("content").toString().count('\n') + 1);
  };
  tool.touches = [](const QJsonObject& input) { return QStringList{input.value("path").toString()}; };
  tool.preview = [](const QJsonObject& input, const ToolContext& ctx) { return planWrite(input, ctx).diff; };
  tool.run = [](const QJsonObject& input, const ToolContext& ctx) {
    QMutexLocker lock(&fileChanges());
    const WritePlan plan = planWrite(input, ctx);
    if (ctx.beforeWrite) {
      ctx.beforeWrite(plan.file);
    }
    ctx.sandboxOrNone()->writeBytes(plan.file, plan.content, ctx.cwd);
    if (ctx.afterWrite) {
      ctx.afterWrite(plan.file, plan.content);
    }
    const DiffStats stats = diffStats(plan.diff);
    const QString verb = plan.created ? "Created" : "Wrote";
    return ToolResult{QString("%1 %2 (+%3 -%4 lines).").arg(verb, input.value("path").toString(), QString::number(stats.added), QString::number(stats.removed)), false, plan.diff};
  };
  return tool;
}

ToolDef editFileTool() {
  ToolDef tool;
  tool.name = "edit_file";
  tool.description = "Replace an exact string in a file. old_string must match the file exactly, including whitespace and indentation, and must "
                     "occur exactly once unless replace_all is true. Read the file first so the match is exact. For several changes to one file, "
                     "put the rest in edits: they apply in order, each to the result of the one before, and either all of them apply or none.";
  tool.schema = Schema::object(
      {
          {"path", Schema::string("File path, relative to the working directory. In the sandbox, /tmp works too, for scratch files")},
          {"old_string", Schema::string("Exact text to replace", 1)},
          {"new_string", Schema::string("Replacement text")},
          {"replace_all", Schema::boolean("Replace every occurrence instead of exactly one")},
          {"edits", Schema::array(Schema::object(
                                      {
                                          {"old_string", Schema::string("Exact text to replace", 1)},
                                          {"new_string", Schema::string("Replacement text")},
                                          {"replace_all", Schema::boolean("Replace every occurrence instead of exactly one")},
                                      },
                                      {"old_string", "new_string"}),
                                  "More replacements to make in the same file, after the first", MAX_EDITS - 1)},
      },
      {"path", "old_string", "new_string"});
  tool.summarize = [](const QJsonObject& input) {
    const QString path = input.value("path").toString();
    const int edits = input.value("edits").toArray().size() + 1;
    if (edits > 1) {
      return QString("%1 (%2 edits)").arg(path).arg(edits);
    }
    return input.value("replace_all").toBool(false) ? QString("%1 (all occurrences)").arg(path) : path;
  };
  tool.touches = [](const QJsonObject& input) { return QStringList{input.value("path").toString()}; };
  tool.preview = [](const QJsonObject& input, const ToolContext& ctx) { return planEdit(input, ctx).diff; };
  tool.run = [](const QJsonObject& input, const ToolContext& ctx) {
    QMutexLocker lock(&fileChanges());
    EditPlan plan;
    try {
      plan = planEdit(input, ctx);
    } catch (const SmithError& error) {
      return ToolResult::error(error.message());
    }
    if (ctx.beforeWrite) {
      ctx.beforeWrite(plan.file);
    }
    ctx.sandboxOrNone()->writeBytes(plan.file, plan.content, ctx.cwd);
    if (ctx.afterWrite) {
      ctx.afterWrite(plan.file, plan.content);
    }
    const DiffStats stats = diffStats(plan.diff);
    const QString times = plan.replaced == 1 ? QString("1 occurrence") : QString("%1 occurrences").arg(plan.replaced);
    return ToolResult{QString("Replaced %1 in %2 (+%3 -%4 lines).").arg(times, input.value("path").toString(), QString::number(stats.added), QString::number(stats.removed)),
                      false, plan.diff};
  };
  return tool;
}

} // namespace Smith
