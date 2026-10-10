"""New Pluribus chat and room replies, as ``rm`` frames for the Clawdmeter.

Every finished agent reply in a shared chat or Room pops up on the device,
whether or not Pluribus thinks Chris saw it (a Mac window can be "open" on the
room while buried under other apps). Source: Pluribus's own durable reply
receipts (``_chat_reply_receipts`` on each session), read over the existing
authenticated local API. Nothing in Pluribus changes.

Each reply is its own small frame; the device queues them (oldest first),
dedups by id, and drops them after ``WINDOW_MINUTES``::

    {"rm": {"i": "<msg id>", "r": "DEAL ROOM", "g": "skeptic", "w": "Sloane",
            "t": "The margin math only works if...", "a": 3}}

Private sessions, meeting-bound chats and agent check-ins (shown separately
from the Team feed) are skipped. Only a short preview leaves the Mac.
"""

from __future__ import annotations

import datetime as dt
import json
import re
from typing import Mapping

import httpx

try:
    from daemon import agent_messages
    from daemon import pluribus_activity as pluribus
except ImportError:
    import agent_messages
    import pluribus_activity as pluribus

PollResult = pluribus.PollResult

BASE_URL = "http://127.0.0.1:8077/api/ask/sessions"
REQUEST_TIMEOUT = 2.5
WINDOW_MINUTES = 180
MAX_REPLIES = 12
TEXT_BYTES = 56            # two lines of Styrene 28 on the 480 px panel
ROOM_BYTES = 18
CONTEXTS = {"agent", "room", "general", "companion"}
SKIP_TURNS = {"room_pass", "system", "recovery", "proactive"}

_detail_cache: dict[str, tuple[str, list[dict]]] = {}   # id -> (updated_at, replies)


def _parse_local(value: object) -> dt.datetime | None:
    if not isinstance(value, str) or not value.strip():
        return None
    try:
        parsed = dt.datetime.fromisoformat(value.strip())
    except ValueError:
        return None
    if parsed.tzinfo is not None:
        parsed = parsed.astimezone().replace(tzinfo=None)
    return parsed


def preview(text: object) -> str:
    """Strip Markdown, keep the start of the reply, trim at a word."""
    if not isinstance(text, str):
        return ""
    text = re.sub(r"!\[([^\]]*)\]\([^)]*\)", lambda m: m[1] or "Attachment", text[:2000])
    text = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"(?m)^\s*(?:#{1,6}\s+|>\s*|[-*+]\s+|\d+[.)]\s+)", "", text)
    text = text.replace("**", "").replace("__", "").replace("`", "")
    line = pluribus.display_text(text, 400)
    if len(line) <= TEXT_BYTES:
        return line
    cut = line[: TEXT_BYTES - 3]
    if " " in cut:
        cut = cut[: cut.rfind(" ")]
    return cut.rstrip(" ,;:-") + "..."


def _completed(message: dict) -> bool:
    text = str(message.get("text") or "").strip()
    return bool(
        message.get("role") == "assistant"
        and message.get("status", "done") == "done"
        and text and text.upper() != "NO_REPLY"
        and message.get("turn_kind") not in SKIP_TURNS
        and not message.get("incomplete") and not message.get("stopped")
        and not text.startswith("_(stopped early")
    )


def session_replies(session: dict, now: dt.datetime) -> list[dict]:
    """Recent completed replies in one session, as ``rm`` items."""
    receipts = (session.get("_chat_reply_receipts") or {}).get("replies") or []
    by_id = {str(m.get("id")): m for m in session.get("messages") or []
             if isinstance(m, dict) and m.get("id")}
    is_room = session.get("context_kind") == "room"
    room = pluribus.display_text(session.get("label") or "Room", ROOM_BYTES).upper() if is_room else ""
    host = session.get("primary_agent_id") or "pluribus"
    out = []
    for receipt in receipts:
        if not isinstance(receipt, dict):
            continue
        message = by_id.get(str(receipt.get("message_id")))
        at = _parse_local(receipt.get("updated_at"))
        if message is None or at is None or not _completed(message):
            continue
        age = max(0, int((now - at).total_seconds() // 60))
        if age >= WINDOW_MINUTES:
            continue
        agent = str(message.get("agent_id") or host)
        out.append({
            "i": re.sub(r"[^0-9A-Za-z]", "", str(message["id"]))[:12],
            "r": room,
            "g": pluribus.display_text(agent, 16),
            "w": agent_messages.display_name(agent),
            "t": preview(message.get("text")),
            "a": age,
        })
    return out


def fetch_recent(
    *,
    env: Mapping[str, str] | None = None,
    now: dt.datetime | None = None,
    client_factory=httpx.Client,
) -> PollResult:
    """Recent replies across all shared chats and Rooms, oldest first.

    ``payload`` is ``{"replies": [...]}``; each item goes to the device as its
    own ``{"rm": item}`` frame.
    """
    token = pluribus.resolve_auth_token(env)
    if not token:
        return PollResult(False)
    now = now or dt.datetime.now()
    headers = {"Authorization": f"Bearer {token}"}
    cutoff = now - dt.timedelta(minutes=WINDOW_MINUTES)
    replies: list[dict] = []
    try:
        with client_factory(timeout=REQUEST_TIMEOUT) as client:
            response = client.get(BASE_URL, headers=headers)
            response.raise_for_status()
            sessions = response.json()
            if not isinstance(sessions, list):
                return PollResult(False)
            live = set()
            for meta in sessions:
                if not isinstance(meta, dict) or meta.get("archived"):
                    continue
                if meta.get("privacy_scope") != "shared" or meta.get("context_kind") not in CONTEXTS:
                    continue
                updated = _parse_local(meta.get("updated_at"))
                sid = meta.get("id")
                if not isinstance(sid, str) or updated is None or updated < cutoff:
                    continue
                live.add(sid)
                stamp = str(meta.get("updated_at"))
                cached = _detail_cache.get(sid)
                if cached is None or cached[0] != stamp:
                    detail = client.get(f"{BASE_URL}/{sid}", headers=headers)
                    detail.raise_for_status()
                    body = detail.json()
                    session = body.get("session", body) if isinstance(body, dict) else {}
                    # Keep receipts with their observation time; ages are
                    # recomputed below so cached entries keep aging.
                    items = session_replies(session, now)
                    for item in items:
                        item["_at"] = (now - dt.timedelta(minutes=item["a"])).isoformat()
                    _detail_cache[sid] = (stamp, items)
                for item in _detail_cache[sid][1]:
                    age = max(0, int((now - dt.datetime.fromisoformat(item["_at"])).total_seconds() // 60))
                    if age < WINDOW_MINUTES:
                        replies.append({**{k: v for k, v in item.items() if k != "_at"}, "a": age})
            for stale in set(_detail_cache) - live:
                del _detail_cache[stale]
    except (httpx.HTTPError, json.JSONDecodeError, ValueError, TypeError):
        return PollResult(False)
    replies.sort(key=lambda r: -r["a"])            # oldest first
    return PollResult(True, {"replies": replies[-MAX_REPLIES:]})


def valid_reply(item: object) -> bool:
    """Shape check for relayed items before they are written to the device."""
    if not isinstance(item, dict):
        return False
    try:
        size = len(json.dumps({"rm": item}, separators=(",", ":")))
    except (TypeError, ValueError):
        return False
    return (size < 400 and isinstance(item.get("i"), str) and item["i"]
            and isinstance(item.get("g"), str) and item["g"]
            and isinstance(item.get("a"), int) and item["a"] >= 0)
