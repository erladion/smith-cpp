// What both apps share for running turns: the approvals and questions the
// agent's threads wait on, and the turn runner with the state it keeps from
// agent events. New in the port.
#include "TestUtil.h"
#include "core/Errors.h"
#include "frontend/Prompts.h"
#include "frontend/Turns.h"

#include <QCoreApplication>
#include <QThread>

#include <atomic>

using namespace Smith;
using namespace TestUtil;

class TestFrontend : public QObject {
  Q_OBJECT

private Q_SLOTS:
  void approvalWaitsForTheDecision();
  void questionWaitsForTheAnswer();
  void dismissAnswersNo();
  void closedPromptsRefuse();
  void eventsBecomeActivityAndEntries();
  void todosOnlyShowDuringATurn();
  void timedTurnReportsStats();
  void turnErrorsAreDescribed();
  void interruptCancelsTheTurn();
  void taskRunsItsFollowUpOrReportsFailure();
  void closeWaitsAndDropsCallbacks();
  void undoNoticeListsWhatHappened();
};

#ifndef Q_MOC_RUN

namespace {

// Runs `ask` on a thread of its own, as the agent would, and keeps what it returned.
template <typename T>
struct Asker {
  explicit Asker(std::function<T()> ask) {
    thread.reset(QThread::create([this, ask]() {
      result = ask();
      done = true;
    }));
    thread->start();
  }
  ~Asker() { thread->wait(); }

  std::unique_ptr<QThread> thread;
  T result{};
  std::atomic<bool> done{false};
};

ApprovalRequest request(const QString& tool, const QString& summary) {
  ApprovalRequest out;
  out.tool = tool;
  out.summary = summary;
  return out;
}

} // namespace

void TestFrontend::approvalWaitsForTheDecision() {
  Prompts prompts;
  int changes = 0;
  connect(&prompts, &Prompts::changed, this, [&]() { changes++; });
  Asker<ApprovalDecision> asker([&]() { return prompts.approve(request("bash", "make test")); });

  QTRY_VERIFY(prompts.approval());
  QCOMPARE(prompts.approval()->summary, QString("make test"));
  QVERIFY(prompts.waiting());
  QVERIFY(!asker.done);

  prompts.decide(ApprovalDecision::AllowAlways);
  QTRY_VERIFY(asker.done);
  QCOMPARE(asker.result, ApprovalDecision::AllowAlways);
  QVERIFY(!prompts.waiting());
  QCOMPARE(changes, 2);

  // Nothing waits: a stray decision is ignored.
  prompts.decide(ApprovalDecision::Deny);
  QCOMPARE(changes, 2);
}

void TestFrontend::questionWaitsForTheAnswer() {
  Prompts prompts;
  int asked = 0;
  connect(&prompts, &Prompts::questionAsked, this, [&]() { asked++; });
  Asker<Answer> asker([&]() { return prompts.ask(Question{"Which database?", {"SQLite", "Postgres"}}); });

  QTRY_VERIFY(prompts.question());
  QCOMPARE(asked, 1);
  QCOMPARE(prompts.question()->options, QStringList({"SQLite", "Postgres"}));
  QVERIFY(!prompts.approval());

  prompts.answer(Answer{Answer::Kind::Option, 1, {}});
  QTRY_VERIFY(asker.done);
  QCOMPARE(asker.result.kind, Answer::Kind::Option);
  QCOMPARE(asker.result.option, 1);
  QVERIFY(!prompts.question());
}

void TestFrontend::dismissAnswersNo() {
  Prompts prompts;
  Asker<ApprovalDecision> approval([&]() { return prompts.approve(request("bash", "rm -rf build")); });
  QTRY_VERIFY(prompts.approval());
  prompts.dismiss();
  QTRY_VERIFY(approval.done);
  QCOMPARE(approval.result, ApprovalDecision::Deny);

  Asker<Answer> question([&]() { return prompts.ask(Question{"Sure?", {}}); });
  QTRY_VERIFY(prompts.question());
  prompts.dismiss();
  QTRY_VERIFY(question.done);
  QCOMPARE(question.result.kind, Answer::Kind::Dismissed);
}

void TestFrontend::closedPromptsRefuse() {
  Prompts prompts;
  Asker<ApprovalDecision> waiting([&]() { return prompts.approve(request("bash", "ls")); });
  QTRY_VERIFY(prompts.approval());
  prompts.close();
  QTRY_VERIFY(waiting.done);
  QCOMPARE(waiting.result, ApprovalDecision::Deny);

  // Asked after closing, with nobody running the event loop: still answered.
  Asker<ApprovalDecision> late([&]() { return prompts.approve(request("bash", "ls")); });
  late.thread->wait();
  QCOMPARE(late.result, ApprovalDecision::Deny);
  Asker<Answer> question([&]() { return prompts.ask(Question{"Still there?", {}}); });
  question.thread->wait();
  QCOMPARE(question.result.kind, Answer::Kind::Dismissed);
}

void TestFrontend::eventsBecomeActivityAndEntries() {
  TurnRunner turns;
  QVERIFY(!turns.apply(Event::ToolPrepare{"grep"}));
  QCOMPARE(turns.activity(), QString("Preparing grep…"));

  QVERIFY(!turns.apply(Event::ToolStart{"c1", "grep", "TODO"}));
  QCOMPARE(turns.activity(), QString("grep TODO"));
  QVERIFY(!turns.apply(Event::ToolStart{"c2", "glob", "*.cpp"}));
  QCOMPARE(turns.runningTools(), 2);
  QVERIFY_MATCH(turns.activity(), "2");
  // A sub-agent's progress, while two tools run.
  QVERIFY(!turns.apply(Event::Activity{"find config › grep port", false}));
  QCOMPARE(turns.activity(), QString("2 in parallel · find config › grep port"));

  const std::optional<Entry> ended = turns.apply(Event::ToolEnd{"c1", "grep", "TODO", "a.cpp:3", false, std::nullopt});
  QVERIFY(ended);
  QCOMPARE(ended->kind, Entry::Kind::Tool);
  QCOMPARE(ended->name, QString("grep"));
  QCOMPARE(ended->output, QString("a.cpp:3"));
  QCOMPARE(turns.activity(), QString("glob *.cpp"));
  turns.apply(Event::ToolEnd{"c2", "glob", "*.cpp", "", true, std::nullopt});
  QCOMPARE(turns.runningTools(), 0);

  // The checklist tool's own result isn't shown, unless it failed.
  QVERIFY(!turns.apply(Event::ToolEnd{"c3", TODO_TOOL, "", "ok", false, std::nullopt}));
  QVERIFY(turns.apply(Event::ToolEnd{"c4", TODO_TOOL, "", "bad list", true, std::nullopt}));

  turns.apply(Event::Activity{"Summarizing…", true});
  QVERIFY(turns.compacting());
  QVERIFY(!turns.apply(Event::Delta{BlockKind::Text, "Hi"}));
  QVERIFY(!turns.compacting());
  QCOMPARE(turns.activity(), QString());

  QVERIFY(!turns.tokens());
  turns.apply(Event::Context{1200, 8192});
  QCOMPARE(turns.tokens(), std::optional<qint64>(1200));
  QCOMPARE(turns.limit(), std::optional<int>(8192));
  turns.forgetContext();
  QVERIFY(!turns.tokens() && !turns.limit());

  const std::optional<Entry> notice = turns.apply(Event::Notice{NoticeLevel::Warn, "careful"});
  QVERIFY(notice && notice->kind == Entry::Kind::Notice && notice->level == NoticeLevel::Warn);
  const std::optional<Entry> message = turns.apply(Event::UserMessage{"also the tests"});
  QVERIFY(message && message->kind == Entry::Kind::User);
  QCOMPARE(message->text, QString("also the tests"));
  QVERIFY(!turns.apply(Event::ResponseEnd{}));
}

void TestFrontend::todosOnlyShowDuringATurn() {
  TurnRunner turns;
  const TodoList list{Todo{"write the test", TodoStatus::InProgress}};
  QVERIFY(!turns.apply(Event::Todos{list}));

  std::optional<Entry> during;
  bool finished = false;
  turns.runTurn(
      "Thinking…", {}, [](const CancelPtr&) { QThread::msleep(50); }, [&](const QString&) { finished = true; });
  QVERIFY(turns.busy());
  during = turns.apply(Event::Todos{list});
  QVERIFY(during && during->kind == Entry::Kind::Todos);
  QCOMPARE(during->todos.size(), 1);
  QTRY_VERIFY(finished);
}

void TestFrontend::timedTurnReportsStats() {
  TurnRunner turns;
  int changes = 0;
  connect(&turns, &TurnRunner::changed, this, [&]() { changes++; });
  QVERIFY(!turns.elapsed());
  QCOMPARE(turns.startedAt(), qint64(0));

  QString error = "unset";
  turns.runTurn(
      "Thinking…", {}, [](const CancelPtr&) { QThread::msleep(60); }, [&](const QString& failure) { error = failure; }, true);
  QVERIFY(turns.busy() && turns.agentRunning());
  QCOMPARE(turns.activity(), QString("Thinking…"));
  QVERIFY(turns.elapsed());
  QVERIFY(turns.startedAt() > 0);
  turns.apply(Event::ToolStart{"c1", "bash", "make"});
  turns.apply(Event::ToolEnd{"c1", "bash", "make", "ok", false, std::nullopt});
  turns.apply(Event::Done{DoneReason::Stop});

  QTRY_COMPARE(error, QString());
  QVERIFY(!turns.busy() && !turns.agentRunning());
  QCOMPARE(turns.activity(), QString());
  QVERIFY(!turns.elapsed());
  QCOMPARE(turns.startedAt(), qint64(0));
  QCOMPARE(turns.lastTurn().reason, QString("stop"));
  QCOMPARE(turns.lastTurn().toolCalls, 1);
  QVERIFY(turns.lastTurn().elapsedMs >= 50);
  QCOMPARE(changes, 2);

  // A turn that isn't timed (compacting) leaves the last stats alone and shows no clock.
  bool finished = false;
  turns.runTurn(
      "Compacting…", {}, [](const CancelPtr&) {}, [&](const QString&) { finished = true; });
  QVERIFY(!turns.elapsed());
  QTRY_VERIFY(finished);
  QCOMPARE(turns.lastTurn().toolCalls, 1);
}

void TestFrontend::turnErrorsAreDescribed() {
  TurnRunner turns;
  QString error;
  turns.runTurn(
      "Thinking…", "http://127.0.0.1:1/v1", [](const CancelPtr&) { throw SmithError("the model gave up"); },
      [&](const QString& failure) { error = failure; });
  QTRY_VERIFY(!error.isEmpty());
  QVERIFY_MATCH(error, "the model gave up");
  QVERIFY(!turns.busy());

  error.clear();
  turns.runTurn(
      "Thinking…", {}, [](const CancelPtr&) { throw 42; }, [&](const QString& failure) { error = failure; });
  QTRY_COMPARE(error, QString("Unexpected error."));
}

void TestFrontend::interruptCancelsTheTurn() {
  TurnRunner turns;
  std::atomic<bool> sawCancel{false};
  bool finished = false;
  turns.runTurn(
      "Thinking…", {},
      [&](const CancelPtr& cancel) {
        for (int i = 0; i < 400 && !cancelled(cancel); i++) {
          QThread::msleep(5);
        }
        sawCancel = cancelled(cancel);
      },
      [&](const QString&) { finished = true; });
  turns.interrupt();
  QTRY_VERIFY(finished);
  QVERIFY(sawCancel);
  // Nothing runs: interrupting is harmless.
  turns.interrupt();
}

void TestFrontend::taskRunsItsFollowUpOrReportsFailure() {
  TurnRunner turns;
  QThread* ui = QThread::currentThread();
  bool followedUp = false;
  QString error;
  turns.runTask(
      "Checking…", {},
      [&]() -> std::function<void()> {
        return [&]() { followedUp = QThread::currentThread() == ui; };
      },
      [&](const QString& failure) { error = failure; });
  QVERIFY(turns.busy());
  // A task can't be interrupted, and takes no queued messages.
  QVERIFY(!turns.agentRunning());
  QCOMPARE(turns.activity(), QString("Checking…"));
  QTRY_VERIFY(followedUp);
  QVERIFY(!turns.busy());
  QCOMPARE(error, QString());

  turns.runTask(
      "Checking…", {}, []() -> std::function<void()> { throw SmithError("no such model"); }, [&](const QString& failure) { error = failure; });
  QTRY_VERIFY(!error.isEmpty());
  QVERIFY_MATCH(error, "no such model");
  QVERIFY(!turns.busy());
}

void TestFrontend::closeWaitsAndDropsCallbacks() {
  auto turns = std::make_unique<TurnRunner>();
  std::atomic<bool> worked{false};
  bool finished = false;
  turns->runTurn(
      "Thinking…", {},
      [&](const CancelPtr& cancel) {
        while (!cancelled(cancel)) {
          QThread::msleep(5);
        }
        worked = true;
      },
      [&](const QString&) { finished = true; });
  bool launched = false;
  turns->launch([]() { QThread::msleep(20); }, [&]() { launched = true; });

  turns->close();
  QVERIFY2(worked, "close interrupted the turn and waited for its thread");
  QCoreApplication::processEvents();
  QVERIFY2(!finished && !launched, "nothing is called back after close");
  turns->close();
}

void TestFrontend::undoNoticeListsWhatHappened() {
  UndoResult result;
  result.restored = QStringList{"/work/shop/src/a.cpp", "/elsewhere/b.cpp"};
  const Entry clean = undoNotice(result, "/work/shop");
  QCOMPARE(clean.level, NoticeLevel::Info);
  QVERIFY_MATCH(clean.text, "^Undid the last turn");
  QVERIFY_MATCH(clean.text, "Restored: src/a\\.cpp, /elsewhere/b\\.cpp");

  result.skipped = QStringList{"/work/shop/c.cpp"};
  result.untracked = QStringList{"bash"};
  const Entry partial = undoNotice(result, "/work/shop");
  QCOMPARE(partial.level, NoticeLevel::Warn);
  QVERIFY_MATCH(partial.text, "Left alone because they changed afterwards: c\\.cpp");
  QVERIFY_MATCH(partial.text, "Not undone: anything changed by bash commands");
}

#endif

QTEST_GUILESS_MAIN(TestFrontend)
#include "test_frontend.moc"
