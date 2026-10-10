"""Stream the Pluribus agent animation clips to the Clawdmeter.

Clips are made by ``tools/make_agent_clips.py`` into ``daemon/agent_clips/``.
Each connection sends every clip once, a slice per daemon loop so usage, music
and messages keep flowing. Over USB the device answers "#have" for a clip it
already holds, so a reconnect costs nothing; over Bluetooth (about 1 MB per clip
at roughly 10 KB/s) each new connection sends them again in the background.
"""

from __future__ import annotations

import json
import struct
import time
import zlib
from dataclasses import dataclass, field
from pathlib import Path

CLIP_DIR = Path(__file__).resolve().parent / "agent_clips"
CLIP_MAGIC = 0xA6
CLIP_START = 0x01
CLIP_CHUNK = 0x02


def log(message: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)


@dataclass
class Clip:
    name: str
    agent: str
    display: str
    fps: int
    w: int
    h: int
    frames: int
    body: bytes
    crc: int = field(init=False)

    def __post_init__(self) -> None:
        self.crc = zlib.crc32(self.body) & 0xFFFFFFFF

    def start_packet(self) -> bytes:
        return struct.pack("<BBIIHBHH16s12s", CLIP_MAGIC, CLIP_START, self.crc, len(self.body),
                           self.frames, self.fps, self.w, self.h,
                           self.agent.encode()[:16], self.display.encode()[:12])

    def chunk_packet(self, offset: int, size: int) -> bytes:
        return struct.pack("<BBII", CLIP_MAGIC, CLIP_CHUNK, self.crc, offset) + \
            self.body[offset:offset + size]


def load_clips(directory: Path = CLIP_DIR) -> list[Clip]:
    clips = []
    for path in sorted(directory.glob("*.clip")):
        try:
            magic, header, body = path.read_bytes().split(b"\n", 2)
            meta = json.loads(header)
            if magic != b"CLIP1":
                continue
            clips.append(Clip(meta["name"], meta["agent"], meta["display"], int(meta["fps"]),
                              int(meta["w"]), int(meta["h"]), int(meta["frames"]), body))
        except (OSError, ValueError, KeyError) as e:
            log(f"Skipping clip {path.name}: {e}")
    return clips


class ClipSender:
    """Sends each clip once per connection, ``budget`` bytes per step."""

    def __init__(self, clips: list[Clip], chunk: int) -> None:
        self.pending = list(clips)
        self.chunk = chunk
        self.offset = -1          # -1: start packet not yet sent
        self.started = 0.0

    @property
    def done(self) -> bool:
        return not self.pending

    async def step(self, session, budget: int) -> None:
        sent = 0
        while self.pending and sent < budget:
            clip = self.pending[0]
            if self.offset < 0:
                status = await session.write_packet(clip.start_packet())
                if status == 2:
                    self.pending.pop(0)          # already on the device
                    continue
                if status != 1:
                    return                        # try again next step
                self.offset = 0
                self.started = time.monotonic()
            status = await session.write_packet(clip.chunk_packet(self.offset, self.chunk))
            if status != 1:
                self.offset = -1                  # device dropped it: restart this clip later
                return
            self.offset += self.chunk
            sent += self.chunk
            if self.offset >= len(clip.body):
                log(f"Agent clip {clip.name} sent ({len(clip.body) // 1024} KB, "
                    f"{time.monotonic() - self.started:.1f}s)")
                self.pending.pop(0)
                self.offset = -1
