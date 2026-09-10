# ES7210 driver provenance

`es7210.cpp`, `es7210.h`, and `audio_hal.h` are from Waveshare's exact-board
Arduino `06_ES7210` example, commit
`225a62bff11b5d0a0b607873860d39485a9a9685`:
https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16/tree/225a62bff11b5d0a0b607873860d39485a9a9685/examples/arduino/06_ES7210

The driver carries Espressif's MIT-style license restricted to Espressif
products, reproduced in the source headers. This target is an Espressif ESP32-S3.

Local changes: compile only for the optional audio test; propagate initialization
and start/stop I2C failures instead of unconditional success; reject failed reads
in read-modify-write; separate the clock register value from the error result.
Wire remains the sole shared I2C owner and provides transaction locking.

Hardware testing exposed a bug in the older example: register 0x40 was left at
0xC3 (PDN_ANA=1), yielding ADC noise but no response to speech. The analog
startup/shutdown register settings now follow current Espressif `esp_codec_dev`
(0x43 while capturing, 0xC0 when stopped, 0x08 mic low-power settings, and the
0x71 → 0x41 enable sequence). Board HAL read-back verifies the analog state.
References:
- https://github.com/espressif/esp-audio-dev/blob/3d22df96b33942791bfcc297e449b7bfd0c9429e/esp_codec_dev/device/es7210/es7210.c
- https://files.waveshare.com/wiki/common/ES7210_DS.pdf (register 0x40, bit 7)

The example uses legacy I2S APIs. Our board HAL uses the pinned Arduino core's
modern IDF standard-I2S RX API instead, with explicit stereo-slot extraction,
bounded reads, and overflow counting. The schematic shows physical microphones
on ADC MIC1/2 and SDOUT1 connected through R38 to GPIO10. ADC MIC3 is the speaker
reference path; the example's high gain on MIC3/4 must NOT be confused with the
ambient microphones. We enable MIC1/2 at 30 dB, 16 kHz, 16-bit standard I2S,
MCLK=256fs on GPIO42. No VAD code is copied for this test.
