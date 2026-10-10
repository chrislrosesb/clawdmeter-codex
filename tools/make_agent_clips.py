#!/usr/bin/env python3
"""Render Pluribus agent animations into clips the Clawdmeter plays when idle.

The animations come from the Pluribus AgentRig recorder (another project, run
read-only into a scratch build folder). This tool records each agent's moves,
crops the character to a 288 px card with rounded black corners, encodes the
frames as baseline JPEG and packs them into ``daemon/agent_clips/<name>.clip``.
The daemon streams new clips to the device; no firmware change is needed to add
or replace one.

Clip file: b"CLIP1\\n" + one JSON header line + frames, each <u32 length><jpeg>.

Usage:
    daemon/.venv/bin/python tools/make_agent_clips.py \\
        [--rig ~/pluribus-2.0/.claude/worktrees/<animation worktree>/apple/AgentRig]
"""

from __future__ import annotations

import argparse
import io
import json
import os
import struct
import subprocess
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "daemon" / "agent_clips"
DEFAULT_RIG = (Path.home() / "pluribus-2.0/.claude/worktrees/"
               "agents-personality-efficiency-f2962f/apple/AgentRig")

SIZE = 288            # card on the 480 px panel (multiple of 16 for JPEG MCUs)
FPS = 10
QUALITY = 68
RADIUS = 28
# Character (pompoms and props included) sits at x≈210..500 of the recorder's
# 720x420 frame; stop above the label panel at y≈295. Skips the 0.5 s lead-in.
CROP = (203, 0, 497, 294)
SLOT = 2.6            # recorder seconds per move

# (clip name, agent id, display name, moves)
CLIPS = [
    ("tori-cheer", "champion", "Tori", ["pompomshake", "cheer", "wink"]),
    ("mila-coffee", "sales-marketing", "Mila", ["sip", "pitch", "call"]),
]


def record(rig: Path, agent: str, moves: list[str], mp4: Path, build: Path) -> None:
    env = dict(os.environ, BOUNCE_RECORD=str(mp4), RIG_AGENT=agent, RIG_MOVES=",".join(moves))
    subprocess.run(["swift", "test", "--scratch-path", str(build),
                    "--filter", "LeanMovesRecordTests"], cwd=rig, env=env, check=True,
                   stdout=subprocess.DEVNULL)


def frames(mp4: Path, seconds: float, work: Path) -> list[Path]:
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-ss", "0.5", "-i", str(mp4),
                    "-t", f"{seconds:.2f}", "-vf", f"fps={FPS}", str(work / "%04d.png")],
                   check=True)
    return sorted(work.glob("*.png"))


def card(png: Path, mask: Image.Image) -> bytes:
    img = Image.open(png).convert("RGB").crop(CROP).resize((SIZE, SIZE), Image.LANCZOS)
    out = Image.new("RGB", (SIZE, SIZE), (0, 0, 0))
    out.paste(img, (0, 0), mask)
    buf = io.BytesIO()
    out.save(buf, "JPEG", quality=QUALITY, optimize=False, progressive=False, subsampling=2)
    return buf.getvalue()


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rig", type=Path, default=DEFAULT_RIG)
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    mask = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, SIZE - 1, SIZE - 1), RADIUS, fill=255)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        build = tmp / "agentrig-build"
        for name, agent, display, moves in CLIPS:
            mp4 = tmp / f"{name}.mp4"
            record(args.rig, agent, moves, mp4, build)
            work = tmp / name
            work.mkdir()
            jpegs = [card(p, mask) for p in frames(mp4, SLOT * len(moves), work)]
            header = {"name": name, "agent": agent, "display": display, "fps": FPS,
                      "w": SIZE, "h": SIZE, "frames": len(jpegs)}
            body = b"".join(struct.pack("<I", len(j)) + j for j in jpegs)
            path = OUT / f"{name}.clip"
            path.write_bytes(b"CLIP1\n" + json.dumps(header).encode() + b"\n" + body)
            print(f"{path.relative_to(ROOT)}: {len(jpegs)} frames, {len(body) // 1024} KB")


if __name__ == "__main__":
    main()
