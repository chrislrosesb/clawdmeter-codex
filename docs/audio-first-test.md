# First audio test — ten seconds to the Mac over USB

Date: 2026-09-09. Status: implemented; USB capture and a prompted three-foot
speech take verified. Chris confirmed intelligibility but found playback quiet.
Higher-gain retest also passes structural/clipping checks; its listening verdict,
six-foot test, and live work-Mac BLE coexistence remain pending.

## Run it

The optional `waveshare_amoled_216_audio_test` build contains the complete current
Clawdmeter display plus the manual audio test. The normal
`waveshare_amoled_216` build does not contain microphone capture.

From the project directory on the connected Mac:

```bash
./audio-test-mac.sh --info
./audio-test-mac.sh --output "$HOME/Downloads/Clawdmeter-audio-tests/speech-3ft.wav"
```

Wait for **RECORDING NOW**, then speak normally for ten seconds. The microphone
stops before USB transfer begins. The command prints the saved WAV path and
diagnostics. Listen by opening that file in Finder/QuickTime. Run again with a
different filename for the six-foot test. Existing files are never overwritten.
`--mic 2` selects the other physical microphone; the default is microphone 1.
Both currently use 37.5 dB ADC gain. `--port /dev/cu.usbmodemNNNNN` selects a USB
device explicitly; auto-detection works only when there is exactly one candidate.
`--info` never records; Control-C cancels an in-progress test. Close serial
monitors before running. The wrapper uses an existing Python/pyserial environment.

To build and flash (normal upload, preserve NVS and the work-Mac bond):

```bash
pio run -d firmware -e waveshare_amoled_216_audio_test
pio run -d firmware -e waveshare_amoled_216_audio_test -t upload --upload-port /dev/cu.usbmodemNNNNN
./daemon/.venv/bin/python -m pytest daemon/tests tools/tests -q
```

Keep the Mac-mini BLE writer stopped and its relay running. Returning to the
normal build removes audio capture without removing any existing display feature.

## Outcome and scope

Record ten seconds from the Clawdmeter's onboard microphone while the existing
display runs, transfer the recording over USB, and save a playable WAV on the
Mac mini. Listen at normal desk distance before investing in continuous recording.

The recording lives temporarily in ESP32 PSRAM, then on the Mac's disk. No SD
card or external SSD is required. Transfer follows capture; this does not create
a live macOS USB microphone device.

Chris explicitly deferred a mute button for this bounded test. Recording starts
only on an explicit command, stops automatically, and never starts at boot.
No button remapping, recording widget, Wi-Fi, SD, VAD, transcription, or Pluribus
upload is included in this milestone.

## User experience

1. Connect the Clawdmeter to the Mac mini with a USB data cable.
2. Run a Mac capture utility with the explicit port and desired output filename.
3. The utility reports readiness and gives a short countdown, then commands one
   recording. Its recording-start message confirms when to speak.
4. Speak normally for ten seconds. Firmware stops automatically and transfers the
   completed recording; the utility shows progress.
5. The utility validates the transfer, saves the WAV, and prints its path. Listen
   on the Mac. Every additional recording requires another explicit invocation.

## Firmware design

- Extend the existing Arduino/PlatformIO application for the Waveshare 2.16 board,
  retaining the pinned toolchain, display features, and Bluetooth behavior.
- Uses the Espressif-origin ES7210 driver from Waveshare's pinned exact-board
  example, with local error-propagation fixes (see its vendored README/license).
  The schematic confirms ADC address 0x40 and physical microphones on MIC1/2.
  SDOUT1 connects through R38 to GPIO10; MIC3 is the speaker-reference/AEC input,
  not a room microphone. Do not copy the example's MIC3/4 gain as the ambient
  microphone configuration. ES8311 remains the unused speaker path.
- Target 16 kHz, signed 16-bit little-endian PCM, one selected microphone channel:
  160,000 samples for ten seconds, occupying 320,000 bytes. The Mac adds a standard
  WAV header, producing a 320,044-byte file.
- Extract the correct microphone slot from stereo/TDM or wider bus samples.
  Do not interpret unverified raw bus bytes as mono audio or average unknown
  channels. Log the selected channel and gain; test the other mic separately if
  needed.
- Allocate the recording explicitly in PSRAM before starting. Measure internal
  free heap, largest free block, and PSRAM before/during/after. The previous 33%
  build-time RAM figure does not establish runtime headroom.
- Use a dedicated capture task, bounded I2S reads, and driver-compatible DMA
  buffers feeding the PSRAM recording. Keep the display loop responsive. Track
  samples, read errors, and overflow events where the driver exposes them.
- Bound setup and capture time. Stop after the target sample count; stalled I2S
  produces an error instead of an indefinitely active recording. Reset or USB
  failure must never restart recording automatically.
- Expose a small capture interface to shared code, with pins/codec configuration
  in the board hardware layer. Use an optional audio-test build flag; disabled
  builds and other boards retain their current behavior.

## Audio ownership and peripheral sharing

Chris chose to remove notification chimes entirely on 2026-09-09. The 2.16-inch
build excludes the chime engine and ES8311 speaker driver, keeps the speaker amp
off, and does not initialize I2S for playback. Microphone capture can own the
audio clocks at 16 kHz; no playback handoff, resampling, or chime restoration is
needed. Stop/release capture on completion or failure. The display continues
running throughout. Other board ports retain their existing speaker behavior.

Reuse the shared I2C bus with compatible locking rather than initializing a
conflicting bus driver. Do not reset touch or power management during ADC setup.
SD remains uninitialized, so the GPIO2 SD-clock/display-reset discrepancy can
wait until the storage milestone. Do not change working display pins incidentally.

## USB transfer and Mac utility

Extend the existing serial command interface with one fixed-duration capture
request and status responses. Reject another capture or screenshot while busy.
Implemented framing is newline-delimited `AUDIO1 {json}`: recording ID, format,
sample count, byte length, and CRC32 in a `begin` packet, then numbered `data`
packets containing up to 192 PCM bytes encoded as hex, and an `end` packet with
the CRC. `start`, `health`, `memory`, and `error` packets report lifecycle and
diagnostics. The receiver requires exactly 320,000 decoded PCM bytes.

This intentionally replaces the initially proposed raw binary stream. Each
bounded packet is a single HWCDC write, serialized by the existing TX mutex;
leading newlines and hex encoding keep ordinary logs out of audio frames without
a global logging lock. The receiver ignores ordinary log lines and rejects
malformed/missing/duplicate/out-of-order/checksum-invalid audio. It never saves
corrupt audio as a completed WAV. Firmware yields between packets, uses a 20 ms
TX timeout and a 30-second total transfer bound, and frees its recording buffer
on completion or failure. Screenshots and second recordings are rejected while busy.

The Python utility must validate format, bounded length, checksum, and completion
before finalizing the WAV from a temporary file. It opens only the selected USB
port, tolerates boot messages, and does not intentionally reset the board via
DTR/RTS. A serial monitor must not own the port concurrently. Measure actual USB
throughput and allow a generous bounded timeout rather than predicting transfer
speed from the nominal 115200 baud setting. USB readiness probes may be retried
because the first response can be lost during endpoint opening; the recording
request itself is NEVER automatically retried. Mic setup occurs only on that
explicit request. Capture has bounded 250 ms I2S reads and a 12-second deadline.

Store recordings and raw diagnostics outside Git, for example in
`/Users/macmini/Downloads/Clawdmeter-audio-tests/`. No automatic upload or ingestion.

## Build and test sequence

1. Preserve current firmware artifacts and their exact commit for rollback.
   Inspect the exact-board audio example and driver compatibility, then implement
   capture and the USB receiver in the current application.
2. Build the target firmware. Check the receiver against complete, truncated,
   corrupted, and timed-out input; run existing daemon regression tests.
3. Confirm the USB port and flash normally. Preserve partitions, NVS, and the
   work-Mac bond. Keep the Mac-mini BLE writer stopped and the relay running.
4. Make three separate ten-second recordings: quiet room, normal speech at about
   three feet, and the same phrase at about six feet. Listen to the raw WAVs;
   adjust gain only when the results justify it.
5. Repeat while animations run and touch switches screens. Check repeated memory
   allocation/release and interrupted-transfer recovery. The speaker stays silent.
6. If the experimental firmware is unstable, restore the saved firmware with a
   normal upload. Record observations in the runbook regardless of the outcome.

## Pass criteria and limits

- Valid checksum, expected format, and exactly 160,000 samples. Playback lasts ten
  seconds at normal speed/pitch, without flat or predominantly clipped samples.
- Raw speech is intelligible at three feet. Record six-foot performance separately
  to judge whether room capture is realistic; normalization cannot substitute for
  adequate microphone input quality.
- No reported overruns, unexplained audible gaps, resets, watchdog failures, or
  frozen screen/touch interaction. Repeated captures do not progressively leak
  memory. Capture releases its resources after recording and failure recovery.
- Recording stops automatically; no further capture occurs without another
  explicit command. The work-Mac owner remains stored.

At home we can prove USB capture and display operation without altering pairing.
Live Claude/Codex updates, Apple Music artwork, and Pluribus over BLE during
recording must be checked with the bonded work Mac connected. Report that check
as pending if it is unavailable. This milestone does not establish continuous
recording reliability, Wi-Fi coexistence, or SD storage behavior.

Deliverables: test firmware, Mac capture utility, three WAVs, and a brief results
record covering channel/gain, sample diagnostics, memory, display behavior, and
limitations. Nothing starts recording at boot or merely because USB is connected.

## Hardware results and microphone startup fix

- Initial transport-only recordings: 160,000 samples each, 320,044-byte WAVs,
  matching CRC32; capture 9,999 ms; USB transfer 1.67 seconds each. The room was
  initially assumed quiet (RMS around 27 / 32,768), but a prompted speech take
  had the same noise-only signature. **These initial files do not prove working
  microphones.** Investigation found the older driver left register 0x40 at
  0xC3: PDN_ANA=1 powers down the analog input. Use the current Espressif startup
  sequence (0x43, mic low-power registers 0x08, reset/enable 0x71 then 0x41),
  not the older Waveshare sequence. Shutdown sets 0x40=0xC0; both analog states,
  input gain and shutdown clock/power registers are read-back checked.
- After that fix, `speech-3ft-fixed.wav` responds to the prompted speech:
  peak 1,193, RMS 136.7 versus about 35–45 in its opening quiet windows;
  no clipped samples. Exactly 160,000 samples / 320,044 WAV bytes, CRC verified,
  capture 9,999 ms and USB transfer 1.67 seconds. Raw playback was initiated on
  the Mac. Chris confirmed it sounded okay but was quiet even through AirPods.
  That sample used 30 dB analog gain. The next build raises gain to 37.5 dB
  (+7.5 dB, about 2.37× amplitude for identical input). Retest at the same distance
  and check clipping before judging the gain change; no Mac normalization is used.
- The 37.5 dB firmware was flashed and tested in `speech-3ft-gain375.wav`:
  peak 1,541, RMS 172.34, zero clipped samples, valid CRC, 9,999 ms capture and
  1.67-second USB transfer. The display loop ran 202 times (maximum gap 123 ms),
  with the same memory baseline and no reported overruns. This is a separate
  spoken take, not a calibrated source-level comparison. Playback was initiated
  for Chris; final volume preference is pending.
- No reported I2S overruns or read errors. The normal display loop executed
  324 and 821 iterations during those captures; maximum observed loop gap 104 ms.
  The post-fix speech take had 179 loop iterations and a maximum gap of 123 ms.
  Screenshot capture also succeeded after the earlier repeat/cancel tests.
- Runtime free internal heap before each capture was exactly 92,136 bytes,
  dropping to 82,196 during capture. Free PSRAM before each was 7,300,888 bytes,
  dropping to 6,973,156. The identical next-capture baseline showed resources
  returning after worker deletion; this is a short repeat check, not a soak test.
- A concurrent recording was rejected as busy. Explicit cancellation stopped
  capture, returned an error, released resources, and restored `busy:false`.
- Pausing Python's reads for four seconds during transfer still completed and
  restored `busy:false`; macOS buffered the USB traffic. This did not force a
  physical disconnection or establish recovery from cable removal.
- Both normal and optional audio-test firmware builds pass. Host suite: 184
  passed, 2 existing skips and 2 pre-existing Windows coroutine warnings.
- Pre-audio rollback artifacts are retained locally outside Git in
  `/Users/macmini/Downloads/clawdmeter-pre-audio-GI7xlF/`, source `6f9154d`.
  Test recordings are local only in `~/Downloads/Clawdmeter-audio-tests/`.

## Later processing responsibilities

Chris confirmed speech detection belongs on the Clawdmeter: the device records,
detects speech, identifies boundaries, and selects recordings for delivery. The
Mac should not be required to perform those decisions. Speech detection remains
a later milestone, after raw microphone quality and continuous capture are proven.

Transcription (turning spoken words into text) runs on either the Mac mini or the
office Mac; both are acceptable. The eventual receiver must expose the same
recording/metadata contract whichever Mac transcribes and feed the result into
Pluribus. The first test simply saves a WAV and does not implement transcription.

## References

- [Project runbook](../CLAUDE.md)
- [Waveshare exact-board examples](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16)
- [Waveshare pin reference](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16#pinout-definition)
- [Espressif codec driver](https://components.espressif.com/components/espressif/esp_codec_dev)
