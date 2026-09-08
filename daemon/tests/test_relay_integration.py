import asyncio

from daemon import claude_usage_daemon as daemon
from daemon import pluribus_activity, relay_source


def test_display_usage_uses_relay_without_local_poll(monkeypatch):
    monkeypatch.setattr(daemon.relay_source, "is_configured", lambda: True)
    monkeypatch.setattr(
        daemon.relay_source,
        "fetch_state",
        lambda: relay_source.RelayState(True, usage={"ok": True, "s": 42}),
    )

    async def local_poll_must_not_run():
        raise AssertionError("work Mac must not poll local Claude")

    monkeypatch.setattr(daemon, "poll_active", local_poll_must_not_run)
    payload, dead = asyncio.run(daemon.poll_display_usage())
    assert payload == {"ok": True, "s": 42}
    assert dead is False


def test_display_usage_retains_device_state_when_relay_is_unavailable(monkeypatch):
    monkeypatch.setattr(daemon.relay_source, "is_configured", lambda: True)
    monkeypatch.setattr(
        daemon.relay_source,
        "fetch_state",
        lambda: relay_source.RelayState(False),
    )
    assert asyncio.run(daemon.poll_display_usage()) == (None, False)


def test_display_pluribus_uses_remote_payload(monkeypatch):
    payload = {"pb": {"t": "Transcript ready", "a": 1}}
    monkeypatch.setattr(daemon.relay_source, "is_configured", lambda: True)
    monkeypatch.setattr(
        daemon.relay_source,
        "fetch_state",
        lambda: relay_source.RelayState(True, pluribus=payload),
    )
    assert daemon.fetch_display_pluribus() == pluribus_activity.PollResult(True, payload)
