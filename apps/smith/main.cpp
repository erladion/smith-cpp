#include "frontend/Runtime.h"
#include "core/Images.h"
#include "core/Agent.h"
#include "core/Config.h"
#include "core/Dirs.h"
#include "core/Errors.h"
#include "core/Http.h"
#include "core/Instructions.h"
#include "core/Jobs.h"
#include "core/Preferences.h"
#include "core/Prompt.h"
#include "core/Reasoning.h"
#include "core/Sandbox.h"
#include "core/Server.h"
#include "core/Session.h"
#include "core/Skills.h"
#include "core/ToolLog.h"
#include "core/Themes.h"
#include "core/Toolchain.h"
#include "core/tools/Tools.h"
#include "tui/Terminal.h"
#include "App.h"
#include "frontend/Duration.h"
#include "frontend/Folders.h"
#include "frontend/Greetings.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLocale>
#include <QMap>
#include <QMutex>
#include <QRegularExpression>

#include <csignal>
#include <cstdio>
#include <iostream>
#include <string>

using namespace Smith;

namespace {

QString helpText() {
  return QString(R"(Usage: smith [options] [folder]

Works in [folder], or the current directory. That folder is the project: the only place the
agent can change files.

Server:
  -e, --endpoint <name>  Use an endpoint from the config file (default: last used, else the config's default)
  --endpoints            List configured endpoints
  --base-url <url>       Use this OpenAI-compatible URL instead of a configured endpoint (env SMITH_BASE_URL)
  --api-key <key>        API key (for --base-url also env SMITH_API_KEY or OPENAI_API_KEY)
  --model <name>         Model (env SMITH_MODEL; default: last used on this server, else the
                         endpoint's configured model, else the first listed)
  --reasoning <level>    %1 (default: last used, else "default")
  --context-size <n>     Context window in tokens, if the server doesn't report it
  --temperature <n>      Sampling temperature (default: the server's)
  --max-steps <n>        Model calls allowed per message (default: maxSteps in the config file, else 50)

Sandbox (where shell commands and the file tools run):
  --sandbox <type>       bwrap | container | off (default: sandbox.type in the config file, else
                         bwrap; Smith won't start if it can't run. "on" means bwrap. off runs
                         commands unsandboxed and must be asked for on every run)
  --network              Allow sandboxed commands to use the network (default: blocked)

Sessions:
  -c, --continue         Resume the most recent session in this directory
  -r, --resume <id|name> Resume a session by id or name (in the folder it was in)
  --sessions             List saved sessions from every folder (smith <folder> --sessions: only that one)
  --delete <id|name>     Delete a session and its tool log (asks first; --yes doesn't)

One-shot:
  -p, --print <prompt>   Run one prompt without the TUI and print the answer
  -y, --yes              With --print, allow every tool call without asking; with --delete, don't ask

  -v, --version          Show the version
  -h, --help             Show this help

Endpoints are configured in ~/.config/smith/config.json; run --endpoints for an example.)")
      .arg(REASONING_LEVELS.join(" | "));
}

void out(const QString& text) {
  std::fputs((text + "\n").toUtf8().constData(), stdout);
  std::fflush(stdout);
}

void err(const QString& text) {
  std::fputs(text.toUtf8().constData(), stderr);
  std::fflush(stderr);
}

void printEndpoints(const Config& config) {
  if (config.endpoints.isEmpty()) {
    out(QString("No endpoints configured. Create %1, for example:\n\n%2").arg(config.file, EXAMPLE_CONFIG));
    out("\nUse \"apiKeyEnv\" to read a key from an environment variable instead of storing it in the file.");
    return;
  }
  out(QString("From %1:\n").arg(config.file));
  for (const Endpoint& e : config.endpoints) {
    QStringList marks;
    if (e.name == config.defaultEndpoint) {
      marks << "default";
    }
    if (!e.apiKey.isEmpty()) {
      marks << "key";
    }
    out(QString("%1 %2 %3 %4").arg(e.name.leftJustified(16), e.baseUrl.leftJustified(36), e.model.leftJustified(24), marks.join(", ")));
  }
  for (const QString& warning : config.warnings) {
    out("\nwarning: " + warning);
  }
}

// Every saved session (or one folder's): name, where it is, when, how long, which model.
void printSessions(const QString& cwd) {
  const QList<SessionInfo> sessions = listSessions(cwd);
  if (sessions.isEmpty()) {
    out(cwd.isEmpty() ? "No saved sessions yet." : "No saved sessions for this folder.");
    return;
  }
  const QStringList header = {"NAME", "FOLDER", "LAST USED", "MSGS", "MODEL", "ID"};
  QList<QStringList> rows;
  for (const SessionInfo& s : sessions) {
    rows.append({s.name, shortPath(s.cwd, 40), QLocale(QLocale::English).toString(s.updated, "M/d/yy, h:mm AP"), QString::number(s.turns),
                 s.endpoint.isEmpty() ? s.model : s.endpoint + "/" + s.model, s.id});
  }
  QList<int> widths;
  for (int i = 0; i < header.size(); i++) {
    int w = header[i].size();
    for (const QStringList& row : rows) {
      w = std::max(w, int(row[i].size()));
    }
    widths << std::min(i < 2 ? 48 : 40, w);
  }
  auto fit = [&](const QStringList& cells) {
    QStringList shown;
    for (int i = 0; i < cells.size(); i++) {
      shown << (cells[i].size() > widths[i] ? cells[i].left(widths[i] - 1) + "…" : cells[i].leftJustified(widths[i]));
    }
    QString line = shown.join("  ");
    while (line.endsWith(' ')) {
      line.chop(1);
    }
    return line;
  };
  out(fit(header));
  for (const QStringList& row : rows) {
    out(fit(row));
  }
  out(QString("\n%1. Resume one with: smith --resume <id or name>").arg(plural(sessions.size(), "session")));
}

// smith --delete: shows the session, asks (unless --yes), and deletes it with its tool log.
void deleteFromCommandLine(const QString& idOrName, bool yes) {
  const QString id = findSession(idOrName);
  QString what = id;
  for (const SessionInfo& info : listSessions()) {
    if (info.id == id) {
      what = QString("\"%1\" (%2) in %3: %4, last used %5")
                 .arg(info.name, id, shortPath(info.cwd, 60), plural(info.turns, "message"), QLocale(QLocale::English).toString(info.updated));
    }
  }
  if (!yes) {
    if (!Tui::Terminal::isInteractive()) {
      throw SmithError(QString("Not deleting %1 without asking; pass --yes to delete it anyway.").arg(what));
    }
    std::fputs(QString("Delete %1, and its tool log? [y/N] ").arg(what).toUtf8().constData(), stdout);
    std::fflush(stdout);
    std::string answer;
    std::getline(std::cin, answer);
    static const QRegularExpression accept("^y(es)?$", QRegularExpression::CaseInsensitiveOption);
    if (!accept.match(QString::fromStdString(answer).trimmed()).hasMatch()) {
      out("Nothing deleted.");
      return;
    }
  }
  deleteSession(id);
  out(QString("Deleted %1.").arg(what));
}

// Cancelled by SIGINT in headless mode.
CancelPtr g_headlessCancel;

void onHeadlessInterrupt(int) {
  if (g_headlessCancel) {
    g_headlessCancel->cancel();
  }
}

int runHeadless(const std::function<std::shared_ptr<Agent>(const AgentHooks&)>& createAgent, const QString& prompt, bool allowAll,
                const QString& baseUrl) {
  g_headlessCancel = makeCancel();
  std::signal(SIGINT, onHeadlessInterrupt);

  auto lock = std::make_shared<QMutex>();
  int toolCalls = 0;
  QString reason;
  AgentHooks hooks;
  hooks.onEvent = [&, lock](const AgentEvent& event) {
    QMutexLocker guard(lock.get());
    std::visit(Overloaded{
                   [](const Event::Delta& delta) {
                     if (delta.kind == BlockKind::Text) {
                       std::fputs(delta.text.toUtf8().constData(), stdout);
                       std::fflush(stdout);
                     }
                   },
                   [](const Event::ResponseEnd&) {
                     std::fputs("\n", stdout);
                     std::fflush(stdout);
                   },
                   [&](const Event::ToolEnd& tool) {
                     toolCalls++;
                     err(QString("[%1 %2] %3\n").arg(tool.isError ? "✗" : "✓", tool.name, tool.summary));
                   },
                   [&](const Event::Notice& notice) {
                     const QString level = notice.level == NoticeLevel::Info ? "info" : notice.level == NoticeLevel::Warn ? "warn" : "error";
                     err(QString("[%1] %2\n").arg(level, notice.text));
                   },
                   [&](const Event::Done& done) { reason = doneReasonName(done.reason); },
                   [](const auto&) {},
               },
               event);
  };
  hooks.approve = [allowAll](const ApprovalRequest& request) {
    if (allowAll) {
      return ApprovalDecision::Allow;
    }
    err(QString("[denied %1] %2 (pass --yes to allow tools)\n").arg(request.tool, request.summary));
    return ApprovalDecision::Deny;
  };

  const std::shared_ptr<Agent> agent = createAgent(hooks);
  QElapsedTimer clock;
  clock.start();
  int code = 0;
  try {
    // Image files the prompt names go along with it.
    const ImageRefs refs = imagesIn(prompt, QDir::currentPath());
    if (!refs.attached.isEmpty()) {
      err(QString("[attached %1]\n").arg(refs.attached.join(", ")));
    }
    for (const QString& problem : refs.problems) {
      err(QString("[warn] %1\n").arg(problem));
    }
    agent->send(prompt, g_headlessCancel, refs.images);
  } catch (const std::exception& error) {
    err(describeError(error, baseUrl) + "\n");
    code = 1;
  }
  err(QString("[%1]\n").arg(turnSummary(reason, clock.elapsed(), toolCalls)));
  return code;
}

int run(QCoreApplication& app) {
  Http::useSystemNetworkSettings();
  for (const QString& note : moveOldFolders()) {
    err(note + "\n");
  }
  secureSessions();

  QCommandLineParser parser;
  addLaunchOptions(parser);
  parser.addOptions({
      QCommandLineOption("endpoints"),
      QCommandLineOption("sessions"),
      QCommandLineOption("delete", "", "id"),
      QCommandLineOption(QStringList{"p", "print"}, "", "prompt"),
      QCommandLineOption(QStringList{"y", "yes"}),
      QCommandLineOption(QStringList{"h", "help"}),
      QCommandLineOption(QStringList{"v", "version"}),
  });
  if (!parser.parse(app.arguments())) {
    throw SmithError(parser.errorText());
  }
  if (parser.isSet("help")) {
    out(helpText());
    return 0;
  }
  if (parser.isSet("version")) {
    out(QString("smith %1").arg(SMITH_VERSION));
    return 0;
  }
  const LaunchOptions launch = readLaunchOptions(parser);
  if (parser.isSet("sessions")) {
    printSessions(launch.folder.isEmpty() ? QString() : resolveFolder(launch.folder, QDir::currentPath()));
    return 0;
  }
  if (parser.isSet("delete")) {
    deleteFromCommandLine(parser.value("delete"), parser.isSet("yes"));
    return 0;
  }
  if (parser.isSet("endpoints")) {
    printEndpoints(loadConfig());
    return 0;
  }
  const bool headless = parser.isSet("print");
  if (!headless && !Tui::Terminal::isInteractive()) {
    throw SmithError("The interactive UI needs a terminal. Use -p <prompt> for non-interactive runs.");
  }

  const std::shared_ptr<Runtime> runtime = Runtime::start(launch);
  const Config& config = runtime->config();
  const ServerPtr server = runtime->server();

  if (headless) {
    for (const QString& warning : runtime->warnings()) {
      err("[warn] " + warning + "\n");
    }
    err(QString("[%1 · %2 · %3]\n").arg(server->label(), runtime->model(), runtime->sandbox()->describe(runtime->projectCwd())));
    const int code = runHeadless([&](const AgentHooks& hooks) { return runtime->createAgent(hooks); }, parser.value("print"), parser.isSet("yes"),
                                 server->baseUrl());
    // Their open output pipes would keep the process alive.
    runtime->shutdown();
    err(QString("[session %1]\n").arg(runtime->session()->id()));
    return code;
  }

  Ui::TuiOptions options;
  options.controller = controllerOptions(runtime);
  options.controller.theme = startingTheme(config.theme, &options.controller.warnings);
  options.codeStream = config.codeStream;

  QElapsedTimer started;
  started.start();
  {
    Ui::App tui(options);
    QObject::connect(&tui, &Ui::App::finished, &app, &QCoreApplication::quit, Qt::QueuedConnection);
    tui.run();
    app.exec();
  }
  runtime->shutdown();
  const std::shared_ptr<Session> session = runtime->session();
  const QString name = session->name();
  static const QRegularExpression whitespace(R"(\s)");
  const QString resumeWith = !name.isEmpty() && !whitespace.match(name).hasMatch() ? name : session->id();
  // As /theme left it.
  const QString goodbye = startingTheme(config.theme).base == Theme::Matrix ? farewell(started.elapsed()) + "\n" : QString();
  out(QString("%1Session %2%3. Resume with: smith --resume %4").arg(goodbye, name.isEmpty() ? QString() : "\"" + name + "\" ", session->id(), resumeWith));
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName("smith");
  QCoreApplication::setApplicationVersion(SMITH_VERSION);
  try {
    return run(app);
  } catch (const std::exception& error) {
    err(errorMessage(error) + "\n");
    return 1;
  }
}
