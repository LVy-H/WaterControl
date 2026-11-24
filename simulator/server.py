#!/usr/bin/env python3
"""Serve the browser simulator and one persistent native controller process."""

import argparse
import json
import math
import os
from pathlib import Path
import selectors
import signal
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


ROOT = Path(__file__).resolve().parent.parent
WEB = Path(__file__).resolve().parent / "web"
FAULTS = frozenset((
    "main_leak", "blockage1", "blockage2", "main_meter_stuck",
    "tank1_wire_open", "tank2_wire_open", "source_wire_open", "downstream_leak1",
))
ASSETS = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/index.html": ("index.html", "text/html; charset=utf-8"),
    "/app.js": ("app.js", "text/javascript; charset=utf-8"),
    "/style.css": ("style.css", "text/css; charset=utf-8"),
    "/fonts/plex-sans-regular.woff2": ("fonts/plex-sans-regular.woff2", "font/woff2"),
    "/fonts/plex-sans-semibold.woff2": ("fonts/plex-sans-semibold.woff2", "font/woff2"),
    "/fonts/plex-mono-regular.woff2": ("fonts/plex-mono-regular.woff2", "font/woff2"),
    "/fonts/LICENSE.txt": ("fonts/LICENSE.txt", "text/plain; charset=utf-8"),
}


class EngineUnavailable(Exception):
    pass


def reject_constant(value):
    raise ValueError("Non-finite JSON numbers are not allowed")


def command_for(payload):
    if not isinstance(payload, dict) or not isinstance(payload.get("action"), str):
        raise ValueError("Expected a JSON object with a string action")
    action = payload["action"]
    fields = {"action"}
    if action in ("reset", "press", "ack"):
        command = action
    elif action in ("pump2", "house1", "network", "isolate", "fault"):
        fields.add("value")
        value = payload.get("value")
        if type(value) is not bool:
            raise ValueError("value must be a JSON boolean")
        command = f"{action} {int(value)}"
        if action == "isolate":
            fields.add("house")
            house = payload.get("house")
            if type(house) is not int or house not in (1, 2):
                raise ValueError("house must be the integer 1 or 2")
            command = f"isolate {house} {int(value)}"
        elif action == "fault":
            fields.add("name")
            name = payload.get("name")
            if not isinstance(name, str) or name not in FAULTS:
                raise ValueError("Unknown fault name")
            command = f"fault {name} {int(value)}"
    elif action == "advance":
        fields.add("value")
        value = payload.get("value")
        if type(value) is not int or not 0 <= value <= 60000:
            raise ValueError("advance value must be an integer from 0 to 60000 ms")
        command = f"advance {value}"
    elif action in ("tank", "source"):
        fields.add("value")
        value = payload.get("value")
        if type(value) not in (int, float) or not 0 <= value <= 100 or not math.isfinite(value):
            raise ValueError("value must be a finite percentage from 0 to 100")
        command = f"{action} {value:.9g}"
        if action == "tank":
            fields.add("house")
            house = payload.get("house")
            if type(house) is not int or house not in (1, 2):
                raise ValueError("house must be the integer 1 or 2")
            command = f"tank {house} {value:.9g}"
    else:
        raise ValueError("Unknown action")
    if set(payload) != fields:
        raise ValueError("Action has missing or unexpected fields")
    return command


class Engine:
    def __init__(self):
        self.lock = threading.Lock()
        self.failure = None
        self.buffer = bytearray()
        self.process = subprocess.Popen(
            [str(ROOT / ".build" / "water-sim"), "--stdio"],
            cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0,
        )
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)

    def _stop(self):
        try:
            self.process.stdin.close()
        except OSError:
            pass
        try:
            self.process.wait(timeout=1)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            try:
                self.process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.selector.close()
        self.process.stdout.close()

    def close(self):
        with self.lock:
            if self.failure is None:
                self.failure = "Native engine has stopped"
                self._stop()

    def exchange(self, command):
        with self.lock:
            if self.failure is not None:
                raise EngineUnavailable(self.failure)
            try:
                if self.process.poll() is not None:
                    raise RuntimeError(f"Native engine exited with code {self.process.returncode}")
                self.process.stdin.write((command + "\n").encode("ascii"))
                self.process.stdin.flush()
                deadline = time.monotonic() + 10
                while b"\n" not in self.buffer:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0 or not self.selector.select(remaining):
                        raise RuntimeError("Native engine response timed out; simulation stopped")
                    chunk = os.read(self.process.stdout.fileno(), 65536)
                    if not chunk:
                        raise RuntimeError("Native engine closed its response stream")
                    self.buffer.extend(chunk)
                    if len(self.buffer) > 131072:
                        raise RuntimeError("Native engine response exceeded 128 KiB")
                line, _, remainder = self.buffer.partition(b"\n")
                self.buffer = bytearray(remainder)
                state = json.loads(line, parse_constant=reject_constant)
                if not isinstance(state, dict):
                    raise RuntimeError("Native engine returned a non-object response")
                if "error" in state:
                    raise RuntimeError(f"Native engine rejected a validated command: {state['error']}")
                if "time_ms" not in state or "outputs" not in state or "logs" not in state:
                    raise RuntimeError("Native engine response is missing state fields")
                return state
            except (OSError, ValueError, RuntimeError) as error:
                self.failure = str(error)
                self._stop()
                raise EngineUnavailable(self.failure) from error


class Server(ThreadingHTTPServer):
    daemon_threads = True
    block_on_close = False

    def __init__(self, port, engine):
        super().__init__(("127.0.0.1", port), Handler)
        self.engine = engine
        self.hosts = {f"127.0.0.1:{port}", f"localhost:{port}"}


class Handler(BaseHTTPRequestHandler):
    server_version = "WaterSimulator/1"

    def reply(self, status, body, content_type="application/json; charset=utf-8"):
        if not isinstance(body, bytes):
            body = json.dumps(body, allow_nan=False, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def valid_host(self):
        if self.headers.get("Host") not in self.server.hosts:
            self.reply(403, {"error": "Use the local simulator address"})
            return False
        return True

    def state_reply(self, command):
        try:
            self.reply(200, self.server.engine.exchange(command))
        except EngineUnavailable as error:
            self.reply(503, {"error": str(error)})

    def do_GET(self):
        if not self.valid_host():
            return
        if self.path == "/api/state":
            self.state_reply("state")
        elif self.path in ASSETS:
            filename, content_type = ASSETS[self.path]
            try:
                body = (WEB / filename).read_bytes()
            except OSError:
                self.reply(500, {"error": "Simulator asset is unavailable"})
                return
            self.reply(200, body, content_type)
        else:
            self.reply(404, {"error": "Not found"})

    def do_POST(self):
        self.connection.settimeout(10)
        self.close_connection = True
        if not self.valid_host():
            return
        origin = self.headers.get("Origin")
        if origin is not None and origin != f"http://{self.headers.get('Host')}":
            self.reply(403, {"error": "Foreign Origin mutations are not allowed"})
            return
        if self.headers.get("Sec-Fetch-Site") == "cross-site":
            self.reply(403, {"error": "Cross-site mutations are not allowed"})
            return
        if self.path != "/api/action":
            self.reply(404, {"error": "Not found"})
            return
        if self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower() != "application/json":
            self.reply(400, {"error": "Content-Type must be application/json"})
            return
        if self.headers.get("Transfer-Encoding") is not None:
            self.reply(400, {"error": "Transfer-Encoding is not supported"})
            return
        try:
            length = int(self.headers.get("Content-Length", ""))
            if not 1 <= length <= 4096:
                raise ValueError("JSON body must contain 1 to 4096 bytes")
            body = self.rfile.read(length)
            if len(body) != length:
                raise ValueError("Incomplete JSON body")
            payload = json.loads(body, parse_constant=reject_constant)
            command = command_for(payload)
        except (ValueError, UnicodeError, OSError, RecursionError) as error:
            self.reply(400, {"error": str(error)})
            return
        self.state_reply(command)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("port must be between 1 and 65535")
    subprocess.run(["make", ".build/water-sim"], cwd=ROOT, check=True)
    engine = Engine()
    server = None
    def interrupt(_signum, _frame):
        raise KeyboardInterrupt
    previous = signal.signal(signal.SIGTERM, interrupt)
    try:
        engine.exchange("state")
        server = Server(args.port, engine)
        print(f"Water simulator: http://127.0.0.1:{args.port}", flush=True)
        print("Shared C++ controller; illustrative hydraulics; not electrical validation.", flush=True)
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass
    finally:
        signal.signal(signal.SIGTERM, previous)
        if server is not None:
            server.server_close()
        engine.close()


if __name__ == "__main__":
    try:
        main()
    except (OSError, subprocess.CalledProcessError, EngineUnavailable) as error:
        print(f"Simulator startup failed: {error}", file=sys.stderr)
        sys.exit(1)
