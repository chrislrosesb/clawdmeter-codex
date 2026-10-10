#pragma once
#include <stddef.h>
#include <stdint.h>
#include <lvgl.h>

// Pluribus agent animation clips for the idle rotation. The Mac streams each
// clip (length-prefixed baseline JPEG frames, see tools/make_agent_clips.py)
// over the same packet path as album art; clips live in PSRAM until reboot and
// play as a rounded card with the agent's name. Boards without PSRAM or the
// JPEG decoder reject clips and never play any.

#define CLIP_MAGIC 0xA6

// Feed one clip packet (start or chunk). Returns 0 rejected, 1 accepted,
// 2 start for a clip the device already holds (sender can skip it).
int agent_clip_packet(const uint8_t* data, size_t len);

void agent_clips_create(lv_obj_t* parent);
lv_obj_t* agent_clips_root(void);
int agent_clips_ready(void);       // complete clips held
bool agent_clips_start(void);      // next clip in rotation; false if none
bool agent_clips_tick(void);       // advance; false once the clip has finished
void agent_clips_stop(void);
bool agent_clips_playing(void);
