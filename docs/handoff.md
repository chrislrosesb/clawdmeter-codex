# Handoff

Short running notes for the next session. The canonical deployment runbook is
`CLAUDE.md`; the shared agent-bridge handoff and decision log hold the Phase 1/2
history.

## 2026-10-10 — Agent-message screen mockup (design only, no firmware change)

**What:** `docs/mockups/agent-messages.html` (open in a browser; assets in
`docs/mockups/agent-messages/`, static preview `agent-messages-preview.png`).
Also published as a private Design canvas:
https://claude.ai/artifact/Ku18KBWZP4gFTcUh2UFbmg

It shows the Waveshare 2.16 AMOLED at its real 480×480 size, 1:1, in four states:

1. **Idle** — the normal Clawd splash, untouched; no badge.
2. **One message** — Tori's rig face (AgentRig champion heads: neutral → wink →
   talk) loops a 3 s "hey" with a head tilt and speech bubble; agent name + age on
   a Mono 18 line, caption on one Styrene-28 line (~24 chars, then ellipsis).
3. **Several messages** — big face takes 3 s turns (hard cuts, like an LVGL screen
   swap) with its caption; row of small faces underneath rings the current one,
   plus a "3 waiting" count.
4. **Talking (v2 placeholder)** — face with mouth flaps, pulsing rings and a level
   meter; "Tap to stop".

All names/captions are sample data. Mila is the current UI name for the
sales-marketing agent (Scarlett is the old name). Type sizes match fonts already
in firmware (Mono 18, Styrene 28, Tiempos 34); web fonts stand in. Content keeps
32 px from the edges for the rounded corners.

**Chris's decisions (2026-10-10, after reviewing):**
- A waiting message interrupts the 30 s rotation; tapping the screen dismisses it
  and rotation resumes.
- Dismiss clears on the device only — no read-sync back to Pluribus/iPhone/iPad.
- Faces are pre-baked into firmware (fastest), not sent over BLE.
- **No audio this round:** drop state 4 (talking), no voice toggle, no phone-mute
  routing. When voices return, the left button (currently HID Space, unused by
  Chris) is the likely on/off switch; voice would likely play from the work Mac.
- The agent-message screen replaces the current Pluribus activity screen; Claude,
  Codex, Now Playing and Clawd screens stay as they are.
- Mockup stays as is (state 4 kept as a labelled placeholder).

**Superseded below:** the interrupt/dismiss/face-art questions are answered above.

**Not decided / next:** whether "message waiting" interrupts the 30 s rotation or
joins it; how a message is dismissed (tap? button?); how face art reaches the
device (pre-baked in flash vs. sent over BLE like album art — PSRAM is available
on this board). Nothing in the payload schema or firmware has changed.

**Verification note:** headless Chrome's `--virtual-time-budget` mis-renders the
state-3 turn-taking (shows two captions overlapping). In a real browser it cycles
cleanly; that was checked live.
