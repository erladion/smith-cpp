#!/usr/bin/env python3
"""A scripted OpenAI-compatible server, for trying Smith without a model.

It streams like llama-server (SSE, tool calls, reasoning_content) and plays a
fixed script chosen by what you type:

  "...greeting..."   plans with todo_write, reads greet.py, edits it (approval
                     with a diff), runs it with bash (approval), checks off the
                     todos and answers in Markdown
  "...database..."   asks you a question with ask_user
  anything else      a one-line reply

Usage:
  python3 scripts/fake_llm.py [port]          # default 18090
  ./build/smith --base-url http://127.0.0.1:18090/v1

For the greeting script, work in a folder with this greet.py:

  import sys

  def greet(name):
      return "Hello, world!"

  if __name__ == "__main__":
      print(greet(sys.argv[1] if len(sys.argv) > 1 else "world"))
"""
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TODOS = ["Find where the greeting is built", "Fix the missing name", "Run the script to check"]


def todos(states):
    return {"todos": [{"content": c, "status": s} for c, s in zip(TODOS, states)]}


FIX_STEPS = [
    {"reasoning": "The user says the greeting is wrong. I'll plan, then read greet.py.",
     "calls": [("todo_write", todos(["in_progress", "pending", "pending"]))]},
    {"text": "Let me look at the script.", "calls": [("read_file", {"path": "greet.py"})]},
    {"reasoning": "greet() ignores its argument. Replace the hard-coded string with an f-string.",
     "calls": [("todo_write", todos(["completed", "in_progress", "pending"])),
               ("edit_file", {"path": "greet.py", "old_string": '    return "Hello, world!"',
                              "new_string": '    return f"Hello, {name}!"'})]},
    {"calls": [("todo_write", todos(["completed", "completed", "in_progress"])),
               ("bash", {"command": "python3 greet.py Anderson"})]},
    {"calls": [("todo_write", todos(["completed", "completed", "completed"]))]},
    {"text": "Fixed. `greet()` ignored its `name` argument and always returned **Hello, world!**.\n\n"
             "## What changed\n\n"
             "- `greet.py`: the greeting is now built from `name`\n"
             "- Checked it by running the script:\n\n"
             "```\n$ python3 greet.py Anderson\nHello, Anderson!\n```\n\n"
             "Nothing else uses `greet()`, so no other changes were needed."},
]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def _json(self, status, body):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path.startswith("/v1/models"):
            self._json(200, {"object": "list", "data": [
                {"id": "qwen3-coder-30b", "object": "model", "max_model_len": 65536},
                {"id": "gpt-oss-20b", "object": "model", "max_model_len": 131072},
                {"id": "qwen3-small", "object": "model", "max_model_len": 32768}]})
        else:
            self._json(404, {"error": {"message": "not found"}})

    def chunk(self, delta, finish=None):
        payload = {"choices": [{"index": 0, "delta": delta, "finish_reason": finish}]}
        self.wfile.write(f"data: {json.dumps(payload)}\n\n".encode())
        self.wfile.flush()

    def stream_text(self, text, kind="content"):
        for i in range(0, len(text), 12):
            self.chunk({kind: text[i:i + 12]})
            time.sleep(0.02)

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        messages = body["messages"]
        last_user = max(i for i, m in enumerate(messages) if m["role"] == "user")
        prompt = messages[last_user]["content"]
        step = sum(1 for m in messages[last_user:] if m["role"] == "assistant")

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        time.sleep(0.3)

        if "greeting" in prompt and step < len(FIX_STEPS):
            spec = FIX_STEPS[step]
        elif "database" in prompt and step == 0:
            spec = {"calls": [("ask_user", {"question": "Which database should the app use?",
                                            "options": ["PostgreSQL", "SQLite", "Keep it in memory for now"]})]}
        elif "database" in prompt:
            spec = {"text": "Going with **SQLite**: a single file, nothing to install."}
        else:
            spec = {"text": "Hello, Mr. Anderson."}

        if spec.get("reasoning"):
            self.stream_text(spec["reasoning"], "reasoning_content")
        if spec.get("text"):
            self.stream_text(spec["text"])
        for index, (name, args) in enumerate(spec.get("calls", [])):
            self.chunk({"tool_calls": [{"index": index, "id": f"c{step}_{index}", "type": "function",
                                        "function": {"name": name, "arguments": json.dumps(args)}}]})
        self.chunk({}, "tool_calls" if spec.get("calls") else "stop")
        usage = {"choices": [], "usage": {"prompt_tokens": 9400 + 600 * step, "completion_tokens": 120}}
        self.wfile.write(f"data: {json.dumps(usage)}\n\ndata: [DONE]\n\n".encode())


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18090
    print(f"Fake model server on http://127.0.0.1:{port}/v1 (ctrl+c stops it)", flush=True)
    try:
        ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
    except KeyboardInterrupt:
        pass
