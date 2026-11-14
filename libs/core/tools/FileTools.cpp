#include "core/tools/Tools.h"

#include "core/Errors.h"
#include "core/Json.h"
#include "core/Paths.h"
#include "core/Sandbox.h"

namespace Smith {

namespace {

constexpr int MAX_OUTPUT_CHARS = 20000;
constexpr int DEFAULT_LINE_LIMIT = 500;
constexpr int MAX_LINE_CHARS = 1000;

// Reads and writes happen inside the sandbox when it's active, so the file
// tools can reach exactly what shell commands can. The path checks come
// first, for clear errors; the sandbox enforces them.
QString readThrough(const QString& file, const ToolContext& ctx) {
  return ctx.sandboxOrNone()->readFile(file, ctx.cwd);
}

std::optional<QString> readIfExists(const QString& file, const ToolContext& ctx) {
  try {
    return readThrough(file, ctx);
  } catch (const SmithError& error) {
    if (error.code() == "ENOENT") {
      return std::nullopt;
    }
    throw;
  }
}

struct WritePlan {
  QString file;
  QString content;
  bool created = false;
  Diff diff;
};

// Works out a write without doing it, so the approval prompt can show the diff.
WritePlan planWrite(const QJsonObject& input, const ToolContext& ctx) {
  const QString file = resolveInRoot(ctx.cwd, input.value("path").toString());
  assertWritable(ctx.cwd, file);
  const std::optional<QString> before = readIfExists(file, ctx);
  const QString content = input.value("content").toString();
  return WritePlan{file, content, !before.has_value(), diffLines(before.value_or(QString()), content)};
}

struct EditPlan {
  QString file;
  QString updated;
  int replaced = 0;
  Diff diff;
};

// Works out an edit without doing it. Throws with a message for the model if it can't apply.
EditPlan planEdit(const QJsonObject& input, const ToolContext& ctx) {
  const QString path = input.value("path").toString();
  const QString oldString = input.value("old_string").toString();
  const QString newString = input.value("new_string").toString();
  const bool replaceAll = input.value("replace_all").toBool(false);
  if (oldString == newString) {
    throw SmithError("old_string and new_string are identical; nothing to change.");
  }
  const QString file = resolveInRoot(ctx.cwd, path);
  assertWritable(ctx.cwd, file);
  const QString text = readThrough(file, ctx);
  const int count = text.count(oldString);
  if (count == 0) {
    throw SmithError(QString("old_string was not found in %1. Read the file and copy the text exactly.").arg(path));
  }
  if (count > 1 && !replaceAll) {
    throw SmithError(
        QString("old_string occurs %1 times in %2. Include more surrounding text to make it unique, or set replace_all.").arg(QString::number(count), path));
  }
  QString updated = text;
  if (replaceAll) {
    updated.replace(oldString, newString);
  } else {
    const int at = text.indexOf(oldString);
    updated = text.left(at) + newString + text.mid(at + oldString.size());
  }
  return EditPlan{file, updated, replaceAll ? count : 1, diffLines(text, updated)};
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
    const QString text = readThrough(file, ctx);
    if (text.contains(QChar(0))) {
      return ToolResult::error(QString("%1 looks like a binary file.").arg(path));
    }
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
    const QString footer = end < lines.size() ? QString("\n\n[Showing lines %1-%2 of %3]").arg(offset).arg(end).arg(lines.size()) : QString();
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
          {"path", Schema::string("File path, relative to the working directory")},
          {"content", Schema::string("The full file contents")},
      },
      {"path", "content"});
  tool.summarize = [](const QJsonObject& input) {
    return QString("%1 (%2 lines)").arg(input.value("path").toString()).arg(input.value("content").toString().count('\n') + 1);
  };
  tool.touches = [](const QJsonObject& input) { return QStringList{input.value("path").toString()}; };
  tool.preview = [](const QJsonObject& input, const ToolContext& ctx) { return planWrite(input, ctx).diff; };
  tool.run = [](const QJsonObject& input, const ToolContext& ctx) {
    const WritePlan plan = planWrite(input, ctx);
    if (ctx.beforeWrite) {
      ctx.beforeWrite(plan.file);
    }
    ctx.sandboxOrNone()->writeFile(plan.file, plan.content, ctx.cwd);
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
                     "occur exactly once unless replace_all is true. Read the file first so the match is exact.";
  tool.schema = Schema::object(
      {
          {"path", Schema::string("File path, relative to the working directory")},
          {"old_string", Schema::string("Exact text to replace", 1)},
          {"new_string", Schema::string("Replacement text")},
          {"replace_all", Schema::boolean("Replace every occurrence instead of exactly one")},
      },
      {"path", "old_string", "new_string"});
  tool.summarize = [](const QJsonObject& input) {
    const QString path = input.value("path").toString();
    return input.value("replace_all").toBool(false) ? QString("%1 (all occurrences)").arg(path) : path;
  };
  tool.touches = [](const QJsonObject& input) { return QStringList{input.value("path").toString()}; };
  tool.preview = [](const QJsonObject& input, const ToolContext& ctx) { return planEdit(input, ctx).diff; };
  tool.run = [](const QJsonObject& input, const ToolContext& ctx) {
    EditPlan plan;
    try {
      plan = planEdit(input, ctx);
    } catch (const SmithError& error) {
      return ToolResult::error(error.message());
    }
    if (ctx.beforeWrite) {
      ctx.beforeWrite(plan.file);
    }
    ctx.sandboxOrNone()->writeFile(plan.file, plan.updated, ctx.cwd);
    if (ctx.afterWrite) {
      ctx.afterWrite(plan.file, plan.updated);
    }
    const DiffStats stats = diffStats(plan.diff);
    const QString times = plan.replaced == 1 ? QString("1 occurrence") : QString("%1 occurrences").arg(plan.replaced);
    return ToolResult{QString("Replaced %1 in %2 (+%3 -%4 lines).").arg(times, input.value("path").toString(), QString::number(stats.added), QString::number(stats.removed)),
                      false, plan.diff};
  };
  return tool;
}

} // namespace Smith
