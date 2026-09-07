import asyncio
import io
import json
import struct
import zlib
from unittest.mock import AsyncMock, patch

from PIL import Image

from daemon import claude_usage_daemon as daemon
from daemon import now_playing


def test_display_text_transliterates_and_bounds():
    assert now_playing.display_text("Beyonc\u00e9 \u2014 D\u00e9j\u00e0 Vu", 40) == "Beyonce - Deja Vu"
    assert now_playing.display_text("x" * 20, 10) == "xxxxxxx..."


def test_wire_payload_is_small_and_has_matching_generation():
    track = now_playing.Track("Song", "Artist", "Album", 243, 64, "abcd1234")
    payload = track.wire_payload()
    assert payload["np"]["g"] == 0xABCD
    assert payload["np"]["p"] == 1
    assert len(json.dumps(payload, separators=(",", ":")).encode()) < 512


def test_rgb565_frame_has_fixed_size_and_black_rounded_corner():
    source = io.BytesIO()
    Image.new("RGB", (20, 40), (255, 0, 0)).save(source, "PNG")
    frame = now_playing._rgb565_frame(source.getvalue(), size=32)
    assert len(frame) == 32 * 32 * 2
    assert frame[:2] == b"\x00\x00"
    center = ((16 * 32) + 16) * 2
    assert frame[center:center + 2] == b"\x00\xf8"


def test_jpeg_frame_is_baseline_compact_and_rounded():
    source = io.BytesIO()
    Image.new("RGB", (20, 40), (255, 0, 0)).save(source, "PNG")
    frame = now_playing._jpeg_frame(source.getvalue(), size=32)
    assert frame.startswith(b"\xff\xd8") and frame.endswith(b"\xff\xd9")
    assert len(frame) < 32 * 32 * 2
    with Image.open(io.BytesIO(frame)) as decoded:
        assert decoded.size == (32, 32)
        assert decoded.info.get("progressive") in (None, 0, False)
        assert max(decoded.getpixel((0, 0))) < 20


def test_query_parses_music_fields():
    output = now_playing.FIELD_SEP.join(["Song", "Artist", "Album", "200", "31", "pid"])
    completed = type("Result", (), {"stdout": output + "\n", "returncode": 0})()
    with patch("daemon.now_playing.subprocess.run", return_value=completed):
        track = now_playing.query()
    assert track is not None
    assert (track.title, track.elapsed, track.duration) == ("Song", 31, 200)


def test_artwork_transport_is_framed_ordered_and_checked():
    client = type("Client", (), {})()
    client.write_gatt_char = AsyncMock(return_value=None)
    session = daemon.Session(client)
    source = io.BytesIO()
    Image.new("RGB", (32, 16), (10, 20, 30)).save(
        source, "JPEG", progressive=False
    )
    frame = source.getvalue()
    ok = asyncio.run(session.write_artwork(frame, 0x1234, width=32, height=16))
    assert ok
    calls = client.write_gatt_char.await_args_list
    start, uuid, response = calls[0].args[1], calls[0].args[0], calls[0].kwargs["response"]
    assert uuid == daemon.RX_CHAR_UUID
    assert response is True
    magic, kind, generation, width, height, encoding, total, crc = struct.unpack(
        "<BBHHHBII", start
    )
    assert (magic, kind, generation, width, height, total) == (
        daemon.ART_MAGIC, daemon.ART_START, 0x1234, 32, 16, len(frame)
    )
    assert encoding == daemon.ART_ENCODING_JPEG
    assert crc == zlib.crc32(frame) & 0xFFFFFFFF
    rebuilt = bytearray()
    expected_offset = 0
    for call in calls[1:]:
        packet = call.args[1]
        magic, kind, generation, offset = struct.unpack("<BBHI", packet[:8])
        assert (magic, kind, generation, offset) == (
            daemon.ART_MAGIC, daemon.ART_CHUNK, 0x1234, expected_offset
        )
        rebuilt.extend(packet[8:])
        expected_offset += len(packet) - 8
    assert bytes(rebuilt) == frame
