import datetime as dt
import json

import httpx

from daemon import agent_messages as source

NOW = dt.datetime(2026, 10, 10, 12, 0, 0)


def entry(i, agent, text="Hello there.", minutes=5, dismissed=False):
    created = (NOW - dt.timedelta(minutes=minutes)).isoformat(timespec="seconds")
    return {"id": f"{i:012x}", "agent_id": agent, "text": text, "kind": "heartbeat",
            "created_at": created, "session_id": agent, "dismissed": dismissed}


def test_newest_message_per_agent_with_total_count():
    feed = [  # newest first, as Pluribus returns it
        entry(1, "champion", "Proud of today's pitch! More soon.", 2),
        entry(2, "skeptic", "Check the Q4 margin math.", 9),
        entry(3, "champion", "Older Tori note.", 30),
        entry(4, "sales-marketing", "Silver Air pricing note", 14),
        entry(5, "coach", "A fourth agent stays counted but not shown.", 40),
    ]
    am = source.compact_feed(feed, now=NOW)["am"]
    assert am["n"] == 5
    assert [m["g"] for m in am["m"]] == ["champion", "skeptic", "sales-marketing"]
    assert am["m"][0] == {"i": "000000000001", "g": "champion", "w": "Tori",
                          "t": "Proud of today's pitch!", "a": 2}
    assert am["m"][2]["w"] == "Mila"


def test_long_captions_trim_at_a_word_and_frame_fits_the_device():
    long = "Saw a fascinating long article about regional airline pricing strategies today"
    feed = [entry(i, a, long * 3) for i, a in enumerate(["darwin", "fde", "pluribus"])]
    frame = source.compact_feed(feed, now=NOW)
    for m in frame["am"]["m"]:
        assert len(m["t"]) <= source.CAPTION_BYTES and m["t"].endswith("...")
    assert len(json.dumps(frame, separators=(",", ":"))) < source.WIRE_LIMIT


def test_dismissed_and_malformed_entries_are_skipped():
    feed = [entry(1, "champion", dismissed=True), {"id": 7}, "junk",
            {**entry(2, "skeptic"), "created_at": "not a date"}]
    assert source.compact_feed(feed, now=NOW) == {"am": {"n": 0, "m": []}}


def test_fetch_waiting_authenticated_and_fails_closed():
    seen = {}

    def handler(request):
        seen["auth"] = request.headers["Authorization"]
        return httpx.Response(200, json={"entries": [entry(1, "therapist")]})

    factory = lambda **kw: httpx.Client(transport=httpx.MockTransport(handler), **kw)
    result = source.fetch_waiting(env={"PLURIBUS_AUTH_TOKEN": "t" * 40},
                                  now=NOW, client_factory=factory)
    assert seen["auth"] == "Bearer " + "t" * 40
    assert result.available and result.payload["am"]["m"][0]["w"] == "Naomi"

    broken = lambda **kw: httpx.Client(
        transport=httpx.MockTransport(lambda r: httpx.Response(500)), **kw)
    assert not source.fetch_waiting(env={"PLURIBUS_AUTH_TOKEN": "t" * 40},
                                    client_factory=broken).available
