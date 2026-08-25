# xzero

Fast, cross-platform coding agent in C: works with any OpenAI-compatible API (OpenAI, Ollama, LM Studio, vLLM, etc).

Chat with your codebase, read and edit files, run commands, and let the agent handle the loop. Similar to Codex / Claude Code / opencode, but a single native binary.

## Features

- Interactive REPL and one-shot mode
- Works with local models (no API key needed) and hosted APIs
- File tools: read, write, edit, grep, glob
- Shell execution with approval
- Session persistence and resume
- Streaming responses
- Windows, macOS, Linux

## Install

Requires CMake 3.20+ and a C17 compiler (MSVC, GCC, or Clang). Dependencies (`libcurl`, `cJSON`) are fetched automatically.

```bash
cmake --preset windows-msvc   # or linux-gcc / macos-clang
cmake --build build --config Release
```

Binary: `build/Release/xzero` (or `build/Release/xzero.exe` on Windows)

## Configuration

On first run `xzero` asks for:

- **Base URL** — e.g. `https://api.openai.com/v1` or `http://localhost:11434/v1` for Ollama
- **API Key** — leave blank for local models
- **Model** — e.g. `gpt-4o-mini`, `qwen2:0.5b`

Config is saved to:
- Windows: `%APPDATA%\xzero\config.json`
- Linux: `~/.config/xzero/config.json`
- macOS: `~/Library/Application Support/xzero/config.json`
- Project override: `./xzero.json` or `./.xzero/config.json`

You can also configure non-interactively:

```bash
xzero --base-url http://localhost:11434/v1 --api-key "" --model qwen2:0.5b --test
xzero --base-url https://api.openai.com/v1 --api-key sk-... --model gpt-4o-mini --test
```

Precedence: CLI flags > environment variables > config file > prompt.

Environment variables:
```
XZERO_BASE_URL / OPENAI_BASE_URL
XZERO_API_KEY / OPENAI_API_KEY
XZERO_MODEL / OPENAI_MODEL
```

## Usage

**Interactive:**

```bash
xzero
```

```
xzero> fix the bug in src/main.c
xzero> /help
```

**One-shot:**

```bash
xzero "add error handling to src/util.c"
xzero --resume <session-id> "continue"
```

**Commands:**

```
/help          - show help
/clear         - clear screen
/exit, /quit   - exit
/model <name>  - switch model
/sessions      - list sessions
/new           - start new session
/compact       - compact history
```

**Other flags:**

```bash
xzero --list-sessions
xzero --version
xzero --help
```

## Tools

The agent can call these tools (some require approval):

| Tool | What it does | Needs approval |
|------|--------------|----------------|
| `read` | Read a file (paged) | no |
| `grep` | Search for pattern in files | no |
| `glob` | Find files by pattern | no |
| `write` | Create/overwrite a file | yes |
| `edit` | Replace text in a file | yes |
| `bash` | Run a shell command | yes |

`bash` shows the exact command before running (e.g. `> bash [call_1] `$ pwd``) and captures output. On Windows, common commands like `pwd`/`ls`/`cat` are translated to `cd`/`dir`/`type`.

Large outputs are truncated with a reference to the full log on disk.

## Sessions

Sessions are saved as JSONL:

- Project-local: `.xzero/sessions/<id>.jsonl` (if `.xzero/` exists)
- Global: `~/.local/share/xzero/sessions` or `%APPDATA%\xzero\sessions`

Long sessions are automatically compacted to keep context manageable.

## Development

```bash
cmake --preset windows-msvc
cmake --build build --config Release

./build/Release/test_config && ./build/Release/test_stream
```

See `CMakePresets.json` for all presets.

## License

MIT License: View [here](LICENSE)