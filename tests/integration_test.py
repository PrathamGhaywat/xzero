#!/usr/bin/env python3
"""End-to-end tests for xzero against a local fake OpenAI server."""

import contextlib
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import traceback
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

BINARY = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else ""


class _Handler(BaseHTTPRequestHandler):
    def _respond(self, status, ctype, payload):
        if isinstance(payload, str):
            payload = payload.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(payload)
        self.wfile.flush()

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length)
        try:
            body = json.loads(raw.decode("utf-8")) if raw else {}
        except Exception:
            body = {}
        self.server.requests.append(body)
        index = len(self.server.requests) - 1
        try:
            if self.server.responder is None:
                raise AssertionError("unexpected POST")
            status, ctype, payload = self.server.responder(index, self.server.requests)
        except Exception:
            self.server.errors.append(traceback.format_exc())
            status, ctype, payload = 500, "text/plain", b"mock server error"
        self._respond(status, ctype, payload)

    def do_GET(self):
        try:
            if self.server.get_responder is None:
                raise AssertionError("unexpected GET")
            status, ctype, payload = self.server.get_responder(self.path, self.headers)
        except Exception:
            self.server.errors.append(traceback.format_exc())
            status, ctype, payload = 500, "text/plain", b"mock server error"
        self._respond(status, ctype, payload)

    def log_message(self, *args):
        pass


@contextlib.contextmanager
def mock_server(responder=None, get_responder=None):
    server = HTTPServer(("127.0.0.1", 0), _Handler)
    server.responder = responder
    server.get_responder = get_responder
    server.requests = []
    server.errors = []
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


def sse(*events):
    lines = ["data: " + json.dumps(event) + "\n\n" for event in events]
    lines.append("data: [DONE]\n\n")
    return "".join(lines)


def tool_call_sse(name, args, call_id="call_1"):
    delta = {
        "choices": [{
            "delta": {
                "role": "assistant",
                "tool_calls": [{
                    "index": 0,
                    "id": call_id,
                    "type": "function",
                    "function": {"name": name, "arguments": json.dumps(args)},
                }],
            }
        }]
    }
    finish = {"choices": [{"delta": {}, "finish_reason": "tool_calls"}]}
    return sse(delta, finish)


def content_sse(text):
    delta = {"choices": [{"delta": {"content": text}}]}
    finish = {"choices": [{"delta": {}, "finish_reason": "stop"}]}
    return sse(delta, finish)


def xzero_env(tmp):
    env = os.environ.copy()
    for name in ("HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "APPDATA", "LOCALAPPDATA"):
        env[name] = str(tmp)
    return env


def run_xzero_args(tmp, args, timeout=60, approve=False):
    stdin_args = {"input": "y\n"} if approve else {"stdin": subprocess.DEVNULL}
    return subprocess.run(
        [BINARY] + args,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout,
        env=xzero_env(tmp),
        cwd=str(tmp),
        **stdin_args,
    )


def run_xzero(tmp, port, prompt, timeout=60, approve=False):
    args = [
        "--config", str(tmp / "config.json"),
        "--base-url", "http://127.0.0.1:%d/v1" % port,
        "--api-key", "test-key",
        "--model", "test-model",
        prompt,
    ]
    return run_xzero_args(tmp, args, timeout=timeout, approve=approve)


def check_server(server):
    if server.errors:
        raise AssertionError("mock server error:\n" + "\n".join(server.errors))


def tool_messages(request):
    return [m for m in request.get("messages", []) if m.get("role") == "tool"]


def test_http_error():
    with tempfile.TemporaryDirectory(prefix="xzero-test-") as td:
        tmp = Path(td)

        def responder(index, requests):
            return 401, "application/json", json.dumps({"error": {"message": "invalid api key"}})

        with mock_server(responder) as server:
            proc = run_xzero(tmp, server.server_port, "hello")
        check_server(server)
        output = (proc.stdout + proc.stderr).lower()
        assert proc.returncode != 0, (proc.returncode, proc.stdout, proc.stderr)
        assert "invalid api key" in output, output


def test_approval_denied_on_eof():
    with tempfile.TemporaryDirectory(prefix="xzero-test-") as td:
        tmp = Path(td)
        target = tmp / "written-without-approval.txt"

        def responder(index, requests):
            if index == 0:
                return 200, "text/event-stream", tool_call_sse(
                    "write", {"path": str(target), "content": "should not exist"})
            contents = [m.get("content", "") for m in tool_messages(requests[index])]
            assert "permission denied by user" in contents, contents
            return 200, "text/event-stream", content_sse("denied-ok")

        with mock_server(responder) as server:
            proc = run_xzero(tmp, server.server_port, "write a file")
        check_server(server)
        assert proc.returncode == 0, (proc.returncode, proc.stdout, proc.stderr)
        assert "denied-ok" in proc.stdout, proc.stdout
        assert not target.exists(), "write tool ran without approval"


def test_multi_iteration_tool_result():
    with tempfile.TemporaryDirectory(prefix="xzero-test-") as td:
        tmp = Path(td)
        source = tmp / "source.txt"
        source.write_text("read-content-123\nsecond line\n", encoding="utf-8")

        def responder(index, requests):
            if index == 0:
                return 200, "text/event-stream", tool_call_sse("read", {"path": str(source)})
            contents = [m.get("content", "") for m in tool_messages(requests[index])]
            assert any("read-content-123" in c for c in contents), contents
            return 200, "text/event-stream", content_sse("read-ok")

        with mock_server(responder) as server:
            proc = run_xzero(tmp, server.server_port, "read the file")
        check_server(server)
        assert proc.returncode == 0, (proc.returncode, proc.stdout, proc.stderr)
        assert "read-ok" in proc.stdout, proc.stdout


def test_optional_field_retry():
    with tempfile.TemporaryDirectory(prefix="xzero-test-") as td:
        tmp = Path(td)

        def responder(index, requests):
            if index == 0:
                assert "stream_options" in requests[0], requests[0].keys()
                return 400, "application/json", json.dumps(
                    {"error": {"message": "unknown field stream_options"}})
            assert "stream_options" not in requests[1], requests[1].keys()
            assert "parallel_tool_calls" not in requests[1], requests[1].keys()
            return 200, "text/event-stream", content_sse("retry-ok")

        with mock_server(responder) as server:
            proc = run_xzero(tmp, server.server_port, "hello")
        check_server(server)
        assert proc.returncode == 0, (proc.returncode, proc.stdout, proc.stderr)
        assert "retry-ok" in proc.stdout, proc.stdout


def test_non_stream_fallback():
    with tempfile.TemporaryDirectory(prefix="xzero-test-") as td:
        tmp = Path(td)

        def responder(index, requests):
            body = {"choices": [{"message": {"role": "assistant", "content": "nonstream-ok"}}]}
            return 200, "application/json", json.dumps(body)

        with mock_server(responder) as server:
            proc = run_xzero(tmp, server.server_port, "hello")
        check_server(server)
        assert proc.returncode == 0, (proc.returncode, proc.stdout, proc.stderr)
        assert "nonstream-ok" in proc.stdout, proc.stdout


def test_bash_exit_status():
    if os.name == "nt":
        return

    with tempfile.TemporaryDirectory(prefix="xzero-test-") as td:
        tmp = Path(td)

        def responder(index, requests):
            if index == 0:
                return 200, "text/event-stream", tool_call_sse(
                    "bash", {"command": "printf 'bash-output\\n'; exit 3"})
            contents = [m.get("content", "") for m in tool_messages(requests[index])]
            assert any("bash-output" in c and "[exit code 3]" in c for c in contents), contents
            return 200, "text/event-stream", content_sse("bash-exit-ok")

        with mock_server(responder) as server:
            proc = run_xzero(tmp, server.server_port, "run a command", approve=True)
        check_server(server)
        assert proc.returncode == 0, (proc.returncode, proc.stdout, proc.stderr)
        assert "bash-exit-ok" in proc.stdout, proc.stdout


def test_bash_timeout():
    if os.name == "nt":
        return

    with tempfile.TemporaryDirectory(prefix="xzero-test-") as td:
        tmp = Path(td)

        def responder(index, requests):
            if index == 0:
                return 200, "text/event-stream", tool_call_sse(
                    "bash", {"command": "sleep 5", "timeout": 300})
            contents = [m.get("content", "") for m in tool_messages(requests[index])]
            assert any("timed out" in c for c in contents), contents
            return 200, "text/event-stream", content_sse("timeout-ok")

        with mock_server(responder) as server:
            proc = run_xzero(tmp, server.server_port, "sleep", timeout=30, approve=True)
        check_server(server)
        assert proc.returncode == 0, (proc.returncode, proc.stdout, proc.stderr)
        assert "timeout-ok" in proc.stdout, proc.stdout


def test_bash_large_output():
    if os.name == "nt":
        return

    with tempfile.TemporaryDirectory(prefix="xzero-test-") as td:
        tmp = Path(td)

        def responder(index, requests):
            if index == 0:
                return 200, "text/event-stream", tool_call_sse(
                    "bash", {"command": "yes x | head -c 200000"})
            contents = [m.get("content", "") for m in tool_messages(requests[index])]
            content = next((c for c in contents if "Full output:" in c), "")
            assert content, contents
            assert "[Showing lines" in content, content[:200]
            assert len(content) < 60 * 1024, len(content)
            match = re.search(r"Full output: ([^\]\n]+)\]", content)
            assert match, content[-500:]
            spill = Path(match.group(1))
            assert spill.exists(), spill
            assert spill.stat().st_size == 200000, spill.stat().st_size
            return 200, "text/event-stream", content_sse("large-ok")

        with mock_server(responder) as server:
            proc = run_xzero(tmp, server.server_port, "run big output", approve=True)
        check_server(server)
        assert proc.returncode == 0, (proc.returncode, proc.stdout, proc.stderr)
        assert "large-ok" in proc.stdout, proc.stdout


def test_wallpaper_url_download():
    if os.name == "nt":
        return  # would actually change the runner's wallpaper

    jpeg = b"\xff\xd8\xff\xe0" + b"x" * 256
    requests = []

    def get_responder(path, headers):
        requests.append((path, headers.get("User-Agent", "")))
        return 200, "image/jpeg", jpeg

    with tempfile.TemporaryDirectory(prefix="xzero-test-") as td:
        tmp = Path(td)
        with mock_server(get_responder=get_responder) as server:
            url = "http://127.0.0.1:%d/wolf.jpg" % server.server_port
            proc = run_xzero_args(tmp, ["--wallpaper", url])
        check_server(server)

        output = proc.stdout + proc.stderr
        assert proc.returncode != 0, (proc.returncode, output)
        assert "only supported on Windows" in output, output
        assert requests and requests[0][0].endswith("/wolf.jpg"), requests
        assert "xzero wallpaper" in requests[0][1], requests

        match = re.search(r"image saved at (.+?)\)\s*$", proc.stderr.strip())
        assert match, proc.stderr
        downloaded = Path(match.group(1))
        assert downloaded.exists(), downloaded
        assert downloaded.suffix == ".jpg", downloaded
        assert downloaded.read_bytes()[:4] == b"\xff\xd8\xff\xe0"
        downloaded.unlink()


def main():
    if not BINARY or not os.path.exists(BINARY):
        raise SystemExit("usage: integration_test.py /path/to/xzero")
    tests = [
        test_http_error,
        test_approval_denied_on_eof,
        test_multi_iteration_tool_result,
        test_optional_field_retry,
        test_non_stream_fallback,
        test_bash_exit_status,
        test_bash_timeout,
        test_bash_large_output,
        test_wallpaper_url_download,
    ]
    for test in tests:
        test()
        print("%s passed" % test.__name__)
    print("integration tests passed")


if __name__ == "__main__":
    main()
