"""Pluribus agent messages waiting for Chris, as the compact ``am`` frame.

Source: the Pluribus Team feed (``GET /api/agents/feed``), which lists the
proactive messages agents sent and Chris has not dismissed in the app.  The
device shows the newest message per agent, so the frame carries up to three
agents plus the total waiting count.  Captions are one short ASCII line; the
full message text never leaves this machine.

Wire shape (always below the firmware's 512-byte RX buffer)::

    {"am": {"n": 4, "m": [{"i": "<feed id>", "g": "champion", "w": "Tori",
                           "t": "Proud of today's pitch", "a": 2}, ...]}}
"""

from __future__ import annotations

import datetime as dt
import json
import re
from typing import Mapping

import httpx

try:
    from daemon import pluribus_activity as pluribus
except ImportError:
    import pluribus_activity as pluribus

PollResult = pluribus.PollResult

FEED_URL = "http://127.0.0.1:8077/api/agents/feed?days=1"
REQUEST_TIMEOUT = 2.5
MAX_AGENTS = 3
CAPTION_BYTES = 40
WIRE_LIMIT = 480

# Mirrors Pluribus's crew_presentation.CREW display names.
NAMES = {
    "pluribus": "Pluribus",
    "darwin": "Darwin",
    "sales-marketing": "Mila",
    "champion": "Tori",
    "coach": "Steve",
    "fde": "Gilfoyle",
    "skeptic": "Sloane",
    "therapist": "Naomi",
}


def display_name(agent_id: str) -> str:
    return NAMES.get(agent_id) or agent_id.replace("-", " ").title()[:16]


def caption(text: object) -> str:
    """First sentence or line, trimmed at a word to one display line."""
    line = pluribus.display_text(text, 400)
    if not line:
        return ""
    first = re.split(r"(?<=[.!?])\s|\s[-|]\s", line, maxsplit=1)[0].strip()
    if len(first) <= CAPTION_BYTES:
        return first.rstrip(".")
    cut = first[: CAPTION_BYTES - 3]
    if " " in cut:
        cut = cut[: cut.rfind(" ")]
    return cut.rstrip(" ,;:-") + "..."


def _age_minutes(created_at: object, now: dt.datetime) -> int | None:
    # Pluribus writes naive local timestamps (datetime.now().isoformat()).
    if not isinstance(created_at, str):
        return None
    try:
        created = dt.datetime.fromisoformat(created_at.strip())
    except ValueError:
        return None
    if created.tzinfo is not None:
        created = created.astimezone().replace(tzinfo=None)
    return max(0, int((now - created).total_seconds() // 60))


def compact_feed(entries: object, *, now: dt.datetime | None = None) -> dict:
    """Newest message per agent (feed is newest first) as the ``am`` frame."""
    now = now or dt.datetime.now()
    items: list[dict] = []
    seen: set[str] = set()
    total = 0
    for entry in entries if isinstance(entries, list) else []:
        if not isinstance(entry, dict) or entry.get("dismissed"):
            continue
        entry_id = entry.get("id")
        agent = entry.get("agent_id")
        age = _age_minutes(entry.get("created_at"), now)
        if (not isinstance(entry_id, str) or not entry_id or not isinstance(agent, str)
                or not agent or age is None):
            continue
        total += 1
        if agent in seen or len(items) >= MAX_AGENTS:
            continue
        seen.add(agent)
        items.append({
            "i": pluribus.display_text(entry_id, 12),
            "g": pluribus.display_text(agent, 16),
            "w": display_name(agent),
            "t": caption(entry.get("text")),
            "a": age,
        })
    frame = {"am": {"n": total, "m": items}}
    while items and len(json.dumps(frame, separators=(",", ":"))) >= WIRE_LIMIT:
        items.pop()
    return frame


def fetch_waiting(
    *,
    env: Mapping[str, str] | None = None,
    now: dt.datetime | None = None,
    client_factory=httpx.Client,
) -> PollResult:
    """Fetch waiting agent messages; unavailable means send nothing."""
    token = pluribus.resolve_auth_token(env)
    if not token:
        return PollResult(False)
    try:
        with client_factory(timeout=REQUEST_TIMEOUT) as client:
            response = client.get(FEED_URL, headers={"Authorization": f"Bearer {token}"})
            response.raise_for_status()
            body = response.json()
    except (httpx.HTTPError, json.JSONDecodeError, ValueError, TypeError):
        return PollResult(False)
    if not isinstance(body, dict) or not isinstance(body.get("entries"), list):
        return PollResult(False)
    return PollResult(True, compact_feed(body["entries"], now=now))
