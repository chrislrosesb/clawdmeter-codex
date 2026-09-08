import json

import httpx

from daemon import relay_source


def _client_factory(handler):
    transport = httpx.MockTransport(handler)

    def factory(**kwargs):
        return httpx.Client(transport=transport, **kwargs)

    return factory


def _write_config(path, *, url="http://100.64.0.1:8765/v1/state", token="t" * 64):
    path.write_text(f"relay_url = {url}\nrelay_token = {token}\n", encoding="utf-8")


def test_read_config_requires_valid_url_and_token(tmp_path):
    path = tmp_path / "config"
    _write_config(path)
    assert relay_source.read_config(path) == relay_source.RelayConfig(
        "http://100.64.0.1:8765/v1/state", "t" * 64
    )

    path.write_text("relay_url = file:///tmp/state\nrelay_token = secret\n")
    assert relay_source.read_config(path) is None
    assert relay_source.is_configured(path)  # fail closed instead of polling local usage


def test_fetch_state_authenticates_and_adjusts_pluribus_age(tmp_path):
    path = tmp_path / "config"
    _write_config(path)
    seen = {}
    body = {
        "version": 1,
        "generated_at": 995,
        "usage": {"observed_at": 990, "payload": {"ok": True, "s": 12}},
        "pluribus": {
            "observed_at": 940,
            "payload": {"pb": {"t": "Saved note", "a": 2}},
        },
    }

    def handler(request):
        seen["url"] = str(request.url)
        seen["auth"] = request.headers.get("authorization")
        return httpx.Response(200, json=body)

    state = relay_source.fetch_state(
        config_path=path, now=1000, client_factory=_client_factory(handler)
    )
    assert state.available
    assert state.usage == {"ok": True, "s": 12}
    assert state.pluribus == {"pb": {"t": "Saved note", "a": 3}}
    assert seen == {
        "url": "http://100.64.0.1:8765/v1/state",
        "auth": "Bearer " + "t" * 64,
    }
    # The response object is not mutated while age is adjusted for the display.
    assert body["pluribus"]["payload"]["pb"]["a"] == 2


def test_fetch_state_rejects_http_error_and_stale_envelope(tmp_path):
    path = tmp_path / "config"
    _write_config(path)

    denied = relay_source.fetch_state(
        config_path=path,
        now=1000,
        client_factory=_client_factory(lambda _request: httpx.Response(401)),
    )
    assert denied == relay_source.RelayState(False)

    stale_body = {"version": 1, "generated_at": 700}
    stale = relay_source.fetch_state(
        config_path=path,
        now=1000,
        client_factory=_client_factory(
            lambda _request: httpx.Response(200, json=stale_body)
        ),
    )
    assert stale == relay_source.RelayState(False)


def test_fetch_state_omits_stale_or_oversized_slots(tmp_path):
    path = tmp_path / "config"
    _write_config(path)
    body = {
        "version": 1,
        "generated_at": 1000,
        "usage": {"observed_at": 800, "payload": {"ok": True}},
        "pluribus": {
            "observed_at": 999,
            "payload": {"pb": {"t": "x" * relay_source.WIRE_LIMIT}},
        },
    }
    state = relay_source.fetch_state(
        config_path=path,
        now=1000,
        client_factory=_client_factory(lambda _request: httpx.Response(200, json=body)),
    )
    assert state == relay_source.RelayState(True, usage=None, pluribus=None)


def test_fetch_state_rejects_wrong_version_and_large_response(tmp_path):
    path = tmp_path / "config"
    _write_config(path)

    wrong = relay_source.fetch_state(
        config_path=path,
        now=1000,
        client_factory=_client_factory(
            lambda _request: httpx.Response(200, json={"version": 2, "generated_at": 1000})
        ),
    )
    assert not wrong.available

    huge = json.dumps({"padding": "x" * (65 * 1024)}).encode()
    too_large = relay_source.fetch_state(
        config_path=path,
        now=1000,
        client_factory=_client_factory(
            lambda _request: httpx.Response(200, content=huge)
        ),
    )
    assert not too_large.available
