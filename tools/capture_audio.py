#!/usr/bin/env python3
"""Explicit ten-second Clawdmeter capture. No transcription, upload, or autostart."""
import argparse
import array
import binascii
import json
import math
import os
from pathlib import Path
import sys
import tempfile
import time
import uuid
import wave

SAMPLES = 160000
BYTE_COUNT = SAMPLES * 2


class CaptureError(Exception):
    pass


def packets(port, deadline):
    """Bounded streaming line decoder; tolerate ordinary device diagnostics."""
    pending = bytearray()
    while time.monotonic() < deadline:
        data = port.read(min(max(getattr(port, "in_waiting", 0), 1), 1024))
        if not data:
            continue
        pending.extend(data)
        while b"\n" in pending:
            raw, _, rest = pending.partition(b"\n")
            pending = bytearray(rest)
            if len(raw) > 2048:
                raise CaptureError("Oversized serial line")
            if not raw.startswith(b"AUDIO1 "):
                continue
            try:
                packet = json.loads(raw[7:])
            except (ValueError, UnicodeError) as exc:
                raise CaptureError("Malformed audio packet") from exc
            if not isinstance(packet, dict):
                raise CaptureError("Invalid audio packet")
            yield packet
        if len(pending) > 2048:
            raise CaptureError("Serial line exceeded size limit")
    raise CaptureError("Timed out waiting for the device; no WAV was saved")


class Receiver:
    """Strict per-recording state machine, independent of serial hardware."""
    def __init__(self, recording_id):
        self.id = recording_id
        self.started = False
        self.header = None
        self.data = bytearray()
        self.done = False
        self.memory = []
        self.start = None
        self.health = None

    def accept(self, packet):
        if packet.get("type") == "error" and packet.get("id") in (None, self.id):
            raise CaptureError(f"Device rejected capture: {packet.get('reason', 'unknown')}")
        if packet.get("id") != self.id:
            return
        kind = packet.get("type")
        if kind == "memory":
            self.memory.append(packet)
        elif kind == "health":
            self.health = packet
        elif kind == "start":
            if self.started or self.header:
                raise CaptureError("Duplicate recording start")
            self.started = True
            self.start = packet
        elif kind == "begin":
            expected = {"version": 1, "rate": 16000, "channels": 1, "bits": 16,
                        "samples": SAMPLES, "bytes": BYTE_COUNT,
                        "overruns": 0, "read_errors": 0}
            if not self.started or self.header:
                raise CaptureError("Unexpected recording header")
            for key, value in expected.items():
                if type(packet.get(key)) is not int or packet[key] != value:
                    raise CaptureError(f"Unexpected format or diagnostic: {key}")
            if type(packet.get("capture_ms")) is not int or not 9500 <= packet["capture_ms"] <= 11000:
                raise CaptureError("Unexpected recording duration")
            if type(packet.get("crc32")) is not int or not 0 <= packet["crc32"] <= 0xffffffff:
                raise CaptureError("Invalid CRC field")
            self.header = packet
        elif kind == "data":
            if not self.header or self.done:
                raise CaptureError("Audio data outside a recording")
            if type(packet.get("offset")) is not int or packet["offset"] != len(self.data):
                raise CaptureError("Missing, duplicate, or out-of-order audio packet")
            encoded = packet.get("hex")
            if not isinstance(encoded, str) or not 2 <= len(encoded) <= 384:
                raise CaptureError("Invalid audio chunk size")
            try:
                chunk = binascii.unhexlify(encoded)
            except (ValueError, binascii.Error) as exc:
                raise CaptureError("Invalid audio encoding") from exc
            expected = min(192, BYTE_COUNT - len(self.data))
            if len(chunk) != expected or expected == 0:
                raise CaptureError("Incorrect audio chunk length")
            self.data.extend(chunk)
        elif kind == "end":
            if not self.header or self.done or len(self.data) != BYTE_COUNT:
                raise CaptureError("Incomplete recording")
            crc = binascii.crc32(self.data)
            if packet.get("crc32") != crc or self.header["crc32"] != crc:
                raise CaptureError("Audio checksum failed")
            self.done = True
        else:
            raise CaptureError(f"Unexpected audio packet type: {kind}")


def save_wav(path, pcm):
    """Publish only complete audio; never replace an existing file or symlink."""
    if len(pcm) != BYTE_COUNT:
        raise CaptureError("Refusing to save incomplete audio")
    path = Path(path).expanduser().absolute()
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".clawdmeter-", suffix=".wav", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as target:
            with wave.open(target, "wb") as wav:
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(16000)
                wav.writeframes(pcm)
            target.flush()
            os.fsync(target.fileno())
        # Same-directory hard link is atomic and fails if destination exists.
        os.link(temporary, path)
    finally:
        os.unlink(temporary)
    return path


def diagnostics(pcm):
    samples = array.array("h", pcm)
    if sys.byteorder != "little":
        samples.byteswap()
    peak = max(abs(v) for v in samples)
    rms = math.sqrt(sum(v * v for v in samples) / len(samples))
    return {"peak": peak, "rms": round(rms, 2),
            "clipped_samples": sum(abs(v) >= 32760 for v in samples),
            "zero_percent": round(100 * samples.count(0) / len(samples), 2)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="Explicit USB serial port; otherwise require exactly one USB modem")
    parser.add_argument("--output", type=Path, help="New WAV path (never overwritten)")
    parser.add_argument("--mic", type=int, choices=(1, 2), default=1)
    parser.add_argument("--countdown", type=int, default=3, choices=range(0, 11))
    parser.add_argument("--info", action="store_true", help="Check readiness without recording")
    args = parser.parse_args()
    if not args.info and args.output is None:
        parser.error("--output is required to record")
    if args.output is not None and os.path.lexists(args.output.expanduser()):
        parser.error("Output already exists; choose a new filename")

    import serial
    from serial.tools import list_ports
    candidates = [p.device for p in list_ports.comports()
                  if p.device.startswith(("/dev/cu.usbmodem", "/dev/ttyACM"))]
    if not args.port and len(candidates) != 1:
        parser.error(f"Specify --port; found USB candidates: {candidates}")
    port_name = args.port or candidates[0]
    recording = False
    with serial.Serial(port=None, baudrate=115200, timeout=0.2, write_timeout=1,
                       exclusive=True) as port:
        # Set BEFORE opening to avoid intentionally toggling reset lines.
        port.dtr = False
        port.rts = False
        port.port = port_name
        port.open()
        port.reset_input_buffer()
        try:
            # USB-Serial/JTAG may drop the first response as the host opens its
            # endpoint. Retry ONLY this read-only handshake, never a recording.
            info = None
            for attempt in range(3):
                port.write(b"\naudio_info\n")
                try:
                    for packet in packets(port, time.monotonic() + 2):
                        if packet.get("type") == "info":
                            info = packet
                            break
                except CaptureError as exc:
                    if not str(exc).startswith("Timed out"):
                        raise
                if info is not None:
                    break
            if info is None:
                raise CaptureError("No response: check USB, close serial monitors, and confirm audio-test firmware")
            if info.get("version") != 1 or info.get("busy") is not False:
                raise CaptureError("Device is busy or firmware is incompatible")
            print(f"Ready: {port_name}; ten-second manual audio test", flush=True)
            if args.info:
                return 0
            for n in range(args.countdown, 0, -1):
                print(f"Starting in {n}…", flush=True)
                time.sleep(1)
            receiver = Receiver(uuid.uuid4().hex[:8])
            port.write(f"audio_record {receiver.id} {args.mic}\n".encode())
            recording = True
            transferred_at = None
            last_percent = -1
            for packet in packets(port, time.monotonic() + 50):
                receiver.accept(packet)
                if packet.get("id") != receiver.id:
                    continue
                kind = packet.get("type")
                if kind == "start":
                    if packet.get("mic") != args.mic or packet.get("seconds") != 10:
                        raise CaptureError("Unexpected microphone or recording length")
                    print("RECORDING NOW — speak normally for ten seconds.", flush=True)
                elif kind == "begin":
                    print("Recording stopped. Receiving over USB…", flush=True)
                    transferred_at = time.monotonic()
                elif kind == "data":
                    percent = len(receiver.data) * 100 // BYTE_COUNT
                    if percent // 25 != last_percent // 25:
                        print(f"Transfer {percent}%", flush=True)
                    last_percent = percent
                if receiver.done and kind == "memory" and packet.get("phase") == "released":
                    break
            recording = False
            if not receiver.done:
                raise CaptureError("Recording did not finish")
            elapsed = time.monotonic() - transferred_at
            path = save_wav(args.output, receiver.data)
            print(f"Saved: {path} (10 seconds, {path.stat().st_size:,} bytes)")
            print(f"Checksum verified; USB transfer {elapsed:.2f}s; "
                  f"capture {receiver.header['capture_ms']}ms; no reported overruns.")
            print("Signal:", json.dumps(diagnostics(receiver.data)))
            print("Memory:", json.dumps(receiver.memory))
            print("Display loop:", json.dumps(receiver.health))
            print("Nothing was uploaded or transcribed. Run again for another recording.")
            return 0
        finally:
            if recording:
                try:
                    port.write(b"\naudio_cancel\n")
                except (OSError, serial.SerialException):
                    pass


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (CaptureError, OSError, ValueError) as exc:
        print(f"Audio test failed: {exc}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("Audio test cancelled; no further recording will start.", file=sys.stderr)
        sys.exit(130)
