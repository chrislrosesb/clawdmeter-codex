"""Apple Music now-playing source and ESP32 artwork preparation.

The source deliberately lives on the Mac: Music.app supplies metadata through
AppleScript and Pillow turn artwork into a compact baseline JPEG the display
can decode.  A failed query is represented as ``None`` and never affects the
Claude usage polling path.
"""

from __future__ import annotations

import hashlib
import json
import subprocess
import tempfile
import unicodedata
import urllib.parse
import urllib.request
from dataclasses import dataclass
from io import BytesIO
from pathlib import Path

from PIL import Image, ImageDraw, ImageOps


ARTWORK_SIZE = 300
ARTWORK_JPEG_QUALITY = 72
QUERY_TIMEOUT = 5
ARTWORK_TIMEOUT = 8
FIELD_SEP = "\x1e"


class NowPlayingError(RuntimeError):
    pass


@dataclass(frozen=True)
class Track:
    title: str
    artist: str
    album: str
    duration: int
    elapsed: int
    track_id: str

    def wire_payload(self) -> dict:
        return {
            "np": {
                "p": 1,
                "t": display_text(self.title, 63),
                "a": display_text(self.artist, 63),
                "l": display_text(self.album, 63),
                "d": max(0, self.duration),
                "e": max(0, min(self.elapsed, self.duration or self.elapsed)),
                "id": self.track_id[:40],
                "g": int(self.track_id[:4], 16),
            }
        }


_QUERY_SCRIPT = r'''
set sep to ASCII character 30
if application "Music" is not running then return ""
tell application "Music"
    if player state is not playing then return ""
    set tr to current track
    set trackName to name of tr as text
    set artistName to artist of tr as text
    set albumName to album of tr as text
    set durationSeconds to duration of tr as integer
    set elapsedSeconds to player position as integer
    try
        set trackKey to persistent ID of tr as text
    on error
        set trackKey to trackName & "|" & artistName & "|" & albumName
    end try
    return trackName & sep & artistName & sep & albumName & sep & durationSeconds & sep & elapsedSeconds & sep & trackKey
end tell
'''


_ARTWORK_SCRIPT = r'''
on run argv
    set outputPath to item 1 of argv
    if application "Music" is not running then return "none"
    tell application "Music"
        if player state is not playing then return "none"
        try
            set artBytes to raw data of artwork 1 of current track
        on error
            return "none"
        end try
    end tell
    try
        set outFile to open for access POSIX file outputPath with write permission
        set eof outFile to 0
        write artBytes to outFile
        close access outFile
        return "ok"
    on error
        try
            close access POSIX file outputPath
        end try
        return "none"
    end try
end run
'''


def display_text(value: str, limit: int) -> str:
    """Return readable ASCII for the firmware's compact Latin-only fonts."""
    replacements = {
        "\u2018": "'", "\u2019": "'", "\u201c": '"', "\u201d": '"',
        "\u2013": "-", "\u2014": "-", "\u2026": "...", "\u00a0": " ",
    }
    value = "".join(replacements.get(ch, ch) for ch in value)
    value = unicodedata.normalize("NFKD", value)
    value = "".join(ch for ch in value if not unicodedata.combining(ch))
    value = value.encode("ascii", "replace").decode()
    value = " ".join(value.split())
    if len(value) <= limit:
        return value
    return value[: max(1, limit - 3)].rstrip() + "..."


def query() -> Track | None:
    """Return the currently playing Apple Music track, without launching Music."""
    try:
        result = subprocess.run(
            ["osascript", "-e", _QUERY_SCRIPT], capture_output=True, text=True,
            timeout=QUERY_TIMEOUT, check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise NowPlayingError(
            "Music.app query timed out; grant Automation permission to the daemon's Python"
        ) from exc
    except OSError as exc:
        raise NowPlayingError(f"could not run osascript: {exc}") from exc
    raw = result.stdout.rstrip("\r\n")
    if result.returncode != 0:
        detail = result.stderr.strip().replace("\n", " ")[:180]
        raise NowPlayingError(detail or f"osascript exited {result.returncode}")
    if not raw:
        return None
    fields = raw.split(FIELD_SEP)
    if len(fields) != 6:
        return None
    try:
        duration, elapsed = int(float(fields[3])), int(float(fields[4]))
    except ValueError:
        return None
    identity = fields[5].strip() or "|".join(fields[:3])
    track_id = hashlib.sha1(identity.encode("utf-8", "replace")).hexdigest()[:16]
    return Track(fields[0], fields[1], fields[2], duration, elapsed, track_id)


def _apple_music_artwork() -> bytes | None:
    with tempfile.TemporaryDirectory(prefix="clawdmeter-art-") as tmp:
        path = Path(tmp) / "cover"
        try:
            result = subprocess.run(
                ["osascript", "-e", _ARTWORK_SCRIPT, str(path)],
                capture_output=True, text=True, timeout=ARTWORK_TIMEOUT, check=False,
            )
            if result.returncode == 0 and result.stdout.strip() == "ok" and path.exists():
                data = path.read_bytes()
                return data if data else None
        except (OSError, subprocess.SubprocessError):
            pass
    return None


def _store_artwork(track: Track) -> bytes | None:
    """Best-effort iTunes Search fallback for streamed-track artwork."""
    term = urllib.parse.quote_plus(f"{track.title} {track.artist}")
    url = f"https://itunes.apple.com/search?term={term}&entity=song&limit=8"
    try:
        with urllib.request.urlopen(url, timeout=ARTWORK_TIMEOUT) as response:
            body = json.loads(response.read(512_000))
        results = body.get("results", [])
        if not results:
            return None
        title = track.title.casefold()
        artist = track.artist.casefold()
        best = max(results, key=lambda item: (
            item.get("trackName", "").casefold() == title,
            item.get("artistName", "").casefold() == artist,
        ))
        art_url = best.get("artworkUrl100")
        if not art_url:
            return None
        art_url = art_url.replace("100x100bb", "600x600bb")
        with urllib.request.urlopen(art_url, timeout=ARTWORK_TIMEOUT) as response:
            return response.read(4_000_000)
    except (OSError, ValueError, json.JSONDecodeError):
        return None


def _rgb565_frame(source: bytes, size: int = ARTWORK_SIZE) -> bytes:
    rounded = _rounded_cover(source, size)

    out = bytearray(size * size * 2)
    pos = 0
    pixels = (rounded.get_flattened_data() if hasattr(rounded, "get_flattened_data")
              else rounded.getdata())
    for r, g, b in pixels:
        value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[pos] = value & 0xFF
        out[pos + 1] = value >> 8
        pos += 2
    return bytes(out)


def _rounded_cover(source: bytes, size: int) -> Image.Image:
    with Image.open(BytesIO(source)) as opened:
        image = ImageOps.fit(opened.convert("RGB"), (size, size), Image.Resampling.LANCZOS)

    # Pre-render the rounded corners. The display receives opaque RGB565, so
    # black corner pixels blend into its true-black OLED background.
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size - 1, size - 1), radius=20, fill=255)
    rounded = Image.new("RGB", (size, size), "black")
    rounded.paste(image, mask=mask)
    return rounded


def _jpeg_frame(source: bytes, size: int = ARTWORK_SIZE,
                quality: int = ARTWORK_JPEG_QUALITY) -> bytes:
    """Return a baseline JPEG suitable for LVGL's tiny JPEG decoder."""
    rounded = _rounded_cover(source, size)
    output = BytesIO()
    rounded.save(
        output, "JPEG", quality=quality, optimize=True, progressive=False,
        subsampling=2,
    )
    return output.getvalue()


def artwork(track: Track, size: int = ARTWORK_SIZE) -> bytes | None:
    """Extract, fall back, crop, and encode a track cover for the ESP32."""
    source = _apple_music_artwork() or _store_artwork(track)
    if not source:
        return None
    try:
        return _jpeg_frame(source, size)
    except (OSError, ValueError):
        return None
