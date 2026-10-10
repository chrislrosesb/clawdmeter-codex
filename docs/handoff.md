# Handoff

Short running notes for the next session. The canonical deployment runbook is
`CLAUDE.md`; the shared agent-bridge handoff and decision log hold the Phase 1/2
history.

## 2026-10-10 — Agent messages behave like phone notifications

**What changed (Chris's choices):** a new message pops the agent screen for 15 s,
then the normal rotation resumes; waiting messages get their own 15 s turn and a
small face + count in the corner of the usage/Codex/music screens. Messages
leave when read in Pluribus on any device, after 5 minutes, or on a tap; a tap
also marks them read in Pluribus so the iPhone notification clears too.

**How:** host sends only unread messages (session `unread_count` + receipts;
check-ins included, Team feed retired) and `rd` frames for ones no longer
unread. Device reports taps (`#tap` over USB, TX value over BLE). Relay gained
`POST /v1/read`. Pluribus Mac app fix (window must be in front to count as
read, Pluribus 8c6de258) shipped in **TestFlight build 214** (iOS + macOS
uploads both succeeded, 2026-10-10).

**Verified on hardware (demo messages):** pop, 15 s hand-back to Clawd, corner
hint on usage, rotation turn, tap report line. Not yet: live read-clearing with
real messages, BLE tap reading on the work Mac (needs `git pull` + restart there).

**Pluribus server fix (8e732bf7, live):** `push.py` `send_read_sync` used to
return early when the badge was unchanged, skipping the silent "clear" push, so
an iPhone notification could stick. It now tracks alerted chats
(`_notified_sessions`) and always clears them; the Pluribus server was
restarted (`make restart`). Note: Pluribus origin/main (422cd93b) is ahead of
the local Pluribus main checkout, which another session had dirty; it was left
alone. Next Pluribus TestFlight build number must be 215 or higher.

## 2026-10-10 — Agent animations in the idle rotation

**What it does:** every idle (Clawd) turn now opens with one of Chris's agents
animating on a cream card with her name (Tori shaking pompoms, Mila sipping
coffee, holding up a pitch, taking a call), then Clawd finishes the turn. Clips
rotate. They need the Mac to send them after each device reboot (about a
minute over USB; several minutes in the background over Bluetooth).

**Adding or refreshing clips:** edit `CLIPS` in `tools/make_agent_clips.py`
(agent id + move names from AgentRig's `FaceMove`), run it with the daemon venv,
commit `daemon/agent_clips/`, restart the daemon. No firmware change. The
recorder lives on the other session's branch `claude/pluribus-animation-redesign-0ffca0`
(worktree `agents-personality-efficiency-f2962f`); once it merges, point `--rig`
at `~/pluribus-2.0/apple/AgentRig`. Only Tori and Mila have rigs today; others
need art from that project first.

**Verified on hardware:** both clips transferred over USB (24-34 s each), Mila
played with correct colors (first build had red/blue swapped: LVGL's tjpgd
outputs B,G,R). Not yet verified: clip transfer over Bluetooth, and playback
smoothness by eye (frames advanced as expected in screenshots).

## 2026-10-10 — Chat and Room replies on the device

**What it does:** every finished agent reply in any shared chat or Room pops up
on the Clawdmeter, even if Pluribus is open on the Mac behind other windows or
Chris has switched apps on the iPhone. Replies queue oldest first, 5 s each, with
the Room name in the header and two lines of the reply. Tap clears everything on
the device; replies drop off after 3 hours. Agent check-ins still show as before.

**Why it ignores read state:** the Mac app marks a room read whenever its window
is open, even fully covered (it checks `scenePhase`, not occlusion). Step 2, only
if the echo while watching a room bothers Chris: make the Mac app count a room
as read only when its window is truly in front (TestFlight), then let app-read
clear the device.

**Pieces:** `daemon/chat_replies.py` (reads `/api/ask/sessions` + each recently
updated session's `_chat_reply_receipts`; no Pluribus change), `fetch_display_rooms`
and the 5 s `rm` loop in `run_session`, relay `rooms` slot
(`relay_server.update_rooms`, `relay_source` parses it), firmware `rm` parsing in
`main.cpp` and `ui_add_room_reply` / the reworked `agents_refresh_visible` in
`ui.cpp` (12-reply ring, oldest-first sort, distinct-agent face row, 176 px face
in multi mode). Tests: `daemon/tests/test_chat_replies.py`.

**Verified:** on the real device over USB with today's real replies (Gilfoyle,
Mila, Tori + Darwin check-in, "5 WAITING"); Room header checked in the sim.
**Not yet:** a live Room conversation end to end, and the work Mac (needs pull).

## 2026-10-10 — USB cable link (Bluetooth kept as fallback)

**What it does:** plug the Clawdmeter into a Mac running the daemon and that Mac
drives it over the cable: no pairing, about 20x faster (a 5 KB cover went over in
0.07 s). Unplugged, everything works over Bluetooth as before. While the cable is
live, the device ignores Bluetooth data, so the cabled Mac is the one owner.

**Pieces:** firmware line protocol in `main.cpp` (`check_serial_cmd`:
`{json}`, `@<hex>` packets, `clawdmeter?` handshake) feeding `ble_usb_packet()`
in `ble.cpp`; the UI treats live USB as "connected". Daemon:
`daemon/usb_link.py` (stdlib termios, exclusive port) and `run_session()` in
`claude_usage_daemon.py`, shared by USB and BLE; USB is tried first each cycle
and a BLE session hands over when a cable appears (30 s check). Tests:
`daemon/tests/test_usb_link.py` (pty-based fake device).

**Verified on hardware (Mac mini, USB):** handshake, usage, the real waiting
Darwin message (screenshot), and test album art + Now Playing.

**Gotchas:** the daemon now holds the serial port when the device is cabled:
stop the LaunchAgent before flashing, `screenshot.sh` or the audio test (they
fail fast with "busy"). The Mac mini restarted mid-session and the device's USB
stopped answering until it was physically replugged; a flash attempt then
succeeded. `launchctl unload` without `-w` does not survive a reboot: the
Mac-mini daemon LaunchAgent came back at login. With the USB code that is now
intended: it drives the device whenever it's cabled to the Mac mini, and stays
idle otherwise. Buttons still go over Bluetooth HID only. Using the cable on
the work Mac needs `git pull` there and a daemon restart.

## 2026-10-10 — Agent-message screen built (firmware + host)

**What it does:** when a Pluribus agent sends Chris a proactive message he has
not dismissed in the app, the Clawdmeter interrupts its rotation and shows that
agent's face (Tori loops look → wink → "hey" with a bubble and a small bob), the
name, age and a one-line caption. Several agents take 3 s turns above a row of
small faces and a "N WAITING" count. A tap clears everything waiting, on the
device only, and the normal rotation resumes. It replaced the old Pluribus
activity screen. No audio.

**Pieces:**
- Host: `daemon/agent_messages.py` reads `GET /api/agents/feed?days=1` (Pluribus
  Team feed = `agent_outreach.send_agent_message` entries, not dismissed), keeps
  the newest per agent (max 3), trims a caption to 40 bytes, and emits
  `{"am":{"n":total,"m":[{"i","g","w","t","a"}]}}`. Used by `relay_server.py`
  (Mac mini) and the local daemon path. The relay's `pluribus` slot is
  pass-through, so the **work Mac needs no update** — it forwards `am` as is.
- Firmware: `SCREEN_AGENTS` in `ui.cpp` (`init_agents_screen`,
  `agents_refresh_visible`, `agents_render`, `update_agents`, `agents_click_cb`).
  Waiting messages pin the screen in `ui_tick_screen_rotation`; a new id wakes
  the panel once (`idle_note_activity`). Dismissed/seen ids are RAM rings of 24.
  The `pb` frame is still acked and ignored so an old host can't hit the usage
  parser. Serial `agentdemo N` (0..3) injects sample messages.
- Faces: `tools/gen_agent_faces.py` → `firmware/src/agent_faces.h` (224 px +
  52 px RGB565, ~0.9 MB flash; Tori from AgentRig heads, others from the
  AgentAnimations medallions). Compiled only with `BOARD_HAS_AGENT_FACES`
  (2.16 envs). Unknown agents (e.g. therapist/Naomi) get a tinted initial.
  2.16 flash is now 81% (`assets/pluribus_logo.h` is no longer included; the file
  is unused and could be deleted).

**Verified:** daemon tests (166 pass), sim fallback states, and the audio-test
build flashed to the real device over USB (normal upload, bond kept); one-agent
hey loop and three-agent turns screenshot-checked on hardware. **Not yet seen
live:** a real agent message over BLE via the work Mac (the feed had nothing
waiting during the build, and the device is bonded to the work Mac).

**Deployment state:** device flashed with `waveshare_amoled_216_audio_test`
(mic test kept). The Mac-mini BLE writer LaunchAgent was found loaded (idle,
"device not held by OS") and was unloaded before flashing, per the runbook.


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

**Verification note:** headless Chrome's `--virtual-time-budget` mis-renders the
state-3 turn-taking (shows two captions overlapping). In a real browser it cycles
cleanly; that was checked live.

## 2026-10-10 — Alternative "agent inbox" mockup (another session, folded in)

A parallel session produced an earlier design-only mockup, left uncommitted in
the main checkout under `mockups/agent-inbox/`. Its handoff note follows, kept
verbatim apart from the heading. The built screen above supersedes its open
questions (Chris chose tap-to-dismiss, device-only, faces baked in flash).

**What:** A mockup of a new "agent inbox" screen for the 2.16-inch AMOLED:
when a Pluribus agent has something for Chris, that agent's face appears with
a one-line caption (e.g. "Tori: Silver Air pricing note"). With several agents
waiting, they take turns in the hero slot and a small row of faces shows the
queue. Three states at the device's exact 480×480 resolution: idle, one
waiting, three waiting.

**Where:** `mockups/agent-inbox/` — `agent-inbox.html` (all states, animated
face), `idle.png`, `one-waiting.png`, `three-waiting.png`, and `README.md`
with the full layout spec, animation timings, and what firmware would need.

**Why it looks the way it does:** It borrows the existing Pluribus activity
screen's chrome (mono eyebrow, three accent rules, cropped dotted mark, mono
footer) so it reads as the same family. The hero face sits in a fixed slot so
nothing jumps as the queue changes. The real Tori head art from
`~/pluribus-2.0/apple/AgentRig` is used; Sloane and the coach come from the
`AgentAnimations` pose sheets. The art is referenced in place, not copied.

**Not done, by request:** no firmware change, no build, no flash, no commit.
The device, its bond, and the work-Mac relay were not touched.

**Open questions for Chris:**

- Which agents should be able to post to the inbox, and what does "Scarlett"
  look like? Only Tori has a face rig today; others would show a still or a
  monogram disc until they get art.
- Should the idle state take a rotation slot at all? The mockup assumes no.
- Is a long press on the face a useful "acknowledge" gesture, or is seeing it
  enough?

**If this moves to firmware:** it is a host-side payload plus one new screen
in `ui.cpp`; the on-device face frames (neutral/closed/talk at Ø200 and Ø48)
need a transfer or baking plan because they are large. See the README's
"What the firmware would need" section. Keep the existing `pb` contract and
the BLE invariants in `CLAUDE.md` unchanged.

**Tooling note:** PNGs are rendered with a local Puppeteer install
(`npm install && node render.js` inside `mockups/agent-inbox/`);
`node_modules/` is git-ignored there. The sandbox in this session could not
run Swift, Python, or `sips`, which is why the renderer is Node-based.
