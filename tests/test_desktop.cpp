// The desktop app's QML, loaded for real (offscreen) against the fake
// server: it must load without QML warnings, and a whole turn must work
// through the backend the window uses. With SMITH_SHOTS_DIR set, it also
// saves screenshots of each step there.

#include "FakeServer.h"
#include "TestUtil.h"

#include "core/Dirs.h"
#include "core/Sandbox.h"
#include "Backend.h"
#include "Window.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QSignalSpy>

using namespace Smith;
using namespace TestUtil;

namespace {

QStringList g_qmlWarnings;

void collectQmlWarnings(QtMsgType type, const QMessageLogContext& context, const QString& message) {
  if (type != QtDebugMsg && type != QtInfoMsg && (message.contains("qrc:") || message.contains(".qml"))) {
    g_qmlWarnings << message;
  }
  std::fprintf(stderr, "%s\n", qPrintable(message));
}

const QString GREET_PY = "import sys\n\n\ndef greet(name):\n    return \"Hello, world!\"\n\n\nif __name__ == \"__main__\":\n"
                         "    print(greet(sys.argv[1] if len(sys.argv) > 1 else \"world\"))\n";

QJsonArray todos(const QStringList& states) {
  const QStringList steps = {"Find where the greeting is built", "Fix the missing name", "Run the script to check"};
  QJsonArray out;
  for (int i = 0; i < steps.size(); i++) {
    out.append(QJsonObject{{"content", steps[i]}, {"status", states[i]}});
  }
  return out;
}

} // namespace

// An item by its objectName, among the visual children of `from` (a view's delegates aren't its QObject children).
QQuickItem* findItem(QQuickItem* from, const QString& name) {
  if (from->objectName() == name) {
    return from;
  }
  for (QQuickItem* child : from->childItems()) {
    if (QQuickItem* found = findItem(child, name)) {
      return found;
    }
  }
  return nullptr;
}

class TestDesktop : public QObject {
  Q_OBJECT

private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();
  void loadsWithoutWarnings();
  void runsAWholeTurn();
  void asksAQuestion();
  void switchesSessions();
  void deletesAnotherSession();
  void landsATaskWorktree();

private:
  void shot(const QString& name);
  QObject* root() const { return m_pEngine->rootObjects().value(0); }

  std::unique_ptr<FakeServer> m_pFake;
  std::unique_ptr<TempDir> m_pHome;
  std::unique_ptr<EnvGuard> m_pConfig;
  std::unique_ptr<EnvGuard> m_pData;
  std::unique_ptr<Desktop::Backend> m_pBackend;
  std::unique_ptr<QQmlApplicationEngine> m_pEngine;
  QString m_shots;
};

#ifndef Q_MOC_RUN

void TestDesktop::initTestCase() {
  qInstallMessageHandler(collectQmlWarnings);
  m_shots = qEnvironmentVariable("SMITH_SHOTS_DIR");
  m_pFake = std::make_unique<FakeServer>();
  m_pFake->setModels(QStringList{"qwen3-coder-30b", "gpt-oss-20b", "qwen3-small"});
  m_pHome = std::make_unique<TempDir>(QDir::tempPath(), "smith-desktop-");
  m_pConfig = std::make_unique<EnvGuard>("XDG_CONFIG_HOME", m_pHome->file("config"));
  m_pData = std::make_unique<EnvGuard>("XDG_DATA_HOME", m_pHome->file("data"));
  // SMITH_TEST_THEME=plain checks the other theme.
  const QString theme = qEnvironmentVariable("SMITH_TEST_THEME", "matrix");
  write(m_pHome->file("config/smith/config.json"),
        QString("{\"default\": \"local\", \"theme\": \"%2\", \"endpoints\": {\"local\": {\"baseURL\": \"%1\", \"model\": \"qwen3-coder-30b\"}}}")
            .arg(m_pFake->baseUrl(), theme));
  write(m_pHome->file("hello-app/greet.py"), GREET_PY);
  write(m_pHome->file("hello-app/AGENTS.md"), "Run with `python3 greet.py <name>`.\n");

  LaunchOptions launch;
  launch.folder = m_pHome->file("hello-app");
  // The real sandbox where it runs; otherwise none, so the test still runs.
  try {
    Sandbox::create(SandboxOptions{})->dispose();
  } catch (const SmithError&) {
    launch.sandbox = "off";
  }

  m_pBackend = std::make_unique<Desktop::Backend>(launch);
  Desktop::Backend::setInstance(m_pBackend.get());
  m_pEngine = std::make_unique<QQmlApplicationEngine>();
  Desktop::loadWindow(*m_pEngine);
  QVERIFY(root());
  if (!m_shots.isEmpty()) {
    QDir().mkpath(m_shots);
    // Mid-rain: the splash while it settles into the title.
    QTest::qWait(900);
    shot("01-splash");
  }
  m_pBackend->start();
  QTRY_COMPARE_WITH_TIMEOUT(m_pBackend->state(), QString("ready"), 20000);
  // Skip the rest of the rain.
  root()->findChild<QObject*>("splash")->setProperty("rainDone", true);
  QTest::qWait(500);
}

void TestDesktop::cleanupTestCase() {
  m_pEngine.reset();
  m_pBackend.reset();
  qInstallMessageHandler(nullptr);
}

void TestDesktop::shot(const QString& name) {
  if (m_shots.isEmpty()) {
    return;
  }
  auto* window = qobject_cast<QQuickWindow*>(root());
  QTest::qWait(250);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  window->grabWindow().save(m_shots + "/" + name + ".png");
#else
  // Qt 5 can't grab an offscreen window; its content item (popups included) can be.
  const QSharedPointer<QQuickItemGrabResult> grab = window->contentItem()->grabToImage();
  QSignalSpy ready(grab.data(), &QQuickItemGrabResult::ready);
  if (ready.wait(5000)) {
    grab->image().save(m_shots + "/" + name + ".png");
  }
#endif
}

void TestDesktop::loadsWithoutWarnings() {
  QVERIFY2(g_qmlWarnings.isEmpty(), qPrintable(g_qmlWarnings.join('\n')));
  // The banner opens the transcript.
  QVERIFY(m_pBackend->transcript()->rowCount() >= 1);
  QCOMPARE(m_pBackend->transcript()->data(m_pBackend->transcript()->index(0), Desktop::TranscriptModel::KindRole).toString(), QString("banner"));
  QCOMPARE(m_pBackend->model(), QString("qwen3-coder-30b"));
  QTRY_COMPARE(m_pBackend->models().size(), 3);
  shot("02-ready");
}

void TestDesktop::runsAWholeTurn() {
  using R = FakeServer;
  QList<QJsonObject> plan{QJsonObject{{"reasoning_content", "The greeting ignores its argument. Plan, then read greet.py."}}};
  plan << R::toolCallDeltas("todo_write", {{"todos", todos({"in_progress", "pending", "pending"})}}, "t1");
  QList<QJsonObject> read{QJsonObject{{"content", "Let me look at the script."}}};
  read << R::toolCallDeltas("read_file", {{"path", "greet.py"}}, "r1");
  m_pFake->setScript({
      R::stream(plan, "tool_calls"),
      R::stream(read, "tool_calls"),
      R::stream({R::callDelta(0, "t2", "todo_write", {{"todos", todos({"completed", "in_progress", "pending"})}}),
                 R::callDelta(1, "e1", "edit_file",
                              {{"path", "greet.py"}, {"old_string", "    return \"Hello, world!\""}, {"new_string", "    return f\"Hello, {name}!\""}})},
                "tool_calls"),
      R::stream({R::callDelta(0, "t3", "todo_write", {{"todos", todos({"completed", "completed", "in_progress"})}}),
                 R::callDelta(1, "b1", "bash", {{"command", "python3 greet.py Anderson"}})},
                "tool_calls"),
      R::stream({R::callDelta(0, "t4", "todo_write", {{"todos", todos({"completed", "completed", "completed"})}})}, "tool_calls"),
      R::text("Fixed. `greet()` ignored its `name` argument and always returned **Hello, world!**.\n\n## What changed\n\n"
              "- `greet.py`: the greeting is now built from `name`\n- Checked it by running the script:\n\n"
              "```\n$ python3 greet.py Anderson\nHello, Anderson!\n```\n\nNothing else uses `greet()`, so no other changes were needed."),
  });

  m_pBackend->submit("fix the greeting bug: it always says Hello, world!");
  QVERIFY(m_pBackend->busy());
  QTRY_VERIFY_WITH_TIMEOUT(!m_pBackend->approval().isEmpty(), 15000);
  QCOMPARE(m_pBackend->approval().value("tool").toString(), QString("edit_file"));
  QVERIFY(!m_pBackend->approval().value("diff").toList().isEmpty());
  shot("03-approve-edit");
  m_pBackend->decide("allow");

  QTRY_VERIFY_WITH_TIMEOUT(!m_pBackend->approval().isEmpty(), 15000);
  QCOMPARE(m_pBackend->approval().value("tool").toString(), QString("bash"));
  QVERIFY(m_pBackend->approval().value("hasSandbox").toBool());
  shot("04-approve-bash");
  m_pBackend->decide("allow");

  QTRY_VERIFY_WITH_TIMEOUT(!m_pBackend->busy(), 15000);
  QCOMPARE(TestUtil::read(m_pHome->file("hello-app/greet.py")).contains("f\"Hello, {name}!\""), true);
  QStringList kinds;
  QString last;
  auto* model = m_pBackend->transcript();
  for (int i = 0; i < model->rowCount(); i++) {
    kinds << model->data(model->index(i), Desktop::TranscriptModel::KindRole).toString();
    last = model->data(model->index(i), Desktop::TranscriptModel::TextRole).toString();
  }
  QVERIFY2(kinds.contains("reasoning") && kinds.contains("todos") && kinds.contains("tool") && kinds.contains("text"), qPrintable(kinds.join(',')));
  QVERIFY_MATCH(last, "^Done in .* · 7 tool calls$");
  // The command really ran.
  bool ran = false;
  for (int i = 0; i < model->rowCount(); i++) {
    ran = ran || model->data(model->index(i), Desktop::TranscriptModel::OutputRole).toString() == "Hello, Anderson!";
  }
  QVERIFY(ran);
  QVERIFY(m_pBackend->canUndo());
  QVERIFY2(g_qmlWarnings.isEmpty(), qPrintable(g_qmlWarnings.join('\n')));
  shot("05-done");

  // The model picker, as ctrl+k or the model chip opens it.
  QMetaObject::invokeMethod(root(), "openPicker", Q_ARG(QVariant, QVariant("model")));
  shot("06-model-picker");
  QTest::keyClick(qobject_cast<QQuickWindow*>(root()), Qt::Key_Escape);
}

void TestDesktop::asksAQuestion() {
  m_pFake->setScript({
      FakeServer::stream({FakeServer::callDelta(0, "q1", "ask_user",
                                                {{"question", "Which database should the app use?"},
                                                 {"options", QJsonArray{"PostgreSQL", "SQLite", "Keep it in memory for now"}}})},
                         "tool_calls"),
      FakeServer::text("Going with **SQLite**: a single file, nothing to install."),
  });
  m_pBackend->submit("set up a database");
  QTRY_VERIFY_WITH_TIMEOUT(!m_pBackend->question().isEmpty(), 15000);
  QCOMPARE(m_pBackend->question().value("options").toStringList().size(), 3);
  shot("07-question");
  m_pBackend->answerOption(1);
  QTRY_VERIFY_WITH_TIMEOUT(!m_pBackend->busy(), 15000);
  const QJsonArray sent = m_pFake->requests().last().value("messages").toArray();
  QCOMPARE(sent[sent.size() - 1].toObject().value("content").toString(), QString("The user chose: SQLite"));
  shot("08-answered");
}

void TestDesktop::switchesSessions() {
  const QString first = m_pBackend->sessionId();
  QTRY_VERIFY(!m_pBackend->sessions().isEmpty());
  m_pBackend->newSession();
  QVERIFY(m_pBackend->sessionId() != first);
  QCOMPARE(m_pBackend->transcript()->rowCount(), 2);
  m_pBackend->openSession(first);
  QCOMPARE(m_pBackend->sessionId(), first);
  // The resumed conversation is replayed: its messages, tools and answers.
  QVERIFY(m_pBackend->transcript()->rowCount() > 10);
  QVERIFY2(g_qmlWarnings.isEmpty(), qPrintable(g_qmlWarnings.join('\n')));
}

void TestDesktop::deletesAnotherSession() {
  // Clicked as you would: hover a session that isn't the current one, then its ✕.
  const QString other = m_pBackend->sessionId();
  m_pBackend->newSession();
  const QString current = m_pBackend->sessionId();
  auto listed = [&](const QString& id) {
    for (const QVariant& session : m_pBackend->sessions()) {
      if (session.toMap().value("id").toString() == id) {
        return true;
      }
    }
    return false;
  };
  QTRY_VERIFY(listed(other));
  auto* window = qobject_cast<QQuickWindow*>(root());
  QQuickItem* row = nullptr;
  QTRY_VERIFY((row = findItem(window->contentItem(), "session:" + other)));
  QQuickItem* remove = findItem(row, "removeSession");
  QVERIFY(remove);
  QTest::mouseMove(window, row->mapToScene(QPointF(row->width() / 2, row->height() / 2)).toPoint());
  QTRY_VERIFY(remove->isVisible());
  const QPoint at = remove->mapToScene(QPointF(remove->width() / 2, remove->height() / 2)).toPoint();
  QTest::mouseMove(window, at);
  QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, at);

  QObject* confirm = root()->findChild<QObject*>("deleteConfirm");
  QVERIFY(confirm);
  QTRY_VERIFY(confirm->property("visible").toBool());
  QCOMPARE(m_pBackend->sessionId(), current);
  QMetaObject::invokeMethod(root()->findChild<QObject*>("confirmDelete"), "clicked");
  QTRY_VERIFY(!listed(other));
  QCOMPARE(m_pBackend->sessionId(), current);
  QTRY_VERIFY(!confirm->property("visible").toBool());
  QVERIFY2(g_qmlWarnings.isEmpty(), qPrintable(g_qmlWarnings.join('\n')));
}

void TestDesktop::landsATaskWorktree() {
  if (!m_pBackend->sandboxActive()) {
    QSKIP("task worktrees need the sandbox, and bwrap can't run here");
  }
  IsolatedGit isolated;
  const QString repo = m_pHome->file("hello-app");
  git(repo, {"init", "-q", "-b", "main"});
  git(repo, {"add", "-A"});
  git(repo, {"commit", "-q", "-m", "Start"});
  // Your own work in progress, which landing must leave alone.
  write(repo + "/NOTES.md", "my own work\n");
  m_pBackend->refreshWorktrees();
  QTRY_VERIFY(m_pBackend->inRepository());

  m_pBackend->createWorktree("shout");
  QTRY_COMPARE_WITH_TIMEOUT(m_pBackend->worktreeBranch(), QString("smith/shout"), 15000);
  const QString path = realPath(m_pHome->file("hello-app-worktrees/shout"));
  QCOMPARE(realPath(m_pBackend->projectPath()), path);
  QTRY_COMPARE(m_pBackend->worktrees().size(), 2);
  QVERIFY(m_pBackend->worktrees().at(1).toMap().value("current").toBool());

  // The agent's work, as its edits leave it.
  write(path + "/greet.py", TestUtil::read(path + "/greet.py").replace("Hello", "HELLO"));
  m_pBackend->refreshWorktrees();
  QTRY_VERIFY(m_pBackend->worktrees().value(1).toMap().value("summary").toString().startsWith("1 file changed"));
  shot("09-worktree");

  m_pFake->setScript({FakeServer::plain("Shout the greeting")});
  m_pBackend->reviewLanding(path);
  QTRY_COMPARE_WITH_TIMEOUT(m_pBackend->landing().value("stage").toString(), QString("review"), 15000);
  QCOMPARE(m_pBackend->landing().value("files").toList().size(), 1);
  QCOMPARE(m_pBackend->landing().value("target").toString(), QString("main"));
  QObject* panel = root()->findChild<QObject*>("landPanel");
  QObject* message = root()->findChild<QObject*>("commitMessage");
  QVERIFY(panel && message);
  QTRY_VERIFY(panel->property("visible").toBool());
  QCOMPARE(message->property("text").toString(), QString("Shout the greeting"));
  shot("10-land-review");

  QMetaObject::invokeMethod(root()->findChild<QObject*>("cherryPick"), "clicked");
  QTRY_COMPARE_WITH_TIMEOUT(m_pBackend->landing().value("stage").toString(), QString("landed"), 15000);
  QCOMPARE(git(repo, {"log", "-1", "--format=%s"}).trimmed(), QString("Shout the greeting"));
  QVERIFY(TestUtil::read(repo + "/greet.py").contains("HELLO"));
  QCOMPARE(TestUtil::read(repo + "/NOTES.md"), QString("my own work\n"));
  shot("11-landed");

  QMetaObject::invokeMethod(root()->findChild<QObject*>("removeWorktree"), "clicked");
  QTRY_VERIFY_WITH_TIMEOUT(!exists(path), 15000);
  QCOMPARE(realPath(m_pBackend->projectPath()), realPath(repo));
  QVERIFY(m_pBackend->worktreeBranch().isEmpty());
  QTRY_COMPARE(m_pBackend->worktrees().size(), 1);
  QVERIFY(m_pBackend->landing().isEmpty());
  QTRY_VERIFY(!panel->property("visible").toBool());
  QVERIFY(git(repo, {"branch", "--list", "smith/shout"}).trimmed().isEmpty());
  QVERIFY2(g_qmlWarnings.isEmpty(), qPrintable(g_qmlWarnings.join('\n')));
  shot("12-removed");
}

#endif

int main(int argc, char** argv) {
  // A window without a display, painted in software so it can be grabbed.
  qputenv("QT_QPA_PLATFORM", "offscreen");
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
#else
  QQuickWindow::setSceneGraphBackend(QSGRendererInterface::Software);
#endif
  QGuiApplication app(argc, argv);
  TestDesktop test;
  return QTest::qExec(&test, argc, argv);
}

#include "test_desktop.moc"
