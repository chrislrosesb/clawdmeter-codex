#pragma once
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,
    SCREEN_CODEX,
    SCREEN_NOW_PLAYING,
    SCREEN_AGENTS,          // Pluribus agent messages; interrupts the rotation
    SCREEN_COUNT,
};

void ui_init(void);
void ui_update(const UsageData* data);
void ui_update_now_playing(const NowPlayingData* data);
// Returns true when a message not seen before arrived (caller wakes the panel).
bool ui_update_agents(const AgentMessagesData* data);
// One chat/Room reply ({"rm":...}); queued with check-ins. Same return meaning.
bool ui_add_room_reply(const AgentMessage* reply);
void ui_update_artwork(const BleArtwork* artwork);
void ui_tick_anim(void);
void ui_tick_screen_rotation(void);
void ui_show_screen(screen_t screen);
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);
