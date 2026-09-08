#!/usr/bin/env python3
"""Validated Phase 2 state feed consumed by the work-Mac BLE daemon.

The Mac mini publishes only compact, display-safe payloads.  This module reads
the relay settings from the existing daemon config, authenticates the request,
rejects stale/oversized responses, and returns the same payload dictionaries the
BLE code already understands.
"""

from __future__ import annotations

import copy
import json
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable
from urllib.parse import urlparse

import httpx


CONFIG_FILE = Path.home() / ".config" / "claude-usage-monitor" / "config"
REQUEST_TIMEOUT = 5.0
ENVELOPE_VERSION = 1
ENVELOPE_MAX_AGE = 180
USAGE_MAX_AGE = 180
PLURIBUS_MAX_AGE = 24 * 60 * 60
WIRE_LIMIT = 512


@dataclass(frozen=True)
class RelayConfig:
    url: str
    token: str


@dataclass(frozen=True)
class RelayState:
    available: bool
    usage: dict | None = None
    pluribus: dict | None = None


def _config_values(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError:
        return values
    for raw in lines:
        line = raw.split("#", 1)[0].strip()
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip().lower()] = value.strip()
    return values


def read_config(path: Path = CONFIG_FILE) -> RelayConfig | None:
    values = _config_values(path)
    url = values.get("relay_url", "").rstrip("/")
    token = values.get("relay_token", "")
    parsed = urlparse(url)
    if parsed.scheme not in ("http", "https") or not parsed.netloc or len(token) < 32:
        return None
    return RelayConfig(url=url, token=token)


def is_configured(path: Path = CONFIG_FILE) -> bool:
    # Treat the URL as the mode switch even if the token or URL is malformed.
    # A damaged work-Mac config must fail closed and preserve the display's last
    # remote state, never silently fall back to that Mac's local Claude/Codex.
    return bool(_config_values(path).get("relay_url", "").strip())


def _wire_payload(value: object) -> dict | None:
    if not isinstance(value, dict):
        return None
    try:
        encoded = json.dumps(value, separators=(",", ":")).encode("utf-8")
    except (TypeError, ValueError):
        return None
    if len(encoded) >= WIRE_LIMIT:
        return None
    return copy.deepcopy(value)


def _slot(body: dict, name: str, now: float, max_age: int) -> tuple[dict | None, float | None]:
    slot = body.get(name)
    if not isinstance(slot, dict):
        return None, None
    observed_at = slot.get("observed_at")
    if not isinstance(observed_at, (int, float)):
        return None, None
    age = now - float(observed_at)
    if age < -60 or age > max_age:
        return None, None
    return _wire_payload(slot.get("payload")), max(0.0, age)


def fetch_state(
    *,
    config_path: Path = CONFIG_FILE,
    now: float | None = None,
    client_factory: Callable[..., httpx.Client] = httpx.Client,
) -> RelayState:
    """Fetch one authenticated relay envelope; failures are safe omissions."""
    config = read_config(config_path)
    if config is None:
        return RelayState(False)
    now = time.time() if now is None else now
    try:
        with client_factory(timeout=REQUEST_TIMEOUT) as client:
            response = client.get(
                config.url,
                headers={"Authorization": f"Bearer {config.token}"},
            )
            response.raise_for_status()
            if len(response.content) > 64 * 1024:
                return RelayState(False)
            body = response.json()
    except (httpx.HTTPError, json.JSONDecodeError, ValueError, TypeError):
        return RelayState(False)
    if not isinstance(body, dict) or body.get("version") != ENVELOPE_VERSION:
        return RelayState(False)
    generated_at = body.get("generated_at")
    if not isinstance(generated_at, (int, float)):
        return RelayState(False)
    envelope_age = now - float(generated_at)
    if envelope_age < -60 or envelope_age > ENVELOPE_MAX_AGE:
        return RelayState(False)

    usage, _ = _slot(body, "usage", now, USAGE_MAX_AGE)
    pluribus, pluribus_age = _slot(body, "pluribus", now, PLURIBUS_MAX_AGE)
    # The compact Pluribus payload carries age-at-observation. Account for relay
    # time so a newly started work Mac never presents old activity as "JUST NOW".
    if pluribus and pluribus_age is not None:
        pb = pluribus.get("pb")
        if isinstance(pb, dict):
            base_age = pb.get("a", 0)
            if isinstance(base_age, int) and base_age >= 0:
                pb["a"] = base_age + int(pluribus_age // 60)
    return RelayState(True, usage=usage, pluribus=pluribus)
