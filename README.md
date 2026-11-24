# Smith

Smith is a coding agent that runs on your own models. It talks to any OpenAI-compatible
`/v1/chat/completions` server (llama.cpp's `llama-server`, vLLM, Ollama, LM Studio or a hosted
API) and works in one project folder at a time. It reads code, edits files, runs commands and
plans with a checklist, and it asks before anything that changes your project.

Commands run in a sandbox (bubblewrap) that can only write to the project. Your home folder,
credentials and the network are hidden from it unless you share them. Every turn can be
undone, and task worktrees let the agent work on a separate branch while you keep your own
checkout.

There are two apps over the same agent: `smith`, for the terminal, and `smith-desktop`, a
window written in QML. They share settings and sessions, so you can start a session in one and
continue it in the other. Smith is a C++20/Qt port of [smith](https://github.com/erladion/smith)
and reads and writes the same files as the TypeScript version.

## Quick start

On Linux, with CMake, a C++20 compiler and Qt 6.6+ or Qt 5.15 installed (see
[Building](#building) for the details):

```sh
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=~/Qt/6.11.2/gcc_64
cmake --build build
./build/apps/smith/smith --base-url http://localhost:8080/v1 ~/git/my-project
```

To keep your servers in a config file instead, create `~/.config/smith/config.json`:

```json
{
  "default": "local",
  "endpoints": {
    "local": { "baseURL": "http://localhost:8080/v1" },
    "gpu-box": { "baseURL": "http://10.0.0.5:8000/v1", "apiKeyEnv": "GPU_BOX_KEY", "model": "qwen3-coder-30b" }
  }
}
```

Then run `smith` in a project folder, or `smith-desktop` for the window. To try Smith without a
model, see [Trying it without a model](#trying-it-without-a-model).

At run time Smith needs `bash`, and `bubblewrap` for the default sandbox. It uses `ripgrep`,
`fd` and `jq` when they're installed.

## Command-line options

`smith [options] [folder]` works in `folder`, or the current directory if none is given. That
folder is the project: the only place the agent can change files. `smith-desktop` takes the same
folder argument and the options marked "both".

| Option | App | What it does |
|---|---|---|
| `-e`, `--endpoint <name>` | both | Use an endpoint from the config file. Default: the last one used, else the config's `default`. |
| `--endpoints` | smith | List the configured endpoints. |
| `--base-url <url>` | both | Use this OpenAI-compatible URL instead of a configured endpoint. |
| `--api-key <key>` | both | The API key for `--base-url`. |
| `--model <name>` | both | The model. Default: the last one used on this server, else the endpoint's `model`, else the first the server lists. |
| `--reasoning <level>` | both | How much the model thinks: `default`, `off`, `low`, `medium` or `high`. Default: the last one used. |
| `--context-size <n>` | both | The context window in tokens, for a server that doesn't report it. |
| `--temperature <n>` | both | The sampling temperature, over any in the config's `params`. Default: the server's. |
| `--max-steps <n>` | both | Model calls allowed per message. Default: `maxSteps` in the config, else 50. |
| `--sandbox <type>` | both | `bwrap` (default), `container` or `off`. Smith won't start if the sandbox can't run. `off` has to be given on every run. |
| `--network` | both | Let sandboxed commands use the network (blocked by default). |
| `-c`, `--continue` | both | Resume the most recent session in this folder. |
| `-r`, `--resume <id\|name>` | both | Resume a session by id or name, in the folder it was in. |
| `--sessions` | smith | List saved sessions from every folder (`smith <folder> --sessions`: only that folder's). |
| `--delete <id\|name>` | smith | Delete a session and its tool log. Asks first, unless `--yes` is given. |
| `-p`, `--print <prompt>` | smith | Run one prompt without the interface. The answer goes to stdout and tool calls to stderr. |
| `-y`, `--yes` | smith | With `--print`, allow every tool call without asking (otherwise those that would ask are refused). With `--delete`, don't ask. |
| `-v`, `--version` | smith | Show the version. |
| `-h`, `--help` | smith | Show the help. |

## Slash commands

Type these in the input box of either app. Tab completes them, and their arguments where it can.

| Command | What it does |
|---|---|
| `/help` | Show the commands and keys. |
| `/endpoint [name]` | Pick a configured endpoint, or switch to one by name. |
| `/model [name]` | Pick a model from the server, or switch to one by name. |
| `/reasoning [level]` | Pick how much the model thinks: `default`, `off`, `low`, `medium` or `high`. |
| `/theme [name]` | Pick how Smith looks: `matrix`, `plain`, `light` or [one of yours](#themes). It switches at once and is remembered. |
| `/sessions [all]` | Switch to another session from this folder (`all`: from every folder). |
| `/name [name]` | Show or set the current session's name. |
| `/open` | Pick a project folder: a recent one, or browse to one. |
| `/new [folder]` | Start a new session, here or in another folder. |
| `/clear` | Start a new session (same as `/new`). |
| `/plan [on\|off]` | Switch [plan mode](#plan-mode): the agent reads and plans, but changes nothing until you switch it off. |
| `/diff` | Show what the agent changed in files this session, file by file, with the whole diff of each. |
| `/undo` | Undo the last turn: restore the files it changed, and edit your message again. |
| `/compact` | Summarize the conversation now, to free context. Smith also does this on its own when the context fills. |
| `/worktree <task>` | Give a task its own worktree: the agent works on a `smith/<task>` branch beside your checkout. |
| `/worktrees` | List the task worktrees: switch to one, land it or drop it. |
| `/land [task]` | Commit a task's changes and bring them to your branch (cherry-pick or merge). |
| `/drop [task]` | Remove a task's worktree and branch, discarding what isn't landed. |
| `/permissions [reset]` | Show what runs here without asking, or forget what you allowed. |
| `/network on\|off` | Allow or block network access for sandboxed commands. |
| `/jobs [stop id\|all]` | List the commands the agent runs in the background, or stop one or all of them. |
| `/skills` | List the available skills. |
| `/exit` | Quit. |

## Keys

| Key | Terminal | Desktop |
|---|---|---|
| enter | Send. While the agent works, it adds the message to what the agent is doing, at its next step. | Same. |
| shift+enter | New line (so do `\` then enter, and ctrl+j). | New line. |
| esc | Interrupt the agent. | Same. |
| shift+tab | Switch plan mode. | Same, in the input box. |
| ↑ / ↓ | Recall earlier input. | |
| tab | Complete a slash command, or the file after `@`. While the agent works, queue the text as the next task. | Same. |
| ↑ (agent working, empty box) | Take the last queued task back to change it. | Click a queued task; × drops it. |
| ctrl+c | Quit. | |
| ctrl+n / ctrl+o / ctrl+k | | New session / open a folder / pick a model. |
| ctrl+b | | Show or hide the sidebar. |
| ctrl+q | | Quit. |

When the agent asks for approval, answer with a key:

| Key | Answer |
|---|---|
| `y` | Allow this once. |
| `a` | Always allow this tool in this project. Not offered for `bash` without the sandbox. |
| `s` | For a shell command: always allow commands that start the same way, such as `npm test` (see [Permissions](#permissions)). |
| `n` | Refuse. The model is told and carries on. |
| esc | Refuse and interrupt the turn. |

## Configuration

Smith reads `~/.config/smith/config.json` (or `$XDG_CONFIG_HOME/smith/config.json`). The file is
optional; an unknown key or a wrong value stops Smith with a message saying where. Run
`smith --endpoints` for an example.

| Key | Default | What it does |
|---|---|---|
| `default` | | The endpoint to use when none is given and none was used before. |
| `endpoints.<name>.baseURL` | | The server's OpenAI-compatible URL, such as `http://localhost:8080/v1`. Required. |
| `endpoints.<name>.apiKey` | | The API key. |
| `endpoints.<name>.apiKeyEnv` | | The environment variable to read the key from, so it isn't stored in the file. Not together with `apiKey`. |
| `endpoints.<name>.model` | | The model to use when you haven't picked one on this server. |
| `endpoints.<name>.contextSize` | | The context window in tokens, for a server that doesn't report it. |
| `endpoints.<name>.params` | | Fields sent with every request to this server, such as [sampling settings](#sampling-settings-and-loops). |
| `endpoints.<name>.loopDetection` | | `loopDetection` for this server. |
| `models.<name>` | | `params` and `loopDetection` for a model on any server. `<name>` is its name, or a pattern such as `"qwen3*"`. |
| `loopDetection` | `false` | Stop a response, or a run of tool calls, that keeps [repeating itself](#sampling-settings-and-loops), and ask the model to move on. |
| `maxSteps` | `50` | Model calls allowed per message you send. |
| `subagents` | `true` | Whether the model can hand work to sub-agents (the `task` tool). |
| `toolLog` | `true` | Whether tool calls are logged to `~/.local/share/smith/logs/<session>.jsonl`. |
| `theme` | `"matrix"` | The [theme](#themes): `"matrix"`, `"plain"`, `"light"` or one of yours. A theme picked with `/theme` takes precedence. |
| `codeStream` | `true` | With the matrix theme: the idle stream of glyphs in the status line. |
| `protectRunLater` | `true` | Changes to files that run outside the sandbox later always ask, and are reported (see [Files that run later](#files-that-run-later)). |
| `permissions.allow` | `[]` | What runs without asking: a tool's name, or `"bash:<start of a command>"`. |
| `permissions.deny` | `[]` | What is always refused, in the same form. |
| `sandbox.type` | `"bwrap"` | `"bwrap"` or `"container"`. `off` can only be given on the command line. |
| `sandbox.network` | `false` | Whether sandboxed commands can use the network. |
| `sandbox.env` | `[]` | More environment variables for commands: `"NAME"` passes yours, `"NAME=value"` sets one. |
| `sandbox.limits.memory` | | A memory limit per command, such as `"8g"` or `"512m"`. |
| `sandbox.limits.processes` | | A limit on processes per command. |
| `sandbox.limits.cpus` | | A CPU limit per command, such as `2` or `0.5`. |
| `sandbox.home.show` | `[]` | Folders in your home folder that commands can read, such as `"~/.m2"`. |
| `sandbox.home.writable` | `[]` | Folders in your home folder that commands can write to, and that keep what they write. |
| `sandbox.container.runtime` | `"podman"` | For `container`: `"podman"` or `"docker"`. |
| `sandbox.container.image` | ubi9 | For `container`: the image (default `registry.access.redhat.com/ubi9/ubi:latest`). |
| `sandbox.container.mounts` | `[]` | For `container`: more folders or files to mount, as `"src"`, `"src:ro"`, `"src:dst"` or `"src:dst:ro"`. |
| `sandbox.projects.<folder>` | | For `container`: an `image` and `mounts` for projects in that folder. The most specific folder wins. |

Smith also remembers some choices between runs, in `~/.config/smith/preferences.json`: the last
endpoint, the model per server, the reasoning level, the theme picked with `/theme`, and what you
allowed per project.

### Sampling settings and loops

Smith sends the model, the messages and your `/reasoning` level; how the model samples is up to
the server unless you say. A thinking model sampled with chat defaults, or too greedily, can
go round in circles in its reasoning. Give it the settings from its model card with `params`:

```json
{
  "models": {
    "qwen3*": { "params": { "temperature": 0.6, "top_p": 0.95, "top_k": 20, "min_p": 0, "presence_penalty": 1.5 } }
  },
  "endpoints": {
    "local": { "baseURL": "http://localhost:8080/v1", "params": { "presence_penalty": 1.0 } }
  }
}
```

- `params` go with every request, as they are. On a model's entry they follow it to any server;
  on an endpoint they're for everything that server runs. A model's entry wins over its
  endpoint's, an exact name over a pattern, and the first pattern that matches over later ones.
  `chat_template_kwargs` and other objects are merged rather than replaced.
- `temperature`, `top_p`, `presence_penalty` and `frequency_penalty` are part of the OpenAI API,
  so any server takes them. `top_k`, `min_p` and `repetition_penalty` work with llama.cpp, vLLM
  and SGLang; some servers ignore them, and a strict proxy may refuse the request, which then
  says so.
- What you choose for a run wins over `params`: `--temperature`, and `/reasoning` (which sets
  `reasoning_effort` and `chat_template_kwargs.enable_thinking`). The fields Smith writes itself
  (`model`, `messages`, `stream`, `tools`) can't be in `params`.

With `"loopDetection": true` (at the top, on an endpoint or on a model), Smith watches what the
model writes as it streams. When the same passage has come four times, it stops the response,
says so, and asks again with a note to stop going over it and take the next step. If the next
response loops too, the turn ends there.

It also watches the tool calls. When the model makes the same call a third time in a turn and it
gives the same result again, Smith adds a note to that result: running it again won't change
anything, so try something else. If a call repeats like that once more, the turn ends. A call
whose result has changed, such as a build after an edit, doesn't count, and the count starts over
when the conversation is compacted.

It is off by default: a response that really does repeat a passage, the same block of code four
times say, would be cut short.

### Permissions

Reading and searching inside the project never asks. Editing files, running commands, and
reading outside the project ask first. An "always" answer is remembered for the project, and
the config file can allow or refuse things everywhere:

```json
"permissions": { "allow": ["edit_file", "bash:npm test"], "deny": ["bash:git push"] }
```

- A `bash:` rule covers one program and its arguments: `bash:npm test` allows `npm test` and
  `npm test src/a.test.js`. It never allows a command that chains or substitutes others (`;`,
  `|`, `&&`, `$(...)`).
- With `--sandbox off`, rules apply more narrowly. There an option or a quoted argument can make
  a program run anything (`git -c`, `find -exec`), so only plain arguments (names and paths) can
  follow the rule, and "always allow bash" isn't offered.
- A deny rule refuses the command wherever it appears in a command line, however it is written:
  `cd x && git push`, `bash -c 'git push'` and `/usr/bin/git -C . push` are all refused. It guards
  against mistakes, not against everything: a script or a git alias that pushes isn't seen.
- `/permissions` lists what runs without asking here, and `/permissions reset` forgets what
  you allowed in this project.

### Themes

`/theme` switches the theme in either app, at once, and remembers the choice. There are three
built in:

| Theme | Look |
|---|---|
| `matrix` | Green on black in monospace, with digital rain at startup, a katakana spinner and Agent Smith's greetings. |
| `plain` | A neutral dark look, with none of that. |
| `light` | The same, dark on light. In the terminal it keeps your terminal's background, so it's for a terminal with a light one. |

In the terminal, the themes use the same colours as the desktop app when the terminal shows
24-bit colour (Smith checks `COLORTERM`, Windows Terminal and `TERM`), or the nearest of 256
colours. With only 16 colours, the terminal's own shades stay.

**Your own themes** are JSON files in `~/.config/smith/themes/`. The file's name is the theme's
name, so `~/.config/smith/themes/night.json` is `/theme night`:

```json
{
  "description": "Easy on the eyes at night",
  "base": "plain",
  "colors": { "window": "#1e1e2e", "surface": "#24273a", "text": "#cdd6f4", "accent": "#89b4fa", "error": "#f38ba8" },
  "terminal": { "magenta": "#cba6f7" }
}
```

| Key | What it does |
|---|---|
| `description` | Shown beside the theme in the picker. |
| `base` | The built-in theme it starts from: `matrix`, `plain` (the default) or `light`. Every colour the file doesn't set is the base's, and a `matrix` base brings the rain, spinner and greetings. |
| `colors` | The desktop app's colours, any of: `window`, `surface`, `surfaceRaised`, `border`, `borderStrong`, `text`, `textDim`, `textFaint`, `accent`, `accentSoft`, `accentWash`, `code`, `codeWash`, `info`, `warn`, `error`, `added`, `addedWash`, `removed`, `removedWash`, `userWash`, `selection`. |
| `terminal` | The terminal's colours, any of: `text`, `red`, `green`, `yellow`, `blue`, `magenta`, `cyan`, `white`, `gray`. |

Colours are `"#rrggbb"` or `"#rgb"`. The terminal follows the desktop colours where they mean
the same thing: `red` follows `error`, `green` `added`, `yellow` `warn`, `blue` `accent`, `cyan`
`code`, `white` `text` and `gray` `textDim`. Set `terminal` only for what should differ. Text
without a colour of its own stays your terminal's, unless `terminal` sets `text`.

Theme files are read each time you pick one, so to try an edit, run `/theme <name>` again. A
file with a mistake isn't offered, and `/theme` says what's wrong with it.

## Environment variables

| Variable | What it does |
|---|---|
| `SMITH_BASE_URL` | The server URL, like `--base-url`. |
| `SMITH_API_KEY`, `OPENAI_API_KEY` | The API key for that URL, like `--api-key`. Commands never see these. |
| `SMITH_MODEL` | The model, like `--model`. |
| `http_proxy`, `https_proxy`, `no_proxy` | Proxies for Smith's own requests. The uppercase names work too. |
| `SSL_CERT_FILE` | A certificate bundle to trust, for a server with its own CA. |
| `XDG_CONFIG_HOME`, `XDG_DATA_HOME` | Where the config and the data folders are (`~/.config` and `~/.local/share` by default). |
| `NO_COLOR` | Turns colour off in the terminal app. |
| `SMITH_TOOL_DIR` | Set for [your own tools](#adding-your-own-tools): the tool's folder. |

The older names `AGENT_BASE_URL`, `AGENT_API_KEY` and `AGENT_MODEL` still work.

Commands in the sandbox get a short list of your variables, not your whole environment: `PATH`,
`HOME`, the locale, compiler and build settings (`CC`, `CFLAGS`, `CMAKE_*` and so on), language
toolchains (`JAVA_HOME`, `GOPATH`, `CARGO_HOME`, `VIRTUAL_ENV`, ...), proxies and certificate
settings. Variables that often hold credentials are left out. Use `sandbox.env` to pass more.

## Tools

These are the tools the model gets:

| Tool | What it does | Asks? |
|---|---|---|
| `read_file` | Read a file, or part of one. | Only outside the project. |
| `list_files`, `glob`, `grep` | Find files and search their contents. | Only outside the project. |
| `edit_file` | Replace text in a file, with one or several edits in a call (all or nothing). | Yes, with a diff. |
| `write_file` | Create or overwrite a file. | Yes, with a diff. |
| `bash` | Run a shell command in the sandbox, optionally in the background. | Yes. |
| `bash_output`, `bash_kill` | Read a background command's output, or stop it. | No. |
| `todo_write` | Keep the checklist the model plans with. | No. |
| `ask_user` | Ask you a question with choices. | It is the question. |
| `skill` | Load a skill's instructions. | No. |
| `task` | Hand a piece of work to a sub-agent. With `can_write`, the sub-agent can change files too; its changes ask through the main agent. | No. |

### Adding your own tools

A folder with a `tool.json` is a tool the model can call. The folder's name is the tool's name
(letters, digits, `_` and `-`, starting with a letter). Smith looks in two places:

| Folder | Whose |
|---|---|
| `~/.config/smith/tools/<name>/` | Yours, in every project. |
| `<project>/.agents/tools/<name>/` | The project's. |

`tool.json` says what the tool is for, what it takes, and what to run:

| Key | Required | What it does |
|---|---|---|
| `description` | yes | What the tool does and when to use it. The model chooses tools by this, so make it clear. |
| `command` | yes | The command to run, with bash, in the project folder. |
| `parameters` | no | A JSON Schema of `"type": "object"` for the arguments. Without one, the tool takes none. |
| `readOnly` | no | `true` lets one of your own tools run without asking, and alongside other read-only calls. A project's tools always ask. |
| `timeoutMs` | no | The time limit, from 1 second to 10 minutes. Default: 5 minutes. |
| `network` | no | `true` gives one of your own tools the network, while the agent's other commands stay offline. |
| `env` | no | Names of variables from Smith's environment that one of your own tools gets, such as a token. Other commands don't get them. |

The command gets the call's arguments as JSON on stdin, and `SMITH_TOOL_DIR` is set to the
tool's folder. What it prints is the result the model sees; a non-zero exit tells the model the
tool failed.

For example, a tool that runs a CMake project's tests, in `.agents/tools/run_tests/`:

```json
{
  "description": "Run the project's tests, or only those whose names match a pattern. Build first.",
  "parameters": {
    "type": "object",
    "properties": { "pattern": { "type": "string", "description": "A regular expression for test names" } }
  },
  "command": "python3 \"$SMITH_TOOL_DIR/run.py\"",
  "timeoutMs": 600000
}
```

with `run.py` beside it:

```python
import json, subprocess, sys

args = json.load(sys.stdin)
command = ["ctest", "--test-dir", "build", "--output-on-failure"]
if args.get("pattern"):
    command += ["-R", args["pattern"]]
sys.exit(subprocess.run(command).returncode)
```

A tool for a service on your network, such as a self-hosted Jira, in `~/.config/smith/tools/jira_issue/`:

```json
{
  "description": "Read a Jira issue: its summary, status, description and comments. Give its key, e.g. PROJ-123.",
  "parameters": {
    "type": "object",
    "properties": { "key": { "type": "string", "description": "The issue key, e.g. PROJ-123" } },
    "required": ["key"]
  },
  "command": "python3 \"$SMITH_TOOL_DIR/issue.py\"",
  "readOnly": true,
  "network": true,
  "env": ["JIRA_URL", "JIRA_TOKEN"]
}
```

with `issue.py` beside it (a personal access token; Jira Server and Data Center take it as a
bearer token):

```python
import json, os, sys, urllib.request

key = json.load(sys.stdin)["key"]
request = urllib.request.Request(
    f"{os.environ['JIRA_URL']}/rest/api/2/issue/{key}?fields=summary,status,description,comment",
    headers={"Authorization": f"Bearer {os.environ['JIRA_TOKEN']}"})
fields = json.load(urllib.request.urlopen(request, timeout=30))["fields"]
print(f"{key}: {fields['summary']} ({fields['status']['name']})\n\n{fields.get('description') or ''}")
for comment in fields["comment"]["comments"]:
    print(f"\n--- {comment['author']['displayName']}:\n{comment['body']}")
```

Set `JIRA_URL` and `JIRA_TOKEN` where you start Smith. With an internal CA, add `SSL_CERT_FILE`
to `env` as well.

Good to know:

- Tools always run in the sandbox, and their folder can be read but not written. They have the
  network as `/network` has it, unless they have `"network": true`. With `--sandbox off`, they
  aren't offered at all.
- `network` and `env` are for your own tools only: a project's tool could have been written by
  the agent, so it doesn't get them (Smith says so at startup). The approval prompt says what a
  tool gets beyond other commands, and Smith warns at startup about a variable in `env` that isn't
  set. In the container sandbox, `network` can't be given to one tool: there it needs
  `/network on`, and the tool says so if it isn't.
- They ask like a shell command does, and `permissions.allow` can name them (`"run_tests"`).
- A project's tools always ask, even with `readOnly`, since the agent could have written them. A
  change under `.agents/tools/` is reported like other [files that run later](#files-that-run-later).
- If a tool has the same name as a built-in one, or its `tool.json` is broken, Smith skips it and
  says why at startup. A project's tool wins over one of yours with the same name.
- To try a tool without the model, run its command with the arguments on stdin:
  `echo '{"pattern": "units"}' | SMITH_TOOL_DIR=.agents/tools/run_tests python3 .agents/tools/run_tests/run.py`.

### Skills

A skill is a folder with a `SKILL.md`: a short header naming the skill and saying when to use it,
followed by the instructions. Only the names and descriptions go into the model's prompt; the
model loads a skill's instructions when it needs them. It's the same format Claude Code and
OpenCode use.

```markdown
---
name: release
description: How to cut a release of this project. Use when asked to release or tag a version.
---
1. Update CHANGELOG.md ...
```

Smith looks in `.agents/skills/` and `.claude/skills/` in the project, then
`~/.config/smith/skills/` and `~/.claude/skills/`. A project's skill wins over one of yours with
the same name. `/skills` lists what was found.

### Project instructions

Smith gives the model the `AGENTS.md` files it finds: `~/.config/smith/AGENTS.md` for all
projects, then one in each folder from the repository's root down to the project folder. In a
folder with no `AGENTS.md`, a `CLAUDE.md` is used instead. Put your conventions, build commands
and anything else the model should know there.

## Safety

### The sandbox

By default every shell command, and the file tools, run under bubblewrap:

- Only the project, `/tmp` and a private home folder are writable. The rest of the system is
  read-only.
- Your home folder is replaced by an empty one, apart from the project and the folders in
  `sandbox.home.show` and `sandbox.home.writable`. Credentials (`~/.ssh`, `~/.aws`, `~/.npmrc`,
  ...) and Smith's own settings stay hidden even then. `/tmp`, `/run` and `/mnt` are empty.
- `/tmp` is the session's own, kept between commands. `write_file` and `edit_file` can write
  scratch files there too; those aren't part of `/undo`.
- The network is off until you turn it on (`--network`, `/network on`, or `sandbox.network`).
- The project's `.git` folder is read-only, so the agent can look at history but not commit.
- `sandbox.limits` caps memory, processes and CPU per command, through `systemd-run` (or
  `prlimit` for processes only).

`--sandbox container` runs commands in a podman or docker container instead, which suits a
project that needs another distribution's tools. `--sandbox off` runs them without isolation.
Then every command asks unless a rule allows it, and commands still get only the short list of
environment variables.

When a command fails because something it needs is hidden, Smith tells the model so. The model
can then ask you to share the folder instead of trying around it.

### Files that run later

Some files in a project run code outside the sandbox later, without you deliberately running
them: `.envrc` (direnv), git hooks and config, editor configs, CI workflows, build files,
`package.json` scripts, `Dockerfile` and compose files. A change to one of those is how
something in the sandbox could get out. With `protectRunLater` on (the default), edits to them
always ask, even when edits are otherwise allowed, and after a turn Smith lists any that a
command changed.

### Undo

`/undo` restores every file the last turn changed with the file tools and puts your message back
in the input box. Files that changed again since then are left alone, and changes made by shell
commands can't be undone; Smith says which.

## Good to know

- **Plan mode.** <a id="plan-mode"></a>Switch it on with shift+tab, `/plan` or the desktop's
  "plan" switch, and ask for something. The agent reads and searches, then answers with a plan.
  It can't edit files, run commands or send a sub-agent to work: those calls are refused, and
  every message you send tells it so. When it's done, Smith asks whether to carry out the plan.
  Yes switches plan mode off and tells the agent to go ahead. Esc (or "Keep planning") stays in
  plan mode, so you can say what to change.
- **Queued tasks.** While the agent works, enter adds what you type to the current task, and tab
  queues it as the next one instead. Queued tasks show under the input and start one by one, each
  as a turn of its own, once the one before it finishes. If a turn is interrupted, fails or stops
  short (the step limit, a cut-off answer), nothing more starts: the queued tasks go back into the
  input box, so you can see what happened first. Commands can't be queued.
- **What changed.** `/diff` (the desktop's "Changes" button) shows every file the agent changed
  in this session, compared with how it was before the agent first touched it: created, changed
  or deleted, with the whole diff. Files that are back as they were (after `/undo`, say) aren't
  listed. Changes made by `bash` commands can't be seen this way; `git diff` shows those.
- **Tokens.** The status line shows what the turn under way (or the last one) used, as the server
  reports it: tokens in, tokens out, and how fast they were generated. "In" counts each model
  call, which sends the whole conversation again, so it grows quickly with tool calls; the
  context figure beside it is how full the window is. Servers that don't report usage show
  nothing.
- **Files.** Type `@` and part of a path or file name (`@Agent`, `@src/main`) and Smith offers the
  project's files (from `rg --files` when ripgrep is installed, so ignored ones are left out); tab
  or enter puts the highlighted one in. When you send, each file you mention as `@path` (or
  `@"path with spaces"`, also outside the project) goes along with the message, numbered as
  `read_file` shows it, so the model doesn't spend a step reading it. Up to 10 files, and the
  first 2,000 lines of a longer one. The transcript shows your words, and a line saying what was
  attached.
- **Images.** Name an image file in your message (a path, quoted or not, as a terminal pastes a
  dropped file) and it is sent along: PNG, JPEG, GIF or WebP, up to 5 MB each, from anywhere on
  disk; `@shot.png` works too. Each picture is labelled with its file, so the model doesn't try to open it. If the
  server refuses the request, the pictures are taken out of the conversation again.
- **Sub-agents.** The model can hand investigations, or with `can_write` whole pieces of work, to
  sub-agents, several at once. Their changes ask through the main agent, show in the transcript,
  and are undone with the turn.
- **Background commands.** The model can start long builds, watchers or servers in the
  background and check on them later. `/jobs` shows them. While the network is off, each command
  has a network of its own, so a background server can't be reached from other commands.
- **The context.** When the conversation nears the model's context window, Smith summarizes the
  older part and carries on. `/compact` does it on demand. If the server doesn't report its
  context size, give it with `--context-size` or `contextSize`.
- **Scripts.** `smith -p "explain src/main.cpp"` runs one prompt and prints the answer. Add `-y`
  to let it change things without asking.
- **Sessions** are saved as you go, in `~/.local/share/smith/sessions/`, readable only by you.
  `-c` continues the last one in a folder, `-r` resumes one by name, and `/name` names the
  current one.

## Task worktrees

Let the agent work on one task while you work on another, in the same repository, without it
being able to commit.

```
› /worktree fix-login
  Made a worktree for the task: ~/git/shop-worktrees/fix-login, on smith/fix-login (from main @ 4e1c2a9).
› The login form accepts an empty password when SSO is off. Fix it, with a test.
  …
› /land
```

- `/worktree <task>` makes a git worktree beside the repository (`<repo>-worktrees/<task>`) on a
  `smith/<task>` branch from what you have checked out, and switches to it with a new session.
  You keep working in your own checkout. The status line shows `⎇ smith/<task>` while you're in
  one.
- **The agent can't commit.** The sandbox shows the repository's `.git` read-only, so `git
  status`, `diff` and `log` work but `commit`, `add` and `stash` fail. That's why `/worktree`
  needs the sandbox.
- `/land` (in the worktree, or `/land <task>` from anywhere) shows what changed and asks how to
  land it: cherry-pick onto your branch, merge, or commit on the task branch only. Then it asks
  for a commit message, prefilled with one the model suggests, and makes the commit as you.
  Landing never touches your uncommitted work: if it would conflict, nothing changes in your
  checkout and you're told which files clash. Afterwards it offers to remove the worktree and its
  branch.
- `/worktrees` lists them, with what each has changed and whether another Smith has it open.
  `/drop [task]` removes one, discarding what isn't landed (it asks first).

A new worktree only has tracked files: ignored ones (`node_modules`, `.env`, build folders) aren't
there, so the agent may need to install or build first.

## The desktop app

`smith-desktop` is the same agent in a window. It has:

- **The conversation:** your messages, answers rendered as Markdown, reasoning folded under
  "thought", tool calls as cards with their diffs or output, and the checklist with its progress.
- **Approvals and questions** in place of the input box while they wait, answered with the same
  keys as in the terminal.
- **A sidebar** with the project (open a folder, or a recent one), its task worktrees (click to
  switch, land or drop on hover) and its sessions (switch, start, rename with a double-click,
  delete); "all" lists every folder's sessions.
- **A top bar** to pick the endpoint, model and reasoning level, switch plan mode and the
  sandbox's network, see the session's changes, undo and compact. The status bar shows plan mode,
  the sandbox, how full the context is, the turn's tokens, and background jobs.

"Theme…" in the ⋯ menu does what `/theme` does.

## Trying it without a model

`scripts/fake_llm.py` is a scripted OpenAI-compatible server (Python 3, no dependencies). It
streams like llama-server and plays a fixed conversation. The tools, approvals and sandbox are
all real; only the model is scripted.

```sh
python3 scripts/fake_llm.py &                 # listens on 127.0.0.1:18090
./build/apps/smith/smith --base-url http://127.0.0.1:18090/v1
```

Ask it to "fix the greeting bug" in a folder with the `greet.py` shown at the top of the script.
It plans with todos, reads the file, asks to edit it (with a diff), asks to run it, and answers in
Markdown. In plan mode (shift+tab first), it reads the file and answers with a plan instead;
carrying out the plan then makes the fix. "Set up a database" shows an `ask_user` question.

## Building

Smith is Linux only: the terminal app uses termios and `/proc`, and the sandbox is bubblewrap.
It needs CMake 3.21+, a C++20 compiler and Qt 6.6+ or Qt 5.15: Core and Network, Test for the
tests, and Quick and Quick Controls 2 for the desktop app (which is skipped when they're
missing). Qt 6 is used when both are found, so leave `CMAKE_PREFIX_PATH` out to build against a
system Qt 5. A Qt 6 older than 6.6 (Ubuntu 24.04's 6.4) is passed over for Qt 5.

With Qt 5.15 on Debian/Ubuntu, the terminal app needs `qtbase5-dev`. The desktop app also needs
`qtdeclarative5-dev` and `qtquickcontrols2-5-dev` to build, and these QML modules to run:
`qml-module-qtquick2`, `qml-module-qtquick-controls2`, `qml-module-qtquick-layouts`,
`qml-module-qtquick-window2`, `qml-module-qt-labs-qmlmodels` and `qml-module-qt-labs-platform`.

| CMake option | What it does |
|---|---|
| `-DSMITH_BUILD_TUI=OFF` | Skip the terminal app. |
| `-DSMITH_BUILD_DESKTOP=OFF` | Skip the desktop app. |
| `-DSMITH_BUILD_TESTS=OFF` | Skip the tests. |
| `-DSMITH_SANITIZE=address,undefined` | Build with AddressSanitizer and UBSan. Run the tests with `LSAN_OPTIONS=suppressions=$PWD/tests/lsan.supp` (for leaks in Qt's offscreen plugin). |
| `-DSMITH_SANITIZE=thread` | Build with ThreadSanitizer. Only useful against a Qt built with it too; the `ThreadSanitizer` workflow builds one. |

The binaries link Qt dynamically, so to run them on another machine, ship the Qt libraries with
them or build against a static Qt.

### Tests

```sh
ctest --test-dir build
```

There is one QtTest suite per area: `units`, `tools`, `sandbox`, `features`, `agent`, `ui`,
`worktrees`, `frontend` (what the two apps share to run turns) and `desktop`. The agent tests run
the real loop against a scripted fake server (`tests/FakeServer.h`), and the sandbox tests run
real commands through bubblewrap. Tests that need something missing skip themselves: bwrap,
ripgrep, or docker with the ubi9 images for the container tests. The `desktop` test loads the
QML offscreen and runs a whole turn through it; with `SMITH_SHOTS_DIR=<dir>` it saves a
screenshot of each step (`SMITH_TEST_THEME=plain` for the other theme).

CI (`.github/workflows/ci.yml`) builds and tests against Qt 5.15 and Qt 6.8 on every push, and
once more with AddressSanitizer and UBSan.

### Layout

```
libs/core/                the agent, no UI. Agent.* is the loop and its turns; AgentModel.cpp
                          talks to the model and compacts; AgentTools.cpp runs tools (read-only
                          ones in parallel) and sub-agents
  Http.*                  blocking HTTP and SSE streaming on QNetworkAccessManager, cancellable
  Server.*                one server: model listing, context-size detection
  Context.*               token estimates, trimming, summary prompt, overflow detection
  Session.*, ToolLog.*    JSONL session logs, per-session tool-call log
  Config.*, Preferences.* config file and remembered choices
  Themes.*                built-in themes and theme files
  Sandbox.*               bubblewrap and "no sandbox"; ContainerSandbox.* for podman/docker
  Permissions.*           allow and deny rules
  Process.*, Jobs.*       QProcess runs in their own process group; background jobs
  Tool.*, CustomTools.*   tool definitions, JSON Schema validation, output truncation; tool.json tools
  tools/                  the built-in tools
  ...                     think-tag parser, diff (Myers), undo checkpoints, run-later files,
                          images and @ mentions, file listing, skills, AGENTS.md loading,
                          system prompt
libs/compat/              header-only shims over the Qt 5/6 differences, below every library
libs/tui/                 terminal toolkit: raw mode and signals, inline renderer, keys, line
                          editor, Markdown, pickers, digital rain. Includes nothing else.
libs/frontend/            what both apps share: Runtime (server, model, sandbox, project,
                          session and agents from the flags and config); Controller, which
                          does everything that isn't drawing (messages, turns, the queues,
                          commands, sessions, model and theme); running turns on worker
                          threads (Turns), approvals and questions (Prompts), transcript
                          entries and session replay, commands and completion (FileIndex
                          lists the project's files for @), task worktrees
apps/smith/               the terminal app: draws what the Controller reports, reads keys,
                          and has its own pickers; main.cpp has the flags and the -p mode
apps/smith-desktop/       the desktop app: Backend (the Controller as QML properties), the
                          transcript model, palette, Markdown to rich text, Window, qml/
tests/                    QtTest suites; FakeServer.h is a scripted streaming server
```

The `tools` suite checks the boundaries: the libraries never include the apps, and the two apps
never include each other.

### How it is put together

- **Threads instead of promises.** The agent's methods block. Each turn runs on a worker thread;
  agent events reach the UI thread as queued calls, and approvals and `ask_user` questions block
  the worker until you answer. Esc sets a cancel token that HTTP requests, processes and job
  waits all check.
- **Parallel tools.** Consecutive read-only tool calls each get a thread, and their results go
  back to the model in call order. Approvals are still asked one at a time.
- **One controller, two faces.** What a command or a message does is written once, in
  `Controller`; the apps connect to its signals to show entries, put text in the input box or
  open a picker. A change in behaviour is made there, and the `controller` suite tests it with
  no UI.
- **The terminal UI is hand-written:** termios raw mode, `QSocketNotifier` on stdin,
  SIGWINCH/SIGTERM through a self-pipe, and an inline renderer that only redraws the live region
  at the bottom.

### Differences from the TypeScript version

- **New here:** the desktop app, task worktrees, remembered and configured permissions, your own
  tools, images, sub-agents that change files, and several edits per `edit_file` call that keep a
  file's encoding.
- **Certificates** come from the system store through OpenSSL. For an extra CA, point
  `SSL_CERT_FILE` at a bundle that includes it (instead of `NODE_EXTRA_CA_CERTS`).
- **JSON key order.** Qt sorts the keys of the JSON Smith writes (sessions, logs, preferences),
  which doesn't affect reading them. Endpoints in the config keep the order the file lists them in.
- **Without a sandbox,** commands ask every time, and they get the same short list of environment
  variables as in the sandbox instead of your whole environment.

## License

MIT; see [LICENSE](LICENSE).
