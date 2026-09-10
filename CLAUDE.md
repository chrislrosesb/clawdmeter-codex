# Project context

ESP32-S3 / ESP32-C6 firmware for a desk-side Claude Code usage monitor. Each
supported board lives in its own `firmware/src/boards/<name>/` folder and is
selected via PlatformIO's `build_src_filter`. Adding a board means dropping in
a new folder + a new `[env:...]` block — `main.cpp`, `ui.cpp`, and `splash.cpp`
never see board-specific code. See [`docs/porting/adding-a-board.md`](docs/porting/adding-a-board.md).

## Chris's current deployment — read before changing or troubleshooting

This section is the canonical runbook for the personalized installation. It
overrides older or generic host guidance elsewhere in this file when the two
conflict. Known-good baseline: commit `e047ee4` on 2026-09-08.

### What is deployed

- Hardware: Waveshare ESP32-S3-Touch-AMOLED-2.16, build environment
  `waveshare_amoled_216`.
- Source and working copy: `/Users/macmini/Clawdmeter-main`; Chris's fork is
  `https://github.com/chrislrosesb/clawdmeter-codex` on `main`.
- Current BLE owner/host: the work Mac. The device intentionally accepts data
  from one bonded owner machine, not several computers simultaneously. The work
  Mac supplies Apple Music locally and writes all screens over BLE.
- Work-Mac host process: `daemon/claude_usage_daemon.py` in `daemon/.venv`,
  managed by `~/Library/LaunchAgents/com.user.claude-usage-daemon.plist`.
- Phase 2 collector: `daemon/relay_server.py`, managed independently by
  `~/Library/LaunchAgents/com.user.clawdmeter-relay.plist`. It is installed and
  live on the Mac mini's Tailscale IPv4 at port 8765. Its private config/token is
  `~/.config/claude-usage-monitor/relay-server.json` (mode 600); never copy that
  token into this repository or logs. The work-Mac receiver and bond transfer are
  complete and verified. Keep the Mac-mini BLE LaunchAgent unloaded while this
  arrangement is active; only the relay LaunchAgent belongs on the Mac mini.
- The display rotates every 30 seconds among the Clawd animation, Claude usage,
  Codex usage, Apple Music Now Playing while music is playing, and the latest
  display-safe Pluribus activity while one is present. Touch advances early and
  restarts the 30-second timer.
- Claude usage comes from Claude Code credentials; Codex usage is read passively
  from local Codex session logs; Apple Music metadata/artwork comes from the Mac
  currently playing the music; Pluribus activity comes from the local Pluribus
  server. Never include transcript text in the display payload.
- This unit has an AXP2101 but no physical battery installed. Its `-1` reading
  must make `ui_update_battery()` hide the icon via `battery_present`; do not
  turn that sentinel into an alarming empty-battery glyph. Boards with no
  battery circuitry remain gated separately by `BoardCaps`.

### macOS BLE invariants (do not "simplify" these)

The device is both a BLE HID keyboard and a custom GATT display. macOS owns the
HID connection and CoreBluetooth lets the Python daemon access GATT over that
OS-held relationship. Treat it as one paired accessory, not two independent BLE
clients.

1. Keep the HID service UUID in the primary advertisement, the Espressif PnP ID,
   complete keyboard report descriptor (including the LED output report), and
   `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=2`.
2. Keep `hid_dev->setHidInfo(0, 0x02)`. A fresh pairing may invoke macOS Keyboard
   Setup Assistant; that is expected and matches the pairing flow that originally
   worked. Do not switch the country back to 33 merely to suppress the assistant:
   that produced a connection that appeared for a moment but did not form the
   durable authenticated bond needed by the daemon.
3. `ServerCallbacks::onConnect` may explicitly call `startSecurity` only when
   `NimBLEDevice::getNumBonds() == 0`. On an existing bond, let NimBLE restore
   security. Restarting security on the OS-held connection raced bond persistence
   and crashed inside NVS.
4. On macOS, the daemon must not call `setup_refresh_subscription()`. The REQ
   notification is only an optional low-latency hint; the daemon sends its first
   payload immediately and polls normally without it. Subscribing during bond
   restoration caused the ESP32 to crash/reconnect before any payload arrived.
   Linux and Windows may retain the subscription.
5. Never notify `req_char` re-entrantly from its `onSubscribe` callback. The
   firmware defers that notification to `ble_tick()`.
6. The owner identity and BLE keys live in NVS. A normal PlatformIO upload keeps
   them. Do not erase flash/NVS as routine troubleshooting: it destroys the bond,
   owner lock, and persisted settings and forces a complete re-pair.

### Correct pairing and ownership transfer

- Normal reconnect: power on the already-paired device and let macOS reconnect.
  Do not clear bonds or repeatedly click Forget for a transient daemon problem.
- Fresh pairing/reset: stop the daemon first. Hold the middle PWR button for at
  least three seconds and **release it while the device is still on**. Bond clearing
  happens on release; holding through a hardware power-off does not perform the
  gesture. Then forget the old entry in macOS Bluetooth settings and pair once.
- Keyboard Setup Assistant on a fresh bond is normal. Complete or dismiss its
  identification flow after the Bluetooth entry is connected; do not interpret it
  as a second accessory.
- A duplicated Forget dialog or a device that briefly moves between Nearby and My
  Devices is macOS caching, not evidence that firmware needs to be erased. Stop the
  daemon, inspect logs/serial, and change one side of the bond at a time.
- To move the unit to another Mac, stop the old Mac's daemon, clear the device bond
  with the hold-and-release gesture, forget it on the old Mac, and only then pair
  it with the new Mac. The single-owner lock is deliberate.

Useful host commands:

```bash
launchctl unload ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist
launchctl load ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist
tail -f ~/Library/Logs/claude-usage-daemon.out.log
tail -f ~/Library/Logs/claude-usage-daemon.err.log
claude auth status
```

Expected healthy daemon startup includes `Connected`,
`macOS: polling without optional refresh subscription`, a compact usage payload,
optional `np` and `pb` payloads, and no repeated `Device disconnected`. An empty
Claude view with `API HTTP 401` in the log is an expired/logged-out Claude CLI
credential, not a BLE failure. Verify `claude auth status` or let Claude Code renew
its token before touching pairing.

On the Mac mini, `claude auth status` can still report `loggedIn: true` while the
cached access token is rejected by the Anthropic API. The relay log is
authoritative: repeated `API HTTP 401` followed by `published no-data state`
means Claude Code must refresh its token by completing one real request (or an
interactive login if requested). Restart the relay or wait for its next
60-second poll, then require `Updated Claude/Codex state`. The compact payload
currently reports both usage screens as unavailable when Claude is
unauthenticated, even if local Codex logs still exist.

### Safe build, flash, and verification procedure

1. Stop the LaunchAgent before pairing experiments, serial diagnosis, or flashing;
   otherwise its automatic reconnect loop obscures the first failure.
2. Confirm the exact port with `ls /dev/cu.* | rg 'usb|modem|serial'`. Chris may
   unplug the device intentionally; a vanished USB port by itself is not a firmware
   crash. Ask/check before diagnosing it as one.
3. Build and test:

   ```bash
   ./daemon/.venv/bin/python -m pytest daemon/tests -q
   pio run -d firmware -e waveshare_amoled_216
   ```

4. Flash with a normal upload, never an erase:

   ```bash
   pio run -d firmware -e waveshare_amoled_216 -t upload \
     --upload-port /dev/cu.usbmodemNNNNN
   ```

   For the office deployment, it is safe to bring only the Clawdmeter back to
   this Mac mini and flash it over USB. A normal upload does not erase the NVS
   owner/bond, so it reconnects to the work Mac afterward without pairing again.
   Keep the Mac-mini BLE writer stopped throughout. The managed office network
   intercepts HTTPS and has blocked PlatformIO dependency downloads in both
   `uv` and Python `requests`; prefer this Mac mini's known-good cached toolchain
   rather than disabling TLS verification.

5. Restart the BLE LaunchAgent only on the machine that owns the device. In the
   current Phase 2 deployment, leave the Mac-mini BLE writer unloaded; the work
   Mac resumes its writer when the device returns there. Verify real payloads in
   the owning host's log. For BLE/firmware faults, capture serial output and
   decode the addresses against the exact ELF in
   `firmware/.pio/build/waveshare_amoled_216/firmware.elf`; do not guess from the
   visible Bluetooth state alone.
6. Observe at least one complete 30-second rotation and one artwork change on real
   hardware. Check that Claude, Codex, music, and Pluribus each appear when their
   source is live and that the daemon does not enter a disconnect loop.

### Manual first audio test — optional firmware build

See [the first audio test plan](docs/audio-first-test.md) (2026-09-09).
Scope: explicitly triggered ten-second microphone capture to PSRAM, then USB
transfer to a WAV on the Mac mini. No SD card, Wi-Fi, continuous recording, or
Pluribus ingestion. Chris deferred a mute control for this bounded test.
Chris removed notification chimes from the 2.16-inch build: speaker amp stays off,
sound HAL calls are no-ops, and the build excludes `chime.cpp` / `es8311.c`.
I2S is left free for microphone capture; no chime handoff or restoration is needed.
Other board ports retain their existing sound support. Firmware `6f9154d` was
normally flashed over USB and boot-verified on the physical device on 2026-09-09:
touch/PMU/IMU initialized, the saved work-Mac owner loaded, and the randomized
splash selected `racing car`. No NVS erase or re-pairing was performed. The
Mac-mini BLE writer was stopped and its relay left running. Live widget/BLE
regression verification awaits the bonded work Mac.

The subsequent `waveshare_amoled_216_audio_test` environment adds explicitly
triggered ten-second ES7210 microphone capture and a Mac USB-to-WAV receiver,
while retaining every existing screen. The normal build remains audio-disabled.
Use `./audio-test-mac.sh --info` for a non-recording readiness check, then
`./audio-test-mac.sh --output "$HOME/Downloads/Clawdmeter-audio-tests/speech-3ft.wav"`
to record. Wait for `RECORDING NOW`; every take requires a new invocation and
filename. No SD, Wi-Fi, VAD, transcription, upload, button remapping, or automatic
recording is present. Keep the Mac-mini BLE writer stopped during these USB tests.

Important hardware lesson: the schematic connects physical microphones to
ES7210 MIC1/2 and SDOUT1 → GPIO10. MIC3 is the speaker-reference/AEC circuit;
do not mistake the vendor example's MIC3/4 high-gain settings for the room mics.
Test defaults: microphone 1 (or `--mic 2`), 30 dB gain, 16 kHz / 16-bit mono,
MCLK GPIO42 at 256fs, BCLK9 / WS45. Modern IDF I2S RX reads stereo and explicitly
extracts the chosen slot. The shared Wire bus keeps its transaction locking;
no second I2C driver, display-pin change, or SD initialization is permitted here.

The older Waveshare ES7210 example also left analog power-down bit 7 set in
register 0x40 (0xC3): valid digital samples were only ADC noise, not microphone
audio. Startup now follows current Espressif settings (0x43, mic low-power 0x08,
enable/reset 0x71 → 0x41); shutdown uses 0xC0. The HAL read-back checks on/off
states and gain. Do not revert these as incidental driver changes.

Hardware captures verified exact ten-second WAVs, CRC, 1.67-second USB
transfers, zero reported overruns, and a responsive display loop. After the
analog fix, the prompted three-foot speech take had clear signal variation
(peak 1,193, RMS 136.7 at 30 dB). Chris confirmed it sounded okay but was quiet
even on AirPods, so ADC gain was raised to 37.5 dB (+7.5 dB, approximately 2.37×
sample amplitude for the same input). The higher-gain firmware was flashed and
a new three-foot take verified: peak 1,541, RMS 172.34, zero clipped samples or
reported overruns, 9,999 ms capture, and 1.67-second USB transfer. Independent
spoken takes are not a calibrated gain measurement. The higher-gain listening
verdict was quiet speech with substantial hiss. Chris then identified that his
case was blocking the microphone. Gain is restored to the original 30 dB for a
case-off, same-distance retest. The 30 dB build was normally flashed and its USB
readiness check passed. The case-off three-foot take at 30 dB completed:
peak 1,022, RMS 74.85, no clipping or reported overruns, 10,000 ms capture,
1.67-second verified USB transfer. Chris confirmed playback was "much better"
with the case off. Keep 30 dB as the current baseline; no noise processing was
added. Separate spoken takes do not establish a controlled SNR comparison.
Check microphone openings and case obstruction before increasing gain or adding
noise processing. This is device-side analog gain, not Mac normalization or AGC. Repeated
captures returned to the same memory baseline; cancellation and busy rejection
passed. A case-off six-foot take at 30 dB also transferred successfully
(`speech-6ft-case-off-gain30.wav`): peak 766, RMS 65.25, no clipping or reported
overruns, 9,999 ms capture, 1.67-second USB transfer, and 178 display-loop
iterations (maximum gap 105 ms). Chris confirmed the six-foot playback
"sounded great." Three-foot and six-foot listening checks have passed with the
case off at 30 dB; retain this baseline without added noise processing.
Live BLE/artwork coexistence with the work Mac remains unverified. Results and protocol are in
`docs/audio-first-test.md`. Preserve the optional build when updating this test
device; flashing the normal environment intentionally removes recording support.

Work-Mac continuation: use the handoff section in `docs/audio-first-test.md`.
The device is already flashed at 30 dB; pull source and run the USB receiver,
without re-pairing, reflashing, or reinstalling the relay. The next bounded task
is capture alongside live work-Mac BLE/artwork updates, not adding audio features.

For the full audio project, speech detection and recording decisions belong on
the Clawdmeter. Transcription may run on either the Mac mini or the office Mac,
then feed Pluribus. Moving speech detection to a Mac was a review suggestion that
Chris rejected; do not use it as the default architecture.

### Artwork memory and performance

- The Mac sends 300×300 baseline JPEG artwork in checked binary chunks. Typical
  covers are roughly 5–30 KB and arrived in about 0.7–3.5 seconds during hardware
  verification. Do not revert to the original 180 KB RGB565 transfer for routine
  use.
- `waveshare_amoled_216` requires `ARDUINO_LOOP_STACK_SIZE=16384`. LVGL's JPEG
  decoder overflowed Arduino's default 8 KB `loopTask` stack and rebooted while
  drawing otherwise-valid artwork. A backtrace through `lv_tjpgd.c:decoder_info`
  plus `Stack canary watchpoint triggered (loopTask)` identifies this failure.
- Artwork buffers remain in PSRAM and use the checked double-buffer protocol.
  Preserve generation matching, CRC validation, JPEG SOI/EOI validation, and cache
  invalidation so an old cover cannot be displayed for a new song.

### Phase 2 — live and verified

The physical device is paired to the office Mac while Claude, Codex, and
Pluribus collection remains on the Mac mini. Apple Music is always read from the
**work Mac**, because that is where playback occurs. BLE itself is not relayed
over Tailscale; compact display state is relayed and the work Mac is the only
machine that writes it to the device.

The host-side relay, installers, work-Mac receiver, local Apple Music/artwork,
and physical bond transfer are installed and verified. The Mac-mini collector
returns authenticated Claude/Codex and Pluribus payloads; its old BLE writer
must remain unloaded. Follow [`docs/work-mac-relay.md`](docs/work-mac-relay.md)
for operation, updates, and recovery.

Implemented architecture:

```text
Mac mini: Claude + Codex + Pluribus collector
    -> authenticated/private state feed over the existing Tailscale path
Work Mac: relay receiver + local Apple Music reader + BLE writer
    -> single bonded Clawdmeter
```

Pluribus remains responsible for its normal push notifications and also exposes
the latest display-safe activity to the collector. The work-Mac receiver merges
that remote state with local Now Playing immediately before writing the existing
GATT payloads. Do not make both Macs compete for BLE ownership, and do not send
music state from the Mac mini.

Implementation details and invariants:

1. `daemon/relay_server.py` runs on the Mac mini, polls the existing local source
   adapters, and publishes a versioned envelope with observation timestamps. It
   binds to the explicit Tailscale IPv4 chosen by `install-relay-macmini.sh`, not
   `0.0.0.0`, and requires a random bearer token stored outside Git with mode 600.
2. The feed carries only the existing sub-512-byte usage and `pb` display payloads.
   It never carries credentials, raw Codex sessions, transcripts, or Apple Music.
3. `daemon/relay_source.py` runs inside the normal daemon on the work Mac. It uses
   bounded requests, rejects invalid/oversized data, rejects envelopes or usage
   older than three minutes, and adds relay transit time to Pluribus activity age.
4. `install-work-mac.sh` verifies the authenticated feed before saving its config,
   enables local Now Playing, then installs the existing BLE LaunchAgent. In relay
   mode, `install-mac.sh` must not require local Claude credentials or configure
   Mac-mini-owned usage settings.
5. A temporary relay failure preserves the last valid device state; it must not
   manufacture zero usage. Apple Music remains independent and local to the work
   Mac throughout.
6. At the office, stop the Mac-mini BLE LaunchAgent but leave the relay LaunchAgent
   running. Then perform the controlled bond transfer described above and complete
   the one-time keyboard setup on the work Mac.
7. Verify sources separately: remote Claude, remote Codex, remote Pluribus, local
   Apple Music metadata, local artwork, then the 30-second rotation and Pluribus
   notification behavior.

Do not change the on-device payload schema merely because the data crosses two
Macs. Phase 2 should be a host-side transport/ownership change; the currently
working firmware and screens are the stable boundary.

Seven ports today (two SoC families, five panel sizes):

- `boards/waveshare_amoled_216/` — original Waveshare ESP32-S3-Touch-AMOLED-2.16 (CO5300, 480×480 square, CST9220 touch, IMU rotation). Build env: `waveshare_amoled_216`.
- `boards/waveshare_amoled_18/` — Waveshare ESP32-S3-Touch-AMOLED-1.8 (368×448 portrait, XCA9554 IO expander). Build env: `waveshare_amoled_18`. **Two panel revisions are auto-detected at boot** (`board_rev()` in `board_init.cpp`, enum in `board_rev.h`): original = SH8601 display + FT3168 touch (0x38); later = CO5300 display + CST816 touch (0x15). One binary drives both.
- `boards/waveshare_amoled_216_c6/` — Waveshare ESP32-C6-Touch-AMOLED-2.16 (SH8601, 480×480, CST9217 touch). Build env: `waveshare_amoled_216_c6`. ESP32-C6 SoC: single-core RISC-V, **no PSRAM**, BLE 5 only.
- `boards/waveshare_amoled_18_c6/` — Waveshare ESP32-C6-Touch-AMOLED-1.8 (368×448 portrait, SH8601, FT3168 touch, TCA9554 expander). Build env: `waveshare_amoled_18_c6`. Same panel as the S3 1.8 but on the C6 SoC. All subsystems (display, touch, BOOT + PWR buttons, battery, BLE) verified on hardware.
- `boards/waveshare_amoled_206/` — Waveshare ESP32-S3-Touch-AMOLED-2.06 (CO5300, 410×502 watch form factor, FT3168 touch, no IO expander, 32 MB flash, PCF85063 RTC, ES8311 codec). Build env: `waveshare_amoled_206`. Display, touch, battery, IMU init, and BLE verified on hardware; the ES8311 chime path is not wired up (`sound.cpp` no-ops).
- `boards/waveshare_lcd_154/` — Waveshare ESP32-S3-Touch-LCD-1.54 (ST7789, 240×240 square, CST816T touch @ 0x15). Build env: `waveshare_lcd_154`. **The first non-AMOLED port**: a plain 4-wire SPI TFT, not QSPI, and the panel has no brightness command — backlight is LEDC PWM on `LCD_BL`. **No PMU**: battery is an ADC divider on GPIO1 and `BAT_EN` (GPIO2) is a power-hold line that must be driven HIGH early in `board_init()` or the board browns out on battery. Three buttons (BOOT + GPIO5 + a PWR-role GPIO4); ES8311 chime wired up; QMI8658 populated but unused (fixed orientation, no rotation).
- `boards/waveshare_lcd_4/` — Waveshare ESP32-S3-Touch-LCD-4 (ST7701 RGB parallel, 480×480 square, GT911 touch). Build env: `waveshare_lcd_4`. **RGB-panel port**: Arduino_ESP32RGBPanel + bounce buffers (tearing fix). IO expander @ 0x24 (TCA9554 / CH32V003) must init before `gfx->begin()` or the panel stays dark; backlight is expander pin 2 (on/off only). No AXP2101 / IMU; KEY/PWR is hardware RST. Single BOOT button (GPIO 0 → Space/PTT).

Plus one non-hardware target: `boards/sim/` — **native desktop simulator** (SDL2 window, 480×480, `platform = native`). Build env: `sim`. See "Desktop simulator" below.

**C6 ports have no PSRAM** — shared code gates on `BOARD_HAS_PSRAM` (absent on C6) to use `MALLOC_CAP_INTERNAL` for LVGL/splash buffers, and the `screenshot` serial command is disabled (`LV_USE_SNAPSHOT=0`), so UI changes on a C6 board must be eyeballed on hardware, not auto-captured.

The shared code calls a small HAL (`firmware/src/hal/`) that each board implements: display, touch, input, power, IMU. Optional features are guarded by `BoardCaps` (runtime) and `BOARD_HAS_*` (compile-time) rather than `#ifdef BOARD_*`.

Connects to a host daemon over BLE; daemon polls Anthropic API for usage data. This file is for future Claude Code sessions to bootstrap quickly. Read this first.

## Hardware (critical pins)

### AMOLED-2.16 (original)
- Display: **CO5300** AMOLED via QSPI (CS=12, SCLK=38, SDIO0..3=4..7, RST=2)
- Touch: **CST9220** via I2C (SDA=15, SCL=14, INT=11, addr=0x5A)
- PMU: **AXP2101** on same I2C bus (addr=0x34) — battery, USB VBUS, PWR button IRQ
- IMU: **QMI8658** on same I2C bus (addr=0x6B) — accelerometer for auto-rotation
- Buttons: GPIO 0 (left → Space/voice-mode), GPIO 18 (right → Shift+Tab/mode-toggle), AXP PKEY (middle → cycle screens; on splash → cycle animations)

### AMOLED-1.8 (newer port)
**Two hardware revisions ship under this name; the firmware probes I2C at boot and picks drivers automatically (`board_rev()`):**
- Display: **SH8601** (original) or **CO5300** (later rev) AMOLED via QSPI (CS=12, **SCLK=11** ← different!, SDIO0..3=4..7, RST routed via XCA9554 EXIO1). Both are `Arduino_OLED` subclasses held behind one base pointer in `display.cpp`. The CO5300's 368-wide active area starts at GRAM column 16, so it gets `CO5300_COL_OFFSET 16` to center; SH8601 needs none.
- Touch: **FT3168** @ 0x38 (original) or **CST816** @ 0x15 (later rev), via I2C (SDA=15, SCL=14, INT=21). Both expose the same FocalTech-style data layout at regs 0x02..0x06, so one inline reader in `touch.cpp` serves both — only the address differs. Avoids vendoring the GPLv3 `Arduino_DriveBus` library. Revision is detected by which touch address ACKs (CST816 present ⇒ CO5300 panel).
- PMU: AXP2101 @ 0x34 (same chip as 2.16 — `XPowersLib` reused; battery is an optional kit add-on but PMU + charging circuitry are populated)
- IMU: QMI8658 @ 0x6B (same chip — initialized for I2C bus health, rotation logic disabled)
- IO expander: **XCA9554 / PCA9554** @ I2C 0x20. Gates LCD_RST, TP_RST, audio amp enable, and reads the PWR button. **`io_expander_init()` MUST run before `gfx->begin()` or `ft3168_init()`** — otherwise display/touch stay in reset and silently fail. PWR button is on EXIO4, active HIGH (verified empirically with the deleted `iox` serial debug command).
- Orientation: **fixed at 0°**. IMU auto-rotation is disabled; `rotate_strip()` / `handle_rotation_change()` are excluded via `#ifndef BOARD_AMOLED_18`.
- Buttons: GPIO 0 (BOOT → Space/voice-mode), XCA9554 EXIO4 (PWR → cycle screens; on splash → cycle animations). **No third button** (GPIO 18 button doesn't exist on this board).

### AMOLED-1.8 (C6) — `waveshare_amoled_18_c6`
ESP32-C6 sibling of the S3 1.8: same 368×448 SH8601 panel + FocalTech touch, different SoC and GPIO map. **All pins/edges below verified on hardware via temporary GPIO/IRQ scans, since Waveshare's wiki publishes no pin table and the third-party BSP's numbers were partly wrong.**
- Display: **SH8601** AMOLED via QSPI (CS=5, SCLK=0, SDIO0..3=1..4, no MCU reset pin — internal POR; effective reset is the TCA9554 power-cycle). Stock `Arduino_SH8601` init (no vendor-register patch — that's only needed on the C6 2.16).
- Touch: **FT3168** (some units FT6146) @ I2C 0x38, INT=15. Same inline FocalTech reader as the S3 1.8 (regs 0x02..0x06); no reset pin (gated by TCA9554 touch power).
- I2C bus: SDA=8, SCL=7 (shared by TCA9554, AXP2101, FT3168, QMI8658, PCF85063 RTC, ES8311 codec).
- IO expander: **TCA9554 / PCA9554** @ 0x20 — here it gates **power**, not reset: **P4 = display power, P5 = touch power, P7 = audio amp**. `io_expander_init()` runs the documented power-on sequence (P4/P5 LOW → 200 ms → HIGH) and **MUST run before `display_hal_init()`** or the panel stays unpowered. Amp (P7) left off (no audio path).
- PMU: AXP2101 @ 0x34 (owned by `power.cpp`, not `board_init` — LCD isn't on an ALDO rail here).
- IMU: QMI8658 @ 0x6B (init'd for bus health, rotation disabled).
- Orientation: **fixed at 0°**, no rotation (no PSRAM headroom).
- Buttons: **GPIO 9** (BOOT → Space/voice-mode, active LOW — *not* the docs' GPIO 0/9 guess; confirmed by scan), **AXP2101 PKEY** (PWR → cycle screens; on splash → cycle animations). The PKEY **SHORT-press IRQ fires on release** — that's the edge `power.cpp` acts on. No secondary button.

### AMOLED-2.06 (watch form factor) — `waveshare_amoled_206`
- Display: **CO5300** AMOLED via QSPI (CS=12, **SCLK=11** ← same as 1.8, SDIO0..3=4..7, RST=8 direct GPIO). 410×502 portrait. Requires **`col_offset1 = 23`** in the `Arduino_CO5300` constructor — the panel's visible viewport sits at a 22–23 column offset inside the controller's internal RAM. Without it, a vertical strip of stale/garbage content shows through on the right edge (23 was picked empirically for centering; Waveshare's reference library uses 22). The 2.16 dodges this because its 480×480 viewport fills the controller's RAM.
- Touch: **FT3168** via I2C (SDA=15, SCL=14, **INT=38, RST=9** direct GPIO, addr=0x38). Same inline FocalTech reader as the 1.8 port (no GPLv3 `Arduino_DriveBus` dependency). Coordinates verified end-to-end with the BLE reset zone.
- PMU: AXP2101 @ 0x34 (same chip as 2.16/1.8 — `XPowersLib` reused). PWR button routes through AXP PKEY IRQs (short / long / positive), same path as the 2.16 — no IO expander.
- IMU: QMI8658 @ 0x6B (initialized for I2C bus health; rotation logic disabled — fixed watch enclosure orientation).
- RTC: **PCF85063** on the same I2C bus, powered through AXP2101 for retention. Not used by Clawdmeter but present for future features.
- Audio codec: **ES8311** + ES7210 ADC on the same I2C bus. The amp path is unverified on this board, so `sound.cpp` no-ops (same posture as the C6 1.8) — the shared `chime.cpp` engine is ready to wire up once it's tested on hardware.
- **No IO expander** despite the Waveshare wiki FAQ implying one. The schematic shows Key3/PWR wired directly to AXP2101 PWRON; touch reset and display reset are direct GPIOs. `board_init()` pulses LCD_RESET (GPIO 8) and TP_RESET (GPIO 9) before display/touch HAL init.
- Buttons: GPIO 0 (BOOT → Space/voice-mode), AXP PKEY (PWR → cycle screens; hold-to-pair). **No third button**.
- Flash: 32 MB. Uses `default_32MB.csv` partition table.

### LCD-4 — `waveshare_lcd_4`
- Display: **ST7701** 480×480 RGB parallel (DE=40, VSYNC=39, HSYNC=38, PCLK=41, R0-4=46/3/8/18/17, G0-5=14/13/12/11/10/9, B0-4=5/45/48/47/21); ST7701 init via SW SPI (CS=42, SCK=2, MOSI=1).
- Touch: **GT911** via I2C (SDA=15, SCL=7), polled (wiki INT=GPIO 16 unused). Probe 0x5D then 0x14.
- IO expander: **addr 0x24** (fallback 0x20) on the same I2C bus — must init before `gfx->begin()` (output 0xFF, config 0x3A). Backlight is expander pin 2.
- No PMU / IMU. Buttons: GPIO 0 only (BOOT → Space/PTT). KEY/PWR is EN/RST (hardware reset). GPIO 18 is display R3.
- RGB tearing fix: pass `bounce_buffer_size_px = LCD_WIDTH * 10` to `Arduino_ESP32RGBPanel`. Do not call `rgbpanel->getFrameBuffer()` after `gfx->begin()`.

## Architecture

```text
firmware/src/
  hal/                      — board-agnostic interfaces shared code calls into
    board_caps.h            — runtime BoardCaps struct (W, H, button_count, has_* flags)
    display_hal.h           — init / begin / set_brightness / draw_bitmap / tick / round_area
    touch_hal.h             — init / read(&x, &y, &pressed)
    input_hal.h             — init / is_held(PRIMARY|SECONDARY)
    power_hal.h             — init / tick / battery_pct / is_charging / pwr_pressed (edge)
    imu_hal.h               — init / tick / rotation_quadrant
  boards/
    waveshare_amoled_216/   — CO5300 + CST9220 + AXP PKEY + QMI8658 rotation
    waveshare_amoled_18/    — SH8601 + FT3168 + AXP + XCA9554 (PWR via EXIO4), no rotation
    waveshare_amoled_216_c6/— C6: SH8601 + CST9217 + AXP PKEY, no PSRAM
    waveshare_amoled_18_c6/ — C6: SH8601 + FT3168 + AXP PKEY + TCA9554 (gates power), no PSRAM
    waveshare_amoled_206/   — CO5300 + FT3168 + AXP PKEY, no IO expander, 32 MB, no rotation
    waveshare_lcd_154/      — ST7789 SPI TFT + CST816T + ADC battery (no PMU), PWM backlight
    waveshare_lcd_4/         — ST7701 RGB parallel + GT911 + expander backlight, no PMU/IMU
    sim/                    — native desktop simulator: SDL2 + Arduino shims + scenario playback
    template/               — copy this to bootstrap a new port
  main.cpp                  — setup() + loop(): HAL calls only, zero #ifdef BOARD_*
  ui.{h,cpp}                — splash, Claude usage, Codex, Bluetooth, Now Playing, and Pluribus screens; compute_layout() picks fonts/positions from board_caps()
  splash.{h,cpp}            — 20×20 pixel-art engine. CELL = min(W,H)/20, centered.
  ble.{h,cpp}               — NimBLE peripheral: custom data service + HID keyboard
  data.h                    — UsageData struct
  icons.h                   — icon arrays. Battery (5×) are RGB565A8 with alpha; rest are raw RGB565.
  logo.h                    — 80×80 RGB565 logo
  font_*.c                  — pre-compiled LVGL 9 bitmap fonts (Tiempos 56/34, Styrene 48/28/24/20/16/14/12, Mono 32/18)
  splash_animations.h       — generated, do not hand-edit
docs/porting/               — adding-a-board.md, hal-contract.md, capability-flags.md
```

Each board folder contains: `board.h` (pins, I2C addresses, `BOARD_HAS_*` flags),
`board_init.cpp` (Wire.begin + any IO expander), `display.cpp`, `touch.cpp`,
`input.cpp`, `power.cpp`, `imu.cpp`, `caps.cpp` (the `BoardCaps` instance), plus
any board-private hardware drivers (e.g. `io_expander.{h,cpp}` on AMOLED-1.8).
PlatformIO's `build_src_filter` includes shared code + one board's folder per env.

## Build / flash

```bash
pio run -d firmware -e waveshare_amoled_216                                     # build 2.16 (S3, default original)
pio run -d firmware -e waveshare_amoled_18                                      # build 1.8 (S3)
pio run -d firmware -e waveshare_amoled_216_c6                                  # build 2.16 (C6)
pio run -d firmware -e waveshare_amoled_18_c6                                   # build 1.8 (C6)
pio run -d firmware -e waveshare_amoled_206                                     # build 2.06 (S3, watch)
pio run -d firmware -e waveshare_lcd_154                                        # build 1.54 (S3, SPI TFT)
pio run -d firmware -e waveshare_lcd_4                                           # build LCD-4 (S3, RGB TFT)
pio run -d firmware -e waveshare_amoled_18 -t upload --upload-port /dev/cu.usbmodem101   # flash 1.8 on macOS
pio run -d firmware -e waveshare_amoled_216 -t upload --upload-port /dev/ttyACM0         # flash 2.16 on Linux
# C6 boards: same native USB-JTAG flashing; flag a chip mismatch ("This chip is ESP32-C6,
# not ESP32-S3") means you picked an S3 env — use a *_c6 env for C6 hardware.
```

If `pio` isn't on PATH: try `~/.platformio/penv/bin/pio` (Linux/macOS pio install) or `brew install platformio` on macOS.

Device path differs by OS: `/dev/cu.usbmodem*` on macOS, `/dev/ttyACM0` on Linux. Both expose the ESP32-S3 native USB-JTAG (no boot-mode dance needed).

## Desktop simulator (`-e sim`) — develop UI without hardware

```bash
sudo apt install libsdl2-dev   # once (macOS: brew install sdl2)
pio run -d firmware -e sim && (cd firmware && .pio/build/sim/program)
```

An SDL2 window stands in for the 480×480 panel; the **full firmware loop runs
unmodified** — `main.cpp`, `ui.cpp`, `splash.cpp`, idle fade, pair gesture,
JSON parsing, usage-rate/chime logic. Only `ble.cpp`/`chime.cpp` are swapped
for stubs. How it works: `boards/sim/` implements the HAL against SDL2, thin
Arduino shims live in `boards/sim/shim/` (`millis`/`Serial`→stdio,
`heap_caps`→malloc, in-memory `Preferences`), and `ble_sim.cpp` plays back
daemon payloads from `firmware/sim/scenario.jsonl` (one JSON line per state +
optional `name`/`hold_ms`; override with `SIM_SCENARIO=<path>`).

Controls (full map in `boards/sim/board.h`): mouse = touch · space =
play/pause scenario · ←/→ = step · 1-9 = jump · d = BLE link toggle ·
b/n = BOOT/secondary buttons · p = PWR · c/-/= = charging/battery ·
s = screenshot BMP · esc = quit.

Headless screenshots (works in CI, no display):
`SDL_VIDEODRIVER=dummy SIM_AUTOSHOT_MS=6000 .pio/build/sim/program` saves
`sim-autoshot.bmp` (or `SIM_AUTOSHOT_PATH`) after 6 s and exits. Combine with
the boot-screen swap trick below to capture any screen. **The sim renders with
desktop LVGL and fake data — always do a final check on real hardware before
merging panel-related changes** (col offsets, rotation, rounding live in the
hardware boards, not shared code).

## QA your own UI changes — don't ask the user

The firmware ships a `screenshot` serial command that dumps the LVGL framebuffer. `./screenshot.sh out.png [port]` captures a PNG sized to the active display (480×480 or 368×448). **Use this on every UI iteration** — Read the PNG with the Read tool, verify the change visually, iterate. Script auto-picks the macOS/Linux default port and falls back to pio's bundled Python if pyserial isn't on the system Python.

The boot screen is `SCREEN_SPLASH` and only advances on a physical button press, so a fresh flash will sit on the splash. To screenshot the screen you're actually editing without asking the user to press a button, **temporarily change the default boot screen** in `main.cpp` (search for `ui_show_screen(SCREEN_SPLASH);`) to `SCREEN_USAGE` / `SCREEN_CONTROLLER` / `SCREEN_BLUETOOTH`, do your iteration, then revert before committing.

## Critical gotchas

1. **CO5300 cannot rotate.** Its MADCTL only supports axis flips, not column/row exchange. Rotation is done by **CPU pixel remapping inside `display_hal_draw_bitmap`** in `boards/waveshare_amoled_216/display.cpp`. We use **PARTIAL render mode with strip rotation** (small 480×40 strips, fast). On rotation change → AMOLED brightness flash → force redraw (handled inside `display_hal_tick`).
2. **OPI PSRAM** required: `board_build.arduino.memory_type = qio_opi` in platformio.ini. Without this, `MALLOC_CAP_SPIRAM` returns NULL and the screen is black.
3. **pioarduino platform required.** GFX Library for Arduino needs Arduino Core 3.x (`esp32-hal-periman.h`), not the 2.x that standard `espressif32` ships. We pin `pioarduino/platform-espressif32` 55.03.38-1.
4. **LVGL 9 font patching.** `lv_font_conv` outputs LVGL 8 format. Must remove `#if LVGL_VERSION_MAJOR >= 8` guards, drop `.cache` field, add `.release_glyph`, `.kerning`, `.static_bitmap`, `.fallback`, `.user_data`. Without patching, fonts render invisible. Full regeneration recipe: `docs/fonts.md`.
5. **Touch reading is centralized inside each board's `touch.cpp`.** The HAL `touch_hal_read()` is called once per loop from `my_touch_cb`; the board's implementation owns its latched `touch_pressed/x/y` state. Don't call the underlying controller from anywhere else — CST9220's `getPoint()` etc. do a full I2C transaction and concurrent callers consume each other's data.
6. **Even-aligned flush regions.** `display_hal_round_area` (called from `rounder_cb`) is what each board uses to enforce this. Required on CO5300, harmless on SH8601.
7. **Touch axis swap/mirror is per-board.** The 2.16's CST9220 needs `setSwapXY(true)` + `setMirrorXY(true, false)` — applied inside `boards/waveshare_amoled_216/touch.cpp::touch_hal_init()`. New ports apply their own.
8. **LVGL RGB565A8 is planar.** `w*h` RGB565 pixels followed by `w*h` alpha bytes; `data_size = w*h*3`, `stride = w*2`. Use `init_icon_dsc_rgb565a8()` for icons that overlap non-uniform backgrounds (e.g. battery over splash). Lucide source PNGs are black-on-transparent — converter must tint to white or icons render invisible. See `tools/png_to_lvgl.js`.
9. **Per-board pre-init is `board_init()`.** Each board's `board_init.cpp` brings up `Wire` and any reset-gating IO expander BEFORE `display_hal_init()`. Skipping the IO expander release on AMOLED-1.8 leaves SH8601 + FT3168 in reset and they silently fail to probe. Same for LCD-4: expander @ 0x24 must run before `gfx->begin()` or the ST7701 stays dark.
10. **No `#ifdef BOARD_*` in shared code.** The whole point of the refactor — if you're about to add one, you probably want a `BoardCaps` field or a per-board file instead. See `docs/porting/capability-flags.md`.
11. **LCD-4 RGB bounce buffers.** `Arduino_RGB_Display` DMA-scans PSRAM. Pass `bounce_buffer_size_px = LCD_WIDTH * 10` so ESP-IDF allocates SRAM bounce buffers. Do not call `rgbpanel->getFrameBuffer()` after `gfx->begin()` — it constructs a second RGB panel and crashes.
12. **LCD-4 has only one user button (GPIO 0 / BOOT).** GPIO 18 is display R3. KEY/PWR is EN/RST (hardware reset). Hold-to-pair and PWR-short animation/brightness cycling are unavailable; tap the panel to toggle splash ↔ usage.

## Icons

`tools/png_to_lvgl.js <input.png> <symbol> [W_MACRO] [H_MACRO] [--tint=RRGGBB | --no-tint]` converts an alpha PNG to RGB565A8. Default tint is white (`0xFFFFFF`) — necessary for Lucide PNGs. Splice output into `firmware/src/icons.h` and use `init_icon_dsc_rgb565a8()` in ui.cpp. Currently only the 5 battery icons use this format; the rest are still raw RGB565 baked over the panel background, fine because they live inside opaque zones.

## Splash animations

17 official Anthropic Clawd animations (core poses + persona scenes), archived
with full provenance in `research/clawd-official/`. Pipeline:

On every entry to `SCREEN_SPLASH`, `splash_show()` draws from a hardware-random
shuffled deck of the complete 17-animation catalog. The deck prevents entry
repeats until all 17 have appeared and avoids an immediate duplicate across deck
boundaries. Do not start at an arbitrary frame: the authored intro/loop/outro and
walking choreography require frame zero. The 20-second within-splash rotation and
mid-display usage-rate changes still select from the current rate group. This
behavior was flashed and boot-verified on the physical 2.16-inch device on
2026-09-08: the work-Mac owner remained present in NVS and the first observed
post-flash selection was `trumpet` rather than the former fixed walk start.

```bash
node tools/convert_official_clawd.js            # → firmware/src/splash_animations.h
node tools/convert_official_clawd.js --verify DIR   # + per-animation PNGs for eyeballing
```

Requires ImageMagick; Laptop and Soccer convert from their Lottie exports
(crisp) rather than GIFs. Frames are bounding-box crops on the official 55×37
art stage (ox/oy = stage offset — every animation shares one idle-Clawd
position, so transitions are seamless), one byte per cell into a per-animation
≤16-color RGB565 palette (index 0 = background, true black), per-frame hold ms
with duplicates collapsed (~400 KB total). The converter also: detects each
animation's **loop region** (gait cycles, scene middles; sailing scene's is
located by cross-matching the standalone sailing-loop asset, which is not
emitted), synthesizes the **eyes** (transparent holes in the source GIFs) as
`#141413` ink via border flood-fill, and applies two contrast recolors
(trumpet notes → ivory, magnifier fedora → gray) via component analysis.

The splash engine (`splash.cpp`) plays intro → loop → outro on a **60×60
stage** (`SPLASH_GRID`, cell = min(W,H)/60 → 8 px on 480, 6 px on 368, 4 px on
240): loops hold until released (walk arrival, scene timer, rotation), so
switches always pass through the shared idle pose. Walkers translate with
foot-locked per-frame schedules and mirror when heading left. Usage-rate
groups pick animations by name; the same rate drives the **corner mascot** on
the usage screen (`splash_mascot_*`, PSRAM boards; C6 falls back to the static
`clawd_still.h` icon) — idle stills, rate-scaled acts, and walk-off/lurk/
walk-back trips. Default boot screen.

**Where the animations come from / finding new ones:** all assets are plain
files under `https://claude.ai/images/clawd/{core,persona}/…` — static assets
are not Cloudflare-gated, only HTML routes are. The asset server returns a
real GIF for a valid filename and an HTML catch-all (both HTTP 200) otherwise,
so **name probing works**: fetch `Clawd-<Name>.gif` and check the magic bytes.
Seven current animations are referenced by no shipped bundle and were found
exactly this way (Anthropic stages seasonal drops — Soccer appeared for the
World Cup). To hunt for new ones: run `research/clawd-official/fetch.sh`
(extend its probe list), and grep a fresh desktop .deb's `ion-dist/` bundles
for `/images/` paths (`research/clawd-official/CLAUDE.md` documents the full
methodology, including the Lottie sources and the assets-proxy).


## User profile / preferences

See `~/.claude/projects/.../memory/` files for persistent context (user is an embedded-beginner senior dev, brand-conscious, prefers iterative UI refinement, dislikes me authoring my own art when third-party assets are intended). Always read those memory files at session start.

## Recent session highlights

- **AMOLED-1.8 chime verified on hardware + EXIO2 touch-kill fix (2026-07-13).** The 1.8's `amp_enable` hook drove both GPIO 46 and XCA9554 EXIO2 ("the unused one is harmless") — but pulling EXIO2 low takes the FT3168 off the I2C bus (chip stops ACKing; IDF reports it as `ESP_ERR_INVALID_STATE`, which reads like a driver wedge and cost a long I2S red-herring chase). Amp enable is GPIO 46 only; EXIO2 must stay HIGH. Chime, touch, buttons, and BLE bond persistence all verified on a real 1.8.
- **Device-abstraction refactor (2026-05-18).** All board-conditional code moved out of shared files into `boards/<name>/` and behind a HAL in `hal/`. ~30 `#ifdef BOARD_*` blocks went to zero. UI is responsive via `compute_layout()` driven by `board_caps()`. New ports add a folder + a PlatformIO env — no shared file edits.
- Added second board port: Waveshare AMOLED-1.8 (368×448 portrait, SH8601, FT3168, XCA9554 IO expander).
- Migrated from Panlee SC01 Plus (480×320 IPS) to Waveshare 2.16" AMOLED (480×480 square). Full hardware/library swap.
- Added IMU auto-rotation, battery indicator, USB-state-aware screen switching.
- Added splash screen with scraped pixel-art animations and 3-button physical input layout.
- Fonts and icons re-scaled ~1.9× for the higher-DPI panel.
- All UI margins widened to 20px to clear the rounded display corners.
- Battery icons converted to RGB565A8 alpha so they blend cleanly over the splash animations.

## Daemon / host side

Bash daemon (`daemon/claude-usage-daemon.sh`) reads OAuth token, polls Anthropic API, sends JSON over BLE GATT. Run with `systemctl --user start claude-usage-daemon`. The unit file's `ExecStart` is the absolute path to the script — repoint it when switching between the worktree and the main checkout.

**Discovery & resilience:**

- Connects by name (`"Clawdmeter"`) on first run, caches resolved MAC at `~/.config/claude-usage-monitor/ble-address`. ESP32 BLE addresses are factory-burned per-chip, so swapping any board invalidates the cache.
- On connect failure: cache is dropped AND device is removed from bluez (`bluetoothctl remove`) so the next scan won't re-pick a dead MAC. Multi-candidate scans pick `head -1` and let the failure cycle converge.
- `POLL_INTERVAL=60`, `TICK=5`. Inner loop wakes every 5s to detect disconnects fast; polls Anthropic when 60s elapsed OR when ESP fires a refresh request.

**GATT characteristics on service `4c41555a-...0001`:**

- `...0002` RX — daemon writes JSON usage payload here.
- `...0003` TX — firmware notifies ack/nack (daemon doesn't subscribe).
- `...0004` REQ — firmware fires `0x01` notify in `onSubscribe` if `has_received_data` is false. Daemon subscribes via `setsid bash -c "stdbuf -oL dbus-monitor … | awk …"`; awk drops a flag file the inner loop picks up. See the `feedback_dbus_monitor_pipe` memory for the three subtle gotchas (pipe buffering, busctl-exits race, `wait` blocking on pipeline jobs).
