#!/usr/bin/env python3
"""Passive Codex (OpenAI) usage source.

Codex CLI records a "token_count" event in its session logs
(``$CODEX_HOME/sessions/YYYY/MM/DD/*.jsonl``, default ``~/.codex``) after every
agent turn. The event carries ``rate_limits`` (used percent, window length,
reset epoch, plan type) and the session's cumulative token usage, which is
everything Clawdmeter needs to render a Codex screen.

This module only reads those local logs: no network calls, no auth handling.
When Codex isn't installed or has never run, :func:`codex_payload` returns
``None`` and the daemon simply omits the field, so devices and hosts without
Codex see no change.

Staleness note: ``used_percent`` reflects the last local Codex activity, but
the reset countdown is recomputed from ``resets_at`` on every poll, so it
stays live even between Codex sessions. A window whose reset epoch has passed
is reported as 0% used.
"""

from __future__ import annotations

import json
import os
import time
from pathlib import Path

# Only the tail of a session log matters (we want the LAST rate_limits event);
# capping the scan keeps a 60s poll cheap even against very long sessions.
MAX_SCAN_BYTES = 4 * 1024 * 1024


def _sessions_dir() -> Path:
    home = os.environ.get("CODEX_HOME")
    base = Path(home).expanduser() if home else Path.home() / ".codex"
    return base / "sessions"


def _day_dir(sessions: Path, when: float) -> Path:
    t = time.localtime(when)
    return sessions / f"{t.tm_year:04d}" / f"{t.tm_mon:02d}" / f"{t.tm_mday:02d}"


def _last_event(path: Path, marker: str) -> dict | None:
    """Parse the last line of ``path`` containing ``marker``, or None."""
    try:
        size = path.stat().st_size
        last: bytes | None = None
        with open(path, "rb") as f:
            if size > MAX_SCAN_BYTES:
                f.seek(size - MAX_SCAN_BYTES)
                f.readline()  # drop the partial line the seek landed in
            needle = marker.encode()
            for raw in f:
                if needle in raw:
                    last = raw
        if last is None:
            return None
        return json.loads(last)
    except (OSError, json.JSONDecodeError, UnicodeDecodeError):
        return None


def _window(w: dict | None, now: float) -> dict | None:
    """Map one rate-limit window to payload fields, or None if unusable."""
    if not isinstance(w, dict):
        return None
    pct = w.get("used_percent")
    if not isinstance(pct, (int, float)):
        return None
    resets = w.get("resets_at")
    mins_left = -1
    if isinstance(resets, (int, float)) and resets > 0:
        if resets <= now:
            pct, mins_left = 0.0, -1  # window lapsed since the last log entry
        else:
            mins_left = int(round((resets - now) / 60))
    return {
        "p": round(float(pct), 1),
        "rm": mins_left,
        "wm": int(w.get("window_minutes") or 0),
    }


def codex_payload(now: float | None = None) -> dict | None:
    """Build the payload's "x" object from local Codex CLI logs, or None.

    Keys (terse, matching the payload's style): "p"/"rm"/"wm" = primary
    window used % / minutes to reset / window length in minutes;
    "p2"/"rm2"/"wm2" = the secondary window when the plan has one;
    "ti"/"to" = today's input/output tokens summed across local sessions;
    "da" = average total tokens/day over the prior 7 days' active days.
    """
    if now is None:
        now = time.time()
    sessions = _sessions_dir()

    # Newest rate_limits event wins; look at today's and yesterday's logs so a
    # just-after-midnight poll still finds last evening's session.
    files: list[Path] = []
    for offset in (0, 86400):
        d = _day_dir(sessions, now - offset)
        if d.is_dir():
            files.extend(d.glob("*.jsonl"))
    if not files:
        return None
    files.sort(key=lambda p: p.stat().st_mtime, reverse=True)

    limits = None
    for path in files:
        evt = _last_event(path, '"rate_limits"')
        if evt:
            limits = (evt.get("payload") or {}).get("rate_limits")
            if limits:
                break
    primary = _window((limits or {}).get("primary"), now)
    if primary is None:
        return None

    out = {"p": primary["p"], "rm": primary["rm"], "wm": primary["wm"]}
    secondary = _window((limits or {}).get("secondary"), now)
    if secondary is not None:
        out.update({"p2": secondary["p"], "rm2": secondary["rm"],
                    "wm2": secondary["wm"]})

    # Today's tokens: each session log's last total_token_usage is cumulative
    # for that session, so one number per file, summed across today's files.
    tokens_in, tokens_out = _day_tokens(_day_dir(sessions, now))
    out["ti"] = tokens_in
    out["to"] = tokens_out

    # 7-day daily average (total tokens/day over the 7 days before today,
    # counting only days with activity). Codex exposes no daily rate limit,
    # so this is the baseline the device's "Daily" panel measures against.
    day_totals = []
    for i in range(1, 8):
        d_in, d_out = _day_tokens(_day_dir(sessions, now - i * 86400))
        if d_in + d_out > 0:
            day_totals.append(d_in + d_out)
    out["da"] = int(sum(day_totals) / len(day_totals)) if day_totals else 0
    return out


def _day_tokens(day_dir: Path) -> tuple[int, int]:
    """Sum (input, output) tokens across one day-dir's session logs."""
    tokens_in = tokens_out = 0
    if day_dir.is_dir():
        for path in day_dir.glob("*.jsonl"):
            evt = _last_event(path, '"total_token_usage"')
            if not evt:
                continue
            usage = (((evt.get("payload") or {}).get("info") or {})
                     .get("total_token_usage") or {})
            tokens_in += int(usage.get("input_tokens") or 0)
            tokens_out += int(usage.get("output_tokens") or 0)
    return tokens_in, tokens_out
