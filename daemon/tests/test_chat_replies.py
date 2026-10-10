import datetime as dt

from daemon import chat_replies as source

NOW = dt.datetime(2026, 10, 10, 12, 0, 0)


def session(kind="room", label="Deal room"):
    def msg(i, agent, text, **kw):
        return {"id": f"{i:08d}-AAAA-BBBB", "role": "assistant", "status": "done",
                "agent_id": agent, "text": text, "turn_kind": "room", **kw}
    stamp = lambda m: (NOW - dt.timedelta(minutes=m)).isoformat()
    return {
        "context_kind": kind, "label": label, "primary_agent_id": "pluribus",
        "messages": [
            {"id": "u1", "role": "user", "text": "Thoughts on pricing?"},
            msg(1, "skeptic", "**The margin math** only works if Silver Air signs for three years, otherwise we lose money."),
            msg(2, "champion", "Love it!"),
            msg(3, "coach", "NO_REPLY"),
            msg(4, "darwin", "Morning check-in", turn_kind="proactive"),
            msg(5, "fde", "old", ),
        ],
        "_chat_reply_receipts": {"version": 1, "replies": [
            {"message_id": "00000001-AAAA-BBBB", "updated_at": stamp(4)},
            {"message_id": "00000002-AAAA-BBBB", "updated_at": stamp(1)},
            {"message_id": "00000003-AAAA-BBBB", "updated_at": stamp(1)},
            {"message_id": "00000004-AAAA-BBBB", "updated_at": stamp(1)},
            {"message_id": "00000005-AAAA-BBBB", "updated_at": stamp(500)},
        ]},
    }


def test_room_replies_become_two_line_items_and_skip_noise():
    items = source.session_replies(session(), NOW)
    assert [i["g"] for i in items] == ["skeptic", "champion"]   # no NO_REPLY, check-in, expired
    sloane = items[0]
    assert sloane["r"] == "DEAL ROOM" and sloane["w"] == "Sloane" and sloane["a"] == 4
    assert sloane["t"].startswith("The margin math only works") and sloane["t"].endswith("...")
    assert len(sloane["t"]) <= source.TEXT_BYTES and "**" not in sloane["t"]
    assert sloane["i"] == "00000001AAAA"
    assert all(source.valid_reply(i) for i in items)


def test_direct_chats_have_no_room_title():
    items = source.session_replies(session(kind="agent", label="Tori"), NOW)
    assert items and all(i["r"] == "" for i in items)
