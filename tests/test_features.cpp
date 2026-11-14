// Port of the original test/features.test.ts. Its "skill folders are readable
// inside the sandbox" test is in test_sandbox.cpp, with the other bwrap tests.

#include "FakeServer.h"
#include "TestUtil.h"

#include "core/Agent.h"
#include "core/Diff.h"
#include "core/Instructions.h"
#include "core/Prompt.h"
#include "core/Skills.h"
#include "core/tools/Tools.h"

using namespace Smith;
using namespace TestUtil;

class TestFeatures : public QObject {
  Q_OBJECT

private Q_SLOTS:
  // diff
  void hunksWithContextGapsAndLineNumbers();
  void reproducesRandomEdits();
  void newFileIsAllAdditions();

  // skill headers
  void parsesSkillYaml();

  // skills and instructions
  void projectSkillsOverridePersonal();
  void instructionsUpToRepoRoot();
  void skillToolLoadsInstructionsAndFiles();

  // approval previews and undo
  void approvalShowsDiffFailingEditDoesntAsk();
  void undoRestoresFilesAndDropsTurn();
  void undoLeavesChangedFilesAlone();
};

#ifndef Q_MOC_RUN

namespace {

// Applies a diff's removals and additions to check it reproduces the new text.
QString applyDiff(const QString& before, const Diff& diff) {
  QStringList lines = before.split('\n');
  if (!lines.isEmpty() && lines.last().isEmpty()) {
    lines.removeLast();
  }
  QSet<int> removed;
  for (const DiffLine& line : diff) {
    if (line.kind == DiffLine::Kind::Remove) {
      removed.insert(line.oldLine);
    }
  }
  QStringList kept;
  for (int i = 0; i < lines.size(); i++) {
    if (!removed.contains(i + 1)) {
      kept << lines[i];
    }
  }
  for (const DiffLine& line : diff) {
    if (line.kind == DiffLine::Kind::Add) {
      kept.insert(line.newLine - 1, line.text);
    }
  }
  return kept.join('\n') + "\n";
}

QString describe(const DiffLine& line) {
  switch (line.kind) {
  case DiffLine::Kind::Gap:
    return QString("… %1").arg(line.count);
  case DiffLine::Kind::Context:
    return "c " + line.text;
  case DiffLine::Kind::Remove:
    return "r " + line.text;
  case DiffLine::Kind::Add:
    return "a " + line.text;
  }
  return {};
}

QString kindName(DiffLine::Kind kind) {
  switch (kind) {
  case DiffLine::Kind::Context:
    return "context";
  case DiffLine::Kind::Remove:
    return "remove";
  case DiffLine::Kind::Add:
    return "add";
  case DiffLine::Kind::Gap:
    return "gap";
  }
  return {};
}

} // namespace

void TestFeatures::hunksWithContextGapsAndLineNumbers() {
  QStringList numbered;
  for (int i = 1; i <= 20; i++) {
    numbered << QString("line %1").arg(i);
  }
  const QString before = numbered.join('\n') + "\n";
  const QString after = QString(before).replace("line 3\n", "line three\n").replace("line 17\n", "");
  const Diff diff = diffLines(before, after, 1);
  QStringList shown;
  for (const DiffLine& line : diff) {
    shown << describe(line);
  }
  QCOMPARE(shown, QStringList({"c line 2", "r line 3", "a line three", "c line 4", "… 11", "c line 16", "r line 17", "c line 18"}));
  QCOMPARE(applyDiff(before, diff), after);
}

void TestFeatures::reproducesRandomEdits() {
  quint64 seed = 7;
  auto rand = [&](int n) {
    seed = (seed * 1103515245 + 12345) % (quint64(1) << 31);
    return int(seed % quint64(n));
  };
  for (int round = 0; round < 50; round++) {
    QStringList a;
    const int length = rand(30);
    for (int i = 0; i < length; i++) {
      a << QString("x%1").arg(rand(6));
    }
    QStringList b = a;
    for (int e = 0; e < 5; e++) {
      const int at = rand(b.size() + 1);
      if (rand(2)) {
        if (at < b.size()) {
          b.removeAt(at);
        }
      } else {
        b.insert(at, QString("y%1").arg(rand(6)));
      }
    }
    const QString before = a.isEmpty() ? QString() : a.join('\n') + "\n";
    const QString after = b.isEmpty() ? QString() : b.join('\n') + "\n";
    // applyDiff always ends with a newline, so an empty result comes back as "\n".
    QCOMPARE(applyDiff(before, diffLines(before, after, 0)), after.isEmpty() ? QString("\n") : after);
  }
}

void TestFeatures::newFileIsAllAdditions() {
  QStringList kinds;
  for (const DiffLine& line : diffLines("", "a\nb\n")) {
    kinds << kindName(line.kind);
  }
  QCOMPARE(kinds, QStringList({"add", "add"}));
}

void TestFeatures::parsesSkillYaml() {
  const Frontmatter parsed = parseFrontmatter(
      "---\nname: cpp-style\ndescription: >\n  House style for C++.\n  Use when writing C++.\nlicense: 'MIT'\nnotes: first part\n  continued here\n---\n# Body\n");
  const QMap<QString, QString> expected{
      {"name", "cpp-style"},
      {"description", "House style for C++. Use when writing C++."},
      {"license", "MIT"},
      {"notes", "first part continued here"},
  };
  QCOMPARE(parsed.data, expected);
  QCOMPARE(parsed.body, QString("# Body\n"));
  QVERIFY(parseFrontmatter("no header").data.isEmpty());
}

void TestFeatures::projectSkillsOverridePersonal() {
  TempDir dir(QDir::tempPath(), "agent-skills-");
  EnvGuard config("XDG_CONFIG_HOME", dir.file("config"));
  const QString project = dir.file("project/.agents/skills");
  const QString personal = dir.file("personal");
  write(project + "/style/SKILL.md", "---\nname: style\ndescription: Project style\n---\nproject body");
  write(personal + "/style/SKILL.md", "---\nname: style\ndescription: Personal style\n---\npersonal body");
  write(personal + "/deploy/SKILL.md", "---\ndescription: How to deploy\n---\nsteps");
  write(personal + "/broken/SKILL.md", "---\nname: broken\n---\nno description");
  const LoadedSkills loaded = loadSkills({project, personal});
  QStringList shown;
  for (const Skill& skill : loaded.skills) {
    shown << QString("%1|%2|%3").arg(skill.name, skill.description, skill.body);
  }
  QCOMPARE(shown, QStringList({"style|Project style|project body", "deploy|How to deploy|steps"}));
  QVERIFY_MATCH(loaded.warnings.join(','), "broken.*no description");
}

void TestFeatures::instructionsUpToRepoRoot() {
  TempDir dir(QDir::tempPath(), "agent-skills-");
  EnvGuard config("XDG_CONFIG_HOME", dir.file("config"));
  const QString repo = dir.file("repo");
  write(dir.file("config/smith/AGENTS.md"), "personal rules");
  write(repo + "/.git/HEAD", "");
  write(repo + "/AGENTS.md", "repo rules");
  write(repo + "/pkg/CLAUDE.md", "package rules (CLAUDE.md fallback)");
  write(dir.file("AGENTS.md"), "outside the repo: ignored");
  const QList<InstructionFile> files = loadInstructions(repo + "/pkg");
  QStringList contents;
  for (const InstructionFile& file : files) {
    contents << file.content;
  }
  QCOMPARE(contents, QStringList({"personal rules", "repo rules", "package rules (CLAUDE.md fallback)"}));

  PromptExtras extras;
  extras.instructions = files;
  extras.skills = {Skill{"s", "Do s", dir.path(), {}}};
  const QString prompt = buildSystemPrompt(repo, extras);
  QVERIFY(prompt.indexOf("personal rules") < prompt.indexOf("repo rules"));
  QVERIFY_MATCH(prompt, "- s: Do s");
}

void TestFeatures::skillToolLoadsInstructionsAndFiles() {
  TempDir dir(QDir::tempPath(), "agent-skills-");
  const QString skillDir = dir.file("skills/deploy");
  write(skillDir + "/SKILL.md", "---\ndescription: Deploying\n---\nRun scripts/deploy.sh");
  write(skillDir + "/scripts/deploy.sh", "echo deploying");
  const ToolDef tool = skillTool(loadSkills({dir.file("skills")}).skills);
  const ToolContext ctx = context(dir.path());
  QVERIFY_MATCH(run(tool, {{"name", "deploy"}}, ctx).content, "Run scripts/deploy\\.sh[\\s\\S]*- scripts/deploy\\.sh");
  QCOMPARE(run(tool, {{"name", "deploy"}, {"file", "scripts/deploy.sh"}}, ctx).content, QString("echo deploying"));
  QVERIFY_THROWS_MATCH(run(tool, {{"name", "deploy"}, {"file", "../../config"}}, ctx), "outside");
  QVERIFY(run(tool, {{"name", "nope"}}, ctx).isError);
}

namespace {

std::unique_ptr<Agent> undoAgent(const FakeServer& fake, const QString& cwd, QList<ApprovalRequest>* approvals = nullptr) {
  AgentOptions options;
  options.server = std::make_shared<Server>(fake.baseUrl(), "x");
  options.model = "fake";
  options.cwd = cwd;
  options.tools = defaultTools();
  options.systemPrompt = "test";
  options.onEvent = [](const AgentEvent&) {};
  options.approve = [approvals](const ApprovalRequest& request) {
    if (approvals) {
      approvals->append(request);
    }
    return ApprovalDecision::Allow;
  };
  return std::make_unique<Agent>(options);
}

QString base(const QString& file) {
  return QFileInfo(file).fileName();
}

} // namespace

void TestFeatures::approvalShowsDiffFailingEditDoesntAsk() {
  FakeServer fake;
  TempDir dir(QDir::tempPath(), "agent-undo-");
  write(dir.file("a.txt"), "one\ntwo\nthree\n");
  fake.setScript({
      FakeServer::stream(FakeServer::toolCallDeltas("edit_file", {{"path", "a.txt"}, {"old_string", "two"}, {"new_string", "TWO"}}), "tool_calls"),
      FakeServer::stream(FakeServer::toolCallDeltas("edit_file", {{"path", "a.txt"}, {"old_string", "missing"}, {"new_string", "x"}}, "call_2"),
                         "tool_calls"),
      FakeServer::text("done"),
  });
  QList<ApprovalRequest> approvals;
  undoAgent(fake, dir.path(), &approvals)->send("edit", makeCancel());
  QVERIFY2(approvals.size() == 1, "only the edit that can apply is shown");
  QStringList kinds;
  for (const DiffLine& line : *approvals.first().preview) {
    kinds << kindName(line.kind);
  }
  QCOMPARE(kinds, QStringList({"context", "remove", "add", "context"}));
  const QJsonArray messages = fake.requests()[2].value("messages").toArray();
  QVERIFY_MATCH(messages.last().toObject().value("content").toString(), "^Error: old_string was not found");
}

void TestFeatures::undoRestoresFilesAndDropsTurn() {
  FakeServer fake;
  TempDir dir(QDir::tempPath(), "agent-undo-");
  write(dir.file("keep.txt"), "original\n");
  auto agent = undoAgent(fake, dir.path());
  fake.setScript({FakeServer::text("hi")});
  agent->send("first turn", makeCancel());
  const int historyAfterFirst = agent->messages().size();

  fake.setScript({
      FakeServer::stream(FakeServer::toolCallDeltas("edit_file", {{"path", "keep.txt"}, {"old_string", "original"}, {"new_string", "changed"}}),
                         "tool_calls"),
      FakeServer::stream(FakeServer::toolCallDeltas("write_file", {{"path", "new.txt"}, {"content", "created\n"}}, "call_2"), "tool_calls"),
      FakeServer::stream(FakeServer::toolCallDeltas("bash", {{"command", "true"}}, "call_3"), "tool_calls"),
      FakeServer::text("done"),
  });
  agent->send("change things", makeCancel());
  QCOMPARE(read(dir.file("keep.txt")), QString("changed\n"));

  const std::optional<UndoResult> result = agent->undo();
  QVERIFY(result.has_value());
  QCOMPARE(result->prompt, QString("change things"));
  QStringList restored;
  for (const QString& file : result->restored) {
    restored << base(file);
  }
  restored.sort();
  QCOMPARE(restored, QStringList({"keep.txt", "new.txt"}));
  QCOMPARE(result->untracked, QStringList({"bash"}));
  QCOMPARE(read(dir.file("keep.txt")), QString("original\n"));
  QVERIFY(!exists(dir.file("new.txt")));
  QCOMPARE(int(agent->messages().size()), historyAfterFirst);
}

void TestFeatures::undoLeavesChangedFilesAlone() {
  FakeServer fake;
  TempDir dir(QDir::tempPath(), "agent-undo-");
  auto agent = undoAgent(fake, dir.path());
  fake.setScript({
      FakeServer::stream(FakeServer::toolCallDeltas("write_file", {{"path", "f.txt"}, {"content", "agent\n"}}), "tool_calls"),
      FakeServer::text("done"),
  });
  agent->send("write", makeCancel());
  write(dir.file("f.txt"), "edited by hand\n");
  const std::optional<UndoResult> result = agent->undo();
  QVERIFY(result.has_value());
  QCOMPARE(result->skipped.size(), 1);
  QCOMPARE(base(result->skipped.first()), QString("f.txt"));
  QCOMPARE(read(dir.file("f.txt")), QString("edited by hand\n"));
  QVERIFY(!agent->undo().has_value());
}

#endif

QTEST_GUILESS_MAIN(TestFeatures)
#include "test_features.moc"
