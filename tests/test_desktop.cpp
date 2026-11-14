// The desktop app's QML, loaded for real (offscreen) against the fake
// server: it must load without QML warnings, and a whole turn must work
// through the backend the window uses. With SMITH_SHOTS_DIR set, it also
// saves screenshots of each step there.

#include "FakeServer.h"
#include "TestUtil.h"

#include "core/Sandbox.h"
#include "Backend.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>

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

class TestDesktop : public QObject {
  Q_OBJECT

private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();
  void loadsWithoutWarnings();
  void runsAWholeTurn();
  void asksAQuestion();
  void switchesSessions();

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

  QQuickStyle::setStyle("Basic");
  m_pBackend = std::make_unique<Desktop::Backend>(launch);
  Desktop::Backend::setInstance(m_pBackend.get());
  m_pEngine = std::make_unique<QQmlApplicationEngine>();
  m_pEngine->loadFromModule("Smith.Desktop", "Main");
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
  window->grabWindow().save(m_shots + "/" + name + ".png");
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

#endif

int main(int argc, char** argv) {
  // A window without a display, painted in software so it can be grabbed.
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
  QGuiApplication app(argc, argv);
  TestDesktop test;
  return QTest::qExec(&test, argc, argv);
}

#include "test_desktop.moc"
