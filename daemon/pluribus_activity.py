"""Local Pluribus latest-activity source for the Clawdmeter daemon.

This module deliberately has no dependency on the Pluribus Python package.  It
resolves the same small configuration surface as Pluribus itself, reads the
existing local bearer token, and turns the authenticated API response into the
compact ``pb`` object understood by the display firmware.
"""

from __future__ import annotations

import datetime as dt
import json
import os
import sys
import tomllib
import unicodedata
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping

import httpx


ACTIVITY_URL = "http://127.0.0.1:8077/api/activity/latest"
REQUEST_TIMEOUT = 2.5
TITLE_BYTES = 64
DETAIL_BYTES = 64
KIND_BYTES = 20
STATUS_BYTES = 16
WIRE_LIMIT = 384  # generous margin below the firmware's 512-byte RX buffer
VALID_STATUSES = {"success", "needs_review", "failed"}


@dataclass(frozen=True)
class PollResult:
    """A successful wire update, or an unavailable/malformed poll to omit."""

    available: bool
    payload: dict | None = None


def _default_library(home: Path | None = None, platform: str | None = None) -> Path:
    home = home or Path.home()
    platform = platform or sys.platform
    if platform == "darwin":
        return home / "Library" / "Application Support" / "Pluribus"
    data_home = os.environ.get("XDG_DATA_HOME")
    return (Path(data_home) if data_home else home / ".local" / "share") / "pluribus"


def _config_candidates(env: Mapping[str, str], cwd: Path | None = None) -> list[Path]:
    candidates: list[Path] = []
    if env.get("PLURIBUS_CONFIG"):
        candidates.append(Path(env["PLURIBUS_CONFIG"]).expanduser())
    candidates.append((cwd or Path.cwd()) / "pluribus.toml")
    # The normal personal installation keeps the two source trees as siblings.
    candidates.append(Path(__file__).resolve().parents[2] / "pluribus-2.0" / "pluribus.toml")
    return candidates


def _load_toml(env: Mapping[str, str], cwd: Path | None = None) -> dict:
    for path in _config_candidates(env, cwd):
        try:
            if path.is_file():
                with path.open("rb") as handle:
                    return tomllib.load(handle)
        except (OSError, tomllib.TOMLDecodeError):
            continue
    return {}


def resolve_auth_token(
    env: Mapping[str, str] | None = None,
    *,
    cwd: Path | None = None,
    home: Path | None = None,
    platform: str | None = None,
) -> str | None:
    """Resolve the existing Pluribus token without creating or logging one."""
    env = env or os.environ
    config = _load_toml(env, cwd)
    configured = env.get("PLURIBUS_AUTH_TOKEN") or config.get("auth", {}).get("token")
    if isinstance(configured, str) and configured.strip():
        return configured.strip()

    raw_library = env.get("PLURIBUS_LIBRARY") or config.get("paths", {}).get("library")
    library = (
        Path(raw_library).expanduser()
        if isinstance(raw_library, str) and raw_library.strip()
        else _default_library(home, platform)
    )
    try:
        token = (library / "auth_token").read_text(encoding="utf-8").strip()
    except OSError:
        return None
    return token or None


def display_text(value: object, byte_limit: int) -> str:
    """Return compact printable ASCII, capped by encoded bytes before JSON."""
    if not isinstance(value, str):
        return ""
    replacements = {
        "\u2018": "'", "\u2019": "'", "\u201c": '"', "\u201d": '"',
        "\u2013": "-", "\u2014": "-", "\u2026": "...", "\u00a0": " ",
    }
    text = "".join(replacements.get(ch, ch) for ch in value)
    text = unicodedata.normalize("NFKD", text)
    text = "".join(ch for ch in text if not unicodedata.combining(ch))
    text = text.encode("ascii", "replace").decode("ascii")
    text = " ".join(text.split())
    encoded = text.encode("utf-8")
    if len(encoded) <= byte_limit:
        return text
    suffix = b"..."
    return encoded[: max(1, byte_limit - len(suffix))].decode("utf-8", "ignore").rstrip() + "..."


def _parse_occurred_at(value: object) -> dt.datetime | None:
    if not isinstance(value, str) or not value.strip():
        return None
    raw = value.strip()
    if raw.endswith("Z"):
        raw = raw[:-1] + "+00:00"
    try:
        parsed = dt.datetime.fromisoformat(raw)
    except ValueError:
        return None
    if parsed.tzinfo is None:
        return None
    return parsed.astimezone(dt.timezone.utc)


def compact_activity(activity: object, *, now: dt.datetime | None = None) -> dict | None:
    """Validate one API activity and return its byte-bounded BLE representation."""
    if not isinstance(activity, dict):
        return None
    event_id = activity.get("id")
    if isinstance(event_id, bool) or not isinstance(event_id, int) or event_id < 0:
        return None
    status = display_text(activity.get("status"), STATUS_BYTES)
    if status not in VALID_STATUSES:
        return None
    title = display_text(activity.get("title"), TITLE_BYTES)
    occurred_at = _parse_occurred_at(activity.get("occurred_at"))
    if not title or occurred_at is None:
        return None
    now = now or dt.datetime.now(dt.timezone.utc)
    if now.tzinfo is None:
        now = now.replace(tzinfo=dt.timezone.utc)
    age = max(0, int((now.astimezone(dt.timezone.utc) - occurred_at).total_seconds() // 60))
    wire = {
        "id": event_id,
        "k": display_text(activity.get("kind"), KIND_BYTES),
        "s": status,
        "t": title,
        "d": display_text(activity.get("detail"), DETAIL_BYTES),
        "a": age,
    }
    encoded = json.dumps({"pb": wire}, separators=(",", ":")).encode("utf-8")
    return wire if len(encoded) < WIRE_LIMIT else None


def fetch_latest(
    *,
    env: Mapping[str, str] | None = None,
    cwd: Path | None = None,
    home: Path | None = None,
    platform: str | None = None,
    now: dt.datetime | None = None,
    client_factory=httpx.Client,
) -> PollResult:
    """Fetch latest activity; unavailable means the daemon must send nothing."""
    token = resolve_auth_token(env, cwd=cwd, home=home, platform=platform)
    if not token:
        return PollResult(False)
    try:
        with client_factory(timeout=REQUEST_TIMEOUT) as client:
            response = client.get(
                ACTIVITY_URL,
                headers={"Authorization": f"Bearer {token}"},
            )
            response.raise_for_status()
            body = response.json()
    except (httpx.HTTPError, json.JSONDecodeError, ValueError, TypeError):
        return PollResult(False)
    if not isinstance(body, dict) or "activity" not in body:
        return PollResult(False)
    if body["activity"] is None:
        return PollResult(True, {"pb": None})
    wire = compact_activity(body["activity"], now=now)
    if wire is None:
        return PollResult(False)
    return PollResult(True, {"pb": wire})
