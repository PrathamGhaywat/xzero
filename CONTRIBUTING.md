# Contributing to xzero

Thanks for considering a contribution!

## Getting Started

1. Fork the repo and clone it
2. Build:

```bash
cmake --preset windows-msvc   # or linux-gcc / macos-clang
cmake --build build --config Release
```

3. Run tests:

```bash
./build/Release/test_config
./build/Release/test_stream
```

## Project Structure

- `src/` — core C17 sources (`agent.c`, `session.c`, `openai.c`, `stream.c`, etc.)
- `src/tools/` — tool implementations (`read`, `write`, `edit`, `bash`, `grep`, `glob`)
- `tests/` — small unit tests
- `CMakeLists.txt` / `CMakePresets.json` — build config (fetches `libcurl` + `cJSON`)

## Guidelines

- **C17, cross-platform** — test on Windows and Linux/macOS where possible. Avoid platform-specific code outside `util.c`, `prompt.c`, `http_curl.c`, `session.c`.
- **Keep the binary small** — prefer single-file or fetched deps over heavy dependencies.
- **Tools are bounded** — outputs must be capped (50KB / 2000 lines) and return a short receipt to the model. See `src/util.c:util_cap_output`.
- **No planner/sub-agents** — the agent is a simple loop (`src/agent.c`). Keep it deterministic.

## Pull Requests

1. Create a feature branch: `git checkout -b feat/my-change`
2. Make your change with clear commit messages
3. Ensure it builds in both Release and Debug:
   ```bash
   cmake --preset windows-msvc-debug
   cmake --build build-debug --config Debug
   ```
4. Update `README.md` if you change user-facing behavior
5. Open a PR — include what you changed and how you tested it

## Reporting Issues

Open an issue with:
- OS and compiler version
- Steps to reproduce
- `xzero --version` and config (redact API keys)
- Relevant logs from `build/`

## Code Style

- C17, warnings enabled (`/W4` on MSVC, `-Wall -Wextra` elsewhere)
- No unnecessary comments — keep code concise
- Run `cmake --build` with no warnings before submitting

## License

By contributing, you agree your contributions will be licensed under the MIT License.
