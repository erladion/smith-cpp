#include "core/tools/Tools.h"
#include "compat/Utf8.h"

#include "core/Errors.h"
#include "core/FileList.h"
#include "core/Glob.h"
#include "core/Paths.h"
#include "core/Process.h"
#include "core/Sandbox.h"
#include "core/Toolchain.h"

#include <QDir>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>

#include <algorithm>

namespace Smith {

namespace {

constexpr int MAX_OUTPUT_CHARS = 15000;
constexpr int MAX_GLOB_RESULTS = 200;
constexpr int MAX_LIST_DEPTH = 6;
constexpr int MAX_LIST_LINES = 300;
constexpr int MAX_FILES_PER_FOLDER = 40;
// Where a search of `target` runs. In a container the project is the same
// folder inside and out, so it's searched here, where ripgrep is; anything
// else is the container's own and is searched inside it.
std::shared_ptr<Sandbox> searchSandbox(const ToolContext& ctx, const QString& target) {
  auto sandbox = ctx.sandboxOrNone();
  if (sandbox->type() == SandboxType::Container && !sandbox->sandboxPath(target, ctx.cwd)) {
    return Sandbox::none();
  }
  return sandbox;
}

std::optional<QString> outsideReason(const QJsonObject& input, const ToolContext& ctx) {
  const QString path = input.value("path").toString();
  if (!path.isEmpty() && resolveForRead(ctx.cwd, path, ctx.sandbox).outside) {
    return OUTSIDE;
  }
  return std::nullopt;
}

QString fileCount(int n) {
  return plural(n, "file");
}

// Every file under `base`, as glob and list_files see them: listed inside the
// sandbox when it's active, so hidden folders stay hidden; with ripgrep when
// it's there (and not in a container), which also skips .gitignored files.
QList<FileEntry> allFiles(const ToolContext& ctx, const std::shared_ptr<Sandbox>& sandbox, const QString& base) {
  const QString rg = sandbox->type() == SandboxType::Container ? QString() : fastTools().rg;
  if (!rg.isEmpty()) {
    return listFiles(sandbox->programSpec(rg, ripgrepListArgs(), ctx.cwd, base), base, ctx.cancel);
  }
  if (sandbox->active()) {
    return listFiles(sandbox->programSpec("find", findArgs(), ctx.cwd, base), base, ctx.cancel);
  }
  return walk(base, ctx.cancel);
}

QStringList ripgrepArgs(const QJsonObject& input, const QString& target) {
  QStringList args{"--line-number", "--no-heading", "--color", "never", "--max-columns", "300", "--max-columns-preview"};
  if (input.value("files_only").toBool(false)) {
    args << "--files-with-matches";
  }
  if (input.value("ignore_case").toBool(false)) {
    args << "--ignore-case";
  }
  if (input.value("context").toInt(0) > 0) {
    args << "--context" << QString::number(input.value("context").toInt());
  }
  if (!input.value("glob").toString().isEmpty()) {
    args << "--glob" << input.value("glob").toString();
  }
  args << "--regexp" << input.value("pattern").toString() << "--" << target;
  return args;
}

QStringList grepArgs(const QJsonObject& input, const QString& target) {
  QStringList args{"-r", "-n", "-I", "-E"};
  for (const QString& dir : SKIP_DIRS) {
    args << "--exclude-dir=" + dir;
  }
  if (input.value("files_only").toBool(false)) {
    args << "-l";
  }
  if (input.value("ignore_case").toBool(false)) {
    args << "-i";
  }
  if (input.value("context").toInt(0) > 0) {
    args << "-C" << QString::number(input.value("context").toInt());
  }
  if (!input.value("glob").toString().isEmpty()) {
    args << "--include=" + QFileInfo(input.value("glob").toString()).fileName();
  }
  args << "-e" << input.value("pattern").toString() << "--" << target;
  return args;
}

struct SearchResult {
  QString output;
  int code = 0;
  bool missing = false;
};

// Whether a sandboxed run failed because the program isn't there: bwrap or the container runtime says so.
bool notInstalled(const QString& runner, int code, const QString& output) {
  if (runner == "bwrap") {
    static const QRegularExpression execvp("^bwrap: execvp ", QRegularExpression::MultilineOption);
    return code == 1 && execvp.match(output).hasMatch();
  }
  if (runner == "podman" || runner == "docker") {
    static const QRegularExpression notFound("executable file not found|no such file or directory", QRegularExpression::CaseInsensitiveOption);
    return (code == 126 || code == 127) && notFound.match(output).hasMatch();
  }
  return false;
}

// Runs a search binary without a shell, so the pattern is never shell-interpreted.
SearchResult runSearch(const SpawnSpec& spec, const QString& cwd, const CancelPtr& cancel) {
  RunOptions options;
  options.cwd = cwd;
  options.cancel = cancel;
  options.mergeChannels = true;
  options.maxCapture = MAX_OUTPUT_CHARS * 4;
  const ProcessResult result = runProcess(spec, options);
  if (!result.started) {
    return SearchResult{result.startError, -1, result.missing};
  }
  const QString output = utf8(result.out);
  return SearchResult{output, result.exitCode, notInstalled(spec.file, result.exitCode, output)};
}

struct Folder {
  QMap<QString, std::shared_ptr<Folder>> folders;
  QStringList files;
  // Files in it and everything below.
  int count = 0;
};

} // namespace

ToolDef grepTool() {
  ToolDef tool;
  tool.name = "grep";
  tool.description = "Search file contents with a regular expression (ripgrep syntax). Respects .gitignore. Returns matching lines as "
                     "path:line:text, or only file paths with files_only. Prefer this over running grep through bash.";
  tool.schema = Schema::object(
      {
          {"pattern", Schema::string("Regular expression to search for", 1)},
          {"path", Schema::string("File or directory to search (default: the working directory); may be outside it, which asks the user")},
          {"glob", Schema::string(R"(Only search files matching this glob, e.g. "*.ts" or "src/**/*.py")")},
          {"ignore_case", Schema::boolean("Case-insensitive search")},
          {"context", Schema::integer("Lines of context around each match", 0, 10)},
          {"files_only", Schema::boolean("Return only the paths of matching files")},
      },
      {"pattern"});
  tool.readOnly = true;
  tool.summarize = [](const QJsonObject& input) {
    QStringList parts{"/" + input.value("pattern").toString() + "/"};
    if (!input.value("path").toString().isEmpty()) {
      parts << "in " + input.value("path").toString();
    }
    if (!input.value("glob").toString().isEmpty()) {
      parts << "(" + input.value("glob").toString() + ")";
    }
    return parts.join(' ');
  };
  tool.approvalReason = outsideReason;
  tool.run = [](const QJsonObject& input, const ToolContext& ctx) {
    const QString path = input.value("path").toString();
    const QString absolute = path.isEmpty() ? ctx.cwd : resolveForRead(ctx.cwd, path, ctx.sandbox).path;
    QString target = QDir(ctx.cwd).relativeFilePath(absolute);
    if (target.isEmpty()) {
      target = ".";
    }
    const auto sandbox = searchSandbox(ctx, absolute);
    // ripgrep when installed (much faster, respects .gitignore), else grep.
    // In a container, this machine's ripgrep isn't there; grep is.
    const QString rg = sandbox->type() == SandboxType::Container ? QString() : fastTools().rg;
    auto search = [&](const QString& command, const QStringList& args) {
      try {
        return runSearch(sandbox->programSpec(command, args, ctx.cwd), ctx.cwd, ctx.cancel);
      } catch (const SmithError& error) {
        return SearchResult{error.message(), 2, false};
      }
    };
    std::optional<SearchResult> result;
    if (!rg.isEmpty()) {
      result = search(rg, ripgrepArgs(input, target));
    }
    if (!result || result->missing) {
      result = search("grep", grepArgs(input, target));
    }
    if (result->missing) {
      return ToolResult::error("Neither rg nor grep is installed.");
    }
    // Both tools exit 1 for "no matches" and 2 for real errors.
    if (result->code == 1 && result->output.trimmed().isEmpty()) {
      return ToolResult{"No matches."};
    }
    if (result->code != 0 && result->code != 1) {
      const QString text = result->output.trimmed();
      return ToolResult::error(text.isEmpty() ? QString("Search failed with exit code %1").arg(result->code) : text);
    }
    QString output = result->output;
    while (output.endsWith('\n') || output.endsWith(' ')) {
      output.chop(1);
    }
    return ToolResult{truncateText(output, MAX_OUTPUT_CHARS)};
  };
  return tool;
}

ToolDef globTool() {
  ToolDef tool;
  tool.name = "glob";
  tool.description = QString(R"(Find files by name with a glob pattern such as "**/*.ts" or "src/**/test_*.py". Returns up to %1 paths, most recently )"
                             "modified first. Skips .git, node_modules, common build directories and (when ripgrep is installed) anything in "
                             ".gitignore.")
                         .arg(MAX_GLOB_RESULTS);
  tool.schema = Schema::object(
      {
          {"pattern", Schema::string("Glob pattern, relative to path", 1)},
          {"path", Schema::string("Directory to search from (default: the working directory); may be outside it, which asks the user")},
      },
      {"pattern"});
  tool.readOnly = true;
  tool.summarize = [](const QJsonObject& input) {
    const QString path = input.value("path").toString();
    return path.isEmpty() ? input.value("pattern").toString() : input.value("pattern").toString() + " in " + path;
  };
  tool.approvalReason = outsideReason;
  tool.run = [](const QJsonObject& input, const ToolContext& ctx) {
    const QString pattern = input.value("pattern").toString();
    if (QDir::isAbsolutePath(pattern) || pattern.split('/').contains("..")) {
      return ToolResult::error("The pattern must be relative and must not contain '..'. Use path to choose where to search.");
    }
    const QString path = input.value("path").toString();
    const QString base = resolveForRead(ctx.cwd, path.isEmpty() ? QString(".") : path, ctx.sandbox).path;
    const auto sandbox = searchSandbox(ctx, base);
    const QRegularExpression regex = globToRegExp(pattern);

    QList<FileEntry> files;
    try {
      files = allFiles(ctx, sandbox, base);
    } catch (const SmithError& error) {
      return ToolResult::error(error.message());
    }
    struct Match {
      QString file;
      qint64 mtime;
    };
    QList<Match> matches;
    for (const FileEntry& entry : files) {
      if (!regex.match(entry.rel).hasMatch()) {
        continue;
      }
      // The sandbox's /tmp is somewhere else on this machine, and a container's
      // own files are nowhere here: those are listed as files, oldest first.
      const std::optional<SandboxPath> view = sandbox->sandboxPath(entry.abs, ctx.cwd);
      qint64 mtime = 0;
      if (!view || !view->host.isEmpty()) {
        const QFileInfo info(view ? view->host : entry.abs);
        if (!info.isFile()) {
          continue;
        }
        mtime = info.lastModified().toMSecsSinceEpoch();
      }
      matches.append({QDir(ctx.cwd).relativeFilePath(entry.abs), mtime});
      // Collect a few more than we show so the newest files are likely among them.
      if (matches.size() >= MAX_GLOB_RESULTS * 5) {
        break;
      }
    }
    if (matches.isEmpty()) {
      return ToolResult{"No files matched."};
    }
    std::stable_sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) { return a.mtime > b.mtime; });
    QStringList shown;
    for (int i = 0; i < std::min(int(matches.size()), MAX_GLOB_RESULTS); i++) {
      shown << matches[i].file;
    }
    const QString more =
        matches.size() > shown.size() ? QString("\n\n[%1+ more not shown; narrow the pattern]").arg(matches.size() - shown.size()) : QString();
    return ToolResult{shown.join('\n') + more};
  };
  return tool;
}

ToolDef listFilesTool() {
  ToolDef tool;
  tool.name = "list_files";
  tool.description = "Show what's in a folder as a tree: folders first, then files, a couple of levels deep. Deeper folders are collapsed to a "
                     "file count. Use it to get the layout of a project or a folder; use glob to find files by name. Skips .git, node_modules, "
                     "common build directories and (when ripgrep is installed) anything in .gitignore.";
  tool.schema = Schema::object(
      {
          {"path", Schema::string("The folder (default: the working directory); may be outside it, which asks the user")},
          {"depth", Schema::integer("Levels of folders to open (default 2)", 1, MAX_LIST_DEPTH)},
      },
      {});
  tool.readOnly = true;
  tool.summarize = [](const QJsonObject& input) {
    const QString path = input.value("path").toString(".");
    return input.contains("depth") ? QString("%1 (depth %2)").arg(path, QString::number(input.value("depth").toInt())) : path;
  };
  tool.approvalReason = outsideReason;
  tool.run = [](const QJsonObject& input, const ToolContext& ctx) {
    const QString given = input.value("path").toString();
    const QString shown = given.isEmpty() ? QString(".") : given;
    const int depth = input.value("depth").toInt(2);
    const QString base = resolveForRead(ctx.cwd, shown, ctx.sandbox).path;
    const auto sandbox = searchSandbox(ctx, base);
    // Check it's a folder, where it's on this machine at all (not a container's own).
    const std::optional<SandboxPath> view = sandbox->sandboxPath(base, ctx.cwd);
    if (!view || !view->host.isEmpty()) {
      const QFileInfo info(view ? view->host : base);
      if (!info.exists()) {
        return ToolResult::error(shown + " doesn't exist.");
      }
      if (!info.isDir()) {
        return ToolResult::error(shown + " is a file; read it with read_file.");
      }
    }

    auto root = std::make_shared<Folder>();
    int total = 0;
    try {
      for (const FileEntry& entry : allFiles(ctx, sandbox, base)) {
        total++;
        const QStringList parts = entry.rel.split('/');
        Folder* folder = root.get();
        folder->count++;
        for (int i = 0; i < parts.size() - 1; i++) {
          auto& next = folder->folders[parts[i]];
          if (!next) {
            next = std::make_shared<Folder>();
          }
          next->count++;
          folder = next.get();
        }
        folder->files << parts.last();
      }
    } catch (const SmithError& error) {
      return ToolResult::error(error.message());
    }
    if (total == 0) {
      return ToolResult{shown + " is empty (or holds only skipped folders)."};
    }

    QString head = shown;
    while (head.size() > 1 && head.endsWith('/')) {
      head.chop(1);
    }
    QStringList lines{QString("%1/ (%2)").arg(head, fileCount(total))};
    bool cut = false;
    auto add = [&](const QString& line) {
      if (lines.size() > MAX_LIST_LINES) {
        cut = true;
        return false;
      }
      lines << line;
      return true;
    };
    auto byName = [](const QString& a, const QString& b) { return QString::localeAwareCompare(a, b) < 0; };
    std::function<void(Folder&, int, const QString&)> render = [&](Folder& folder, int level, const QString& indent) {
      QStringList names = folder.folders.keys();
      std::sort(names.begin(), names.end(), byName);
      for (const QString& name : names) {
        const auto& sub = folder.folders.value(name);
        if (level >= depth) {
          if (!add(QString("%1%2/ (%3)").arg(indent, name, fileCount(sub->count)))) {
            return;
          }
        } else {
          if (!add(indent + name + "/")) {
            return;
          }
          render(*sub, level + 1, indent + "  ");
          if (cut) {
            return;
          }
        }
      }
      std::sort(folder.files.begin(), folder.files.end(), byName);
      for (int i = 0; i < std::min(int(folder.files.size()), MAX_FILES_PER_FOLDER); i++) {
        if (!add(indent + folder.files[i])) {
          return;
        }
      }
      if (folder.files.size() > MAX_FILES_PER_FOLDER) {
        add(indent + "… " + plural(folder.files.size() - MAX_FILES_PER_FOLDER, "more file"));
      }
    };
    render(*root, 1, "  ");
    if (cut) {
      lines << QString("\n[Stopped at %1 lines; list a subfolder, or use a smaller depth]").arg(MAX_LIST_LINES);
    }
    return ToolResult{lines.join('\n')};
  };
  return tool;
}

} // namespace Smith
