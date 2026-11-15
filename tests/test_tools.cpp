// Port of the original test/tools.test.ts, test/boundaries.test.ts and test/config.test.ts.

#include "FakeServer.h"
#include "TestUtil.h"

#include "core/Config.h"
#include "core/Jobs.h"
#include "core/Prompt.h"
#include "core/Server.h"
#include "core/Toolchain.h"
#include "core/tools/Tools.h"

#include <QDirIterator>
#include <QElapsedTimer>

using namespace Smith;
using namespace TestUtil;

class TestTools : public QObject {
  Q_OBJECT

private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();

  // file tools
  void writeThenReadNumberedLines();
  void readFileRefusesBinary();
  void editFileRequiresUniqueMatch();
  void writesStayInsideEvenThroughSymlink();
  void writesReplaceWholeFiles();
  void readsOutsideAskForApproval();
  void credentialsAreOffLimits();

  // search tools
  void grepFindsMatches();
  void globListsAndSkipsNodeModules();
  void globPatternsCantLeave();
  void globSyntax();

  // fast tools
  void ripgrepRespectsGitignore();
  void fallsBackWithoutRipgrep();
  void promptSuggestsOnlyInstalledTools();

  // write protection
  void refusesToWriteIntoGit();

  // bash
  void bashCapturesOutputAndExitCodes();
  void bashKillsTimeouts();

  // list_files
  void listFilesTree();
  void listFilesErrors();

  // background jobs (run in order: they share the job table)
  void jobsReturnAtOnceAndReadInPieces();
  void jobsUntilStopsWaiting();
  void jobsKillStopsChildren();
  void jobsClearRestartsNumbering();

  // boundaries
  void tuiIncludesOnlyItself();
  void librariesDontIncludeApps();
  void appsDontIncludeEachOther();
  void desktopTextIsPlain();

  // config file
  void configMissingFile();
  void configEndpointsAndApiKeyEnv();
  void configMaxSteps();
  void configRejectsInvalidFiles();
  void configWarnsAboutReadableKeys();

  // choosing a model
  void prefersRememberedThenConfiguredThenFirst();
  void configuredModelWhenListingFails();
  void configuredContextSizeWins();

private:
  ToolResult call(const ToolDef& tool, const QJsonObject& input) { return run(tool, input, m_ctx); }

  std::unique_ptr<TempDir> m_pDir;
  ToolContext m_ctx;
  std::shared_ptr<BackgroundJobs> m_pJobs;
  std::unique_ptr<FakeServer> m_pFake;
};

#ifndef Q_MOC_RUN

void TestTools::initTestCase() {
  m_pDir = std::make_unique<TempDir>(QDir::tempPath(), "agent-tools-");
  m_ctx = context(m_pDir->path());
  m_pJobs = std::make_shared<BackgroundJobs>();
  m_pFake = std::make_unique<FakeServer>();
}

void TestTools::cleanupTestCase() {
  m_pJobs->clear();
  m_pJobs.reset();
  m_pFake.reset();
  m_pDir.reset();
}

void TestTools::writeThenReadNumberedLines() {
  call(writeFileTool(), {{"path", "sub/a.txt"}, {"content", "one\ntwo\nthree\n"}});
  const ToolResult res = call(readFileTool(), {{"path", "sub/a.txt"}, {"offset", 2}, {"limit", 1}});
  QVERIFY(!res.isError);
  QVERIFY_MATCH(res.content, "^2\\ttwo");
  QVERIFY_MATCH(res.content, "Showing lines 2-2 of 3");
}

void TestTools::readFileRefusesBinary() {
  // A NUL early on, with text after it: Qt 5 would decode only up to the NUL and miss it.
  QFile file(m_pDir->file("image.png"));
  QVERIFY(file.open(QIODevice::WriteOnly));
  file.write(QByteArray("\x89PNG\r\n\x1a\n\0\0\0\rIHDR then more bytes", 30));
  file.close();
  const ToolResult res = call(readFileTool(), {{"path", "image.png"}});
  QVERIFY(res.isError);
  QVERIFY_MATCH(res.content, "image\\.png looks like a binary file");
}

void TestTools::editFileRequiresUniqueMatch() {
  call(writeFileTool(), {{"path", "b.txt"}, {"content", "x = 1\nx = 1\n"}});
  const ToolResult dup = call(editFileTool(), {{"path", "b.txt"}, {"old_string", "x = 1"}, {"new_string", "x = 2"}});
  QVERIFY(dup.isError);
  QVERIFY_MATCH(dup.content, "occurs 2 times");

  const ToolResult all = call(editFileTool(), {{"path", "b.txt"}, {"old_string", "x = 1"}, {"new_string", "x = $&2"}, {"replace_all", true}});
  QVERIFY(!all.isError);
  QCOMPARE(read(m_pDir->file("b.txt")), QString("x = $&2\nx = $&2\n"));
}

void TestTools::writesReplaceWholeFiles() {
  // Renamed into place: the mode stays, a link still leads to the file, and nothing is left beside it.
  write(m_pDir->file("atomic/run.sh"), "old\n");
  chmod(m_pDir->file("atomic/run.sh"), 0755);
  QVERIFY(QFile::link("run.sh", m_pDir->file("atomic/link.sh")));
  call(writeFileTool(), {{"path", "atomic/run.sh"}, {"content", "new\n"}});
  QCOMPARE(read(m_pDir->file("atomic/run.sh")), QString("new\n"));
  QVERIFY(QFileInfo(m_pDir->file("atomic/run.sh")).isExecutable());
  call(writeFileTool(), {{"path", "atomic/link.sh"}, {"content", "linked\n"}});
  QVERIFY(QFileInfo(m_pDir->file("atomic/link.sh")).isSymLink());
  QCOMPARE(read(m_pDir->file("atomic/run.sh")), QString("linked\n"));
  QCOMPARE(QDir(m_pDir->file("atomic")).entryList(QDir::Files | QDir::Hidden | QDir::System), QStringList({"link.sh", "run.sh"}));

  // A file that can be written in a folder that can't.
  write(m_pDir->file("locked/a.txt"), "old\n");
  chmod(m_pDir->file("locked"), 0555);
  call(writeFileTool(), {{"path", "locked/a.txt"}, {"content", "new\n"}});
  chmod(m_pDir->file("locked"), 0755);
  QCOMPARE(read(m_pDir->file("locked/a.txt")), QString("new\n"));
}

void TestTools::writesStayInsideEvenThroughSymlink() {
  QVERIFY_THROWS_MATCH(call(writeFileTool(), {{"path", "../outside.txt"}, {"content", "x"}}), "outside the working directory");
  QVERIFY(QFile::link(QDir::tempPath(), m_pDir->file("escape")));
  QVERIFY_THROWS_MATCH(call(writeFileTool(), {{"path", "escape/pwned.txt"}, {"content", "x"}}), "outside the working directory");
}

void TestTools::readsOutsideAskForApproval() {
  QCOMPARE(reason(readFileTool(), {{"path", "sub/a.txt"}}, m_ctx), std::optional<QString>());
  QVERIFY_MATCH(reason(readFileTool(), {{"path", "/etc/hostname"}}, m_ctx).value_or(""), "outside the project");
  QVERIFY_MATCH(reason(grepTool(), {{"pattern", "x"}, {"path", "../.."}}, m_ctx).value_or(""), "outside the project");
  QVERIFY_MATCH(reason(globTool(), {{"pattern", "*"}, {"path", "/usr"}}, m_ctx).value_or(""), "outside the project");
  QCOMPARE(reason(grepTool(), {{"pattern", "x"}}, m_ctx), std::optional<QString>());
  // Approval is the agent's job; called directly, the tool just reads.
  QVERIFY_MATCH(call(readFileTool(), {{"path", "/etc/os-release"}}).content, "^1\\t");
}

void TestTools::credentialsAreOffLimits() {
  // Not under /tmp: that is off limits by itself and would hide whether the ~/.ssh rule works.
  TempDir home(QDir::homePath(), ".agent-test-home-");
  EnvGuard env("HOME", home.path());
  write(home.file(".ssh/id_ed25519"), "PRIVATE");
  QVERIFY(QFile::link(home.file(".ssh"), m_pDir->file("keys")));
  for (const QString& path : {QString("~/.ssh/id_ed25519"), home.file(".ssh/id_ed25519"), QString("keys/id_ed25519"), QString("/proc/self/environ")}) {
    const std::optional<QString> message = thrown([&]() { call(readFileTool(), {{"path", path}}); });
    QVERIFY2(message && message->contains("off limits"), qPrintable(path));
  }
  QVERIFY_THROWS_MATCH(call(grepTool(), {{"pattern", "PRIVATE"}, {"path", "~/.ssh"}}), "off limits");
  QVERIFY_THROWS_MATCH(call(globTool(), {{"pattern", "*"}, {"path", "/proc"}}), "off limits");
  QFile::remove(m_pDir->file("keys"));
}

void TestTools::grepFindsMatches() {
  call(writeFileTool(), {{"path", "src/app.ts"}, {"content", "const answer = 42;\nfunction main() {}\n"}});
  call(writeFileTool(), {{"path", "src/notes.md"}, {"content", "answer pending\n"}});

  const ToolResult hits = call(grepTool(), {{"pattern", "answer\\s*="}, {"glob", "*.ts"}});
  QVERIFY(!hits.isError);
  QVERIFY_MATCH(hits.content, "src/app\\.ts:1:const answer = 42;");
  QVERIFY_NO_MATCH(hits.content, "notes\\.md");

  QStringList files = call(grepTool(), {{"pattern", "answer"}, {"files_only", true}, {"path", "src"}}).content.split('\n');
  files.sort();
  QCOMPARE(files, QStringList({"src/app.ts", "src/notes.md"}));

  QCOMPARE(call(grepTool(), {{"pattern", "nothing-matches-this"}}).content, QString("No matches."));
}

void TestTools::globListsAndSkipsNodeModules() {
  call(writeFileTool(), {{"path", "node_modules/pkg/index.ts"}, {"content", ""}});
  const ToolResult res = call(globTool(), {{"pattern", "**/*.ts"}});
  QVERIFY_MATCH(res.content, "src/app\\.ts");
  QVERIFY_NO_MATCH(res.content, "node_modules");
  QCOMPARE(call(globTool(), {{"pattern", "**/*.nope"}}).content, QString("No files matched."));
}

void TestTools::globPatternsCantLeave() {
  for (const QString pattern : {"../*", "src/../../*", "/etc/*"}) {
    QVERIFY2(call(globTool(), {{"pattern", pattern}}).isError, qPrintable(pattern));
  }
}

void TestTools::globSyntax() {
  call(writeFileTool(), {{"path", "g/a.test.ts"}, {"content", ""}});
  call(writeFileTool(), {{"path", "g/deep/b.test.js"}, {"content", ""}});
  call(writeFileTool(), {{"path", "g/c.md"}, {"content", ""}});
  QStringList found = call(globTool(), {{"pattern", "g/**/*.test.{ts,js}"}}).content.split('\n');
  found.sort();
  QCOMPARE(found, QStringList({"g/a.test.ts", "g/deep/b.test.js"}));
  QCOMPARE(call(globTool(), {{"pattern", "g/?.md"}}).content, QString("g/c.md"));
  QVERIFY2(call(globTool(), {{"pattern", "*.md"}}).content == "No files matched.", "no slash means top level only");
}

void TestTools::ripgrepRespectsGitignore() {
  if (fastTools().rg.isEmpty()) {
    QSKIP("rg not installed");
  }
  QDir().mkpath(m_pDir->file(".git"));
  call(writeFileTool(), {{"path", ".gitignore"}, {"content", "generated/\n"}});
  call(writeFileTool(), {{"path", "generated/out.ts"}, {"content", ""}});
  call(writeFileTool(), {{"path", "kept/in.ts"}, {"content", ""}});
  const ToolResult res = call(globTool(), {{"pattern", "**/*.ts"}});
  QVERIFY_MATCH(res.content, "kept/in\\.ts");
  QVERIFY_NO_MATCH(res.content, "generated");
}

void TestTools::fallsBackWithoutRipgrep() {
  if (!fastTools().rg.isEmpty()) {
    // The fast-tool lookup is done once per process; with rg installed, there's no fallback to test here.
    QSKIP("rg is installed");
  }
  call(writeFileTool(), {{"path", "fallback/a.txt"}, {"content", "needle here\n"}});
  QVERIFY_MATCH(call(grepTool(), {{"pattern", "needle"}, {"path", "fallback"}}).content, "fallback/a\\.txt:1:needle here");
  QVERIFY_MATCH(call(globTool(), {{"pattern", "fallback/*.txt"}}).content, "fallback/a\\.txt");
}

void TestTools::promptSuggestsOnlyInstalledTools() {
  PromptExtras extras;
  extras.fastTools = FastTools{"/usr/bin/rg", "/usr/bin/fdfind", {}};
  QVERIFY_MATCH(buildSystemPrompt("/x", extras), "rg \\(ripgrep\\) instead of grep -r, fdfind instead of find");
  extras.fastTools = FastTools{};
  QVERIFY_NO_MATCH(buildSystemPrompt("/x", extras), "Faster tools");
}

void TestTools::refusesToWriteIntoGit() {
  QVERIFY_THROWS_MATCH(call(writeFileTool(), {{"path", ".git/hooks/pre-commit"}, {"content", "evil"}}), "Refusing to modify \\.git");
  const ToolResult edit = call(editFileTool(), {{"path", ".git/config"}, {"old_string", "a"}, {"new_string", "b"}});
  QVERIFY(edit.isError);
  QVERIFY_MATCH(edit.content, "Refusing to modify \\.git");
}

void TestTools::bashCapturesOutputAndExitCodes() {
  const ToolResult ok = call(bashTool(), {{"command", "echo hi; echo err >&2"}});
  QVERIFY(!ok.isError);
  QVERIFY_MATCH(ok.content, "hi\\nerr");
  const ToolResult fail = call(bashTool(), {{"command", "exit 3"}});
  QVERIFY(fail.isError);
  QVERIFY_MATCH(fail.content, "Exit code 3");
}

void TestTools::bashKillsTimeouts() {
  QElapsedTimer clock;
  clock.start();
  const ToolResult res = call(bashTool(), {{"command", "sleep 5"}, {"timeout_ms", 200}});
  QVERIFY(res.isError);
  QVERIFY_MATCH(res.content, "timed out");
  QVERIFY(clock.elapsed() < 3000);
}

void TestTools::listFilesTree() {
  const QString base = m_pDir->file("tree");
  for (const QString rel : {"README.md", "src/main.ts", "src/lib/a.ts", "src/lib/deep/b.ts", "node_modules/x/index.js"}) {
    write(base + "/" + rel, "");
  }
  for (int i = 0; i < 45; i++) {
    write(base + QString("/many/f%1.txt").arg(i, 2, 10, QChar('0')), "");
  }
  const QString out = call(listFilesTool(), {{"path", "tree"}}).content;
  QStringList kept;
  for (const QString& line : out.split('\n')) {
    if (!line.contains("many/") && !matches(line, "^ {4}f\\d\\d\\.txt|more files$")) {
      kept << line;
    }
  }
  QCOMPARE(kept.join('\n'), QString("tree/ (49 files)\n  src/\n    lib/ (2 files)\n    main.ts\n  README.md"));
  QVERIFY(QRegularExpression("^ {2}many/\\n {4}f00\\.txt", QRegularExpression::MultilineOption).match(out).hasMatch());
  QVERIFY2(QRegularExpression("^ {4}… 5 more files$", QRegularExpression::MultilineOption).match(out).hasMatch(), qPrintable(out));
  QVERIFY(QRegularExpression("^ {2}src/ \\(3 files\\)$", QRegularExpression::MultilineOption)
              .match(call(listFilesTool(), {{"path", "tree"}, {"depth", 1}}).content)
              .hasMatch());
  QVERIFY(QRegularExpression("^ {6}deep/ \\(1 file\\)$", QRegularExpression::MultilineOption)
              .match(call(listFilesTool(), {{"path", "tree"}, {"depth", 3}}).content)
              .hasMatch());
}

void TestTools::listFilesErrors() {
  QVERIFY_MATCH(call(listFilesTool(), {{"path", "tree/README.md"}}).content, "is a file; read it with read_file");
  QVERIFY_MATCH(call(listFilesTool(), {{"path", "nope"}}).content, "nope doesn't exist");
  QDir().mkpath(m_pDir->file("empty"));
  QVERIFY_MATCH(call(listFilesTool(), {{"path", "empty"}}).content, "empty is empty");
}

void TestTools::jobsReturnAtOnceAndReadInPieces() {
  const ToolDef bg = bashTool(m_pJobs);
  const ToolList tools = jobTools(m_pJobs);
  QElapsedTimer clock;
  clock.start();
  const ToolResult res = call(bg, {{"command", "echo first; sleep 1.5; echo second"}, {"run_in_background", true}});
  QVERIFY(clock.elapsed() < 1500);
  QVERIFY_MATCH(res.content, "^Started background job bg1\\b[\\s\\S]*Output so far:\\nfirst");
  QCOMPARE(m_pJobs->running(), 1);

  const ToolResult waited = call(tools[0], {{"id", "bg1"}, {"wait_ms", 5000}});
  QVERIFY2(waited.content == "first\nsecond\n\n[Job bg1: exited with code 0]", qPrintable("what the start showed stays unread: " + waited.content));
  QVERIFY_MATCH(call(tools[0], {{"id", "bg1"}}).content, "^\\(no new output\\)");
  QCOMPARE(m_pJobs->running(), 0);
}

void TestTools::jobsUntilStopsWaiting() {
  const ToolDef bg = bashTool(m_pJobs);
  const ToolList tools = jobTools(m_pJobs);
  const ToolResult res = call(bg, {{"command", "sleep 0.3; echo 'server listening on :8080'; sleep 30"}, {"run_in_background", true}});
  const QString id = QRegularExpression("job (bg\\d+)").match(res.content).captured(1);
  QElapsedTimer clock;
  clock.start();
  const ToolResult ready = call(tools[0], {{"id", id}, {"wait_ms", 10000}, {"until", "listening on"}});
  QVERIFY(clock.elapsed() < 5000);
  QVERIFY_MATCH(ready.content, "server listening on :8080[\\s\\S]*running; the output matched /listening on/");
  QVERIFY_MATCH(call(tools[1], {{"id", id}}).content, QString("Stopped job %1 \\(stopped \\(SIGKILL\\)\\)").arg(id));
  QVERIFY_MATCH(call(tools[1], {{"id", id}}).content, "had already ended");
}

void TestTools::jobsKillStopsChildren() {
  const ToolDef bg = bashTool(m_pJobs);
  const ToolList tools = jobTools(m_pJobs);
  const ToolResult res = call(bg, {{"command", "sleep 30 & sleep 30; wait"}, {"run_in_background", true}});
  const QString id = QRegularExpression("job (bg\\d+)").match(res.content).captured(1);
  call(tools[1], {{"id", id}});
  QCOMPARE(m_pJobs->running(), 0);
  QVERIFY_THROWS_MATCH(call(tools[0], {{"id", "bg99"}}), "No background job bg99\\. Jobs: bg1, bg2, bg3\\.");
  const ToolResult failed = call(bg, {{"command", "echo nope >&2; exit 4"}, {"run_in_background", true}});
  QVERIFY(failed.isError);
  QVERIFY_MATCH(failed.content, "exited with code 4 right away[\\s\\S]*nope");
  QVERIFY2(tools[1].subagents == false, "sub-agents can read jobs, not stop them");
}

void TestTools::jobsClearRestartsNumbering() {
  const ToolDef bg = bashTool(m_pJobs);
  call(bg, {{"command", "sleep 30"}, {"run_in_background", true}});
  QCOMPARE(m_pJobs->clear(), 1);
  QVERIFY(m_pJobs->list().isEmpty());
  QVERIFY_MATCH(call(bg, {{"command", "sleep 30"}, {"run_in_background", true}}).content, "job bg1\\b");
  m_pJobs->clear();
}

void TestTools::desktopTextIsPlain() {
  // What the model writes reaches these as text. A Text left to detect rich text renders
  // markup in it, and fetches an <img>'s address from this machine.
  QStringList offenders;
  int seen = 0;
  QDirIterator it(QString(SMITH_SOURCE_DIR) + "/apps/smith-desktop/qml", {"*.qml"}, QDir::Files, QDirIterator::Subdirectories);
  static const QRegularExpression opening(R"(\b(Text|Label)\s*\{)");
  while (it.hasNext()) {
    const QString file = it.next();
    const QString source = read(file);
    auto found = opening.globalMatch(source);
    while (found.hasNext()) {
      const QRegularExpressionMatch match = found.next();
      int depth = 0;
      int end = match.capturedEnd() - 1;
      for (; end < source.size(); end++) {
        depth += source[end] == '{' ? 1 : source[end] == '}' ? -1 : 0;
        if (depth == 0) {
          break;
        }
      }
      seen++;
      if (!source.mid(match.capturedStart(), end - match.capturedStart()).contains("textFormat:")) {
        offenders << QString("%1:%2").arg(QFileInfo(file).fileName()).arg(source.left(match.capturedStart()).count('\n') + 1);
      }
    }
  }
  QVERIFY(seen > 50);
  QVERIFY2(offenders.isEmpty(), qPrintable("Text without a textFormat (use Text.PlainText): " + offenders.join(", ")));
}

namespace {

// The quoted includes of a folder's sources that aren't allowed: each must
// start with one of `prefixes`, or (with `local`) name a file in that folder.
QStringList includeOffenders(const QString& folder, const QStringList& prefixes, bool local, int& files) {
  const QString dir = QString(SMITH_SOURCE_DIR) + "/" + folder;
  QStringList offenders;
  QDirIterator it(dir, {"*.h", "*.cpp"}, QDir::Files, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    const QString file = it.next();
    files++;
    auto includes = QRegularExpression("^#include \"([^\"]+)\"", QRegularExpression::MultilineOption).globalMatch(read(file));
    while (includes.hasNext()) {
      const QString spec = includes.next().captured(1);
      const bool allowed = std::any_of(prefixes.begin(), prefixes.end(), [&](const QString& prefix) { return spec.startsWith(prefix); }) ||
                           (local && !spec.contains('/') && QFileInfo::exists(dir + "/" + spec));
      if (!allowed) {
        offenders << QDir(dir).relativeFilePath(file) + ": " + spec;
      }
    }
  }
  return offenders;
}

} // namespace

void TestTools::tuiIncludesOnlyItself() {
  // libs/tui is a general terminal toolkit: it must not know about the agent or the apps.
  // libs/compat (Qt 5/6 shims) sits below every library.
  int files = 0;
  const QStringList offenders = includeOffenders("libs/tui", {"tui/", "compat/"}, false, files);
  QVERIFY(files > 0);
  QVERIFY2(offenders.isEmpty(), qPrintable("libs/tui must include only itself and compat: " + offenders.join(", ")));
}

void TestTools::librariesDontIncludeApps() {
  // The agent knows no frontend; what the apps share knows the agent, not the apps.
  int files = 0;
  QStringList offenders = includeOffenders("libs/core", {"core/", "compat/"}, false, files);
  offenders << includeOffenders("libs/frontend", {"core/", "frontend/", "compat/"}, false, files);
  offenders << includeOffenders("libs/compat", {"compat/"}, false, files);
  QVERIFY(files > 0);
  QVERIFY2(offenders.isEmpty(), qPrintable("libs/ must not include the apps: " + offenders.join(", ")));
}

void TestTools::appsDontIncludeEachOther() {
  // Each app is built from the libraries and its own files only.
  const QStringList libraries = {"core/", "tui/", "frontend/", "compat/"};
  int files = 0;
  QStringList offenders = includeOffenders("apps/smith", libraries, true, files);
  offenders << includeOffenders("apps/smith-desktop", libraries, true, files);
  QVERIFY(files > 0);
  QVERIFY2(offenders.isEmpty(), qPrintable("an app includes something that isn't its own or a library: " + offenders.join(", ")));
}

namespace {

QString writeConfig(const QString& dir, const QString& name, const QJsonObject& content, int permissions = 0600) {
  const QString file = dir + "/" + name;
  write(file, QString::fromUtf8(QJsonDocument(content).toJson()));
  chmod(file, permissions);
  return file;
}

} // namespace

void TestTools::configMissingFile() {
  QVERIFY(loadConfig(m_pDir->file("nope.json")).endpoints.isEmpty());
}

void TestTools::configEndpointsAndApiKeyEnv() {
  EnvGuard key("TEST_GPU_KEY", QString("sk-from-env"));
  EnvGuard unset("TEST_UNSET_KEY", std::nullopt);
  // Written by hand, in this order: endpoints keep the file's order.
  const QString file = m_pDir->file("ok.json");
  write(file, "{\n  \"default\": \"gpu\",\n  \"endpoints\": {\n    \"local\": { \"baseURL\": \"http://localhost:8080/v1\" },\n"
              "    \"gpu\": { \"baseURL\": \"http://10.0.0.5:8000/v1\", \"apiKeyEnv\": \"TEST_GPU_KEY\", \"model\": \"qwen\", \"contextSize\": 65536 },\n"
              "    \"missing\": { \"baseURL\": \"http://x/v1\", \"apiKeyEnv\": \"TEST_UNSET_KEY\" }\n  }\n}\n");
  chmod(file, 0600);
  const Config config = loadConfig(file);
  QCOMPARE(config.defaultEndpoint, QString("gpu"));
  QCOMPARE(config.endpoints.size(), 3);
  const Endpoint& gpu = config.endpoints[1];
  QCOMPARE(gpu.name, QString("gpu"));
  QCOMPARE(gpu.baseUrl, QString("http://10.0.0.5:8000/v1"));
  QCOMPARE(gpu.apiKey, QString("sk-from-env"));
  QCOMPARE(gpu.model, QString("qwen"));
  QCOMPARE(gpu.contextSize, std::optional<int>(65536));
  QCOMPARE(config.endpoints[2].name, QString("missing"));
  QVERIFY_MATCH(config.warnings.join('\n'), "TEST_UNSET_KEY is not set");
}

void TestTools::configMaxSteps() {
  QCOMPARE(loadConfig(writeConfig(m_pDir->path(), "steps.json", {{"maxSteps", 120}})).maxSteps, std::optional<int>(120));
  QCOMPARE(loadConfig(writeConfig(m_pDir->path(), "nosteps.json", {})).maxSteps, std::optional<int>());
  QVERIFY_THROWS_MATCH(loadConfig(writeConfig(m_pDir->path(), "badsteps.json", {{"maxSteps", 0}})), "Invalid[\\s\\S]*maxSteps");
  QVERIFY_THROWS_MATCH(loadConfig(writeConfig(m_pDir->path(), "strsteps.json", {{"maxSteps", "50"}})), "Invalid[\\s\\S]*maxSteps");
}

void TestTools::configRejectsInvalidFiles() {
  const QString bad = m_pDir->file("bad.json");
  write(bad, "{ nope");
  QVERIFY_THROWS_MATCH(loadConfig(bad), "not valid JSON");
  QVERIFY_THROWS_MATCH(loadConfig(writeConfig(m_pDir->path(), "url.json", {{"endpoints", QJsonObject{{"a", QJsonObject{{"baseURL", "not a url"}}}}}})),
                       "Invalid[\\s\\S]*baseURL");
  QVERIFY_THROWS_MATCH(
      loadConfig(writeConfig(m_pDir->path(), "typo.json", {{"endpoints", QJsonObject{{"a", QJsonObject{{"baseURL", "http://x/v1"}, {"modle", "m"}}}}}})),
      "modle");
  QVERIFY_THROWS_MATCH(
      loadConfig(writeConfig(m_pDir->path(), "default.json", {{"default", "b"}, {"endpoints", QJsonObject{{"a", QJsonObject{{"baseURL", "http://x/v1"}}}}}})),
      "\"b\" is not defined");
  QVERIFY_THROWS_MATCH(loadConfig(writeConfig(m_pDir->path(), "both.json",
                                              {{"endpoints", QJsonObject{{"a", QJsonObject{{"baseURL", "http://x/v1"}, {"apiKey", "k"}, {"apiKeyEnv", "K"}}}}}})),
                       "not both");
}

void TestTools::configWarnsAboutReadableKeys() {
  const QString file =
      writeConfig(m_pDir->path(), "open.json", {{"endpoints", QJsonObject{{"a", QJsonObject{{"baseURL", "http://x/v1"}, {"apiKey", "sk"}}}}}}, 0644);
  QVERIFY_MATCH(loadConfig(file).warnings.join('\n'), "chmod 600");
  chmod(file, 0600);
  QCOMPARE(loadConfig(file).warnings, QStringList());
}

namespace {

ServerPtr endpoint(const FakeServer& fake, const QString& model = {}, std::optional<int> contextSize = std::nullopt) {
  return Server::fromEndpoint(Endpoint{"t", fake.baseUrl(), {}, model, contextSize});
}

} // namespace

void TestTools::prefersRememberedThenConfiguredThenFirst() {
  m_pFake->setModels(QStringList{"a", "b", "c"});
  QCOMPARE(endpoint(*m_pFake, "c")->chooseModel({"b"}), QString("b"));
  QCOMPARE(endpoint(*m_pFake, "c")->chooseModel({"gone"}), QString("c"));
  QCOMPARE(endpoint(*m_pFake)->chooseModel({QString()}), QString("a"));
}

void TestTools::configuredModelWhenListingFails() {
  m_pFake->setModels(std::nullopt);
  QCOMPARE(endpoint(*m_pFake, "m")->chooseModel({}), QString("m"));
  QVERIFY_THROWS_MATCH(endpoint(*m_pFake)->chooseModel({}), "Could not list models");
}

void TestTools::configuredContextSizeWins() {
  QCOMPARE(endpoint(*m_pFake, {}, 1234)->contextSize("anything"), std::optional<int>(1234));
}

#endif

QTEST_GUILESS_MAIN(TestTools)
#include "test_tools.moc"
