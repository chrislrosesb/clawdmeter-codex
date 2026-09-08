import json
import threading
from urllib.error import HTTPError
from urllib.request import Request, urlopen

import pytest

from daemon import relay_server


def test_load_config_validates_required_fields(tmp_path):
    path = tmp_path / "relay.json"
    path.write_text(
        json.dumps({"bind_host": "100.64.0.1", "port": 8765, "token": "x" * 32})
    )
    assert relay_server.load_config(path) == relay_server.ServerConfig(
        "100.64.0.1", 8765, "x" * 32
    )

    path.write_text(json.dumps({"bind_host": "", "port": 0, "token": "short"}))
    with pytest.raises(ValueError):
        relay_server.load_config(path)


def test_state_store_preserves_zero_timestamps_and_copies_payloads():
    store = relay_server.StateStore()
    usage = {"ok": True, "s": 8}
    store.update_usage(usage, observed_at=0)
    first = store.snapshot(generated_at=0)
    assert first == {
        "version": 1,
        "generated_at": 0,
        "usage": {"observed_at": 0, "payload": usage},
    }
    first["usage"]["payload"]["s"] = 99
    assert store.snapshot(generated_at=0)["usage"]["payload"]["s"] == 8


def test_state_store_rejects_oversized_payload():
    store = relay_server.StateStore()
    with pytest.raises(ValueError):
        store.update_pluribus({"pb": {"t": "x" * relay_server.MAX_WIRE_BYTES}})


def test_http_endpoint_requires_bearer_token_and_hides_unknown_paths():
    store = relay_server.StateStore()
    store.update_usage({"ok": True, "s": 5}, observed_at=100)
    token = "secret-token-" + "x" * 32
    server = relay_server.ThreadingHTTPServer(
        ("127.0.0.1", 0), relay_server.make_handler(store, token)
    )
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}"
    try:
        with pytest.raises(HTTPError) as denied:
            urlopen(base + "/v1/state", timeout=2)
        assert denied.value.code == 401

        request = Request(
            base + "/v1/state", headers={"Authorization": f"Bearer {token}"}
        )
        with urlopen(request, timeout=2) as response:
            body = json.load(response)
            assert response.headers["Cache-Control"] == "no-store"
        assert body["version"] == 1
        assert body["usage"]["payload"] == {"ok": True, "s": 5}

        unknown = Request(
            base + "/private", headers={"Authorization": f"Bearer {token}"}
        )
        with pytest.raises(HTTPError) as missing:
            urlopen(unknown, timeout=2)
        assert missing.value.code == 404
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)
