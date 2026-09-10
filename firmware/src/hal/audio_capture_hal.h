#pragma once
#include <stddef.h>
#include <stdint.h>

// Optional, explicitly triggered test input. No initialization at boot.
// A single audio worker owns this interface; no LVGL calls are allowed here.
#ifdef CLAWDMETER_AUDIO_TEST
bool audio_capture_hal_begin(unsigned microphone);
bool audio_capture_hal_read(int16_t* mono, size_t capacity, size_t* samples);
bool audio_capture_hal_end();
uint32_t audio_capture_hal_overruns();
#endif
