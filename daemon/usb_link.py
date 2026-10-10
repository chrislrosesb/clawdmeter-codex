"""USB cable link to the Clawdmeter (macOS/Linux, standard library only).

When the device is plugged into this Mac, the daemon prefers this link over
Bluetooth: it is roughly 20x faster for artwork and needs no pairing. The
firmware reads newline-terminated lines on its USB serial port:

    {json}          one display payload (same JSON as the BLE RX write)
    @<hex>          one binary packet (artwork start/chunk frames)
    clawdmeter?     handshake / keepalive -> "CLAWDMETER usb1"

Data lines are answered "#ok" or "#err". Firmware log lines share the port and
are skipped. While USB is active the device ignores Bluetooth data writes, so
the cabled Mac is the single owner. Flashing, screenshots and the audio test
need the same port: stop the daemon first, as the runbook already says.
"""

from __future__ import annotations

import asyncio
import fcntl
import glob
import json
import os
import select
import struct
import sys
import termios
import time
import zlib

PORT_GLOBS = ("/dev/cu.usbmodem*",) if sys.platform == "darwin" else ("/dev/ttyACM*",)
HANDSHAKE = b"clawdmeter?\n"
BANNER = b"CLAWDMETER usb1"
ACK_TIMEOUT = 2.0
CHUNK_DATA = 1024           # artwork bytes per @ line (firmware line buffer fits 1 KB)
KEEPALIVE_INTERVAL = 30.0   # firmware treats USB as live for 150 s after traffic

ART_MAGIC = 0xA5
ART_START = 0x01
ART_CHUNK = 0x02


def log(message: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)


class UsbPort:
    """A raw, non-blocking serial port with line reads."""

    def __init__(self, path: str) -> None:
        self.path = path
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            # Exclusive: a flash or screenshot tool fails fast instead of
            # sharing the port and losing lines to this reader.
            fcntl.ioctl(self.fd, termios.TIOCEXCL)
        except OSError:
            os.close(self.fd)
            raise
        attrs = termios.tcgetattr(self.fd)
        attrs[0] = 0                                  # iflag: raw
        attrs[1] = 0                                  # oflag: raw
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[3] = 0                                  # lflag: no echo/canonical
        attrs[4] = attrs[5] = termios.B115200         # baud is ignored by USB CDC
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        termios.tcflush(self.fd, termios.TCIOFLUSH)
        self._buf = b""
        self.taps: list[str] = []   # "#tap" lines seen while waiting for replies

    def close(self) -> None:
        if self.fd >= 0:
            try:
                os.close(self.fd)
            finally:
                self.fd = -1

    def write(self, data: bytes) -> None:
        view = memoryview(data)
        deadline = time.monotonic() + ACK_TIMEOUT
        while view:
            try:
                n = os.write(self.fd, view)
                view = view[n:]
            except BlockingIOError:
                if time.monotonic() > deadline:
                    raise OSError("USB write timed out")
                select.select([], [self.fd], [], 0.05)

    def read_line(self, timeout: float) -> bytes | None:
        deadline = time.monotonic() + timeout
        while b"\n" not in self._buf:
            left = max(0.0, deadline - time.monotonic())
            ready, _, _ = select.select([self.fd], [], [], left)
            if not ready and left <= 0:
                return None
            if ready:
                chunk = os.read(self.fd, 4096)
                if not chunk:
                    raise OSError("USB port closed")
                self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        return line.rstrip(b"\r")

    def wait_for(self, prefixes: tuple[bytes, ...], timeout: float) -> bytes | None:
        """Next line starting with one of ``prefixes``; firmware logs are skipped."""
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            line = self.read_line(left)
            if line is None:
                return None
            if line.startswith(b"#tap "):
                self.taps.extend(i for i in line[5:].decode(errors="ignore").split(",") if i)
                continue
            if line.startswith(prefixes):
                return line


def handshake(port: UsbPort) -> bool:
    port.write(HANDSHAKE)
    return port.wait_for((BANNER,), ACK_TIMEOUT) is not None


def find_device() -> UsbPort | None:
    """Open the first USB serial port that answers the Clawdmeter handshake."""
    for pattern in PORT_GLOBS:
        for path in sorted(glob.glob(pattern)):
            try:
                port = UsbPort(path)
            except OSError:
                continue   # busy (flashing, screenshot tool) or vanished
            try:
                if handshake(port):
                    return port
            except OSError:
                pass
            port.close()
    return None


def device_present() -> bool:
    """Cheap check before a handshake: is any candidate port plugged in?"""
    return any(glob.glob(p) for p in PORT_GLOBS)


class UsbSession:
    """Same write API as the BLE ``Session``, over the USB cable."""

    def __init__(self, port: UsbPort) -> None:
        self.port = port
        self.refresh_requested = asyncio.Event()   # never set; API parity
        self.write_lock = asyncio.Lock()
        self.connected = True
        self._last_keepalive = time.monotonic()

    @property
    def is_connected(self) -> bool:
        return self.connected

    def _send_line(self, line: bytes) -> int:
        """1 = "#ok", 2 = "#have" (clip already on the device), 0 = "#err"."""
        for attempt in range(2):
            self.port.write(line + b"\n")
            reply = self.port.wait_for((b"#ok", b"#err", b"#have"), ACK_TIMEOUT)
            if reply is None:
                raise OSError("no reply from device")
            if reply == b"#ok":
                return 1
            if reply == b"#have":
                return 2
            time.sleep(0.05)   # "#err" for JSON usually means the last one is still queued
        return 0

    async def _run(self, fn, *args):
        try:
            async with self.write_lock:
                return await asyncio.to_thread(fn, *args)
        except OSError as e:
            log(f"USB link lost: {e}")
            self.connected = False
            return False

    async def keepalive(self) -> None:
        if time.monotonic() - self._last_keepalive < KEEPALIVE_INTERVAL:
            return
        self._last_keepalive = time.monotonic()

        def ping() -> bool:
            if not handshake(self.port):
                raise OSError("handshake not answered")
            return True

        await self._run(ping)

    async def write_payload(self, payload: dict) -> bool:
        data = json.dumps(payload, separators=(",", ":")).encode()
        if len(data) >= 512:
            log(f"Refusing oversized JSON payload ({len(data)} bytes)")
            return False
        log(f"Sending (USB): {data.decode()}")
        ok = await self._run(self._send_line, data)
        if ok:
            self._last_keepalive = time.monotonic()
        return bool(ok)

    async def write_packet(self, packet: bytes) -> int:
        """One binary packet (agent clip start/chunk); 0/1/2 as ``_send_line``."""
        return int(await self._run(self._send_line, b"@" + packet.hex().encode()) or 0)

    async def write_artwork(self, frame: bytes, generation: int,
                            width: int = 300, height: int = 300,
                            encoding: int = 1) -> bool:
        generation &= 0xFFFF
        start = struct.pack("<BBHHHBII", ART_MAGIC, ART_START, generation, width,
                            height, encoding, len(frame), zlib.crc32(frame) & 0xFFFFFFFF)
        started = time.monotonic()

        def send() -> bool:
            if not self._send_line(b"@" + start.hex().encode()):
                return False
            for offset in range(0, len(frame), CHUNK_DATA):
                packet = struct.pack("<BBHI", ART_MAGIC, ART_CHUNK, generation, offset)
                packet += frame[offset:offset + CHUNK_DATA]
                if not self._send_line(b"@" + packet.hex().encode()):
                    return False
            return True

        ok = await self._run(send)
        if ok:
            log(f"Artwork sent over USB ({len(frame)} bytes, "
                f"{time.monotonic() - started:.2f}s)")
        return bool(ok)

    async def take_taps(self) -> list[str]:
        """Messages tapped away on the device ("#tap" lines) since last call."""
        def drain() -> list[str]:
            # Pick up a tap that arrived while nothing was being sent.
            while True:
                line = self.port.read_line(0)
                if line is None:
                    break
                if line.startswith(b"#tap "):
                    self.port.taps.extend(
                        i for i in line[5:].decode(errors="ignore").split(",") if i)
            taps, self.port.taps = self.port.taps, []
            return taps

        return await self._run(drain) or []

    async def disconnect(self) -> None:
        self.connected = False
        self.port.close()
