import binascii
import importlib.util
import json
from pathlib import Path
import time
import wave

import pytest

spec = importlib.util.spec_from_file_location("capture_audio", Path(__file__).parents[1] / "capture_audio.py")
audio = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audio)
ID = "1234abcd"
PCM = bytes(range(256)) * 1250
CRC = binascii.crc32(PCM)


def packet(kind, **fields):
    return {"id": ID, "type": kind, **fields}


def header(**overrides):
    return packet("begin", **({"version": 1, "rate": 16000, "channels": 1,
                              "bits": 16, "samples": 160000, "bytes": 320000,
                              "crc32": CRC, "capture_ms": 10000,
                              "overruns": 0, "read_errors": 0} | overrides))


def ready():
    receiver = audio.Receiver(ID)
    receiver.accept(packet("start"))
    receiver.accept(header())
    return receiver


def feed(receiver):
    for offset in range(0, len(PCM), 192):
        receiver.accept(packet("data", offset=offset, hex=PCM[offset:offset + 192].hex()))


def test_complete_recording_and_wav(tmp_path):
    receiver = ready()
    feed(receiver)
    receiver.accept(packet("end", crc32=CRC))
    assert receiver.done
    path = audio.save_wav(tmp_path / "test.wav", receiver.data)
    assert path.stat().st_size == 320044
    with wave.open(str(path)) as wav:
        assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getnframes()) == (1, 2, 16000, 160000)
        assert wav.readframes(160000) == PCM


@pytest.mark.parametrize("override", [{"rate": 44100}, {"bytes": 999999999}, {"version": 2},
                                     {"channels": True}, {"overruns": 1}, {"capture_ms": 1000},
                                     {"crc32": -1}, {"read_errors": 1}])
def test_reject_bad_header(override):
    receiver = audio.Receiver(ID)
    receiver.accept(packet("start"))
    with pytest.raises(audio.CaptureError):
        receiver.accept(header(**override))


def test_truncated_or_corrupt():
    receiver = ready()
    with pytest.raises(audio.CaptureError, match="Incomplete"):
        receiver.accept(packet("end", crc32=CRC))
    feed(receiver)
    with pytest.raises(audio.CaptureError, match="checksum"):
        receiver.accept(packet("end", crc32=CRC ^ 1))


@pytest.mark.parametrize("fields", [{"offset": 1, "hex": "00" * 192},
                                   {"offset": 0, "hex": "00"},
                                   {"offset": 0, "hex": "gg" * 192},
                                   {"offset": 0, "hex": "00" * 193}])
def test_invalid_chunk(fields):
    with pytest.raises(audio.CaptureError):
        ready().accept(packet("data", **fields))


def test_duplicate_and_stale():
    receiver = ready()
    receiver.accept({"type": "data", "id": "deadbeef", "offset": 0})
    chunk = packet("data", offset=0, hex=PCM[:192].hex())
    receiver.accept(chunk)
    with pytest.raises(audio.CaptureError):
        receiver.accept(chunk)


def test_no_overwrite_or_incomplete_file(tmp_path):
    path = tmp_path / "existing.wav"
    path.write_bytes(b"original")
    with pytest.raises(FileExistsError):
        audio.save_wav(path, PCM)
    assert path.read_bytes() == b"original"
    with pytest.raises(audio.CaptureError):
        audio.save_wav(tmp_path / "bad.wav", PCM[:-1])
    assert list(tmp_path.iterdir()) == [path]


class Port:
    def __init__(self, data):
        self.data = bytearray(data)

    def read(self, count):
        result = bytes(self.data[:count])
        del self.data[:count]
        return result


def test_fragmented_serial_with_logs():
    data = b"boot log\nother log\nAUDIO1 " + json.dumps(packet("start")).encode() + b"\n"
    assert next(audio.packets(Port(data), time.monotonic() + 1)) == packet("start")


@pytest.mark.parametrize("data", [b"AUDIO1 not-json\n", b"AUDIO1 []\n", b"X" * 2049])
def test_bad_serial(data):
    with pytest.raises(audio.CaptureError):
        next(audio.packets(Port(data), time.monotonic() + 1))


def test_timeout():
    with pytest.raises(audio.CaptureError, match="Timed out"):
        next(audio.packets(Port(b""), time.monotonic() - 1))


def test_device_failure_and_data_before_header():
    with pytest.raises(audio.CaptureError, match="Device rejected"):
        ready().accept(packet("error", reason="i2s_read"))
    with pytest.raises(audio.CaptureError):
        audio.Receiver(ID).accept(packet("data", offset=0, hex="00"))
