// Task worktrees: creating them, what changed, landing the work on the
// user's branch, removing them, and a Runtime working in one. New in the
// port; the original has no worktree support.
#include "FakeServer.h"
#include "TestUtil.h"
#include "core/Agent.h"
#include "core/Dirs.h"
#include "frontend/Runtime.h"
#include "frontend/Worktrees.h"

#include <QCoreApplication>
#include <QFile>

using namespace Smith;
using namespace TestUtil;

class TestWorktrees : public QObject {
  Q_OBJECT

private Q_SLOTS:
  void readsGitDirsWithoutGit();
  void findsTheBranchWhenHeadIsntPlain();
  void createsTaskWorktreeBesideRepo();
  void reusesTaskWorktreeAndBranch();
  void rejectsBadTaskNames();
  void changesIncludeNewFilesButNotIgnored();
  void diffsSplitByFileWithGaps();
  void landCherryPicksAndKeepsYourWork();
  void landMerges();
  void landStopsOnConflict();
  void landStopsForYourChangesInTheWay();
  void branchOnlyLeavesCommitsToLand();
  void removeKeepsUnlandedWorkUnlessDiscarded();
  void openWorktreesAreMarked();
  void runtimeInWorktree();
  void suggestsCommitMessage();
};

#ifndef Q_MOC_RUN

namespace {

// A repository "shop" on main, with a task worktree for "fix-login".
struct Shop {
  IsolatedGit isolated;
  TempDir root;
  QString main;
  Worktree task;

  explicit Shop(bool withTask = true) {
    main = root.file("shop");
    makeRepo(main, {{"login.ts", "check(password)\n"}, {"cart.ts", "total()\n"}, {".gitignore", "node_modules/\n"}});
    if (withTask) {
      task = createTaskWorktree(main, "fix-login").worktree;
    }
  }

  QString head(const QString& dir) const { return git(dir, {"rev-parse", "--short", "HEAD"}).trimmed(); }
};

QStringList paths(const QList<FileChange>& files) {
  QStringList out;
  for (const FileChange& file : files) {
    out << QString("%1 %2").arg(file.status).arg(file.path);
  }
  return out;
}

} // namespace

void TestWorktrees::readsGitDirsWithoutGit() {
  Shop shop;
  const std::optional<GitDirs> main = gitDirs(shop.main);
  QVERIFY(main);
  QVERIFY(!main->linked);
  QCOMPARE(main->commonDir, shop.main + "/.git");

  const std::optional<GitDirs> task = gitDirs(shop.task.path);
  QVERIFY(task);
  QVERIFY(task->linked);
  QCOMPARE(task->commonDir, realPath(shop.main + "/.git"));
  QVERIFY(task->gitDir.startsWith(task->commonDir + "/worktrees/"));
  QCOMPARE(currentBranch(shop.task.path), QString("smith/fix-login"));
  QCOMPARE(currentBranch(shop.main), QString("main"));
  QVERIFY(!gitDirs(shop.root.path()));
}

void TestWorktrees::findsTheBranchWhenHeadIsntPlain() {
  Shop shop(false);
  // An older layout: HEAD a symlink to the branch, so the file holds a commit.
  const QString head = shop.main + "/.git/HEAD";
  QVERIFY(QFile::remove(head));
  QVERIFY(QFile::link("refs/heads/main", head));
  QCOMPARE(currentBranch(shop.main), QString("main"));

  QVERIFY(QFile::remove(head));
  QVERIFY(QFile::copy(shop.main + "/.git/refs/heads/main", head));
  QCOMPARE(git(shop.main, {"status", "--short", "--branch"}).section('\n', 0, 0), QString("## HEAD (no branch)"));
  QCOMPARE(currentBranch(shop.main), QString());
}

void TestWorktrees::createsTaskWorktreeBesideRepo() {
  IsolatedGit isolated;
  TempDir root;
  const QString main = root.file("shop");
  makeRepo(main, {{"a.txt", "a\n"}});
  const CreatedWorktree created = createTaskWorktree(main, "fix-login");
  QVERIFY(!created.reused);
  QCOMPARE(created.worktree.path, realPath(root.file("shop-worktrees/fix-login")));
  QCOMPARE(created.worktree.branch, QString("smith/fix-login"));
  QCOMPARE(created.worktree.task, QString("fix-login"));
  QVERIFY_MATCH(created.from, "^main @ [0-9a-f]{7}");
  QCOMPARE(read(created.worktree.path + "/a.txt"), QString("a\n"));

  const QList<Worktree> listed = listWorktrees(created.worktree.path);
  QCOMPARE(listed.size(), 2);
  QVERIFY(listed.first().main);
  QCOMPARE(listed.first().branch, QString("main"));
  QVERIFY(listed.first().task.isEmpty());
  QCOMPARE(listed.last().task, QString("fix-login"));
  QCOMPARE(mainCheckout(created.worktree.path), listed.first().path);
}

void TestWorktrees::reusesTaskWorktreeAndBranch() {
  Shop shop;
  const CreatedWorktree again = createTaskWorktree(shop.main, "fix-login");
  QVERIFY(again.reused);
  QCOMPARE(again.worktree.path, shop.task.path);

  // The branch kept after its worktree was removed comes back with its work.
  write(shop.task.path + "/login.ts", "check(password) && password\n");
  landWorktree(shop.task, "Reject empty passwords", LandMode::BranchOnly);
  removeWorktree(shop.task, false, false);
  QVERIFY(!exists(shop.task.path));
  const CreatedWorktree back = createTaskWorktree(shop.main, "fix-login");
  QVERIFY(!back.reused);
  QCOMPARE(back.from, QString("smith/fix-login"));
  QCOMPARE(read(back.worktree.path + "/login.ts"), QString("check(password) && password\n"));

  // A folder in the way that isn't the task's worktree.
  QDir().mkpath(shop.root.file("shop-worktrees/other"));
  QVERIFY_THROWS_MATCH(createTaskWorktree(shop.main, "other"), "already exists");
}

void TestWorktrees::rejectsBadTaskNames() {
  Shop shop(false);
  for (const QString name : {"", "has space", "../escape", "-flag", "x.lock", "a..b"}) {
    QVERIFY_THROWS_MATCH(createTaskWorktree(shop.main, name), "task name|Name the task");
  }
  TempDir plain;
  QVERIFY_THROWS_MATCH(createTaskWorktree(plain.path(), "x"), "isn't in a git repository");
}

void TestWorktrees::changesIncludeNewFilesButNotIgnored() {
  Shop shop;
  QVERIFY(worktreeChanges(shop.task).isEmpty());
  write(shop.task.path + "/login.ts", "check(password)\nrequire(password)\n");
  write(shop.task.path + "/login.test.ts", "test()\n");
  write(shop.task.path + "/node_modules/dep.js", "ignored\n");
  QFile::remove(shop.task.path + "/cart.ts");

  const WorktreeChanges changes = worktreeChanges(shop.task);
  QCOMPARE(paths(changes.files), QStringList({"D cart.ts", "A login.test.ts", "M login.ts"}));
  QCOMPARE(changes.added, 2);
  QCOMPARE(changes.removed, 1);
  QVERIFY(changes.diff.contains("+test()"));
  QVERIFY(!changes.diff.contains("node_modules"));
  QVERIFY(changes.pendingCommits.isEmpty());
  // The worktree's own index wasn't touched: the new file is still untracked.
  QVERIFY(git(shop.task.path, {"status", "--porcelain"}).contains("?? login.test.ts"));
}

void TestWorktrees::diffsSplitByFileWithGaps() {
  Shop shop;
  QStringList lines;
  for (int i = 1; i <= 20; i++) {
    lines << QString("line %1").arg(i);
  }
  write(shop.task.path + "/cart.ts", lines.join('\n') + "\n");
  git(shop.task.path, {"commit", "-q", "-am", "Longer cart"});
  lines[2] = "line three";
  lines[17] = "line eighteen";
  write(shop.task.path + "/cart.ts", lines.join('\n') + "\n");
  write(shop.task.path + "/new.ts", "a\nb\n");

  const QList<FileDiff> diffs = fileDiffs(worktreeChanges(shop.task));
  QCOMPARE(diffs.size(), 2);
  QCOMPARE(diffs.at(0).change.path, QString("cart.ts"));
  QCOMPARE(diffs.at(1).change.path, QString("new.ts"));
  QCOMPARE(diffs.at(1).change.status, QChar('A'));

  QStringList cart;
  for (const DiffLine& line : diffs.at(0).diff) {
    switch (line.kind) {
    case DiffLine::Kind::Gap:
      cart << QString("gap %1").arg(line.count);
      break;
    case DiffLine::Kind::Remove:
      cart << QString("-%1 %2").arg(line.oldLine).arg(line.text);
      break;
    case DiffLine::Kind::Add:
      cart << QString("+%1 %2").arg(line.newLine).arg(line.text);
      break;
    case DiffLine::Kind::Context:
      cart << QString(" %1 %2").arg(line.oldLine).arg(line.text);
      break;
    }
  }
  // git's three lines of context around each change; the lines between are a gap.
  QCOMPARE(cart.first(), QString(" 1 line 1"));
  QVERIFY(cart.contains("-3 line 3"));
  QVERIFY(cart.contains("+3 line three"));
  QVERIFY(cart.contains("gap 8"));
  QVERIFY(cart.contains("-18 line 18"));
  QVERIFY(cart.contains("+18 line eighteen"));
  QCOMPARE(cart.last(), QString(" 20 line 20"));

  QCOMPARE(diffs.at(1).diff.size(), 2);
  QCOMPARE(diffs.at(1).diff.at(0).newLine, 1);
  QCOMPARE(diffs.at(1).diff.at(1).text, QString("b"));
}

void TestWorktrees::landCherryPicksAndKeepsYourWork() {
  Shop shop;
  write(shop.task.path + "/login.ts", "check(password) && password\n");
  write(shop.task.path + "/login.test.ts", "test()\n");
  // Your own uncommitted work in another file, and a commit made meanwhile.
  write(shop.main + "/cart.ts", "total() * tax\n");
  write(shop.main + "/README", "shop\n");
  git(shop.main, {"add", "README"});
  git(shop.main, {"commit", "-q", "-m", "Add README"});

  const LandResult result = landWorktree(shop.task, "Reject empty passwords", LandMode::CherryPick);
  QVERIFY2(result.problem.isEmpty(), qPrintable(result.problem));
  QCOMPARE(result.target, QString("main"));
  QCOMPARE(result.landed, shop.head(shop.main));
  QCOMPARE(git(shop.main, {"log", "-1", "--format=%s"}).trimmed(), QString("Reject empty passwords"));
  QCOMPARE(read(shop.main + "/login.ts"), QString("check(password) && password\n"));
  QCOMPARE(read(shop.main + "/login.test.ts"), QString("test()\n"));
  QCOMPARE(read(shop.main + "/cart.ts"), QString("total() * tax\n"));
  QVERIFY(git(shop.main, {"status", "--porcelain"}).contains(" M cart.ts"));
  QCOMPARE(git(shop.task.path, {"log", "-1", "--format=%s"}).trimmed(), QString("Reject empty passwords"));
  QVERIFY(worktreeChanges(shop.task).isEmpty());

  // Landed work: the worktree goes cleanly, with its branch.
  removeWorktree(shop.task, true, false);
  QVERIFY(!exists(shop.task.path));
  QVERIFY(!exists(shop.root.file("shop-worktrees")));
  QVERIFY(git(shop.main, {"branch", "--list", "smith/fix-login"}).trimmed().isEmpty());
}

void TestWorktrees::landMerges() {
  Shop shop;
  write(shop.task.path + "/login.ts", "check(password) && password\n");
  const LandResult result = landWorktree(shop.task, "Reject empty passwords", LandMode::Merge);
  QVERIFY2(result.problem.isEmpty(), qPrintable(result.problem));
  QCOMPARE(read(shop.main + "/login.ts"), QString("check(password) && password\n"));
  // Nothing happened on main meanwhile, so it fast-forwards to the task's commit.
  QCOMPARE(shop.head(shop.main), result.commit);
}

void TestWorktrees::landStopsOnConflict() {
  Shop shop;
  write(shop.task.path + "/login.ts", "check(password) && password\n");
  write(shop.main + "/login.ts", "checkSso() || check(password)\n");
  git(shop.main, {"commit", "-q", "-am", "SSO"});
  const QString before = shop.head(shop.main);

  const LandResult result = landWorktree(shop.task, "Reject empty passwords", LandMode::CherryPick);
  QVERIFY_MATCH(result.problem, "conflicts with main.*smith/fix-login");
  QCOMPARE(result.conflicts, QStringList{"login.ts"});
  QVERIFY(!result.commit.isEmpty());
  // Main is as it was: same commit, clean, no cherry-pick in progress.
  QCOMPARE(shop.head(shop.main), before);
  QVERIFY(git(shop.main, {"status", "--porcelain"}).trimmed().isEmpty());
  QVERIFY(!exists(shop.main + "/.git/CHERRY_PICK_HEAD"));
  // The work is kept on the branch, waiting to land.
  QCOMPARE(worktreeChanges(shop.task).pendingCommits.size(), 1);

  QCOMPARE(landWorktree(shop.task, {}, LandMode::Merge).conflicts, QStringList{"login.ts"});
  QVERIFY(!exists(shop.main + "/.git/MERGE_HEAD"));
}

void TestWorktrees::landStopsForYourChangesInTheWay() {
  Shop shop;
  write(shop.task.path + "/login.ts", "check(password) && password\n");
  write(shop.main + "/login.ts", "// still deciding\ncheck(password)\n");
  const QString before = shop.head(shop.main);

  const LandResult result = landWorktree(shop.task, "Reject empty passwords", LandMode::CherryPick);
  QVERIFY_MATCH(result.problem, "Your uncommitted changes .* touch the same files");
  QCOMPARE(result.conflicts, QStringList{"login.ts"});
  QCOMPARE(read(shop.main + "/login.ts"), QString("// still deciding\ncheck(password)\n"));
  QCOMPARE(shop.head(shop.main), before);

  // Once yours are out of the way, landing again picks up the commit already made.
  git(shop.main, {"checkout", "--", "login.ts"});
  const LandResult again = landWorktree(shop.task, {}, LandMode::CherryPick);
  QVERIFY2(again.problem.isEmpty(), qPrintable(again.problem));
  QVERIFY(again.commit.isEmpty());
  QCOMPARE(read(shop.main + "/login.ts"), QString("check(password) && password\n"));
}

void TestWorktrees::branchOnlyLeavesCommitsToLand() {
  Shop shop;
  write(shop.task.path + "/login.ts", "check(password) && password\n");
  QVERIFY_THROWS_MATCH(landWorktree(shop.task, "  ", LandMode::BranchOnly), "needs a message");
  const LandResult result = landWorktree(shop.task, "Reject empty passwords", LandMode::BranchOnly);
  QVERIFY(result.problem.isEmpty());
  QVERIFY(result.target.isEmpty());
  QCOMPARE(read(shop.main + "/login.ts"), QString("check(password)\n"));
  const WorktreeChanges changes = worktreeChanges(shop.task);
  QVERIFY(changes.files.isEmpty());
  QCOMPARE(changes.pendingCommits.size(), 1);
  QVERIFY_MATCH(changes.pendingCommits.first(), "^[0-9a-f]+ Reject empty passwords$");
  QVERIFY(!changes.isEmpty());
}

void TestWorktrees::removeKeepsUnlandedWorkUnlessDiscarded() {
  Shop shop;
  write(shop.task.path + "/login.ts", "check(password) && password\n");
  QVERIFY_THROWS_MATCH(removeWorktree(shop.task, true, false), "changes that aren't landed");
  QVERIFY(exists(shop.task.path + "/login.ts"));
  removeWorktree(shop.task, true, true);
  QVERIFY(!exists(shop.task.path));
  QCOMPARE(read(shop.main + "/login.ts"), QString("check(password)\n"));
  QVERIFY_THROWS_MATCH(removeWorktree(listWorktrees(shop.main).first(), true, true), "own checkout");
}

void TestWorktrees::openWorktreesAreMarked() {
  Shop shop;
  QCOMPARE(listWorktrees(shop.main).last().openBy, qint64(0));
  markWorktreeOpen(shop.task.path);
  QCOMPARE(listWorktrees(shop.main).last().openBy, QCoreApplication::applicationPid());
  // The main checkout isn't marked: it's yours.
  markWorktreeOpen(shop.main);
  QCOMPARE(listWorktrees(shop.main).first().openBy, qint64(0));
  clearWorktreeOpen(shop.task.path);
  QCOMPARE(listWorktrees(shop.main).last().openBy, qint64(0));

  // A record left by a Smith that's gone (or a reused pid) doesn't count.
  const QString marker = gitDirs(shop.task.path)->gitDir + "/smith.pid";
  write(marker, QString("%1 1\n").arg(QCoreApplication::applicationPid()));
  QCOMPARE(listWorktrees(shop.main).last().openBy, qint64(0));
  write(marker, "999999999 1\n");
  QCOMPARE(listWorktrees(shop.main).last().openBy, qint64(0));
}

void TestWorktrees::runtimeInWorktree() {
  Shop shop;
  TempDir data;
  EnvGuard config("XDG_CONFIG_HOME", data.file("config"));
  EnvGuard share("XDG_DATA_HOME", data.file("data"));
  FakeServer fake;
  LaunchOptions launch;
  launch.folder = shop.task.path;
  launch.baseUrl = fake.baseUrl();
  launch.sandbox = "off";
  const std::shared_ptr<Runtime> runtime = Runtime::start(launch);

  // git data is visible to commands (read-only, in a sandbox that shows it).
  QVERIFY(runtime->sandbox()->readOnlyPaths().contains(realPath(shop.main + "/.git")));
  QCOMPARE(runtime->project().worktreeBranch, QString("smith/fix-login"));
  const QString prompt = messageText(runtime->createAgent({})->messages().first());
  QVERIFY_MATCH(prompt, "worktree made for this task, on the branch smith/fix-login\\. Leave your changes uncommitted");
  QCOMPARE(listWorktrees(shop.main).last().openBy, QCoreApplication::applicationPid());

  // Back in the main checkout: nothing about worktrees, and the mark is gone.
  runtime->switchProject(shop.main);
  QVERIFY(runtime->project().worktreeBranch.isEmpty());
  QVERIFY(!runtime->sandbox()->readOnlyPaths().contains(realPath(shop.main + "/.git")));
  QVERIFY(!messageText(runtime->createAgent({})->messages().first()).contains("worktree"));
  QCOMPARE(listWorktrees(shop.main).last().openBy, qint64(0));

  runtime->switchProject(shop.task.path);
  QCOMPARE(listWorktrees(shop.main).last().openBy, QCoreApplication::applicationPid());
  runtime->shutdown();
  QCOMPARE(listWorktrees(shop.main).last().openBy, qint64(0));
}

void TestWorktrees::suggestsCommitMessage() {
  Shop shop;
  write(shop.task.path + "/login.ts", "check(password) && password\n");
  write(shop.task.path + "/login.test.ts", "test()\n");
  const WorktreeChanges changes = worktreeChanges(shop.task);
  FakeServer fake;
  AgentOptions options;
  options.server = std::make_shared<Server>(fake.baseUrl(), "x");
  options.model = "fake";
  options.cwd = shop.task.path;
  options.todos = false;
  const auto agent = std::make_shared<Agent>(options);

  fake.setScript({FakeServer::plain("<think>short and clear</think>\n\"Reject empty passwords.\"\nMore text")});
  QCOMPARE(suggestCommitMessage(agent, shop.task, changes, "the login accepts empty passwords", makeCancel()),
           QString("Reject empty passwords"));
  const QJsonObject request = fake.requests().last();
  QVERIFY(request.value("tools").isUndefined());
  const QString input = request.value("messages").toArray().at(1).toObject().value("content").toString();
  QVERIFY_MATCH(input, "the login accepts empty passwords");
  QVERIFY_MATCH(input, "A login.test.ts\nM login.ts");
  QVERIFY_MATCH(input, "\\+test\\(\\)");

  // Without the model's help, a plain one.
  fake.setScript({FakeServer::failure(500, QJsonObject{{"message", "down"}})});
  QCOMPARE(suggestCommitMessage(agent, shop.task, changes, {}, makeCancel()), QString("Changes from smith/fix-login"));
  QCOMPARE(suggestCommitMessage(nullptr, shop.task, changes, {}, makeCancel()), QString("Changes from smith/fix-login"));
}

#endif

QTEST_GUILESS_MAIN(TestWorktrees)
#include "test_worktrees.moc"
