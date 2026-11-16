// The desktop app's QML, loaded for real (offscreen) against the fake
// server: it must load without QML warnings, and a whole turn must work
// through the backend the window uses. With SMITH_SHOTS_DIR set, it also
// saves screenshots of each step there.

#include "FakeServer.h"
#include "TestUtil.h"

#include "core/Dirs.h"
#include "core/Preferences.h"
#include "core/Themes.h"
#include "core/Sandbox.h"
#include "Backend.h"
#include "Window.h"

#include <QFont>
#include <QGuiApplication>
#include <functional>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QWheelEvent>

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
  void tooltipsFollowTheTheme();
  void scrollsWithTheWheel();
  void suggestionsStayInTheirBox();
  void longCommandsStayInTheWindow();
  void switchesTheme();
  void plansThenCarriesOut();

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

void TestDesktop::tooltipsFollowTheTheme() {
  // Hovered as you would: the Compact button's tooltip, in the theme's colors and small text.
  auto* window = qobject_cast<QQuickWindow*>(root());
  QQuickItem* compact = nullptr;
  std::function<void(QQuickItem*)> find = [&](QQuickItem* item) {
    if (!compact && item->property("tip").toString().startsWith("Summarize")) {
      compact = item;
    }
    for (QQuickItem* child : item->childItems()) {
      find(child);
    }
  };
  find(window->contentItem());
  QVERIFY(compact);
  QTest::mouseMove(window, compact->mapToScene(QPointF(compact->width() / 2, compact->height() / 2)).toPoint());
  QObject* tip = nullptr;
  QTRY_VERIFY((tip = [&]() -> QObject* {
    for (QObject* candidate : compact->findChildren<QObject*>("smithToolTip")) {
      if (candidate->property("opened").toBool()) {
        return candidate;
      }
    }
    return nullptr;
  }()));
  auto color = [&](const char* name) { return m_pBackend->palette().value(name).value<QColor>(); };
  QCOMPARE(tip->property("background").value<QQuickItem*>()->property("color").value<QColor>(), color("surfaceRaised"));
  QCOMPARE(tip->property("contentItem").value<QQuickItem*>()->property("color").value<QColor>(), color("text"));
  QCOMPARE(tip->property("font").value<QFont>().pixelSize(), 12);
  shot("13-tooltip");
  QTest::mouseMove(window, QPoint(5, window->height() - 5));
  QTRY_VERIFY(!tip->property("visible").toBool());
  QVERIFY2(g_qmlWarnings.isEmpty(), qPrintable(g_qmlWarnings.join('\n')));
}

void TestDesktop::scrollsWithTheWheel() {
  auto* window = qobject_cast<QQuickWindow*>(root());
  auto* view = root()->findChild<QQuickItem*>("transcript");
  QVERIFY(view);
  // More than fits, then at the end, as after a turn.
  for (int i = 0; i < 4; i++) {
    m_pBackend->submit("/help");
  }
  QTest::qWait(200);
  QMetaObject::invokeMethod(view, "toEnd");
  QTest::qWait(200);
  const qreal before = view->property("contentY").toReal();

  // Turns of the wheel over the conversation, each later than the last as a real one is.
  const QPointF at = view->mapToScene(QPointF(view->width() / 2, view->height() / 2));
  static ulong timestamp = 1000;
  auto turn = [&](int notches) {
    for (int i = 0; i < std::abs(notches); i++) {
      QWheelEvent wheel(at, window->mapToGlobal(at.toPoint()), QPoint(), QPoint(0, notches > 0 ? 120 : -120), Qt::NoButton, Qt::NoModifier,
                        Qt::NoScrollPhase, false);
      wheel.setTimestamp(timestamp += 100);
      QCoreApplication::sendEvent(window, &wheel);
      QTest::qWait(20);
    }
  };
  auto atEnd = [&]() { return view->property("atYEnd").toBool(); };
  turn(5);
  QTRY_VERIFY2(view->property("contentY").toReal() < before - 50, "the wheel scrolls the conversation");
  // Scrolled up, new output doesn't pull it back down.
  QTRY_VERIFY(!view->property("follow").toBool());
  // Back down to the end, it follows again.
  for (int i = 0; i < 40 && !atEnd(); i++) {
    turn(-1);
  }
  QTRY_VERIFY(atEnd());
  QTRY_VERIFY(view->property("follow").toBool());
  QVERIFY2(g_qmlWarnings.isEmpty(), qPrintable(g_qmlWarnings.join('\n')));
}

void TestDesktop::suggestionsStayInTheirBox() {
  auto* window = qobject_cast<QQuickWindow*>(root());
  auto* input = root()->findChild<QQuickItem*>("composerInput");
  auto* box = root()->findChild<QQuickItem*>("suggestions");
  QVERIFY(input && box);
  window->requestActivate();
  input->forceActiveFocus();
  input->setProperty("text", "/");
  QTRY_VERIFY(box->isVisible());
  // Long hints (/worktree's) are cut short inside the box, not drawn past it and the window's edge.
  QList<QQuickItem*> texts;
  std::function<void(QQuickItem*)> collect = [&](QQuickItem* item) {
    if (QString(item->metaObject()->className()).startsWith("QQuickText") && item->isVisible() && !item->property("text").toString().isEmpty()) {
      texts << item;
    }
    for (QQuickItem* child : item->childItems()) {
      collect(child);
    }
  };
  // Every row, once the list has laid them out.
  QTRY_VERIFY((texts.clear(), collect(box), texts.size() > 10));
  for (QQuickItem* text : texts) {
    const QRectF inBox = text->mapRectToItem(box, QRectF(0, 0, text->width(), text->height()));
    QVERIFY2(inBox.right() <= box->width(), qPrintable(text->property("text").toString()));
  }
  shot("16-suggestions");
  input->setProperty("text", "");
  QTRY_VERIFY(!box->isVisible());
  QVERIFY2(g_qmlWarnings.isEmpty(), qPrintable(g_qmlWarnings.join('\n')));
}

void TestDesktop::longCommandsStayInTheWindow() {
  // A command long enough that offering to allow commands like it names a long rule.
  const QString command = "ls /tmp/" + QString(400, 'x');
  // The answer repeats it in a code block, as models do, with a long line of words too.
  const QString answer = "It ran:\n\n```bash\n" + command + "\necho" + QString(" word").repeated(120) + "\n```\n\nDone.";
  m_pFake->setScript({FakeServer::stream({FakeServer::callDelta(0, "b9", "bash", {{"command", command}})}, "tool_calls"), FakeServer::text(answer)});
  m_pBackend->submit("list it");
  QTRY_VERIFY_WITH_TIMEOUT(!m_pBackend->approval().isEmpty(), 15000);
  QVERIFY(m_pBackend->approval().value("similar").toString().size() > 300);
  auto* window = qobject_cast<QQuickWindow*>(root());
  auto* panel = root()->findChild<QQuickItem*>("approvalPanel");
  QVERIFY(panel);
  QTRY_VERIFY(panel->isVisible() && panel->width() > 0);
  QTest::qWait(100);
  // Everything in the panel, buttons included, inside the window.
  std::function<void(QQuickItem*)> check = [&](QQuickItem* item) {
    if (item->isVisible() && item->width() > 0) {
      const QRectF inWindow = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
      QVERIFY2(inWindow.right() <= window->width() + 0.5, qPrintable(QString("%1 reaches %2").arg(item->metaObject()->className()).arg(inWindow.right())));
      for (QQuickItem* child : item->childItems()) {
        check(child);
      }
    }
  };
  check(panel);
  shot("14-long-command");
  // Allowed, it runs and its card in the conversation keeps the command and output inside too.
  m_pBackend->decide("allow");
  QTRY_VERIFY_WITH_TIMEOUT(!m_pBackend->busy(), 15000);
  auto* view = root()->findChild<QQuickItem*>("transcript");
  QMetaObject::invokeMethod(view, "toEnd");
  QTest::qWait(300);
  QQuickItem* card = nullptr;
  std::function<void(QQuickItem*)> findCard = [&](QQuickItem* item) {
    if (item->property("name").toString() == "bash" && item->property("summary").toString().contains(QString(300, 'x'))) {
      card = item;
    }
    for (QQuickItem* child : item->childItems()) {
      findCard(child);
    }
  };
  findCard(view);
  QVERIFY(card);
  check(card);
  // The answer's code block wraps inside the answer instead of running past it.
  QQuickItem* answerText = nullptr;
  std::function<void(QQuickItem*)> findAnswer = [&](QQuickItem* item) {
    if (QString(item->metaObject()->className()).contains("TextEdit") && item->property("text").toString().contains("word word")) {
      answerText = item;
    }
    for (QQuickItem* child : item->childItems()) {
      findAnswer(child);
    }
  };
  QTRY_VERIFY((findAnswer(view), answerText));
  QVERIFY2(answerText->property("contentWidth").toReal() <= answerText->width() + 1,
           qPrintable(QString("drawn %1 wide in %2").arg(answerText->property("contentWidth").toReal()).arg(answerText->width())));
  shot("15-long-command-ran");
  QVERIFY2(g_qmlWarnings.isEmpty(), qPrintable(g_qmlWarnings.join('\n')));
}

void TestDesktop::switchesTheme() {
  const QString before = m_pBackend->theme();
  // From another theme, when the run is already light (SMITH_TEST_THEME=light).
  if (before == "light") {
    m_pBackend->setTheme("plain");
  }
  auto color = [&](const char* name) { return m_pBackend->palette().value(name).value<QColor>(); };
  // Theme files set colors by the names the window uses.
  QStringList names = m_pBackend->palette().keys();
  names.removeAll("monoFamily");
  names.removeAll("textFamily");
  QStringList known = DESKTOP_COLORS;
  names.sort();
  known.sort();
  QCOMPARE(names, known);
  // The ⋯ menu's items are drawn in the theme, not the style's default look.
  QObject* menu = root()->findChild<QObject*>("moreMenu");
  QVERIFY(menu);
  QMetaObject::invokeMethod(menu, "open");
  QTRY_VERIFY(menu->property("opened").toBool());
  auto item = [&](int index) {
    QQuickItem* found = nullptr;
    QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem*, found), Q_ARG(int, index));
    return found;
  };
  QCOMPARE(menu->property("count").toInt(), 4);
  QCOMPARE(item(0)->property("text").toString(), QString("Theme…"));
  auto textColor = [&](int index) { return item(index)->property("contentItem").value<QQuickItem*>()->property("color").value<QColor>(); };
  QCOMPARE(textColor(1), color("text"));
  shot("17-menu");
  QMetaObject::invokeMethod(menu, "close");

  // /theme without a name opens the picker; with one it switches now, and is remembered.
  m_pBackend->submit("/theme");
  QTRY_COMPARE(root()->property("pickerKind").toString(), QString("theme"));
  QObject* picker = root()->findChild<QObject*>("picker");
  QVERIFY(picker);
  QTRY_VERIFY(picker->property("opened").toBool());
  QMetaObject::invokeMethod(picker, "close");
  m_pBackend->submit("/theme light");
  QCOMPARE(m_pBackend->theme(), QString("light"));
  QCOMPARE(color("window"), QColor("#f6f7f9"));
  QTRY_COMPARE(root()->property("color").value<QColor>(), color("window"));
  QCOMPARE(textColor(1), color("text"));
  QCOMPARE(loadPreferences().theme, QString("light"));
  QTRY_VERIFY(!picker->property("visible").toBool());
  shot("18-light");

  // A theme file of yours: listed in the picker, and its colors over its base's.
  write(m_pHome->file("config/smith/themes/night.json"), R"({"description": "Easy at night", "base": "plain", "colors": {"accent": "#ff8800"}})");
  bool listed = false;
  for (const QVariant& choice : m_pBackend->themeChoices()) {
    listed = listed || (choice.toMap().value("value") == "night" && choice.toMap().value("hint") == "Easy at night");
  }
  QVERIFY(listed);
  m_pBackend->submit("/theme night");
  QCOMPARE(m_pBackend->theme(), QString("night"));
  QCOMPARE(color("accent"), QColor("#ff8800"));
  QCOMPARE(color("window"), QColor("#17181b"));
  QVERIFY(!m_pBackend->matrix());
  // Edited, then picked again: the new colors.
  write(m_pHome->file("config/smith/themes/night.json"), R"({"base": "plain", "colors": {"accent": "#00aa55"}})");
  m_pBackend->setTheme("night");
  QCOMPARE(color("accent"), QColor("#00aa55"));

  m_pBackend->setTheme(before);
  QCOMPARE(m_pBackend->theme(), before);
  QVERIFY2(g_qmlWarnings.isEmpty(), qPrintable(g_qmlWarnings.join('\n')));
}

void TestDesktop::plansThenCarriesOut() {
  using R = FakeServer;
  const QString readme = m_pHome->file("hello-app/README.md");
  m_pFake->setScript({
      R::stream(R::toolCallDeltas("write_file", {{"path", "README.md"}, {"content", "# Hello\n"}}, "w1"), "tool_calls"),
      R::text("## Plan\n\n1. Add a README.md that says how to run greet.py."),
      R::stream(R::toolCallDeltas("write_file", {{"path", "README.md"}, {"content", "# Hello\n\nRun `python3 greet.py <name>`.\n"}}, "w2"), "tool_calls"),
      R::text("Added README.md."),
  });
  m_pFake->clearRequests();

  // The switch in the top bar; the input box says so.
  QObject* toggle = root()->findChild<QObject*>("planToggle");
  QVERIFY(toggle);
  QMetaObject::invokeMethod(toggle, "toggled");
  QVERIFY(m_pBackend->planning());
  QTRY_VERIFY(toggle->property("on").toBool());
  QCOMPARE(root()->findChild<QObject*>("composerInput")->property("placeholderText").toString().left(15), QString("Ask for a plan:"));

  m_pBackend->submit("add a readme");
  QTRY_VERIFY_WITH_TIMEOUT(!m_pBackend->busy(), 15000);
  // The write was refused without asking, and the plan offered.
  QVERIFY(!QFile::exists(readme));
  QTRY_COMPARE(root()->property("pickerKind").toString(), QString("plan"));
  QObject* picker = root()->findChild<QObject*>("picker");
  QTRY_VERIFY(picker->property("opened").toBool());
  QVERIFY_MATCH(m_pBackend->usage(), "^\\d+ in · \\d+ out");
  QVERIFY_MATCH(m_pBackend->usageDetail(), "^The last turn: 2 model calls, 200 prompt tokens sent .* and 20 generated");
  shot("19-plan-ready");

  // Carry it out: plan mode off, and the agent starts.
  // The first choice, as enter picks it.
  QMetaObject::invokeMethod(picker, "choose", Q_ARG(QVariant, 0));
  QTRY_VERIFY(!picker->property("visible").toBool());
  QVERIFY(!m_pBackend->planning());
  QTRY_VERIFY_WITH_TIMEOUT(!m_pBackend->approval().isEmpty(), 15000);
  m_pBackend->decide("allow");
  QTRY_VERIFY_WITH_TIMEOUT(!m_pBackend->busy(), 15000);
  QVERIFY(QFile::exists(readme));
  const QJsonArray sent = m_pFake->requests().value(2).value("messages").toArray();
  QVERIFY_MATCH(messageText(sent.last().toObject()), "^Go ahead with the plan\\.$");

  // Changes: each file with all of its diff.
  auto* model = m_pBackend->transcript();
  const int before = model->rowCount();
  QMetaObject::invokeMethod(root()->findChild<QObject*>("changesButton"), "clicked");
  QVERIFY(model->rowCount() > before + 1);
  bool listed = false;
  for (int i = before; i < model->rowCount(); i++) {
    const QModelIndex index = model->index(i);
    if (model->data(index, Desktop::TranscriptModel::SummaryRole).toString() == "README.md") {
      listed = model->data(index, Desktop::TranscriptModel::NameRole).toString() == "created" && model->data(index, Desktop::TranscriptModel::FullDiffRole).toBool();
    }
  }
  QVERIFY(listed);
  QVERIFY2(g_qmlWarnings.isEmpty(), qPrintable(g_qmlWarnings.join('\n')));
  shot("20-changes");
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
