# Smith (C++/Qt)

A C++20/Qt 6 port of [smith](https://github.com/erladion/smith): a small agentic coding harness
with a terminal UI. It talks to any OpenAI-compatible `/v1/chat/completions` endpoint
(llama.cpp's `llama-server`, vLLM, Ollama, LM Studio).

Behaviour, flags, config files, session logs and tool logs are the same as the TypeScript
version, so the original README's sections on configuration, endpoints, sandbox settings,
commands and security apply unchanged. Both versions read and write the same
`~/.config/smith/` and `~/.local/share/smith/` folders.

## Building

Needs CMake 3.21+, a C++20 compiler and Qt 6.5+ (Core, Network; Test for the tests;
Quick and Quick Controls 2 for the desktop app, which is skipped when they're missing).

```sh
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=~/Qt/6.11.2/gcc_64
cmake --build build
ctest --test-dir build
./build/smith --help
./build/smith-desktop       # the desktop app
```

The tests are a port of the original TypeScript suite, one QtTest suite per original file
(`units`, `tools`, `sandbox`, `features`, `agent`, `ui`), plus `desktop` for the desktop
app. Each original case keeps its name
and assertions. The agent tests run the real loop against a scripted fake server
(`tests/FakeServer.h`), and the sandbox tests run real commands through bubblewrap. Tests
that need something missing here skip themselves: bwrap, ripgrep, or docker with the ubi9
images for the container tests.

`-DSMITH_BUILD_TESTS=OFF` skips the tests, `-DSMITH_BUILD_DESKTOP=OFF` the desktop app. At run time it needs bash, and `bubblewrap` for
the default sandbox (`--sandbox off` runs without one); `ripgrep`, `fd` and `jq` are used when present.

## Trying it without a model

`scripts/fake_llm.py` is a scripted OpenAI-compatible server (Python 3, no dependencies).
It streams like llama-server and plays a fixed conversation. The tools, approvals and
sandbox are all real; only the model is scripted.

```sh
python3 scripts/fake_llm.py &                 # listens on 127.0.0.1:18090
./build/smith --base-url http://127.0.0.1:18090/v1
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
src/core/Agent.*          the loop: stream, run tools (read-only ones in parallel), feed results back; compaction
src/core/Http.*           blocking HTTP and SSE streaming on QNetworkAccessManager, cancellable
src/core/Server.*         one server: model listing, context-size detection
src/core/Context.*        token estimates, trimming, summary prompt, overflow detection
src/core/Session.*        JSONL session logs        src/core/ToolLog.*   per-session tool-call log
src/core/Config.*         config file               src/core/Preferences.*
src/core/Sandbox.*        bubblewrap and "no sandbox"; ContainerSandbox.* for podman/docker
src/core/Process.*        QProcess runs in their own process group, with timeout and cancel
src/core/Jobs.*           background jobs, on a thread of their own
src/core/Tool.*           tool definitions, JSON Schema validation, output truncation
src/core/tools/           read_file, write_file, edit_file, list_files, grep, glob, bash,
                          bash_output, bash_kill, skill, task, todo_write, ask_user
src/core/...              think-tag parser, diff (Myers), undo checkpoints, run-later files,
                          glob, skills, AGENTS.md loading, system prompt
src/tui/                  terminal toolkit: raw mode and signals, inline renderer, keys,
                          line editor, Markdown, pickers, digital rain. Doesn't include src/core.
src/ui/                   the agent's TUI: App (screen, keys, turns), commands, sessions,
                          endpoint/model switching, transcript entries, status line
src/app/                  what every frontend shares: Runtime (server, model, sandbox, project,
                          session and agents from the flags and config) and the frontend hooks
src/desktop/              the desktop app: Backend (the QML-facing controller), the transcript
                          model, palette, Markdown to rich text, the rain item, qml/
src/main.cpp              flags, the TUI or headless (-p) mode
tests/                    QtTest suites; FakeServer.h is a scripted streaming server
```

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
