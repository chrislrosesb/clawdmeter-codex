import asyncio
import os
import pty
import threading

import pytest

usb_link = pytest.importorskip("daemon.usb_link")


class FakeDevice(threading.Thread):
    """Plays the firmware's side of the USB line protocol on a pty."""

    def __init__(self, fd):
        super().__init__(daemon=True)
        self.fd = fd
        self.lines = []

    def run(self):
        buf = b""
        while True:
            try:
                chunk = os.read(self.fd, 4096)
            except OSError:
                return
            if not chunk:
                return
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self.lines.append(line)
                os.write(self.fd, b"splash entry: -> trumpet\n")   # log noise
                if line == b"clawdmeter?":
                    os.write(self.fd, b"CLAWDMETER usb1\n")
                elif line[:1] in (b"{", b"@"):
                    os.write(self.fd, b"#ok\n")


def make_session():
    device_fd, host_fd = pty.openpty()
    device = FakeDevice(device_fd)
    device.start()
    port = usb_link.UsbPort(os.ttyname(host_fd))
    os.close(host_fd)
    return usb_link.UsbSession(port), device


def test_handshake_payload_and_artwork_over_the_line_protocol():
    session, device = make_session()
    assert usb_link.handshake(session.port)
    assert asyncio.run(session.write_payload({"am": {"n": 0, "m": []}}))
    frame = b"\xff\xd8" + bytes(2500) + b"\xff\xd9"
    assert asyncio.run(session.write_artwork(frame, generation=7))
    assert device.lines[1] == b'{"am":{"n":0,"m":[]}}'
    art = [l for l in device.lines if l.startswith(b"@")]
    assert len(art) == 1 + 3                       # start + three 1 KB chunks
    assert bytes.fromhex(art[1][1:].decode())[:2] == b"\xa5\x02"
    asyncio.run(session.disconnect())


def test_silent_device_marks_link_lost():
    device_fd, host_fd = pty.openpty()             # nobody answers
    session = usb_link.UsbSession(usb_link.UsbPort(os.ttyname(host_fd)))
    usb_link.ACK_TIMEOUT, saved = 0.2, usb_link.ACK_TIMEOUT
    try:
        assert not asyncio.run(session.write_payload({"ok": False}))
        assert not session.is_connected
    finally:
        usb_link.ACK_TIMEOUT = saved
        os.close(device_fd)
        os.close(host_fd)
