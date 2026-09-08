#!/usr/bin/env python3
"""Tailnet-only Phase 2 collector for the Mac mini.

This process polls the same local Claude, Codex, and Pluribus sources as the BLE
daemon, stores only compact display payloads, and serves them to the work Mac via
an authenticated HTTP endpoint.  It never handles Apple Music and never connects
to the Clawdmeter over BLE.
"""

from __future__ import annotations

import argparse
import asyncio
import copy
import hmac
import json
import signal
import threading
import time
from dataclasses import dataclass
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

try:
    from daemon import claude_usage_daemon as usage_source
    from daemon import pluribus_activity as pluribus_source
except ImportError:
    import claude_usage_daemon as usage_source
    import pluribus_activity as pluribus_source


DEFAULT_CONFIG = Path.home() / ".config" / "claude-usage-monitor" / "relay-server.json"
USAGE_INTERVAL = 60.0
PLURIBUS_INTERVAL = 15.0
MAX_WIRE_BYTES = 512


def log(message: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)


@dataclass(frozen=True)
class ServerConfig:
    bind_host: str
    port: int
    token: str


def load_config(path: Path) -> ServerConfig:
    try:
        body = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot read relay config {path}: {exc}") from exc
    bind_host = body.get("bind_host")
    port = body.get("port", 8765)
    token = body.get("token")
    if not isinstance(bind_host, str) or not bind_host:
        raise ValueError("relay bind_host is required")
    if not isinstance(port, int) or not 1 <= port <= 65535:
        raise ValueError("relay port must be 1..65535")
    if not isinstance(token, str) or len(token) < 32:
        raise ValueError("relay token must contain at least 32 characters")
    return ServerConfig(bind_host=bind_host, port=port, token=token)


def _valid_wire_payload(payload: object) -> bool:
    if not isinstance(payload, dict):
        return False
    try:
        return len(json.dumps(payload, separators=(",", ":")).encode("utf-8")) < MAX_WIRE_BYTES
    except (TypeError, ValueError):
        return False


class StateStore:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._usage: dict | None = None
        self._pluribus: dict | None = None

    def update_usage(self, payload: dict, observed_at: float | None = None) -> None:
        if not _valid_wire_payload(payload):
            raise ValueError("invalid or oversized usage payload")
        slot = {
            "observed_at": time.time() if observed_at is None else observed_at,
            "payload": payload,
        }
        with self._lock:
            self._usage = slot

    def update_pluribus(self, payload: dict, observed_at: float | None = None) -> None:
        if not _valid_wire_payload(payload):
            raise ValueError("invalid or oversized Pluribus payload")
        slot = {
            "observed_at": time.time() if observed_at is None else observed_at,
            "payload": payload,
        }
        with self._lock:
            self._pluribus = slot

    def snapshot(self, generated_at: float | None = None) -> dict:
        with self._lock:
            usage = copy.deepcopy(self._usage)
            pluribus = copy.deepcopy(self._pluribus)
        body = {
            "version": 1,
            "generated_at": time.time() if generated_at is None else generated_at,
        }
        if usage is not None:
            body["usage"] = usage
        if pluribus is not None:
            body["pluribus"] = pluribus
        return body


def make_handler(store: StateStore, token: str):
    class RelayHandler(BaseHTTPRequestHandler):
        server_version = "ClawdmeterRelay/1"

        def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
            supplied = self.headers.get("Authorization", "")
            expected = f"Bearer {token}"
            if not hmac.compare_digest(supplied, expected):
                self.send_error(HTTPStatus.UNAUTHORIZED)
                return
            if self.path not in ("/v1/state", "/health"):
                self.send_error(HTTPStatus.NOT_FOUND)
                return
            body = {"ok": True} if self.path == "/health" else store.snapshot()
            encoded = json.dumps(body, separators=(",", ":")).encode("utf-8")
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(encoded)))
            self.end_headers()
            self.wfile.write(encoded)

        def log_message(self, _format: str, *_args: object) -> None:
            return

    return RelayHandler


async def collect(store: StateStore, stop_event: asyncio.Event) -> None:
    next_usage = 0.0
    next_pluribus = 0.0
    while not stop_event.is_set():
        now = time.time()
        if now >= next_usage:
            next_usage = now + USAGE_INTERVAL
            try:
                payload, all_dead = await usage_source.poll_active()
                if payload is not None:
                    usage_source.add_codex_field(payload)
                    store.update_usage(payload)
                    log("Updated Claude/Codex state")
                elif all_dead:
                    store.update_usage({"ok": False})
                    log("Claude credentials unavailable; published no-data state")
            except Exception as exc:
                log(f"Usage collection failed: {type(exc).__name__}")

        if now >= next_pluribus:
            next_pluribus = now + PLURIBUS_INTERVAL
            try:
                result = await asyncio.to_thread(pluribus_source.fetch_latest)
                if result.available and result.payload is not None:
                    store.update_pluribus(result.payload)
                    log("Updated Pluribus state")
            except Exception as exc:
                log(f"Pluribus collection failed: {type(exc).__name__}")

        try:
            await asyncio.wait_for(stop_event.wait(), timeout=1.0)
        except asyncio.TimeoutError:
            pass


async def run(config: ServerConfig) -> None:
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, stop_event.set)
        except NotImplementedError:
            pass

    store = StateStore()
    httpd = ThreadingHTTPServer(
        (config.bind_host, config.port),
        make_handler(store, config.token),
    )
    server_thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    server_thread.start()
    log(f"Relay listening on {config.bind_host}:{config.port}")
    try:
        await collect(store, stop_event)
    finally:
        httpd.shutdown()
        httpd.server_close()
        server_thread.join(timeout=5)
        log("Relay stopped")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    args = parser.parse_args()
    try:
        config = load_config(args.config)
    except ValueError as exc:
        raise SystemExit(str(exc)) from exc
    asyncio.run(run(config))


if __name__ == "__main__":
    main()
