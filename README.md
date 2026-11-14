# Smith (C++/Qt)

A C++20/Qt 6 port of [smith](https://github.com/erladion/smith): a small agentic coding harness
with a terminal UI. It talks to any OpenAI-compatible `/v1/chat/completions` endpoint
(llama.cpp's `llama-server`, vLLM, Ollama, LM Studio).

Behaviour, flags, config files, session logs and tool logs are the same as the TypeScript
version, so the original README's sections on configuration, endpoints, sandbox settings,
commands and security apply unchanged. Both versions read and write the same
`~/.config/smith/` and `~/.local/share/smith/` folders.

## Building

Needs CMake 3.21+, a C++20 compiler and Qt 6.6+ (Core, Network; Test for the tests;
Quick and Quick Controls 2 for the desktop app, which is skipped when they're missing).

```sh
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=~/Qt/6.11.2/gcc_64
cmake --build build
ctest --test-dir build
./build/apps/smith/smith --help
./build/apps/smith-desktop/smith-desktop
```

The tests are a port of the original TypeScript suite, one QtTest suite per original file
(`units`, `tools`, `sandbox`, `features`, `agent`, `ui`), plus `desktop` for the desktop
app. Each original case keeps its name
and assertions. The agent tests run the real loop against a scripted fake server
(`tests/FakeServer.h`), and the sandbox tests run real commands through bubblewrap. Tests
that need something missing here skip themselves: bwrap, ripgrep, or docker with the ubi9
images for the container tests.

There are two applications, `smith` (the terminal app) and `smith-desktop`, over shared
libraries. Each builds on its own: `-DSMITH_BUILD_DESKTOP=OFF` builds only the terminal app,
`-DSMITH_BUILD_TUI=OFF` only the desktop app, and `-DSMITH_BUILD_TESTS=OFF` skips the tests.
At run time they need bash, and `bubblewrap` for the default sandbox (`--sandbox off` runs without one); `ripgrep`, `fd` and `jq` are used when present.

## Trying it without a model

`scripts/fake_llm.py` is a scripted OpenAI-compatible server (Python 3, no dependencies).
It streams like llama-server and plays a fixed conversation. The tools, approvals and
sandbox are all real; only the model is scripted.

```sh
python3 scripts/fake_llm.py &                 # listens on 127.0.0.1:18090
./build/apps/smith/smith --base-url http://127.0.0.1:18090/v1
```

Ask it to "fix the greeting bug" in a folder with the `greet.py` shown at the top of
the script: it plans with todos, reads the file, asks to edit it (with a diff), asks to run
it, and answers in Markdown. "Set up a database" shows an `ask_user` question.

## The desktop app

`smith-desktop` is the same agent in a window, written in QML. It takes the same folder
argument and flags as `smith` (`-e`, `--base-url`, `--model`, `--sandbox`, `-c`, `-r`, ...),
and uses the same config, preferences, sessions and tool logs, so a session can be started
in one and continued in the other.

- **The conversation:** your messages, the answer rendered as Markdown (through the TUI's
  renderer, so both read it the same way), reasoning folded under "thought", tool calls as
  cards with diffs or their output, and the checklist with its progress.
- **Approvals and questions** replace the input box while they wait. y / a / n / esc answer
  an approval, as in the terminal.
- **The sidebar** holds the project (open a folder, or a recent one) and its sessions: switch,
  start a new one, rename the current one (double-click), delete others; "all" lists every
  folder's.
- **The top bar** picks the endpoint, model and reasoning level, switches the sandbox's
  network, and has undo and compact. The status bar shows the sandbox, how full the context
  is, and background jobs.
- **Slash commands** work in the input box too, with the same completion as the TUI.
- **Keys:** enter sends (while the agent works, it queues the message), shift+enter adds a line,
  esc interrupts, ctrl+n new session, ctrl+o open a folder, ctrl+k pick a model, ctrl+b
  hides the sidebar.

The theme follows `"theme"` in the config: `matrix` (default) is green on near-black in
monospace, with a digital-rain splash that settles into "AGENT SMITH" and Agent Smith's
greetings; `plain` is a neutral dark theme. The `desktop` test loads the QML offscreen and
runs a whole turn through it; with `SMITH_SHOTS_DIR=<dir>` it saves screenshots of each step
(`SMITH_TEST_THEME=plain` for the other theme).

## Layout

```
libs/core/                the agent, no UI. Agent.* is the loop: stream, run tools (read-only
                          ones in parallel), feed results back; compaction
  Http.*                  blocking HTTP and SSE streaming on QNetworkAccessManager, cancellable
  Server.*                one server: model listing, context-size detection
  Context.*               token estimates, trimming, summary prompt, overflow detection
  Session.*, ToolLog.*    JSONL session logs, per-session tool-call log
  Config.*, Preferences.* config file and remembered choices
  Sandbox.*               bubblewrap and "no sandbox"; ContainerSandbox.* for podman/docker
  Process.*, Jobs.*       QProcess runs in their own process group; background jobs
  Tool.*                  tool definitions, JSON Schema validation, output truncation
  tools/                  read_file, write_file, edit_file, list_files, grep, glob, bash,
                          bash_output, bash_kill, skill, task, todo_write, ask_user
  ...                     think-tag parser, diff (Myers), undo checkpoints, run-later files,
                          glob, skills, AGENTS.md loading, system prompt
libs/tui/                 terminal toolkit: raw mode and signals, inline renderer, keys, line
                          editor, Markdown, pickers, digital rain. Includes nothing else.
libs/frontend/            what both apps share: Runtime (server, model, sandbox, project,
                          session and agents from the flags and config), the frontend hooks,
                          transcript entries and session replay, commands and completion,
                          greetings, spinner frames, durations
apps/smith/               the terminal app: App (screen, keys, turns), session commands,
                          endpoint/model switching, entry rendering, status line; main.cpp
                          has the flags and the headless (-p) mode
apps/smith-desktop/       the desktop app: Backend (the QML-facing controller), the transcript
                          model, palette, Markdown to rich text, the rain item, qml/
tests/                    QtTest suites; FakeServer.h is a scripted streaming server
```

The `tools` suite checks the boundaries: the libraries never include the apps, and the two
apps never include each other.

## How the port is put together

- **Threads instead of promises.** The agent's methods block. The TUI runs each turn on a
  worker thread; agent events reach the UI thread as queued calls, and approvals and
  `ask_user` questions block the worker until the user answers on the UI thread. Esc sets
  a cancel token that HTTP requests, processes and job waits all poll.
- **Parallel tools.** Consecutive read-only tool calls each get a thread; results go back to
  the model in call order. Approvals are still asked one at a time.
- **The TUI is hand-written**, like the original: termios raw mode, `QSocketNotifier` on
  stdin, SIGWINCH/SIGTERM through a self-pipe, and an inline renderer that only redraws the
  live region at the bottom. Grapheme segmentation uses `QTextBoundaryFinder`.

## Differences from the TypeScript version

- **Proxies and certificates.** Qt reads `http_proxy`/`https_proxy`/`no_proxy`; the uppercase
  names are copied over at startup. Certificates come from the system store through
  OpenSSL; for an extra CA, point `SSL_CERT_FILE` at a bundle that includes it (instead of
  `NODE_EXTRA_CA_CERTS`).
- **JSON key order.** Qt's JSON objects sort their keys, so keys in the JSON Smith writes
  (sessions, logs, preferences) come out sorted, which doesn't affect reading them.
  Endpoints in the config file still keep the order the file lists them in.
- **Deployment.** The binary links Qt dynamically. To run it on another machine, ship the Qt
  libraries with it (or build against a static Qt) instead of the Node single-binary builds.
