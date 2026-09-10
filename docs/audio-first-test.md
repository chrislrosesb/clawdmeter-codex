# First audio test — ten seconds to the Mac over USB

Date: 2026-09-09. Status: plan only; audio capture is not implemented or tested.

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
- Use a supported Espressif ES7210 driver and verified Waveshare configuration.
  Confirm the ADC address, I2S slot layout, and microphone channel against the
  exact-board example before coding. ES8311 remains the speaker path on this board.
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
Send a versioned header containing recording ID, format, sample count, byte
length, and CRC32, followed by that exact number of PCM bytes and an end marker.

Serial already carries logs and screenshots. Serialize all serial writers for
the binary response so unrelated text cannot corrupt it. Transfer from a worker
in bounded chunks, yielding between writes; disconnected or blocked USB must time
out without freezing LVGL or BLE. Free the recording buffer on completion or a
bounded failure timeout.

The Python utility must validate format, bounded length, checksum, and completion
before finalizing the WAV from a temporary file. It opens only the selected USB
port, tolerates boot messages, and does not intentionally reset the board via
DTR/RTS. A serial monitor must not own the port concurrently. Measure actual USB
throughput and allow a generous bounded timeout rather than predicting transfer
speed from the nominal 115200 baud setting.

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
limitations. Writing this plan does not initiate implementation or recording.

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
