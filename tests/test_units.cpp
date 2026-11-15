// Port of the original test/units.test.ts.

#include "TestUtil.h"

#include "core/Context.h"
#include "core/Dirs.h"
#include "core/Errors.h"
#include "core/Preferences.h"
#include "core/Prompt.h"
#include "core/Images.h"
#include "core/Permissions.h"
#include "core/RunLater.h"
#include "core/Session.h"
#include "core/Think.h"
#include "core/Tool.h"
#include "core/ToolLog.h"

#include <QProcess>

using namespace Smith;
using namespace TestUtil;

class TestUnits : public QObject {
  Q_OBJECT

private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();

  // ThinkTagParser
  void thinkHandlesTagsSplitAtEveryPosition();
  void thinkDropsStrayClosingTagAndKeepsLookAlikes();

  // context helpers
  void trimOldToolResultsKeepsRecentOutputs();
  void chooseTailNeverStartsOnToolMessage();

  // sessions
  void sessionFilesArePrivate();
  void deletingASessionRemovesItsLogs();
  void toolLogAppendsPrivateLines();
  void preferencesArePrivate();
  void sessionsHaveNames();
  void recordsReplacesAndReloads();
  void listingsShowFirstMessage();

  // connection errors
  void connectionErrorsShowCauseAndHint();
  void connectionErrorWithoutCause();

  // the rename
  void movesOldFoldersOnce();

  // long command output
  void truncateLogKeepsErrorLines();
  void promptSaysWhereCommandsRun();

  // smith --delete
  void deleteAsksFirst();

  // run-later files
  void runLaterRecognisedByNameAndPlace();
  void runLaterSnapshots();
  void runLaterSeesGitFolders();
  void messagesNameTheirImages();
  void permissionRulesCoverCommands();

private:
  std::unique_ptr<TempDir> m_pData;
  std::unique_ptr<EnvGuard> m_pDataEnv;
};

#ifndef Q_MOC_RUN

namespace {

struct ThinkResult {
  QString text;
  QString reasoning;
};

ThinkResult think(const QStringList& chunks) {
  ThinkTagParser parser;
  QList<Segment> segments;
  for (const QString& chunk : chunks) {
    segments << parser.push(chunk);
  }
  segments << parser.flush();
  ThinkResult result;
  for (const Segment& segment : segments) {
    (segment.kind == BlockKind::Text ? result.text : result.reasoning) += segment.text;
  }
  return result;
}

Message tool(const QString& id, const QString& content) {
  return toolMessage(id, content);
}

} // namespace

void TestUnits::initTestCase() {
  m_pData = std::make_unique<TempDir>(QDir::tempPath(), "agent-sessions-");
  m_pDataEnv = std::make_unique<EnvGuard>("XDG_DATA_HOME", m_pData->path());
}

void TestUnits::cleanupTestCase() {
  m_pDataEnv.reset();
  m_pData.reset();
}

void TestUnits::thinkHandlesTagsSplitAtEveryPosition() {
  const QString full = "<think>deep thoughts</think>final answer";
  for (int i = 1; i < full.size(); i++) {
    const ThinkResult result = think({full.left(i), full.mid(i)});
    QVERIFY2(result.text == "final answer" && result.reasoning == "deep thoughts", qPrintable(QString("split at %1").arg(i)));
  }
}

void TestUnits::thinkDropsStrayClosingTagAndKeepsLookAlikes() {
  ThinkResult result = think({"reasoning?</think>answer"});
  QCOMPARE(result.text, QString("reasoning?answer"));
  QCOMPARE(result.reasoning, QString());
  result = think({"a < b and <thing>"});
  QCOMPARE(result.text, QString("a < b and <thing>"));
  result = think({"ends with <thi"});
  QCOMPARE(result.text, QString("ends with <thi"));
  QCOMPARE(result.reasoning, QString());
}

void TestUnits::trimOldToolResultsKeepsRecentOutputs() {
  Messages messages;
  for (int i = 0; i < 5; i++) {
    messages << tool(QString("t%1").arg(i), QString("output %1 ").arg(i) + QString(500, 'z'));
  }
  const TrimResult trimmed = trimOldToolResults(messages, 3);
  QCOMPARE(trimmed.count, 2);
  QVERIFY_MATCH(messageText(trimmed.messages[0]), "removed to save context");
  QVERIFY_MATCH(messageText(trimmed.messages[4]), "^output 4");
}

void TestUnits::chooseTailNeverStartsOnToolMessage() {
  const QJsonArray calls{QJsonObject{{"id", "a"}, {"type", "function"}, {"function", QJsonObject{{"name", "bash"}, {"arguments", "{}"}}}}};
  const Messages messages = {
      userMessage("u"),
      Message{{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", calls}},
      tool("a", QString(2000, 'r')),
      tool("b", "small"),
  };
  // Budget fits only the last tool message, which can't stand alone.
  QCOMPARE(chooseTail(messages, 10), int(messages.size()));
  QCOMPARE(chooseTail(messages, 10000), 0);
}

void TestUnits::sessionFilesArePrivate() {
  auto session = Session::create("/private", "m", {});
  session->record(HistoryChange{HistoryChange::Type::Append, userMessage("secret stuff"), {}});
  QCOMPARE(mode(session->file()), QString("600"));
  QCOMPARE(mode(sessionsDir()), QString("700"));
  QCOMPARE(mode(QFileInfo(sessionsDir()).path()), QString("700"));

  // As an older version left them:
  chmod(session->file(), 0644);
  chmod(sessionsDir(), 0755);
  secureSessions();
  QCOMPARE(mode(session->file()), QString("600"));
  QCOMPARE(mode(sessionsDir()), QString("700"));

  // Reopening and writing also fixes a file's permissions.
  chmod(session->file(), 0644);
  Session::open(session->id()).session->record(HistoryChange{HistoryChange::Type::Append, Message{{"role", "assistant"}, {"content", "ok"}}, {}});
  QCOMPARE(mode(session->file()), QString("600"));
}

void TestUnits::deletingASessionRemovesItsLogs() {
  auto session = Session::create("/del", "m", {});
  session->record(HistoryChange{HistoryChange::Type::Append, userMessage("throwaway"), {}});
  const QString id = session->id();
  ToolLog([id]() { return id; }, []() { return QString("/del"); }).write(ToolRun{"2026-01-01T00:00:00.000Z", "bash", QJsonObject{}, {}, "ok", 1, "", {}});
  QVERIFY(exists(toolLogFile(id)));
  QCOMPARE(deleteSession(id), QStringList({session->file(), toolLogFile(id)}));
  QVERIFY(!exists(session->file()) && !exists(toolLogFile(id)));
  QVERIFY_THROWS_MATCH(deleteSession(id), "No session");
  QVERIFY_THROWS_MATCH(deleteSession("../config"), "Not a session id");
}

void TestUnits::toolLogAppendsPrivateLines() {
  QString id = "s-one";
  ToolLog log([&id]() { return id; }, []() { return QString("/project"); });
  const ToolRun run{"2026-11-06T10:00:00.000Z", "bash", QJsonObject{{"command", "ls"}}, "allowed", "ok", 12, "a\nb", {}};
  log.write(run);
  ToolRun big = run;
  big.output = QString(20000, 'x');
  log.write(big);
  id = "s-two";
  ToolRun grep = run;
  grep.tool = "grep";
  grep.approval.clear();
  grep.status = "invalid";
  log.write(grep);

  const QStringList lines = read(toolLogFile("s-one")).trimmed().split('\n');
  const QJsonObject first = QJsonDocument::fromJson(lines[0].toUtf8()).object();
  const QJsonObject expected{{"time", run.time}, {"session", "s-one"}, {"cwd", "/project"},     {"tool", "bash"},
                             {"input", QJsonObject{{"command", "ls"}}},       {"approval", "allowed"}, {"status", "ok"}, {"durationMs", 12},
                             {"output", "a\nb"}};
  QCOMPARE(first, expected);
  const QJsonObject second = QJsonDocument::fromJson(lines[1].toUtf8()).object();
  QCOMPARE(second.value("output").toString().size(), 10000);
  QCOMPARE(second.value("outputChars").toInt(), 20000);
  const QJsonObject other = QJsonDocument::fromJson(read(toolLogFile("s-two")).toUtf8()).object();
  QCOMPARE(other.value("tool").toString(), QString("grep"));
  QVERIFY(!other.contains("approval"));
  QCOMPARE(mode(toolLogFile("s-one")), QString("600"));
  QCOMPARE(mode(QFileInfo(toolLogFile("s-one")).path()), QString("700"));
}

void TestUnits::preferencesArePrivate() {
  const QString config = m_pData->file("config");
  EnvGuard env("XDG_CONFIG_HOME", config);
  savePreferences(PreferenceChange{"high", {}, {}, {}});
  QCOMPARE(loadPreferences().reasoning, QString("high"));
  QCOMPARE(mode(config + "/smith/preferences.json"), QString("600"));
  QCOMPARE(mode(config + "/smith"), QString("700"));
  QVERIFY2(mode(config) == "755", "only the agent's own folder is made private");
}

void TestUnits::sessionsHaveNames() {
  QCOMPARE(defaultName("Fix the login bug. Also the tests."), QString("Fix the login bug."));
  QCOMPARE(defaultName("Please refactor the whole authentication module so that it uses the new token service"),
           QString("Please refactor the whole authentication module…"));

  auto early = Session::create("/names/a", "m", {});
  // Named before the first message.
  early->rename("auth-rework");
  early->record(HistoryChange{HistoryChange::Type::Append, userMessage("Rework auth"), {}});
  auto late = Session::create("/names/b", "m", {});
  late->record(HistoryChange{HistoryChange::Type::Append, userMessage("Add a version flag. Thanks"), {}});
  late->rename("version flag");
  auto unnamed = Session::create("/names/c", "m", {});
  unnamed->record(HistoryChange{HistoryChange::Type::Append, userMessage("Look into the flaky test"), {}});

  QMap<QString, SessionInfo> byId;
  for (const SessionInfo& info : listSessions()) {
    byId.insert(info.id, info);
  }
  QCOMPARE(byId.value(early->id()).name, QString("auth-rework"));
  QVERIFY(byId.value(early->id()).named);
  QCOMPARE(byId.value(late->id()).name, QString("version flag"));
  QVERIFY(byId.value(late->id()).named);
  QCOMPARE(byId.value(unnamed->id()).name, QString("Look into the flaky test"));
  QVERIFY(!byId.value(unnamed->id()).named);
  QCOMPARE(Session::open(late->id()).session->name(), QString("version flag"));

  QVERIFY2(findSession("AUTH-rework") == early->id(), "names match ignoring case");
  QCOMPARE(findSession(late->id()), late->id());
  Session::create("/names/d", "m", {})->rename("auth-rework");
  auto twin = Session::create("/names/e", "m", {});
  twin->rename("auth-rework");
  twin->record(HistoryChange{HistoryChange::Type::Append, userMessage("x"), {}});
  QVERIFY_THROWS_MATCH(findSession("auth-rework"), "Several sessions are named \"auth-rework\"");
  QVERIFY_THROWS_MATCH(findSession("nope"), "No session with the id or name \"nope\"");
}

void TestUnits::recordsReplacesAndReloads() {
  auto session = Session::create("/proj", "m1", {});
  QVERIFY2(!exists(session->file()), "nothing written until the first change");

  session->record(HistoryChange{HistoryChange::Type::Append, userMessage("hello there"), {}});
  session->record(HistoryChange{HistoryChange::Type::Append, Message{{"role", "assistant"}, {"content", "hi"}}, {}});
  session->record(HistoryChange{HistoryChange::Type::Replace, {}, {userMessage("summary")}});
  session->record(HistoryChange{HistoryChange::Type::Append, Message{{"role", "assistant"}, {"content", "after"}}, {}});
  session->recordModel("m2", "gpu-box");
  // Simulate a crash in the middle of writing a line.
  QFile file(session->file());
  QVERIFY(file.open(QIODevice::Append));
  file.write("{\"type\":\"append\",\"mess");
  file.close();

  const Session::Opened opened = Session::open(session->id());
  QCOMPARE(opened.session->model(), QString("m2"));
  QCOMPARE(opened.session->endpoint(), QString("gpu-box"));
  QCOMPARE(opened.messages, Messages({userMessage("summary"), Message{{"role", "assistant"}, {"content", "after"}}}));

  const QList<SessionInfo> listed = listSessions("/proj");
  QCOMPARE(listed.size(), 1);
  QCOMPARE(listed.first().id, session->id());
  QCOMPARE(listSessions("/elsewhere").size(), 0);
}

void TestUnits::listingsShowFirstMessage() {
  auto make = [](const QString& first, const QStringList& extra) {
    auto session = Session::create("/listing", "m", {});
    session->record(HistoryChange{HistoryChange::Type::Append, userMessage(first), {}});
    for (const QString& text : extra) {
      session->record(HistoryChange{HistoryChange::Type::Append, userMessage(text), {}});
    }
    // Distinct modification times.
    QThread::msleep(15);
    return session->id();
  };
  const QString older = make("Fix   the\nlogin bug", {"and the tests"});
  const QString newer = make(SUMMARY_PREFIX + "summary text\n\n---\n\nAdd a version flag", {});

  const QList<SessionInfo> listed = listSessions("/listing");
  QCOMPARE(listed.size(), 2);
  QCOMPARE(listed[0].id, newer);
  QCOMPARE(listed[0].preview, QString("Add a version flag"));
  QCOMPARE(listed[0].turns, 1);
  QCOMPARE(listed[1].id, older);
  QCOMPARE(listed[1].preview, QString("Fix the login bug"));
  QCOMPARE(listed[1].turns, 2);
  const QList<SessionInfo> limited = listSessions("/listing", 1);
  QCOMPARE(limited.size(), 1);
  QCOMPARE(limited.first().id, newer);
}

void TestUnits::connectionErrorsShowCauseAndHint() {
  // What Qt reports for a certificate it can't verify.
  const QString tls = describeError(ConnectionError("SSL handshake failed: unable to verify the first certificate", "CERT"), "https://x/v1");
  QVERIFY_MATCH(tls, "^Could not reach https://x/v1: SSL handshake failed: unable to verify the first certificate\\.");
  QVERIFY_MATCH(tls, "update-ca-trust");
  QVERIFY_MATCH(describeError(ConnectionError("Connection refused", "ECONNREFUSED"), "u"), "Nothing is listening");
  QVERIFY_MATCH(describeError(ConnectionError("Host gpu-box not found", "ENOTFOUND"), "u"), "doesn't resolve");
  QVERIFY_MATCH(describeError(ConnectionError("Socket operation timed out", "ETIMEDOUT"), "u"), "HTTPS_PROXY");
  QVERIFY_MATCH(describeError(ConnectionError("Connection refused", "ECONNREFUSED"), "u"), "\\(ECONNREFUSED\\)");
}

void TestUnits::connectionErrorWithoutCause() {
  QVERIFY_MATCH(describeError(ConnectionError(""), "u"), "Could not reach u\\. Is the server running\\?");
}

void TestUnits::movesOldFoldersOnce() {
  TempDir base(QDir::tempPath(), "smith-rename-");
  EnvGuard config("XDG_CONFIG_HOME", base.file("config"));
  EnvGuard data("XDG_DATA_HOME", base.file("data"));
  write(base.file("config/agent/config.json"), "{}");
  write(base.file("data/agent/sessions/s.jsonl"), "");

  const QStringList notes = moveOldFolders();
  QCOMPARE(notes.size(), 2);
  QVERIFY_MATCH(notes[0], "agent is now called Smith");
  QVERIFY(exists(configDir() + "/config.json"));
  QVERIFY(exists(dataDir() + "/sessions/s.jsonl"));
  QVERIFY(!exists(base.file("config/agent")));

  // Done once: a later old folder (some other program's) is left alone.
  write(base.file("config/agent/config.json"), "{}");
  QCOMPARE(moveOldFolders(), QStringList());
  QVERIFY(exists(base.file("config/agent/config.json")));
}

void TestUnits::truncateLogKeepsErrorLines() {
  QStringList lines;
  for (int i = 0; i < 2000; i++) {
    lines << QString("compiling module %1 ... ok").arg(i);
  }
  lines[900] = "src/parser.c:41:7: error: 'tokn' undeclared (first use in this function)";
  lines[901] = "   41 |   if (tokn == END) {";
  lines[902] = "      |       ^~~~";
  lines[1200] = "Build summary: 0 errors in the cache step";
  const QString out = truncateLog(lines.join('\n'), 4000);
  QVERIFY2(out.size() <= 4400, qPrintable(QString::number(out.size())));
  QVERIFY_MATCH(out, "^compiling module 0 \\.\\.\\. ok");
  QVERIFY_MATCH(out, "\\[\\.\\.\\. \\d+ lines omitted \\.\\.\\.\\]\\nsrc/parser\\.c:41:7: error: 'tokn' undeclared.*\\n {3}41 \\| {3}if \\(tokn == END\\) "
                     "\\{\\n {6}\\| {7}\\^~~~\\n\\[\\.\\.\\.");
  QVERIFY_NO_MATCH(out, "0 errors in the cache step");
  QVERIFY_MATCH(out, "compiling module 1999 \\.\\.\\. ok$");
  QCOMPARE(truncateLog("short", 100), QString("short"));
}

void TestUnits::promptSaysWhereCommandsRun() {
  PromptExtras extras;
  extras.sandbox = QStringList{"Commands run in a test sandbox."};
  const QString prompt = buildSystemPrompt("/p", extras);
  QVERIFY_MATCH(prompt, "Where commands run:\\n- Commands run in a test sandbox\\.");
  QVERIFY_MATCH(prompt, "find out how this project does it");
  QVERIFY_MATCH(prompt, "larger timeout_ms");
}

void TestUnits::deleteAsksFirst() {
  TempDir data(QDir::tempPath(), "smith-delete-");
  EnvGuard env("XDG_DATA_HOME", data.path());
  auto session = Session::create("/del", "m", {});
  session->record(HistoryChange{HistoryChange::Type::Append, userMessage("fix the login"), {}});
  session->rename("login-fix");

  auto runCli = [&](const QStringList& args) {
    QProcess process;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("XDG_DATA_HOME", data.path());
    env.insert("XDG_CONFIG_HOME", data.file("config"));
    process.setProcessEnvironment(env);
    process.start(SMITH_BINARY, args);
    process.closeWriteChannel();
    process.waitForFinished(30000);
    return std::make_tuple(process.exitCode(), QString::fromUtf8(process.readAllStandardOutput()), QString::fromUtf8(process.readAllStandardError()));
  };

  const auto [refusedCode, refusedOut, refusedErr] = runCli({"--delete", "login-fix"});
  QVERIFY(refusedCode != 0);
  QVERIFY_MATCH(refusedErr, "Not deleting \"login-fix\" .* without asking; pass --yes");
  QVERIFY(exists(session->file()));

  const auto [doneCode, doneOut, doneErr] = runCli({"--delete", "login-fix", "--yes"});
  QVERIFY2(doneCode == 0, qPrintable(doneErr));
  QVERIFY_MATCH(doneOut, "Deleted \"login-fix\" \\(.+\\) in /del: 1 message");
  QVERIFY(!exists(session->file()));
}

void TestUnits::runLaterRecognisedByNameAndPlace() {
  for (const QString rel : {".envrc", "package.json", "web/package.json", ".vscode/tasks.json", ".husky/pre-commit", ".github/workflows/ci.yml", "Makefile",
                             "lib/foo.mk", "CMakeLists.txt", "pyproject.toml", "requirements-dev.txt", "gradle/wrapper/gradle-wrapper.properties",
                             "Dockerfile", "compose.yaml", ".devcontainer/devcontainer.json", ".pre-commit-config.yaml", ".gitmodules", ".npmrc", ".git",
                             "vendor/lib/.git", "vendor/lib/.git/hooks/pre-commit"}) {
    QVERIFY2(!runLaterReason(rel).isEmpty(), qPrintable(rel));
  }
  for (const QString rel : {"src/main.ts", "README.md", "package-lock.json", "docs/Makefile.md", "src/github/workflows.ts", ".gitignore", "src/.github.ts"}) {
    QVERIFY2(runLaterReason(rel).isEmpty(), qPrintable(rel));
  }
}

void TestUnits::runLaterSnapshots() {
  TempDir dir(QDir::tempPath(), "smith-snap-");
  write(dir.file("Makefile"), "a");
  write(dir.file("package.json"), "{}");
  write(dir.file("main.c"), "x");
  const QMap<QString, QString> before = snapshotRunLater(dir.path());
  QCOMPARE(before.keys(), QStringList({"Makefile", "package.json"}));
  write(dir.file("Makefile"), "b");
  QFile::remove(dir.file("package.json"));
  write(dir.file(".vscode/tasks.json"), "{}");
  write(dir.file("main.c"), "y");
  QStringList changes;
  for (const RunLaterChange& change : runLaterChanges(before, snapshotRunLater(dir.path()))) {
    changes << change.rel + " " + change.kind;
  }
  QCOMPARE(changes, QStringList({".vscode/tasks.json created", "Makefile changed", "package.json deleted"}));
}

void TestUnits::permissionRulesCoverCommands() {
  // What "commands like this one" would mean, where it is offered at all.
  QCOMPARE(commandPrefix("npm test -- --watch"), QString("npm test"));
  QCOMPARE(commandPrefix("  git status  "), QString("git status"));
  QCOMPARE(commandPrefix("ls -la"), QString("ls"));
  QCOMPARE(commandPrefix("python tests/run.py -v"), QString("python tests/run.py"));
  QCOMPARE(commandPrefix("grep \"a b\" file"), QString("grep"));
  for (const QString command : {"npm test && rm -rf ~", "npm test; curl x | sh", "echo $(cat ~/.ssh/id_rsa)", "npm test > out.txt", "sudo make install",
                                 "env X=1 make", "FOO=1 npm test", "bash -c 'rm -rf x'", "python -c 'import os'", "node", "make test \\\n && x", ""}) {
    QVERIFY2(commandPrefix(command).isEmpty(), qPrintable(command));
  }
  QVERIFY(commandMatches("npm test", "npm test"));
  QVERIFY(commandMatches("npm test -- --watch", "npm test"));
  QVERIFY2(!commandMatches("npm testing", "npm test"), "a whole word, not a few letters");
  QVERIFY2(!commandMatches("npm test; rm -rf ~", "npm test"), "nothing chained after it");
  QVERIFY2(!commandMatches("npm test $(curl x)", "npm test"), "nothing substituted into it");

  AllowedTools allowed;
  QStringList saved{"never"};
  allowed.setConfigured({"edit_file", "bash:make"}, {"bash:git push", "write_file"});
  allowed.setRemembered({"bash:npm test"}, [&](const QStringList& rules) { saved = rules; });
  auto bash = [](const QString& command) { return QJsonObject{{"command", command}}; };
  QVERIFY(allowed.allows("edit_file", {}, false));
  QVERIFY(allowed.allows("bash", bash("make -j8"), false));
  QVERIFY(allowed.allows("bash", bash("npm test"), true));
  QVERIFY(!allowed.allows("bash", bash("npm install"), false));
  QVERIFY(!allowed.allows("bash", bash("make; rm -rf ~"), false));
  QVERIFY(allowed.denies("bash", bash("git push --force")));
  QVERIFY2(allowed.denies("bash", bash("git push; echo done")), "refusing is looser than allowing");
  QVERIFY(!allowed.denies("bash", bash("git status")));
  QVERIFY(allowed.denies("write_file", {}));

  // All of bash, once allowed, covers any command in a sandbox, and none without one.
  allowed.insert("bash");
  QCOMPARE(saved, QStringList({"bash:npm test", "bash"}));
  QVERIFY(allowed.allows("bash", bash("anything | at all"), false));
  QVERIFY(!allowed.allows("bash", bash("anything | at all"), true));
  QVERIFY(allowed.allows("bash", bash("make test"), true));

  allowed.forget();
  QCOMPARE(saved, QStringList());
  QVERIFY(!allowed.allows("bash", bash("npm test"), false));
  QVERIFY2(allowed.allows("bash", bash("make"), false), "the config file's rules stay");
}

void TestUnits::messagesNameTheirImages() {
  TempDir dir(QDir::tempPath(), "smith-images-");
  auto put = [&](const QString& name, const QByteArray& data) {
    QDir().mkpath(QFileInfo(dir.file(name)).path());
    QFile file(dir.file(name));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(data);
  };
  const QByteArray png("\x89PNG\r\n\x1a\n....", 12);
  put("shot.png", png);
  put("my shots/after fix.JPG", QByteArray("\xFF\xD8\xFF\xE0....", 8));
  put("notes.png", "not really a picture");
  put("big.png", png + QByteArray(6 * 1024 * 1024, 'x'));

  const QString text = QString("Compare shot.png with 'my shots/after fix.JPG' and my\\ shots/after\\ fix.JPG, also file://%1 and notes.png, big.png, "
                               "gone.png, main.c")
                           .arg(dir.file("shot.png"));
  const ImageRefs refs = imagesIn(text, dir.path());
  // Each file once, however it was written; what only looks like an image, or isn't there, is left as text.
  QCOMPARE(refs.images.size(), 2);
  QCOMPARE(refs.images[0].name, QString("shot.png"));
  QCOMPARE(refs.images[0].type, QString("image/png"));
  QCOMPARE(refs.images[0].data, png);
  QCOMPARE(refs.images[1].type, QString("image/jpeg"));
  QCOMPARE(refs.attached, QStringList({"shot.png (1 kB)", "after fix.JPG (1 kB)"}));
  QCOMPARE(refs.problems.size(), 1);
  QVERIFY_MATCH(refs.problems[0], "^big\\.png is 6\\.0 MB; images over 5\\.0 MB aren't attached");
  QVERIFY(imagesIn("no pictures here", dir.path()).images.isEmpty());

  const Message message = userMessage("look", refs.images);
  QVERIFY(hasImages(message));
  QCOMPARE(messageText(message), QString("look"));
  const QJsonArray parts = message.value("content").toArray();
  QCOMPARE(parts.size(), 3);
  QCOMPARE(parts[1].toObject().value("image_url").toObject().value("url").toString(), QString("data:image/png;base64,") + png.toBase64());
  QCOMPARE(withoutImages(message), userMessage("look"));
  QCOMPARE(userMessage("look", {}), userMessage("look"));

  // A picture counts as a picture, not as the text of its bytes.
  const Message heavy = userMessage("look", {Image{"big.png", "image/png", QByteArray(3 * 1024 * 1024, 'x')}});
  QVERIFY(estimateTokens(heavy) < 2000);
  QVERIFY(estimateTokens(Messages{heavy}) < 2000);
  QVERIFY(estimateTokens(heavy) > estimateTokens(userMessage("look")) + 1000);
}

void TestUnits::runLaterSeesGitFolders() {
  TempDir dir(QDir::tempPath(), "smith-snap-git-");
  write(dir.file("main.c"), "x");
  write(dir.file("old/.git/config"), "[core]\n");
  const QMap<QString, QString> before = snapshotRunLater(dir.path());
  QCOMPARE(before.keys(), QStringList({"old/.git"}));

  // A repository made where there was none, one that gained a hook, and a .git file pointing at a folder.
  write(dir.file(".git/config"), "[core]\n\tfsmonitor = ./run-me\n");
  write(dir.file("old/.git/hooks/pre-commit"), "#!/bin/sh\n");
  write(dir.file("deep/er/.git"), "gitdir: ../../elsewhere\n");
  write(dir.file("elsewhere/config"), "[core]\n");
  const QMap<QString, QString> made = snapshotRunLater(dir.path());
  QStringList changes;
  for (const RunLaterChange& change : runLaterChanges(before, made)) {
    changes << change.rel + " " + change.kind;
    QVERIFY_MATCH(change.why, "git runs its hooks");
  }
  QCOMPARE(changes, QStringList({".git created", "deep/er/.git created", "old/.git changed"}));

  // What a .git file points to counts as well.
  write(dir.file("elsewhere/hooks/post-checkout"), "#!/bin/sh\n");
  changes.clear();
  for (const RunLaterChange& change : runLaterChanges(made, snapshotRunLater(dir.path()))) {
    changes << change.rel + " " + change.kind;
  }
  QCOMPARE(changes, QStringList({"deep/er/.git changed"}));
}

#endif

QTEST_GUILESS_MAIN(TestUnits)
#include "test_units.moc"
