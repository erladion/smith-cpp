// The Controller both apps drive: messages and turns, the queues, plan mode,
// commands and sessions, against the fake server, with no UI at all.

#include "FakeServer.h"
#include "TestUtil.h"

#include "core/Sandbox.h"
#include "core/tools/Tools.h"
#include "frontend/Controller.h"

using namespace Smith;
using namespace TestUtil;

class TestController : public QObject {
  Q_OBJECT

private Q_SLOTS:
  void initTestCase();
  void init();
  void cleanup();

  void startsWithTheBanner();
  void messagesRunTurns();
  void busyRefusesCommandsAndQueuesMessages();
  void queuedTasksFollowInOrder();
  void planModeOffersThePlan();
  void commandsReplyOrAskForPickers();
  void appCommandsComeFirst();
  void newSessionStartsOver();

private:
  // What the controller reported, in order.
  struct Seen {
    QList<Entry> entries;
    QList<QList<Entry>> startedOver;
    QStringList pickers;
    QStringList restored;
    QStringList appended;
  };

  void make();
  QStringList texts() const;
  bool idle() const { return !m_pController->turns().busy(); }

  std::unique_ptr<FakeServer> m_pFake;
  std::unique_ptr<TempDir> m_pDir;
  std::unique_ptr<Controller> m_pController;
  Seen m_seen;
  QStringList m_started;
  QStringList m_renamed;
};

#ifndef Q_MOC_RUN

void TestController::initTestCase() {
  m_pFake = std::make_unique<FakeServer>();
  m_pDir = std::make_unique<TempDir>(QDir::tempPath(), "smith-controller-");
}

void TestController::init() {
  m_pFake->clearRequests();
  m_pFake->setScript({});
  m_seen = {};
  m_started.clear();
  m_renamed.clear();
  make();
}

void TestController::cleanup() {
  m_pController.reset();
}

void TestController::make() {
  ControllerOptions options;
  const ServerPtr server = std::make_shared<Server>(m_pFake->baseUrl(), "x");
  const QString cwd = m_pDir->path();
  options.createAgent = [server, cwd](const AgentHooks& hooks, const std::optional<AgentSettings>&) {
    AgentOptions agent;
    agent.server = server;
    agent.model = "fake";
    agent.cwd = cwd;
    agent.tools = defaultTools();
    agent.systemPrompt = "test";
    agent.onEvent = hooks.onEvent;
    agent.approve = hooks.approve;
    agent.ask = hooks.ask;
    return std::make_shared<Agent>(agent);
  };
  options.project.cwd = cwd;
  options.server = server;
  options.sandbox = Sandbox::none();
  options.savePreferences = [](const PreferenceUpdate&) {};
  options.sessions.currentId = []() { return QString("s1"); };
  options.sessions.currentName = []() { return QString(); };
  options.sessions.list = [](bool) { return QList<SessionInfo>(); };
  options.sessions.startNew = [this](const QString& model, const QString&) { m_started << model; };
  options.sessions.rename = [this](const QString& name) { m_renamed << name; };
  options.theme.name = "plain";
  options.theme.base = Theme::Plain;

  m_pController = std::make_unique<Controller>();
  Controller* controller = m_pController.get();
  connect(controller, &Controller::entryAdded, this, [this](const Entry& entry) { m_seen.entries << entry; });
  connect(controller, &Controller::agentEvent, this, [this](const AgentEvent&, const std::optional<Entry>& entry) {
    if (entry) {
      m_seen.entries << *entry;
    }
  });
  connect(controller, &Controller::startedOver, this, [this](const QList<Entry>& entries) { m_seen.startedOver << entries; });
  connect(controller, &Controller::pickerRequested, this, [this](const QString& kind, const QString& arg) { m_seen.pickers << (kind + ":" + arg); });
  connect(controller, &Controller::inputRestored, this, [this](const QString& text) { m_seen.restored << text; });
  connect(controller, &Controller::inputAppended, this, [this](const QString& text) { m_seen.appended << text; });
  controller->attach(options);
}

QStringList TestController::texts() const {
  QStringList out;
  for (const Entry& entry : m_seen.entries) {
    out << entry.text;
  }
  return out;
}

void TestController::startsWithTheBanner() {
  m_pController->start();
  QCOMPARE(m_seen.startedOver.size(), 1);
  const Entry& banner = m_seen.startedOver[0].first();
  QCOMPARE(banner.kind, Entry::Kind::Banner);
  QCOMPARE(banner.model, QString("fake"));
  QCOMPARE(banner.session, QString("s1"));
}

void TestController::messagesRunTurns() {
  m_pFake->setScript({FakeServer::text("Hello, Mr. Anderson.")});
  m_pController->submit("  hi  ");
  QVERIFY(m_pController->turns().agentRunning());
  QTRY_VERIFY(idle());
  QCOMPARE(m_seen.entries.first().kind, Entry::Kind::User);
  QCOMPARE(m_seen.entries.first().text, QString("hi"));
  QVERIFY_MATCH(m_seen.entries.last().text, "^Done in ");
  QCOMPARE(messageText(m_pFake->requests()[0].value("messages").toArray().last().toObject()), QString("hi"));
  // Blank input does nothing.
  m_pController->submit("   ");
  QVERIFY(idle());
}

void TestController::busyRefusesCommandsAndQueuesMessages() {
  m_pFake->setScript({FakeServer::delayed(FakeServer::text("First."), 800), FakeServer::text("Second.")});
  m_pController->submit("first");
  m_pController->submit("/help");
  QVERIFY_MATCH(texts().last(), "^Commands work once the agent is done");
  QCOMPARE(m_seen.restored, QStringList({"/help"}));
  // A message goes to the agent at its next step.
  m_pController->submit("and also this");
  QCOMPARE(m_pController->agent()->queuedMessages(), QStringList({"and also this"}));
  QTRY_VERIFY(idle());
}

void TestController::queuedTasksFollowInOrder() {
  m_pFake->setScript({FakeServer::delayed(FakeServer::text("One."), 800), FakeServer::text("Two."), FakeServer::text("Three.")});
  m_pController->submit("one");
  m_pController->queueNext("two");
  m_pController->queueNext("three");
  m_pController->queueNext("/compact");
  QVERIFY_MATCH(texts().last(), "^Commands can't be queued");
  QCOMPARE(m_pController->turns().nextTasks(), QStringList({"two", "three"}));
  QTRY_VERIFY(m_pFake->requests().size() == 3 && idle());
  QCOMPARE(messageText(m_pFake->requests()[2].value("messages").toArray().last().toObject()), QString("three"));
}

void TestController::planModeOffersThePlan() {
  m_pFake->setScript({FakeServer::text("1. Do it."), FakeServer::text("Done.")});
  m_pController->submit("/plan");
  QVERIFY(m_pController->agent()->planning());
  m_pController->submit("plan it");
  QTRY_VERIFY(idle());
  QCOMPARE(m_seen.pickers, QStringList({"plan:"}));
  m_pController->carryOutPlan();
  QVERIFY(!m_pController->agent()->planning());
  QTRY_VERIFY(m_pFake->requests().size() == 2 && idle());
  QCOMPARE(messageText(m_pFake->requests()[1].value("messages").toArray().last().toObject()), QString("Go ahead with the plan."));
}

void TestController::commandsReplyOrAskForPickers() {
  m_pController->submit("/reasoning nope");
  QVERIFY_MATCH(texts().last(), "^Unknown reasoning level \"nope\"");
  m_pController->submit("/reasoning high");
  QCOMPARE(m_pController->agent()->reasoning(), QString("high"));
  m_pController->submit("/bogus");
  QVERIFY_MATCH(texts().last(), "^Unknown command /bogus");
  m_pController->submit("/name my task");
  QCOMPARE(m_renamed, QStringList({"my task"}));
  m_pController->submit("/undo");
  QCOMPARE(texts().last(), QString("Nothing to undo."));
  m_pController->submit("/plan maybe");
  QVERIFY_MATCH(texts().last(), "^Use /plan to switch plan mode");
  // Without an argument, the app's picker.
  for (const QString& command : {"/model", "/endpoint", "/reasoning", "/sessions all", "/open", "/skills", "/jobs", "/theme"}) {
    m_pController->submit(command);
  }
  QCOMPARE(m_seen.pickers, QStringList({"model:", "endpoint:", "reasoning:", "sessions:all", "open:", "skills:", "jobs:", "theme:"}));
}

void TestController::appCommandsComeFirst() {
  QStringList taken;
  m_pController->setAppCommands([&](const QString& command, const QString& arg) {
    if (command == "/worktree") {
      taken << arg;
      return true;
    }
    return false;
  });
  m_pController->submit("/worktree fix-login");
  QCOMPARE(taken, QStringList({"fix-login"}));
  m_pController->setHelp([]() { return QString("the app's help"); });
  m_pController->submit("/help");
  QCOMPARE(texts().last(), QString("the app's help"));
}

void TestController::newSessionStartsOver() {
  m_pFake->setScript({FakeServer::text("Hi.")});
  m_pController->submit("hello");
  QTRY_VERIFY(idle());
  QCOMPARE(m_pController->agent()->messages().size(), 3);
  m_pController->submit("/new");
  QCOMPARE(m_started, QStringList({"fake"}));
  QCOMPARE(m_pController->agent()->messages().size(), 1);
  QCOMPARE(m_seen.startedOver.size(), 1);
  QCOMPARE(m_seen.startedOver[0].first().kind, Entry::Kind::Banner);
  QVERIFY_MATCH(m_seen.startedOver[0][1].text, "^Started a new session\\. The previous one is s1");
}

#endif

QTEST_GUILESS_MAIN(TestController)
#include "test_controller.moc"
