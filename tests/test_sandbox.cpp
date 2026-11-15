// Port of the original test/sandbox.test.ts and test/container.test.ts.
// Runs real commands through bubblewrap; skipped where bwrap can't run.
// The container tests need docker and the ubi9 images (they don't pull).

#include "FakeServer.h"
#include "TestUtil.h"

#include "core/ContainerSandbox.h"
#include "core/Dirs.h"
#include "core/Jobs.h"
#include "core/tools/Tools.h"
#include "frontend/Worktrees.h"

#include <QElapsedTimer>
#include <QProcess>

#include <unistd.h>

using namespace Smith;
using namespace TestUtil;

class TestSandbox : public QObject {
  Q_OBJECT

private Q_SLOTS:
  void initTestCase();

  // bash in the sandbox
  void projectIsReadableAndWritable();
  void outsideIsReadOnlyOrHidden();
  void credentialsInHomeAreHidden();
  void gitIsReadOnly();
  void gitWorksReadOnlyInWorktree();
  void secretsInEnvironmentStayOut();
  void hiddenSymlinksAreFollowed();
  void networkBlockedUnlessEnabled();
  void tmpIsPrivateAndKept();
  void fileToolsGoThroughSandbox();
  void fileToolsReadSessionTmp();
  void timedOutCommandIsKilledWithChildren();

  // the home folder in the bwrap sandbox
  void onlyProjectToolsAndChosenFoldersShow();
  void repositoryAroundProjectIsReadable();
  void failuresComeWithNotes();
  void buildVariablesPassIn();
  void backgroundJobsShareTmp();
  void limitsApply();

  // fallbacks
  void offMeansPlainBashWithoutKeys();
  void sandboxIsRequired();

  // skill folders (from features.test.ts, which needs bwrap too)
  void skillFoldersReadableInSandbox();

  // containers
  void mountsAreReadAsWritten();
  void containerSandbox();
  void containerVariables();

private:
  bool m_bwrap = true;
};

#ifndef Q_MOC_RUN

namespace {

std::shared_ptr<Sandbox> bwrap(bool network = false, const QStringList& env = {}, const std::optional<Limits>& limits = std::nullopt,
                               const HomeView& home = {}) {
  SandboxOptions options;
  options.type = SandboxType::Bwrap;
  options.network = network;
  options.env = env;
  options.limits = limits;
  options.home = home;
  return Sandbox::create(options);
}

ToolResult sh(const std::shared_ptr<Sandbox>& sandbox, const QString& cwd, const QString& command, std::optional<int> timeoutMs = std::nullopt) {
  QJsonObject input{{"command", command}};
  if (timeoutMs) {
    input["timeout_ms"] = *timeoutMs;
  }
  return run(bashTool(), input, context(cwd, sandbox));
}

// A fake home with credentials in it; the project lives inside it, which
// checks that binding the project doesn't re-expose the hidden paths.
struct FakeHome {
  TempDir home{QDir::tempPath(), "agent-home-"};
  TempDir outside{QDir::tempPath(), "agent-outside-"};
  EnvGuard env{"HOME", home.path()};

  FakeHome() {
    write(home.file(".ssh/id_ed25519"), "PRIVATE KEY");
    write(home.file(".netrc"), "machine x password hunter2");
    QDir().mkpath(home.file(".git/hooks"));
    write(home.file(".git/config"), "[core]\n");
    write(home.file("hello.txt"), "hi\n");
  }

  QString project() const { return home.path(); }
};

} // namespace

void TestSandbox::initTestCase() {
  try {
    bwrap()->dispose();
  } catch (const SmithError&) {
    m_bwrap = false;
  }
}

#define REQUIRE_BWRAP()                                                                                                    \
  if (!m_bwrap) {                                                                                                          \
    QSKIP("bwrap is not available");                                                                                       \
  }

void TestSandbox::projectIsReadableAndWritable() {
  REQUIRE_BWRAP();
  FakeHome fake;
  const ToolResult res = sh(bwrap(), fake.project(), "cat hello.txt && echo new > made.txt && cat made.txt");
  QVERIFY2(!res.isError, qPrintable(res.content));
  QCOMPARE(res.content, QString("hi\nnew"));
  QCOMPARE(read(fake.home.file("made.txt")), QString("new\n"));
}

void TestSandbox::outsideIsReadOnlyOrHidden() {
  REQUIRE_BWRAP();
  FakeHome fake;
  const QString out =
      sh(bwrap(), fake.project(), QString("touch /usr/agent-test 2>&1; ls -A %1 2>&1; ls -A /run /mnt 2>/dev/null | grep -v : ; echo done").arg(fake.outside.path()))
          .content;
  QVERIFY_MATCH(out, "Read-only file system");
  QVERIFY2(matches(out, "No such file or directory"), "other /tmp directories are not visible");
  QVERIFY_MATCH(out, "done$");
  QVERIFY_NO_MATCH(out, "WSL|docker|dbus|wsl");
}

void TestSandbox::credentialsInHomeAreHidden() {
  REQUIRE_BWRAP();
  FakeHome fake;
  // ~/.ssh is an empty directory; ~/.netrc is covered by an unreadable placeholder.
  const QString out = sh(bwrap(), fake.project(), "ls -A ~/.ssh; cat ~/.netrc; echo end").content;
  QVERIFY_NO_MATCH(out, "id_ed25519|hunter2");
  QVERIFY_MATCH(out, "end$");
}

void TestSandbox::gitIsReadOnly() {
  REQUIRE_BWRAP();
  FakeHome fake;
  const QString out =
      sh(bwrap(), fake.project(), "echo '[core] fsmonitor = evil' >> .git/config; mv .git .git-old; touch .git/hooks/pre-commit; echo end").content;
  QVERIFY_MATCH(out, "Read-only file system|Device or resource busy");
  QCOMPARE(read(fake.home.file(".git/config")), QString("[core]\n"));
  QVERIFY(exists(fake.home.file(".git")));
  QVERIFY(!exists(fake.home.file(".git/hooks/pre-commit")));
}

void TestSandbox::gitWorksReadOnlyInWorktree() {
  REQUIRE_BWRAP();
  FakeHome fake;
  IsolatedGit isolated;
  const QString main = fake.home.file("shop");
  makeRepo(main, {{"login.ts", "check(password)\n"}});
  const Worktree task = createTaskWorktree(main, "fix-login").worktree;
  write(task.path + "/login.ts", "check(password) && password\n");
  // As the runtime does for a worktree: its git data in the main repository is shown read-only.
  const std::shared_ptr<Sandbox> sandbox = bwrap();
  sandbox->setReadOnlyPaths({gitDirs(task.path)->commonDir});

  const QString looked = sh(sandbox, task.path, "git status --short; git diff --stat; git log --format=%s; git branch --show-current").content;
  QVERIFY_MATCH(looked, " M login.ts");
  QVERIFY_MATCH(looked, "1 file changed");
  QVERIFY_MATCH(looked, "Start");
  QVERIFY_MATCH(looked, "smith/fix-login");
  // The rest of the main checkout stays hidden.
  QVERIFY_MATCH(sh(sandbox, task.path, "cat " + main + "/login.ts; echo end").content, "No such file or directory");

  const QString committed = sh(sandbox, task.path, "git add -A; git commit -qm sneaky; git stash; echo end").content;
  QVERIFY_MATCH(committed, "Read-only file system");
  QCOMPARE(git(main, {"log", "--all", "--format=%s"}).trimmed(), QString("Start"));
  QVERIFY(git(task.path, {"status", "--porcelain"}).contains(" M login.ts"));
}

void TestSandbox::secretsInEnvironmentStayOut() {
  REQUIRE_BWRAP();
  FakeHome fake;
  EnvGuard agentKey("AGENT_API_KEY", QString("sk-agent"));
  EnvGuard token("GITHUB_TOKEN", QString("ghp_secret"));
  const QString out = sh(bwrap(), fake.project(), "env").content;
  QVERIFY_NO_MATCH(out, "sk-agent|ghp_secret");
  QVERIFY(QRegularExpression("^PATH=", QRegularExpression::MultilineOption).match(out).hasMatch());
}

void TestSandbox::hiddenSymlinksAreFollowed() {
  REQUIRE_BWRAP();
  FakeHome fake;
  // Dotfile managers make ~/.npmrc or the whole ~/.config a symlink into a repository.
  TempDir dotfiles(homeDir(), ".agent-dotfiles-");
  write(dotfiles.file("config/gh/hosts.yml"), "token: SECRET-gh");
  write(dotfiles.file("npmrc"), "SECRET-npm");
  QVERIFY(QFile::link(dotfiles.file("config"), fake.home.file(".config")));
  QVERIFY(QFile::link(dotfiles.file("npmrc"), fake.home.file(".npmrc")));
  QVERIFY(QFile::link(dotfiles.file("missing"), fake.home.file(".pypirc")));
  const ToolResult res = sh(bwrap(), fake.project(), "cat ~/.npmrc ~/.config/gh/hosts.yml 2>/dev/null; echo end");
  QVERIFY2(!res.isError, qPrintable(res.content));
  QVERIFY_NO_MATCH(res.content, "SECRET");
}

void TestSandbox::networkBlockedUnlessEnabled() {
  REQUIRE_BWRAP();
  FakeHome fake;
  FakeServer server;
  const QString probe = QString("(exec 3<>/dev/tcp/127.0.0.1/%1) 2>/dev/null && echo connected || echo blocked").arg(server.port());
  QCOMPARE(sh(bwrap(), fake.project(), probe).content, QString("blocked"));
  QCOMPARE(sh(bwrap(true), fake.project(), probe).content, QString("connected"));
}

void TestSandbox::tmpIsPrivateAndKept() {
  REQUIRE_BWRAP();
  FakeHome fake;
  const auto sandbox = bwrap();
  // Empty but for the test project, which lives under /tmp here and is mounted in.
  const QString listing =
      sh(sandbox, fake.project(), QString("ls -A /tmp /var/tmp | grep -v : | grep -vx %1 | grep . ; echo end").arg(QFileInfo(fake.project()).fileName()))
          .content;
  QVERIFY2(listing == "end", qPrintable("starts empty: " + listing));
  sh(sandbox, fake.project(), "echo kept > /tmp/a && echo also > /var/tmp/b");
  QCOMPARE(sh(sandbox, fake.project(), "cat /tmp/a /var/tmp/b").content, QString("kept\nalso"));
  // Another sandbox (another agent) has its own.
  QVERIFY_MATCH(sh(bwrap(), fake.project(), "cat /tmp/a 2>&1").content, "No such file or directory");
}

void TestSandbox::fileToolsGoThroughSandbox() {
  REQUIRE_BWRAP();
  FakeHome fake;
  const auto sandbox = bwrap();
  const QString project = fake.project();
  const ToolContext ctx = context(project, sandbox);

  const ToolResult wrote = run(writeFileTool(), {{"path", "deep/er/new.txt"}, {"content", "fresh\n"}}, ctx);
  QVERIFY2(!wrote.isError, qPrintable(wrote.content));
  QCOMPARE(read(project + "/deep/er/new.txt"), QString("fresh\n"));
  QVERIFY_MATCH(run(editFileTool(), {{"path", "deep/er/new.txt"}, {"old_string", "fresh"}, {"new_string", "edited"}}, ctx).content, "Replaced 1 occurrence");
  QVERIFY_MATCH(run(readFileTool(), {{"path", "deep/er/new.txt"}}, ctx).content, "^1\\tedited$");
  try {
    run(readFileTool(), {{"path", "missing.txt"}}, ctx);
    QFAIL("expected ENOENT");
  } catch (const SmithError& error) {
    QCOMPARE(error.code(), QString("ENOENT"));
  }

  // What the sandbox hides or protects stays so, whatever the path checks let through.
  QString netrc;
  try {
    netrc = sandbox->readFile(project + "/.netrc", project);
  } catch (const SmithError& error) {
    netrc = error.message();
  }
  QVERIFY_NO_MATCH(netrc, "hunter2");
  QVERIFY_THROWS_MATCH(sandbox->writeFile("/usr/agent-test/x.txt", "x", project), "only allows writes inside the project");
  QVERIFY_THROWS_MATCH(sandbox->writeFile(project + "/.git/config", "evil", project), "only allows writes inside the project");
  QCOMPARE(read(project + "/.git/config"), QString("[core]\n"));

  // Listing and searching run in it too: hidden folders list as empty.
  const QString listed = run(globTool(), {{"pattern", "**/*"}}, ctx).content;
  QVERIFY_MATCH(listed, "hello\\.txt");
  QVERIFY_NO_MATCH(listed, "id_ed25519");
  QVERIFY_MATCH(run(grepTool(), {{"pattern", "edited"}}, ctx).content, "deep/er/new\\.txt:1:edited");
  QCOMPARE(run(grepTool(), {{"pattern", "PRIVATE KEY"}, {"path", ".ssh"}}, ctx).content, QString("No matches."));
}

void TestSandbox::fileToolsReadSessionTmp() {
  REQUIRE_BWRAP();
  FakeHome fake;
  const auto sandbox = bwrap();
  const ToolContext ctx = context(fake.project(), sandbox);
  run(bashTool(), {{"command", "mkdir -p /tmp/out && echo \"error: boom\" > /tmp/out/log.txt && ln -s /usr/share /tmp/away"}}, ctx);

  QVERIFY2(!reason(readFileTool(), {{"path", "/tmp/out/log.txt"}}, ctx), "no approval needed");
  QVERIFY_MATCH(run(readFileTool(), {{"path", "/tmp/out/log.txt"}}, ctx).content, "^1\\terror: boom$");
  QVERIFY_MATCH(run(globTool(), {{"pattern", "*.txt"}, {"path", "/tmp/out"}}, ctx).content, "log\\.txt");
  QVERIFY_MATCH(run(grepTool(), {{"pattern", "boom"}, {"path", "/tmp/out"}}, ctx).content, "log\\.txt:1:error: boom");
  // A link out of it is a read outside the project, which asks.
  QVERIFY(reason(readFileTool(), {{"path", "/tmp/away/x"}}, ctx).has_value());
  // Without a sandbox, /tmp is the machine's own and stays off limits.
  QVERIFY_THROWS_MATCH(run(readFileTool(), {{"path", "/tmp/out/log.txt"}}, context(fake.project())), "off limits");
}

void TestSandbox::timedOutCommandIsKilledWithChildren() {
  REQUIRE_BWRAP();
  FakeHome fake;
  QElapsedTimer clock;
  clock.start();
  const ToolResult res = sh(bwrap(), fake.project(), "sleep 30 & sleep 30", 300);
  QVERIFY(res.isError);
  QVERIFY_MATCH(res.content, "timed out");
  QVERIFY(clock.elapsed() < 5000);
}

namespace {

// A home with a repository whose subfolder is the project, another project
// with a secret, a tool on PATH, a cache and a shown folder.
struct HomeLayout {
  TempDir home{QDir::tempPath(), "smith-home-"};
  QString repo = home.file("work");
  QString project = repo + "/app";
  EnvGuard homeEnv{"HOME", home.path()};
  EnvGuard pathEnv{"PATH", home.file("tools/bin") + ":" + qEnvironmentVariable("PATH")};

  HomeLayout() {
    QDir().mkpath(repo + "/.git");
    QDir().mkpath(project);
    write(repo + "/README.md", "repo readme\n");
    write(home.file("other/secret.txt"), "hunter2");
    write(home.file("tools/bin/mytool"), "#!/bin/sh\necho tool ran\n");
    chmod(home.file("tools/bin/mytool"), 0755);
    write(home.file("tools/lib.txt"), "lib");
    write(home.file("shown/notes.txt"), "shown notes");
  }

  std::shared_ptr<Sandbox> create() const {
    auto sandbox = bwrap(false, {}, std::nullopt, HomeView{{home.file("shown")}, {home.file("cache")}});
    sandbox->setReadOnlyPaths({repo});
    return sandbox;
  }
};

} // namespace

void TestSandbox::onlyProjectToolsAndChosenFoldersShow() {
  REQUIRE_BWRAP();
  HomeLayout layout;
  const auto sandbox = layout.create();
  const QString out =
      sh(sandbox, layout.project, "cat ~/other/secret.txt 2>&1; mytool; cat ~/tools/lib.txt ~/shown/notes.txt; echo; touch ~/shown/x 2>&1; echo end").content;
  QVERIFY_NO_MATCH(out, "hunter2");
  QVERIFY_MATCH(out, "No such file or directory");
  QVERIFY2(matches(out, "tool ran\\nlib"), qPrintable("a bin folder on PATH shows with its parent: " + out));
  QVERIFY_MATCH(out, "shown notes");
  QVERIFY_MATCH(out, "Read-only file system");

  // Home itself is writable, privately, and kept between commands.
  sh(sandbox, layout.project, "mkdir -p ~/.cache/build && echo cached > ~/.cache/build/x && echo kept > ~/cache/y");
  QCOMPARE(sh(sandbox, layout.project, "cat ~/.cache/build/x").content, QString("cached"));
  QVERIFY2(!exists(layout.home.file(".cache")), "the private home isn't the real one");
  QVERIFY2(read(layout.home.file("cache/y")) == "kept\n", "writable folders are the real ones");

  // The file tools say why a home file is missing.
  QVERIFY_THROWS_MATCH(sandbox->readFile(layout.home.file("other/secret.txt"), layout.project), "home folder is hidden in the sandbox");
  sandbox->dispose();
}

void TestSandbox::repositoryAroundProjectIsReadable() {
  REQUIRE_BWRAP();
  HomeLayout layout;
  const auto sandbox = layout.create();
  const QString out = sh(sandbox, layout.project, "cat ../README.md; touch ../new 2>&1; echo made > here.txt && cat here.txt").content;
  QVERIFY_MATCH(out, "repo readme");
  QVERIFY_MATCH(out, "Read-only file system");
  QVERIFY_MATCH(out, "made$");
  sandbox->dispose();
}

void TestSandbox::failuresComeWithNotes() {
  REQUIRE_BWRAP();
  HomeLayout layout;
  const auto sandbox = layout.create();
  QVERIFY_MATCH(sh(sandbox, layout.project, "touch /usr/smith-test").content,
                "\\[Exit code 1\\]\\n\\[Sandbox: only the working directory, /tmp and the private home folder are writable");
  QVERIFY_MATCH(sh(sandbox, layout.project, "cat ~/other/secret.txt").content, "\\[Sandbox: the home folder is hidden");
  QVERIFY2(!matches(sh(sandbox, layout.project, "cat nothing-here.txt").content, "\\[Sandbox"), "a plain missing file in the project is just that");
  QVERIFY_MATCH(sandbox->explainFailure("curl: (6) Could not resolve host: example.com", layout.project).value_or(""), "network is off");
  QCOMPARE(sandbox->explainFailure("error: expected ';'", layout.project), std::optional<QString>());

  const QString notes = sandbox->promptNotes(layout.project).join('\n');
  QVERIFY_MATCH(notes, "Only the working directory, /tmp and the home folder \\(below\\) are writable");
  QVERIFY_MATCH(notes, "apart from ~/tools, ~/shown \\(read-only\\) and ~/cache \\(writable, and kept\\)");
  QVERIFY_MATCH(notes, "network is off unless the user turns it on");
  sandbox->dispose();
}

void TestSandbox::buildVariablesPassIn() {
  REQUIRE_BWRAP();
  HomeLayout layout;
  EnvGuard cc("CC", QString("gcc-13"));
  EnvGuard auth("NPM_CONFIG__AUTH", QString("token"));
  EnvGuard flag("MY_FLAG", QString("on"));
  const auto sandbox = bwrap(false, {"MY_FLAG", "BUILD_MODE=release"});
  QCOMPARE(sh(sandbox, layout.project, "echo \"$CC|$NPM_CONFIG__AUTH|$MY_FLAG|$BUILD_MODE\"").content, QString("gcc-13||on|release"));
  sandbox->dispose();
}

void TestSandbox::backgroundJobsShareTmp() {
  REQUIRE_BWRAP();
  HomeLayout layout;
  const auto sandbox = layout.create();
  auto jobs = std::make_shared<BackgroundJobs>();
  const ToolContext ctx = context(layout.project, sandbox);
  run(bashTool(jobs),
      {{"command", "cat ~/other/secret.txt 2>&1; sleep 0.5; echo done > /tmp/from-job; echo ready; sleep 30"}, {"run_in_background", true}}, ctx);
  const QString ready = run(jobTools(jobs)[0], {{"id", "bg1"}, {"wait_ms", 10000}, {"until", "ready"}}, ctx).content;
  QVERIFY_NO_MATCH(ready, "hunter2");
  QCOMPARE(sh(sandbox, layout.project, "cat /tmp/from-job").content, QString("done"));
  jobs->clear();
  sandbox->dispose();
}

void TestSandbox::limitsApply() {
  REQUIRE_BWRAP();
  HomeLayout layout;
  const auto sandbox = bwrap(false, {}, Limits{std::nullopt, 64, std::nullopt});
  const SpawnSpec spec = sandbox->spawnSpec("true", layout.project);
  if (spec.file == "prlimit") {
    QCOMPARE(sh(sandbox, layout.project, "ulimit -u").content, QString("64"));
  } else {
    QCOMPARE(spec.file, QString("systemd-run"));
    QVERIFY(spec.args.contains("TasksMax=64"));
    QCOMPARE(sh(sandbox, layout.project, "echo ok").content, QString("ok"));
  }
  // Memory and CPU need systemd; without it, the agent says so.
  const auto partial = bwrap(false, {}, Limits{qint64(1) << 30, 64, std::nullopt});
  if (partial->spawnSpec("true", layout.project).file == "prlimit") {
    QVERIFY_MATCH(partial->warnings().join(','), "memory limit needs systemd-run");
  }
  sandbox->dispose();
  partial->dispose();
}

void TestSandbox::offMeansPlainBashWithoutKeys() {
  EnvGuard agentKey("AGENT_API_KEY", QString("sk-agent"));
  EnvGuard smithKey("SMITH_API_KEY", QString("sk-smith"));
  SandboxOptions options;
  options.type = SandboxType::Off;
  const SpawnSpec spec = Sandbox::create(options)->spawnSpec("true", "/");
  QCOMPARE(spec.file, QString("bash"));
  QVERIFY(!spec.env.contains("AGENT_API_KEY"));
  QVERIFY(!spec.env.contains("SMITH_API_KEY"));

  // Nor anything else that isn't on the short list, unless sandbox.env passes it.
  EnvGuard token("GITHUB_TOKEN", QString("ghp_secret"));
  EnvGuard other("DEPLOY_ENV", QString("staging"));
  const SpawnSpec plain = Sandbox::create(options)->spawnSpec("true", "/");
  QVERIFY(plain.env.contains("PATH"));
  QVERIFY(!plain.env.contains("GITHUB_TOKEN"));
  QVERIFY(!plain.env.contains("DEPLOY_ENV"));
  QVERIFY(!Sandbox::create(options)->programSpec("rg", {}, "/").env.contains("GITHUB_TOKEN"));
  options.env = QStringList{"DEPLOY_ENV", "MODE=test"};
  const SpawnSpec passed = Sandbox::create(options)->spawnSpec("true", "/");
  QCOMPARE(passed.env.value("DEPLOY_ENV"), QString("staging"));
  QCOMPARE(passed.env.value("MODE"), QString("test"));
  QVERIFY(!passed.env.contains("GITHUB_TOKEN"));
}

void TestSandbox::sandboxIsRequired() {
  EnvGuard path("PATH", QString("/nonexistent"));
  QVERIFY_THROWS_MATCH(bwrap(), "bubblewrap.*not installed[\\s\\S]*dnf install bubblewrap[\\s\\S]*--sandbox off");
  SandboxOptions options;
  options.type = SandboxType::Off;
  QVERIFY2(!Sandbox::create(options)->active(), "only an explicit off runs unsandboxed");
}

void TestSandbox::skillFoldersReadableInSandbox() {
  REQUIRE_BWRAP();
  TempDir project(QDir::tempPath(), "agent-proj-");
  TempDir skills(homeDir(), ".agent-test-skills-");
  write(skills.file("hello.sh"), "echo hello from the skill");
  const auto sandbox = bwrap();
  sandbox->setReadOnlyPaths({skills.path()});
  const QString command = QString("bash %1/hello.sh; touch %1/x 2>&1 | grep -o 'Read-only'").arg(skills.path());
  QCOMPARE(sh(sandbox, project.path(), command).content, QString("hello from the skill\nRead-only"));
  sandbox->dispose();
}

void TestSandbox::mountsAreReadAsWritten() {
  const QString home = homeDir();
  Mount mount = parseMount("~/.m2/repository");
  QCOMPARE(mount.source, home + "/.m2/repository");
  QCOMPARE(mount.target, home + "/.m2/repository");
  QVERIFY(!mount.readOnly);
  mount = parseMount("~/.gitconfig:ro");
  QCOMPARE(mount.source, home + "/.gitconfig");
  QCOMPARE(mount.target, home + "/.gitconfig");
  QVERIFY(mount.readOnly);
  mount = parseMount("/data/x:/opt/x:ro");
  QCOMPARE(mount.source, QString("/data/x"));
  QCOMPARE(mount.target, QString("/opt/x"));
  QVERIFY(mount.readOnly);
}

namespace {

const QString MINIMAL = "registry.access.redhat.com/ubi9/ubi-minimal:latest";
const QString FULL = "registry.access.redhat.com/ubi9/ubi:latest";

bool hasImage(const QString& image) {
  QProcess docker;
  docker.start("docker", {"image", "inspect", image});
  return docker.waitForFinished(10000) && docker.exitStatus() == QProcess::NormalExit && docker.exitCode() == 0;
}

bool containersAvailable() {
  static const bool available = hasImage(MINIMAL) && hasImage(FULL);
  return available;
}

QStringList ours() {
  QProcess docker;
  docker.start("docker", {"ps", "-q", "--filter", "label=smith=1"});
  docker.waitForFinished(10000);
  return QString::fromUtf8(docker.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
}

std::shared_ptr<Sandbox> container(const QStringList& mounts, const QList<ContainerProject>& projects, const QStringList& env = {},
                                   const std::optional<Limits>& limits = std::nullopt) {
  SandboxOptions options;
  options.type = SandboxType::Container;
  options.env = env;
  options.limits = limits;
  options.container = ContainerSettings{"docker", MINIMAL, mounts, projects};
  return Sandbox::create(options);
}

} // namespace

void TestSandbox::containerSandbox() {
  if (!containersAvailable()) {
    QSKIP("docker or the ubi9 images aren't available");
  }
  const QStringList before = ours();
  TempDir base(QDir::tempPath(), "smith-container-");
  const QString project = base.file("project");
  const QString shared = base.file("shared");
  write(project + "/.git/config", "[core]\n");
  write(project + "/hello.txt", "hi\n");
  write(shared + "/dep.txt", "a dependency\n");
  write(base.file("secret.txt"), "hunter2");
  const auto sandbox = container({shared + ":ro", base.file("missing")}, {}, {}, Limits{std::nullopt, 256, std::nullopt});
  const ToolContext ctx = context(project, sandbox);
  auto shell = [&](const QString& command, std::optional<int> timeout = std::nullopt) { return sh(sandbox, project, command, timeout).content; };
  auto newContainers = [&]() {
    QStringList fresh;
    for (const QString& id : ours()) {
      if (!before.contains(id)) {
        fresh << id;
      }
    }
    return fresh;
  };

  // Commands run in the image, in the project, with only the project and mounts from here.
  QVERIFY_MATCH(shell("cat /etc/os-release"), "Red Hat Enterprise Linux");
  QCOMPARE(shell("pwd"), project);
  QVERIFY_MATCH(shell("cat hello.txt; echo made > made.txt; cat made.txt"), "^hi\\nmade$");
  QVERIFY2(QFileInfo(project + "/made.txt").ownerId() == ::getuid(), "files made in the project are yours");
  QVERIFY_MATCH(shell(QString("cat %1 2>&1").arg(base.file("secret.txt"))), "No such file or directory");
  QVERIFY_MATCH(shell(QString("cat %1/dep.txt; touch %1/x 2>&1").arg(shared)), "a dependency\\n.*Read-only file system");
  QVERIFY_MATCH(shell("echo x >> .git/config 2>&1; echo end"), "Read-only file system");
  QCOMPARE(read(project + "/.git/config"), QString("[core]\n"));
  QVERIFY2(shell("ls /sys/class/net") == "lo", "no network but loopback");
  QVERIFY_MATCH(sandbox->warnings().join('\n'), "missing doesn't exist; it's skipped");
  QVERIFY_MATCH(sandbox->describe(project), "in a docker container \\(.*ubi-minimal.*\\): only the project and 2 mounts, network off");

  // It describes itself to the model, and explains failures it causes.
  const QString notes = sandbox->promptNotes(project).join('\n');
  QVERIFY_MATCH(notes, "docker container from the image .*ubi-minimal");
  QVERIFY_MATCH(notes, "as the user's own account");
  QVERIFY_MATCH(notes, QString("and %1 \\(read-only\\)").arg(QRegularExpression::escape(shared)));
  QVERIFY_MATCH(shell(QString("touch %1/x").arg(shared)), "\\[Sandbox: the path above is read-only in the container");
  QVERIFY_MATCH(sandbox
                    ->explainFailure("Error: This command has to be run with superuser privileges (under the root user on most systems). Permission denied",
                                     project)
                    .value_or(""),
                "installing system packages isn't possible");

  // One container for the session: /tmp and the like last between commands.
  shell("echo kept > /tmp/state");
  QCOMPARE(shell("cat /tmp/state"), QString("kept"));
  QCOMPARE(newContainers().size(), 1);

  // The file tools: the project directly, everything else from the container, without asking.
  QVERIFY_MATCH(run(writeFileTool(), {{"path", "src/a.txt"}, {"content", "alpha\n"}}, ctx).content, "Created src/a\\.txt");
  QVERIFY_MATCH(run(editFileTool(), {{"path", "src/a.txt"}, {"old_string", "alpha"}, {"new_string", "beta"}}, ctx).content, "Replaced 1");
  QCOMPARE(shell("cat src/a.txt"), QString("beta"));
  QVERIFY(!reason(readFileTool(), {{"path", "/etc/os-release"}}, ctx));
  QVERIFY_MATCH(run(readFileTool(), {{"path", "/etc/os-release"}}, ctx).content, "Red Hat");
  shell("mkdir -p /tmp/logs && echo 'error: boom' > /tmp/logs/build.log");
  QVERIFY_MATCH(run(readFileTool(), {{"path", "/tmp/logs/build.log"}}, ctx).content, "error: boom");
  QVERIFY_MATCH(run(globTool(), {{"pattern", "*.log"}, {"path", "/tmp/logs"}}, ctx).content, "build\\.log");
  QVERIFY_MATCH(run(grepTool(), {{"pattern", "boom"}, {"path", "/tmp/logs"}}, ctx).content, "build\\.log:1:error: boom");
  QVERIFY_MATCH(run(listFilesTool(), {{"path", "/tmp/logs"}}, ctx).content, "^/tmp/logs/ \\(1 file\\)\\n {2}build\\.log$");
  QVERIFY_MATCH(run(grepTool(), {{"pattern", "beta"}}, ctx).content, "src/a\\.txt:1:beta");
  QVERIFY_MATCH(run(globTool(), {{"pattern", "**/*.txt"}}, ctx).content, "hello\\.txt");

  // A timeout kills the command and everything it started, inside the container.
  QElapsedTimer clock;
  clock.start();
  QVERIFY_MATCH(shell("sleep 60 & sleep 60; echo never", 1500), "timed out");
  QVERIFY(clock.elapsed() < 10000);
  QThread::msleep(500);
  QCOMPARE(shell("for p in /proc/[0-9]*; do tr '\\0' ' ' < $p/cmdline 2>/dev/null; echo; done | grep -c '^sleep 60' || true"), QString("0"));

  // Background jobs run in the container: they share it, and stopping one ends it inside.
  {
    auto jobs = std::make_shared<BackgroundJobs>();
    const ToolResult started =
        run(bashTool(jobs), {{"command", "echo $$ > /tmp/job.pid; sleep 0.5; echo ready; sleep 60"}, {"run_in_background", true}}, ctx);
    QVERIFY_MATCH(started.content, "Started background job bg1");
    QVERIFY_MATCH(run(jobTools(jobs)[0], {{"id", "bg1"}, {"wait_ms", 10000}, {"until", "ready"}}, ctx).content, "ready");
    const QString pid = shell("cat /tmp/job.pid");
    QCOMPARE(shell(QString("test -d /proc/%1 && echo alive").arg(pid)), QString("alive"));
    run(jobTools(jobs)[1], {{"id", "bg1"}}, ctx);
    QThread::msleep(500);
    QCOMPARE(shell(QString("test -d /proc/%1 && echo alive || echo gone").arg(pid)), QString("gone"));
    QVERIFY_MATCH(sandbox->promptNotes(project).join(' '), "reachable at localhost from later commands");
    jobs->clear();
  }

  // Switching the network starts a new container; dispose removes them.
  const QString first = shell("cat /etc/hostname");
  sandbox->setNetwork(true);
  QVERIFY(shell("cat /etc/hostname") != first);
  QVERIFY(shell("ls /sys/class/net") != "lo");
  sandbox->setNetwork(false);
  sandbox->dispose();
  QCOMPARE(newContainers(), QStringList());

  // A project can have its own image.
  const auto own = container({}, {ContainerProject{base.path(), FULL, {}}});
  const QString found = sh(own, project, "command -v dnf microdnf").content;
  QVERIFY_MATCH(found, "/dnf");
  QVERIFY_NO_MATCH(found, "microdnf");
  own->dispose();
}

void TestSandbox::containerVariables() {
  if (!containersAvailable()) {
    QSKIP("docker or the ubi9 images aren't available");
  }
  TempDir dir(QDir::tempPath(), "smith-container-env-");
  EnvGuard proxy("HTTPS_PROXY", QString("http://proxy.example:3128"));
  const auto sandbox = container({}, {}, {"BUILD_MODE=release"});
  QCOMPARE(sh(sandbox, dir.path(), "echo \"$BUILD_MODE|$HTTPS_PROXY\"").content, QString("release|"));
  sandbox->setNetwork(true);
  QCOMPARE(sh(sandbox, dir.path(), "echo \"$BUILD_MODE|$HTTPS_PROXY\"").content, QString("release|http://proxy.example:3128"));
  sandbox->dispose();
}

#endif

QTEST_GUILESS_MAIN(TestSandbox)
#include "test_sandbox.moc"
