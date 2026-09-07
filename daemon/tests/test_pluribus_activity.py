import datetime as dt
import json
from pathlib import Path

import httpx

from daemon import pluribus_activity as source


NOW = dt.datetime(2026, 9, 7, 20, 0, tzinfo=dt.timezone.utc)


def activity(**overrides):
    value = {
        "id": 42,
        "kind": "transcript",
        "status": "success",
        "title": "Transcript filed",
        "detail": "Meeting notes",
        "occurred_at": "2026-09-07T19:48:30Z",
        "deep_link": "pluribus://inbox/42",
    }
    value.update(overrides)
    return value


def test_resolve_token_uses_env_without_reading_file(tmp_path):
    assert source.resolve_auth_token(
        {"PLURIBUS_AUTH_TOKEN": "secret-env"}, cwd=tmp_path, home=tmp_path
    ) == "secret-env"


def test_resolve_token_follows_config_library_then_auth_file(tmp_path):
    library = tmp_path / "library"
    library.mkdir()
    (library / "auth_token").write_text("file-secret\n")
    (tmp_path / "pluribus.toml").write_text(f'[paths]\nlibrary = "{library}"\n')
    assert source.resolve_auth_token({}, cwd=tmp_path, home=tmp_path) == "file-secret"


def test_configured_toml_token_wins(tmp_path):
    (tmp_path / "pluribus.toml").write_text('[auth]\ntoken = "toml-secret"\n')
    assert source.resolve_auth_token({}, cwd=tmp_path, home=tmp_path) == "toml-secret"


def test_display_text_is_ascii_and_utf8_byte_bounded():
    value = source.display_text("Crème — " + "U0001f680" * 100, 20)
    assert value.isascii()
    assert len(value.encode("utf-8")) <= 20
    assert value.endswith("...")


def test_compact_activity_maps_contract_and_computes_age():
    wire = source.compact_activity(activity(), now=NOW)
    assert wire == {
        "id": 42,
        "k": "transcript",
        "s": "success",
        "t": "Transcript filed",
        "d": "Meeting notes",
        "a": 11,
    }


def test_maximum_supported_payload_keeps_fixed_margin():
    wire = source.compact_activity(
        activity(kind="k" * 200, title='"' * 200, detail="\\" * 300), now=NOW
    )
    encoded = json.dumps({"pb": wire}, separators=(",", ":")).encode()
    assert wire is not None
    assert len(encoded) < source.WIRE_LIMIT < 512


def test_malformed_or_unknown_status_is_omitted():
    assert source.compact_activity(activity(status="pending"), now=NOW) is None
    assert source.compact_activity(activity(occurred_at="yesterday"), now=NOW) is None
    assert source.compact_activity({"id": 1}, now=NOW) is None


class FakeClient:
    def __init__(self, response, calls, **_kwargs):
        self.response = response
        self.calls = calls

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False

    def get(self, url, headers):
        self.calls.append((url, headers))
        return self.response


def factory_for(response, calls):
    return lambda **kwargs: FakeClient(response, calls, **kwargs)


def test_fetch_latest_sends_authenticated_local_request_without_leaking_token(capsys):
    calls = []
    response = httpx.Response(200, json={"activity": activity()}, request=httpx.Request("GET", source.ACTIVITY_URL))
    result = source.fetch_latest(
        env={"PLURIBUS_AUTH_TOKEN": "never-print-this"}, now=NOW,
        client_factory=factory_for(response, calls),
    )
    assert result.available and result.payload["pb"]["id"] == 42
    assert calls == [(source.ACTIVITY_URL, {"Authorization": "Bearer never-print-this"})]
    assert "never-print-this" not in capsys.readouterr().out


def test_successful_null_is_explicit_clear():
    response = httpx.Response(200, json={"activity": None}, request=httpx.Request("GET", source.ACTIVITY_URL))
    result = source.fetch_latest(
        env={"PLURIBUS_AUTH_TOKEN": "secret"},
        client_factory=factory_for(response, []),
    )
    assert result == source.PollResult(True, {"pb": None})


def test_unreachable_http_error_malformed_and_missing_token_are_omitted():
    def broken_factory(**_kwargs):
        raise httpx.ConnectError("offline")

    assert not source.fetch_latest(env={}, cwd=Path("/not/here"), home=Path("/not/here")).available
    assert not source.fetch_latest(
        env={"PLURIBUS_AUTH_TOKEN": "secret"}, client_factory=broken_factory
    ).available
    bad = httpx.Response(200, content=b"not-json", request=httpx.Request("GET", source.ACTIVITY_URL))
    assert not source.fetch_latest(
        env={"PLURIBUS_AUTH_TOKEN": "secret"}, client_factory=factory_for(bad, [])
    ).available


def test_stale_activity_is_sent_for_firmware_to_expire():
    wire = source.compact_activity(activity(occurred_at="2026-09-05T00:00:00Z"), now=NOW)
    assert wire is not None
    assert wire["a"] > 24 * 60
