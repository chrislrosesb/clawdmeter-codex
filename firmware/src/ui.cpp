#include "ui.h"
#include "splash.h"
#include <lvgl.h>
#include <src/misc/cache/instance/lv_image_cache.h>
#include <time.h>
#include "logo.h"
#include "clawd_still.h"
#include "icons.h"
#include "assets/pluribus_logo.h"
#include "hal/board_caps.h"

// Custom fonts (scaled for 314 PPI, ~1.9x from original 165 PPI)
LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_styrene_12);
LV_FONT_DECLARE(font_mono_32);
LV_FONT_DECLARE(font_mono_18);

// Layout values computed from the active board's geometry. Populated once
// in ui_init() and treated as const for the rest of the program. Adding a
// new display size means extending compute_layout() with another
// breakpoint — never editing the screen-builder functions below.
struct Layout {
    int16_t scr_w, scr_h;
    int16_t margin;
    int16_t title_y;
    int16_t content_y;
    int16_t content_w;

    // Usage screen
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_bar_y;
    int16_t usage_reset_y;
    int16_t bar_h;
    int16_t panel_pad_x, panel_pad_y;
    int16_t pill_pad_x, pill_pad_y;
    const lv_font_t* title_font;     // screen title / clock
    const lv_font_t* pct_font;       // big percentage number
    const lv_font_t* ent_pct_font;   // enterprise spending number
    const lv_font_t* pill_font;      // "Current" / "Weekly" pill
    const lv_font_t* reset_font;     // "Resets in ..." line
    const lv_font_t* pace_font;      // enterprise "Under/On/Over pace" line
    const lv_font_t* anim_font;      // animated status line
    int16_t anim_y;                  // status line offset from bottom
    bool    small_icons;             // 40px logo + 24px battery (vs 80/48) on small screens
    int16_t title_nudge;             // title x-shift balancing the corner logo
    int16_t logo_y;                  // logo top edge
    int16_t batt_y;                  // battery icon top edge
    int16_t batt_w;                  // battery icon width, for position math

    // Pairing hint / idle screen
    int16_t pair_y1, pair_y2, pair_y3;
    int16_t idle_px;                 // sleeping-creature size on the idle screen

    // Bluetooth screen
    int16_t bt_info_panel_h;
    int16_t bt_reset_zone_h;
    const lv_font_t* bt_title_font;
    const lv_font_t* bt_status_font;
    const lv_font_t* bt_device_font;
    const lv_font_t* bt_credit_1_font;
    const lv_font_t* bt_credit_2_font;
};
static Layout L = {};

// Pick layout values from the active board's pixel dimensions. The two
// existing boards happen to land on the two breakpoints below; new ports
// inherit the closer one — visually OK, may need a polish pass for
// pixel-perfect alignment but never blocks the port from booting.
static void compute_layout(const BoardCaps& c) {
    L.scr_w = c.width;
    L.scr_h = c.height;
    L.margin = 20;
    L.title_y = 30;

    // Values shared by the two original breakpoints; the small branch below
    // overrides them wholesale.
    L.bar_h = 24;
    L.panel_pad_x = 16;
    L.panel_pad_y = 12;
    L.pill_pad_x = 18;
    L.pill_pad_y = 6;
    L.title_font   = &font_tiempos_56;
    L.pct_font     = &font_styrene_48;
    L.ent_pct_font = &font_tiempos_56;
    L.pill_font    = &font_styrene_28;
    L.reset_font   = &font_styrene_28;
    L.pace_font    = &font_styrene_16;
    L.anim_font    = &font_mono_32;
    L.anim_y = -15;
    L.small_icons = false;
    L.title_nudge = 16;
    L.logo_y = L.title_y - 10;
    L.batt_y = L.title_y;
    L.batt_w = ICON_BATTERY_W;
    L.pair_y1 = 40;
    L.pair_y2 = 120;
    L.pair_y3 = 160;
    L.idle_px = 160;

    if (c.height >= 460) {
        // Large layout — tuned for 480x480 (AMOLED-2.16).
        L.content_y = 100;
        L.usage_panel_h = 150;
        L.usage_panel_gap = 16;
        L.usage_bar_y = 56;
        L.usage_reset_y = 94;
        L.bt_info_panel_h = 160;
        L.bt_reset_zone_h = 110;
        L.bt_title_font    = &font_tiempos_56;
        L.bt_status_font   = &font_styrene_48;
        L.bt_device_font   = &font_styrene_28;
        L.bt_credit_1_font = &font_styrene_24;
        L.bt_credit_2_font = &font_styrene_20;
    } else if (c.height >= 300) {
        // Compact layout — tuned for 368x448 (AMOLED-1.8).
        L.content_y = 85;
        L.usage_panel_h = 130;
        L.usage_panel_gap = 12;
        L.usage_bar_y = 48;
        L.usage_reset_y = 78;
        L.bt_info_panel_h = 140;
        L.bt_reset_zone_h = 90;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_20;
        L.bt_credit_1_font = &font_styrene_16;
        L.bt_credit_2_font = &font_styrene_14;
    } else {
        // Small layout — tuned for 240x240 (LCD-1.54 and similar square TFTs).
        // Everything shrinks: fonts two steps down, panels ~half height, and
        // the corner logo/battery switch to the 40px/24px small assets.
        L.margin = 8;
        L.title_y = 4;
        L.content_y = 44;
        L.usage_panel_h = 74;
        L.usage_panel_gap = 6;
        L.usage_bar_y = 30;
        L.usage_reset_y = 46;
        L.bar_h = 12;
        L.panel_pad_x = 10;
        L.panel_pad_y = 6;
        L.pill_pad_x = 8;
        L.pill_pad_y = 2;
        L.title_font   = &font_tiempos_34;
        L.pct_font     = &font_styrene_24;
        L.ent_pct_font = &font_tiempos_34;
        L.pill_font    = &font_styrene_14;
        L.reset_font   = &font_styrene_14;
        L.pace_font    = &font_styrene_12;
        L.anim_font    = &font_mono_18;
        // Center the status line in the strip below the weekly panel; flush
        // against the bottom edge it reads as unevenly spaced.
        L.anim_y = -10;
        L.small_icons = true;
        L.title_nudge = 8;
        L.logo_y = 2;
        L.batt_y = 10;
        L.batt_w = ICON_BATTERY_SMALL_W;
        L.pair_y1 = 12;
        L.pair_y2 = 56;
        L.pair_y3 = 80;
        L.idle_px = 96;
        L.bt_info_panel_h = 90;
        L.bt_reset_zone_h = 60;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_20;
        L.bt_device_font   = &font_styrene_14;
        L.bt_credit_1_font = &font_styrene_12;
        L.bt_credit_2_font = &font_styrene_12;
    }

    L.content_w = L.scr_w - 2 * L.margin;
}

// Anthropic brand palette — design tokens live in theme.h
#include "theme.h"
#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_ACCENT    THEME_ACCENT
#define COL_GREEN     THEME_GREEN
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_BAR_BG    THEME_BAR_BG

// ---- Usage screen widgets (single non-splash view) ----
static lv_obj_t* usage_container;
static lv_obj_t* lbl_title;
// Clock fed by the daemon: base epoch (local wall-clock seconds) + the lv_tick at
// which it landed, so the title ticks forward locally between 60s payloads.
static long     clock_base_epoch = 0;
static uint32_t clock_base_ms = 0;
static int      clock_fmt = 24;   // 12 or 24, set from the daemon payload
static int      clock_last_min = -1;   // last rendered minute; avoids redrawing the title every tick
static lv_obj_t* usage_group;   // the two usage panels — shown when connected
static lv_obj_t* pair_group;    // pairing hint — shown when disconnected
static lv_obj_t* bar_session;
static lv_obj_t* lbl_session_pct;
static lv_obj_t* lbl_session_label;
static lv_obj_t* lbl_session_reset;
static lv_obj_t* bar_weekly;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* lbl_weekly_label;
static lv_obj_t* lbl_weekly_reset;
static lv_obj_t* panel_session = nullptr;
static lv_obj_t* panel_weekly = nullptr;
// Enterprise-only widgets inside panel_session
static lv_obj_t* lbl_session_pct_sym = nullptr;  // "%" in smaller font
static lv_obj_t* lbl_spending_desc = nullptr;     // "of your monthly budget"
static lv_obj_t* lbl_spending_status = nullptr;   // "Under pace" / "On pace" / "Over pace"
static lv_obj_t* lbl_anim;      // status line: connection state + whimsical idle

// ---- Codex screen widgets (optional third view, fed by the payload's "x") ----
static lv_obj_t* codex_container = nullptr;
static lv_obj_t* panel_codex1;
static lv_obj_t* bar_codex1;
static lv_obj_t* lbl_codex1_pct;
static lv_obj_t* lbl_codex1_label;
static lv_obj_t* lbl_codex1_reset;
static lv_obj_t* panel_codex2;
static lv_obj_t* bar_codex2;
static lv_obj_t* lbl_codex2_pct;
static lv_obj_t* lbl_codex2_label;
static lv_obj_t* lbl_codex2_reset;
static lv_obj_t* lbl_codex_anim;           // animated status line (white variant)
// Daily panel — today's tokens vs the 7-day average (Codex has no daily rate
// limit, see ui_update). Occupies the second panel slot on one-window plans.
static lv_obj_t* panel_codex_daily;
static lv_obj_t* bar_codex_daily;
static lv_obj_t* lbl_codex_daily_val;
static lv_obj_t* lbl_codex_daily_label;
static lv_obj_t* lbl_codex_daily_sub;
static lv_obj_t* openai_img;               // corner mark shown on the Codex screen
static lv_image_dsc_t openai_dsc;
static bool      codex_data_seen = false;  // last live payload carried Codex data

// ---- Apple Music now-playing screen (optional; Mac daemon only) ----
static lv_obj_t* music_container = nullptr;
static lv_obj_t* music_art = nullptr;
static lv_obj_t* music_art_placeholder = nullptr;
static lv_obj_t* lbl_music_title = nullptr;
static lv_obj_t* lbl_music_artist = nullptr;
static lv_obj_t* bar_music = nullptr;
static lv_obj_t* lbl_music_elapsed = nullptr;
static lv_obj_t* lbl_music_remaining = nullptr;
static lv_image_dsc_t music_art_dsc = {};
static bool music_screen_supported = false;
static bool music_playing = false;
static uint32_t music_last_update_ms = 0;
static uint32_t music_progress_base_ms = 0;
static int music_elapsed_base = 0;
static int music_duration = 0;
static uint16_t music_art_generation = 0;
static uint16_t music_expected_generation = 0;
static const uint32_t MUSIC_FRESH_MS = 30000;

// ---- Pluribus recent-activity screen (local server, optional) ----
static lv_obj_t* pluribus_container = nullptr;
static lv_obj_t* pluribus_mark = nullptr;
static lv_obj_t* lbl_pluribus_title = nullptr;
static lv_obj_t* lbl_pluribus_detail = nullptr;
static lv_obj_t* lbl_pluribus_status = nullptr;
static lv_obj_t* lbl_pluribus_footer = nullptr;
static lv_image_dsc_t pluribus_logo_dsc = {};
static bool pluribus_screen_supported = false;
static bool pluribus_present = false;
static int64_t pluribus_id = -1;
static uint32_t pluribus_received_ms = 0;
static uint32_t pluribus_received_age = 0;
static uint32_t pluribus_last_rendered_age = UINT32_MAX;
static char pluribus_kind[21] = {};
static const uint32_t PLURIBUS_EXPIRY_MINUTES = 24 * 60;

// ---- Battery indicator (shared, on top) ----
static lv_obj_t* battery_img;
static lv_obj_t* logo_img;
static bool      battery_present = false;
static lv_image_dsc_t battery_dscs[5];  // empty, low, medium, full, charging

// ---- Live-data freshness → which usage sub-view to show ----
// usage panels when data is flowing, an idle "Zzz" screen when the host is
// connected but no usage update landed within DATA_FRESH_MS, the pairing hint
// when BLE is down. Re-evaluated every loop in ui_tick_anim().
static lv_obj_t* idle_group;            // the "Zzz" idle screen
static uint32_t  last_data_ms = 0;      // lv_tick when the last valid usage update landed
static bool      data_received = false; // any valid update since boot
static bool      data_ok = true;        // last payload's ok flag; a {"ok":false} beat = "no fresh data"
static int       view_state = -1;       // -1 unknown / 0 pair / 1 idle / 2 usage
static const uint32_t DATA_FRESH_MS = 90000;  // usage counts as "live" within this window (daemon sends ~60s)

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static screen_t current_screen = SCREEN_USAGE;
static uint32_t last_screen_change_ms = 0;
static const uint32_t SCREEN_ROTATE_MS = 30000;
static bool     s_ble_connected = false;   // cached BLE connection state
static uint32_t connected_at_ms = 0;       // when we last entered CONNECTED ("Connected" dwell)

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS     4000

static const char* const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))  // 10: ping-pong 0..5..0

static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

static const char* const anim_messages[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Actualizing", "Envisioning", "Pondering",
    "Baking", "Finagling", "Pontificating",
    "Booping", "Flibbertigibbeting", "Processing",
    "Brewing", "Forging", "Puttering",
    "Calculating", "Forming", "Puzzling",
    "Cerebrating", "Frolicking", "Reticulating",
    "Channelling", "Generating", "Ruminating",
    "Churning", "Germinating", "Scheming",
    "Clauding", "Hatching", "Schlepping",
    "Coalescing", "Herding", "Shimmying",
    "Cogitating", "Honking", "Shucking",
    "Combobulating", "Hustling", "Simmering",
    "Computing", "Ideating", "Smooshing",
    "Concocting", "Imagining", "Spelunking",
    "Conjuring", "Incubating", "Spinning",
    "Considering", "Inferring", "Stewing",
    "Contemplating", "Jiving", "Sussing",
    "Cooking", "Manifesting", "Synthesizing",
    "Crafting", "Marinating", "Thinking",
    "Creating", "Meandering", "Tinkering",
    "Crunching", "Moseying", "Transmuting",
    "Deciphering", "Mulling", "Unfurling",
    "Deliberating", "Mustering", "Unravelling",
    "Determining", "Musing", "Vibing",
    "Discombobulating", "Noodling", "Wandering",
    "Divining", "Percolating", "Whirring",
    "Doing", "Wibbling",
    "Effecting", "Wizarding",
    "Working", "Wrangling",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "Resets in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

// Codex rate-limit windows are self-describing (length in minutes); label the
// pill from the length so plans with only a weekly window read correctly.
static const char* window_pill_text(int window_mins) {
    if (window_mins >= 8000) return "Weekly";               // 10080 = 7 days
    if (window_mins > 0 && window_mins <= 360) return "Current";  // 300 = 5h
    return "Window";
}

// Compact human token count: 999, 12.5K, 3.2M, 1.1B.
static void format_tokens(long v, char* buf, size_t len) {
    if (v >= 1000000000L)    snprintf(buf, len, "%.1fB", v / 1e9);
    else if (v >= 1000000L)  snprintf(buf, len, "%.1fM", v / 1e6);
    else if (v >= 1000L)     snprintf(buf, len, "%.1fK", v / 1e3);
    else                     snprintf(buf, len, "%ld", v);
}

// Forward decls — callbacks defined near ui_show_screen below
static void global_click_cb(lv_event_t* e);

static void format_track_time(int seconds, char* buf, size_t len) {
    if (seconds < 0) seconds = 0;
    snprintf(buf, len, "%d:%02d", seconds / 60, seconds % 60);
}

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, L.panel_pad_x, 0);
    lv_obj_set_style_pad_right(panel, L.panel_pad_x, 0);
    lv_obj_set_style_pad_top(panel, L.panel_pad_y, 0);
    lv_obj_set_style_pad_bottom(panel, L.panel_pad_y, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    return bar;
}

// Small clock-dot overlaid on a usage bar. Its position is the fraction of
// the rate-limit window that has elapsed, so the fill to its left/right shows
// at a glance whether consumption is running ahead of or behind even pace.
static lv_obj_t* make_clock_marker(lv_obj_t* parent) {
    const int size = L.small_icons ? 16 : 28;
    const int hand = L.small_icons ? 2 : 3;
    const int inset = L.small_icons ? 3 : 5;
    const int center = size / 2;

    lv_obj_t* marker = lv_obj_create(parent);
    lv_obj_set_size(marker, size, size);
    lv_obj_set_style_radius(marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(marker, COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(marker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(marker, COL_BG, 0);
    lv_obj_set_style_border_width(marker, L.small_icons ? 2 : 3, 0);
    lv_obj_set_style_pad_all(marker, 0, 0);
    lv_obj_clear_flag(marker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(marker, LV_OBJ_FLAG_CLICKABLE);

    // Hands point to 9 and 12, matching the compact clock symbol in the
    // reference UI without requiring another bitmap or font glyph.
    lv_obj_t* minute_hand = lv_obj_create(marker);
    lv_obj_set_size(minute_hand, hand, center - inset + 1);
    lv_obj_set_pos(minute_hand, center - hand / 2, inset);
    lv_obj_set_style_bg_color(minute_hand, COL_BG, 0);
    lv_obj_set_style_bg_opa(minute_hand, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(minute_hand, 0, 0);
    lv_obj_set_style_radius(minute_hand, hand, 0);
    lv_obj_set_style_pad_all(minute_hand, 0, 0);
    lv_obj_clear_flag(minute_hand, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(minute_hand, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* hour_hand = lv_obj_create(marker);
    lv_obj_set_size(hour_hand, center - inset + 1, hand);
    lv_obj_set_pos(hour_hand, inset, center - hand / 2);
    lv_obj_set_style_bg_color(hour_hand, COL_BG, 0);
    lv_obj_set_style_bg_opa(hour_hand, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hour_hand, 0, 0);
    lv_obj_set_style_radius(hour_hand, hand, 0);
    lv_obj_set_style_pad_all(hour_hand, 0, 0);
    lv_obj_clear_flag(hour_hand, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(hour_hand, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
    return marker;
}

static void update_clock_marker(lv_obj_t* bar, int reset_mins, int window_mins) {
    lv_obj_t* marker = bar ? (lv_obj_t*)lv_obj_get_user_data(bar) : nullptr;
    if (!marker) return;
    if (reset_mins < 0 || window_mins <= 0) {
        lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    int elapsed_mins = window_mins - reset_mins;
    if (elapsed_mins < 0) elapsed_mins = 0;
    if (elapsed_mins > window_mins) elapsed_mins = window_mins;

    const int bar_w = L.content_w - 2 * L.panel_pad_x;
    const int size = L.small_icons ? 16 : 28;
    int center_x = (int)(((long)bar_w * elapsed_mins + window_mins / 2) / window_mins);
    int x = center_x - size / 2;
    if (x < 0) x = 0;
    if (x > bar_w - size) x = bar_w - size;
    lv_obj_set_pos(marker, x, L.usage_bar_y + (L.bar_h - size) / 2);
    lv_obj_clear_flag(marker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(marker);
}

static void init_icon_dsc_rgb565a8(lv_image_dsc_t* dsc, int w, int h, const uint8_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = data;
    dsc->data_size = w * h * 3;
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, L.pill_font, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, L.pill_pad_x, 0);
    lv_obj_set_style_pad_right(lbl, L.pill_pad_x, 0);
    lv_obj_set_style_pad_top(lbl, L.pill_pad_y, 0);
    lv_obj_set_style_pad_bottom(lbl, L.pill_pad_y, 0);
    return lbl;
}

static void init_battery_icons(void) {
    if (L.small_icons) {
        init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_SMALL_W, ICON_BATTERY_SMALL_H, icon_battery_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_SMALL_W, ICON_BATTERY_LOW_SMALL_H, icon_battery_low_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_SMALL_W, ICON_BATTERY_MEDIUM_SMALL_H, icon_battery_medium_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_SMALL_W, ICON_BATTERY_FULL_SMALL_H, icon_battery_full_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_SMALL_W, ICON_BATTERY_CHARGING_SMALL_H, icon_battery_charging_small_data);
        return;
    }
    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
}

// ======== Usage Screen ========

static lv_obj_t* make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                                  lv_obj_t** out_pct, lv_obj_t** out_pill,
                                  lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, L.usage_panel_h);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, L.pct_font, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, L.usage_bar_y,
                        L.content_w - 2 * L.panel_pad_x, L.bar_h);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, L.reset_font, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, L.usage_reset_y);

    return panel;
}

// Pairing hint — shown when disconnected so the screen isn't empty and the
// user knows how to (re)pair. Wording matches the 3-second release gesture.
static void build_pair_group(lv_obj_t* parent) {
    pair_group = lv_obj_create(parent);
    lv_obj_set_size(pair_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(pair_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(pair_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pair_group, 0, 0);
    lv_obj_set_style_pad_all(pair_group, 0, 0);
    lv_obj_clear_flag(pair_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* l1 = lv_label_create(pair_group);
    lv_label_set_text(l1, "To pair");
    lv_obj_set_style_text_font(l1, L.bt_status_font, 0);
    lv_obj_set_style_text_color(l1, COL_TEXT, 0);
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, L.pair_y1);

    lv_obj_t* l2 = lv_label_create(pair_group);
    lv_label_set_text(l2, "hold the power button");
    lv_obj_set_style_text_font(l2, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l2, COL_DIM, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, L.pair_y2);

    lv_obj_t* l3 = lv_label_create(pair_group);
    lv_label_set_text(l3, "for 3 seconds, then release");
    lv_obj_set_style_text_font(l3, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l3, COL_DIM, 0);
    lv_obj_align(l3, LV_ALIGN_TOP_MID, 0, L.pair_y3);

    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);  // ui_update_ble_status decides
}

// Idle "Zzz" screen — shown when the host is connected but no usage update has
// landed recently (token expired, daemon down, host asleep…). Full-screen, like
// the pairing hint, so we never render hours-old numbers as if they were live.
static void build_idle_group(lv_obj_t* parent) {
    idle_group = lv_obj_create(parent);
    lv_obj_set_size(idle_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(idle_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(idle_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(idle_group, 0, 0);
    lv_obj_set_style_pad_all(idle_group, 0, 0);
    lv_obj_clear_flag(idle_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    // A shrunk-down resting creature (the official cloud-ride animation)
    // sits between the header and the status line; the animated "Listening…"
    // status line carries the words, so no extra text is needed here.
    lv_obj_t* creature = splash_mini_create(idle_group, "cloud", L.idle_px);
    if (creature) lv_obj_align(creature, LV_ALIGN_CENTER, 0, -20);

    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);  // update_view_state decides
}

static void init_usage_screen(lv_obj_t* scr) {
    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(usage_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lbl_title = lv_label_create(usage_container);
    lv_label_set_text(lbl_title, "Usage");
    lv_obj_set_style_text_font(lbl_title, L.title_font, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    // The nudge balances the corner logo on the left; smaller on small
    // screens where the logo is 40px and the battery icon sits closer.
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, L.title_nudge, L.title_y);

    // Usage panels (shown when connected) live in a transparent full-size group
    // so they can be toggled against the pairing hint as one unit.
    usage_group = lv_obj_create(usage_container);
    lv_obj_set_size(usage_group, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_group, 0, 0);
    lv_obj_set_style_bg_opa(usage_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_group, 0, 0);
    lv_obj_set_style_pad_all(usage_group, 0, 0);
    lv_obj_clear_flag(usage_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    panel_session = make_usage_panel(usage_group, L.content_y, "Current",
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);
    lv_obj_set_user_data(bar_session, make_clock_marker(panel_session));

    // Enterprise-only overlays inside panel_session — hidden until enterprise data arrives
    lbl_session_pct_sym = lv_label_create(panel_session);
    lv_label_set_text(lbl_session_pct_sym, "%");
    lv_obj_set_style_text_font(lbl_session_pct_sym, L.reset_font, 0);
    lv_obj_set_style_text_color(lbl_session_pct_sym, COL_TEXT, 0);
    lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_desc = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_desc, "of your monthly budget");
    lv_obj_set_style_text_font(lbl_spending_desc, L.reset_font, 0);
    lv_obj_set_style_text_color(lbl_spending_desc, COL_DIM, 0);
    lv_obj_set_pos(lbl_spending_desc, 0, L.usage_reset_y);
    lv_obj_add_flag(lbl_spending_desc, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_status = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_status, "");
    lv_obj_set_style_text_font(lbl_spending_status, L.pace_font, 0);
    lv_obj_set_pos(lbl_spending_status, 0, L.usage_reset_y + 20);
    lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);

    panel_weekly = make_usage_panel(usage_group,
                     L.content_y + L.usage_panel_h + L.usage_panel_gap, "Weekly",
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);
    lv_obj_set_user_data(bar_weekly, make_clock_marker(panel_weekly));
    // Recolor enabled so enterprise period box can color pace and reset separately
    lv_label_set_recolor(lbl_weekly_reset, true);

    build_pair_group(usage_container);
    build_idle_group(usage_container);

    // Status line — always visible on the usage view. Driven by ui_tick_anim().
    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, L.anim_font, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, L.anim_y);
}

// ======== Codex Screen ========
// Same skeleton as the usage view: title + up to two window panels + a token
// summary line. Only reachable (see global_click_cb) while live payloads carry
// Codex data, so it never shows placeholder or stale content.

static void init_codex_screen(lv_obj_t* scr) {
    codex_container = lv_obj_create(scr);
    lv_obj_set_size(codex_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(codex_container, 0, 0);
    lv_obj_set_style_bg_opa(codex_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(codex_container, 0, 0);
    lv_obj_set_style_pad_all(codex_container, 0, 0);
    lv_obj_clear_flag(codex_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(codex_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* title = lv_label_create(codex_container);
    lv_label_set_text(title, "Codex");
    lv_obj_set_style_text_font(title, L.title_font, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    // Screen-centered (no title_nudge): the panels below are the dominant
    // shapes and an offset title reads as misaligned against them.
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, L.title_y);

    panel_codex1 = make_usage_panel(codex_container, L.content_y, "Current",
                     &lbl_codex1_pct, &lbl_codex1_label,
                     &bar_codex1, &lbl_codex1_reset);
    lv_obj_set_user_data(bar_codex1, make_clock_marker(panel_codex1));

    // The second slot holds either the plan's secondary rate window or the
    // Daily panel (one-window plans) — ui_update picks which is visible.
    const int slot2_y = L.content_y + L.usage_panel_h + L.usage_panel_gap;
    panel_codex2 = make_usage_panel(codex_container, slot2_y, "Weekly",
                     &lbl_codex2_pct, &lbl_codex2_label,
                     &bar_codex2, &lbl_codex2_reset);
    lv_obj_set_user_data(bar_codex2, make_clock_marker(panel_codex2));

    panel_codex_daily = make_usage_panel(codex_container, slot2_y, "Daily",
                     &lbl_codex_daily_val, &lbl_codex_daily_label,
                     &bar_codex_daily, &lbl_codex_daily_sub);

    // Animated status line, same as the usage view but in white so the accent
    // color stays Claude's. Driven by ui_tick_anim().
    lbl_codex_anim = lv_label_create(codex_container);
    lv_label_set_text(lbl_codex_anim, "");
    lv_obj_set_style_text_font(lbl_codex_anim, L.anim_font, 0);
    lv_obj_set_style_text_color(lbl_codex_anim, COL_TEXT, 0);
    lv_obj_align(lbl_codex_anim, LV_ALIGN_BOTTOM_MID, 0, L.anim_y);

    lv_obj_add_flag(codex_container, LV_OBJ_FLAG_HIDDEN);  // ui_show_screen decides
}

static void init_music_screen(lv_obj_t* scr) {
#ifdef BOARD_HAS_PSRAM
    music_screen_supported = L.scr_w >= 400 && L.scr_h >= 450;
#else
    music_screen_supported = false;
#endif
    if (!music_screen_supported) return;

    music_container = lv_obj_create(scr);
    lv_obj_set_size(music_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(music_container, 0, 0);
    lv_obj_set_style_bg_color(music_container, COL_BG, 0);
    lv_obj_set_style_bg_opa(music_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(music_container, 0, 0);
    lv_obj_set_style_pad_all(music_container, 0, 0);
    lv_obj_clear_flag(music_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(music_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* header = lv_label_create(music_container);
    lv_label_set_text(header, "NOW PLAYING");
    lv_obj_set_style_text_font(header, &font_styrene_16, 0);
    lv_obj_set_style_text_color(header, COL_DIM, 0);
    lv_obj_set_style_text_letter_space(header, 3, 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 17);

    music_art_placeholder = lv_obj_create(music_container);
    lv_obj_set_size(music_art_placeholder, 300, 300);
    lv_obj_set_pos(music_art_placeholder, (L.scr_w - 300) / 2, 50);
    lv_obj_set_style_bg_color(music_art_placeholder, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(music_art_placeholder, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(music_art_placeholder, 0, 0);
    lv_obj_set_style_radius(music_art_placeholder, 20, 0);
    lv_obj_clear_flag(music_art_placeholder, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(music_art_placeholder, LV_OBJ_FLAG_EVENT_BUBBLE);

    music_art = lv_image_create(music_container);
    lv_obj_set_pos(music_art, (L.scr_w - 300) / 2, 50);
    lv_obj_add_flag(music_art, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(music_art, LV_OBJ_FLAG_EVENT_BUBBLE);

    lbl_music_title = lv_label_create(music_container);
    lv_obj_set_width(lbl_music_title, L.scr_w - 56);
    lv_label_set_long_mode(lbl_music_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(lbl_music_title, "");
    lv_obj_set_style_text_font(lbl_music_title, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_music_title, COL_TEXT, 0);
    lv_obj_set_style_text_align(lbl_music_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(lbl_music_title, 28, 360);

    lbl_music_artist = lv_label_create(music_container);
    lv_obj_set_width(lbl_music_artist, L.scr_w - 56);
    lv_label_set_long_mode(lbl_music_artist, LV_LABEL_LONG_DOT);
    lv_label_set_text(lbl_music_artist, "");
    lv_obj_set_style_text_font(lbl_music_artist, &font_styrene_20, 0);
    lv_obj_set_style_text_color(lbl_music_artist, COL_DIM, 0);
    lv_obj_set_style_text_align(lbl_music_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(lbl_music_artist, 28, 397);

    bar_music = lv_bar_create(music_container);
    lv_obj_set_size(bar_music, L.scr_w - 64, 5);
    lv_obj_set_pos(bar_music, 32, 434);
    lv_bar_set_range(bar_music, 0, 1000);
    lv_obj_set_style_bg_color(bar_music, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_music, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_music, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_music, 3, LV_PART_INDICATOR);

    lbl_music_elapsed = lv_label_create(music_container);
    lbl_music_remaining = lv_label_create(music_container);
    lv_obj_set_style_text_font(lbl_music_elapsed, &font_styrene_14, 0);
    lv_obj_set_style_text_color(lbl_music_elapsed, COL_DIM, 0);
    lv_obj_set_style_text_font(lbl_music_remaining, &font_styrene_14, 0);
    lv_obj_set_style_text_color(lbl_music_remaining, COL_DIM, 0);
    lv_obj_set_pos(lbl_music_elapsed, 32, 446);
    lv_obj_align(lbl_music_remaining, LV_ALIGN_TOP_RIGHT, -32, 446);
    lv_label_set_text(lbl_music_elapsed, "0:00");
    lv_label_set_text(lbl_music_remaining, "-0:00");

    lv_obj_add_flag(music_container, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t* make_accent_rule(lv_obj_t* parent, int x, lv_color_t color) {
    lv_obj_t* rule = lv_obj_create(parent);
    lv_obj_set_size(rule, 42, 4);
    lv_obj_set_pos(rule, x, 82);
    lv_obj_set_style_bg_color(rule, color, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rule, 0, 0);
    lv_obj_set_style_radius(rule, 2, 0);
    lv_obj_set_style_pad_all(rule, 0, 0);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(rule, LV_OBJ_FLAG_EVENT_BUBBLE);
    return rule;
}

static void init_pluribus_screen(lv_obj_t* scr) {
    pluribus_screen_supported = L.scr_w >= 400 && L.scr_h >= 450;
    if (!pluribus_screen_supported) return;

    pluribus_container = lv_obj_create(scr);
    lv_obj_set_size(pluribus_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(pluribus_container, 0, 0);
    lv_obj_set_style_bg_color(pluribus_container, COL_BG, 0);
    lv_obj_set_style_bg_opa(pluribus_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pluribus_container, 0, 0);
    lv_obj_set_style_pad_all(pluribus_container, 0, 0);
    lv_obj_clear_flag(pluribus_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(pluribus_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    init_icon_dsc_rgb565a8(&pluribus_logo_dsc, PLURIBUS_LOGO_W,
                           PLURIBUS_LOGO_H, pluribus_logo_data);
    pluribus_mark = lv_image_create(pluribus_container);
    lv_image_set_src(pluribus_mark, &pluribus_logo_dsc);
    // Deliberately crop the mark at the top/right edges like Pluribus share cards.
    lv_obj_set_pos(pluribus_mark, L.scr_w - 174, -52);
    lv_obj_add_flag(pluribus_mark, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* eyebrow = lv_label_create(pluribus_container);
    lv_label_set_text(eyebrow, "PLURIBUS  \xC2\xB7  RECENT ACTIVITY");
    lv_obj_set_style_text_font(eyebrow, &font_mono_18, 0);
    lv_obj_set_style_text_color(eyebrow, lv_color_hex(0x7faee8), 0);
    lv_obj_set_pos(eyebrow, 28, 38);
    lv_obj_add_flag(eyebrow, LV_OBJ_FLAG_EVENT_BUBBLE);

    make_accent_rule(pluribus_container, 28, lv_color_hex(0xe84f9c));
    make_accent_rule(pluribus_container, 72, lv_color_hex(0x8d5de2));
    make_accent_rule(pluribus_container, 116, lv_color_hex(0x41b1e8));

    lbl_pluribus_title = lv_label_create(pluribus_container);
    lv_obj_set_size(lbl_pluribus_title, L.scr_w - 56, 170);
    lv_label_set_long_mode(lbl_pluribus_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lbl_pluribus_title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(lbl_pluribus_title, lv_color_hex(0xf3eadb), 0);
    lv_obj_set_style_text_line_space(lbl_pluribus_title, -4, 0);
    lv_obj_set_pos(lbl_pluribus_title, 28, 116);
    lv_obj_add_flag(lbl_pluribus_title, LV_OBJ_FLAG_EVENT_BUBBLE);

    lbl_pluribus_detail = lv_label_create(pluribus_container);
    lv_obj_set_size(lbl_pluribus_detail, L.scr_w - 76, 62);
    lv_label_set_long_mode(lbl_pluribus_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lbl_pluribus_detail, &font_styrene_20, 0);
    lv_obj_set_style_text_color(lbl_pluribus_detail, lv_color_hex(0x79c8e8), 0);
    lv_obj_set_pos(lbl_pluribus_detail, 30, 302);
    lv_obj_add_flag(lbl_pluribus_detail, LV_OBJ_FLAG_EVENT_BUBBLE);

    lbl_pluribus_status = lv_label_create(pluribus_container);
    lv_obj_set_style_text_font(lbl_pluribus_status, &font_styrene_16, 0);
    lv_obj_set_style_text_letter_space(lbl_pluribus_status, 2, 0);
    lv_obj_set_style_bg_opa(lbl_pluribus_status, LV_OPA_30, 0);
    lv_obj_set_style_radius(lbl_pluribus_status, 14, 0);
    lv_obj_set_style_pad_left(lbl_pluribus_status, 14, 0);
    lv_obj_set_style_pad_right(lbl_pluribus_status, 14, 0);
    lv_obj_set_style_pad_top(lbl_pluribus_status, 7, 0);
    lv_obj_set_style_pad_bottom(lbl_pluribus_status, 7, 0);
    lv_obj_set_pos(lbl_pluribus_status, 28, 382);
    lv_obj_add_flag(lbl_pluribus_status, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* footer_rule = lv_obj_create(pluribus_container);
    lv_obj_set_size(footer_rule, 116, 2);
    lv_obj_set_pos(footer_rule, 28, 438);
    lv_obj_set_style_bg_color(footer_rule, lv_color_hex(0x34415c), 0);
    lv_obj_set_style_bg_opa(footer_rule, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(footer_rule, 0, 0);
    lv_obj_set_style_pad_all(footer_rule, 0, 0);
    lv_obj_clear_flag(footer_rule, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(footer_rule, LV_OBJ_FLAG_EVENT_BUBBLE);

    lbl_pluribus_footer = lv_label_create(pluribus_container);
    lv_obj_set_width(lbl_pluribus_footer, L.scr_w - 56);
    lv_obj_set_style_text_font(lbl_pluribus_footer, &font_mono_18, 0);
    lv_obj_set_style_text_color(lbl_pluribus_footer, lv_color_hex(0x777d8b), 0);
    lv_obj_set_style_text_letter_space(lbl_pluribus_footer, 1, 0);
    lv_obj_set_pos(lbl_pluribus_footer, 28, 448);
    lv_obj_add_flag(lbl_pluribus_footer, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_add_flag(pluribus_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Public API ========

void ui_init(void) {
    compute_layout(board_caps());

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

#ifndef BOARD_HAS_PSRAM
    // Static corner mascot (see clawd_still.h) — the animated one needs PSRAM.
    if (L.small_icons) init_icon_dsc_rgb565a8(&logo_dsc, CLAWD_STILL_SMALL_W, CLAWD_STILL_SMALL_H, clawd_still_small_data);
    else               init_icon_dsc_rgb565a8(&logo_dsc, CLAWD_STILL_W, CLAWD_STILL_H, clawd_still_data);
#endif
    init_battery_icons();

    init_usage_screen(scr);
    init_codex_screen(scr);
    init_music_screen(scr);
    init_pluribus_screen(scr);
    splash_init(scr);

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    // Corner mascot in the old logo slot. The still Clawd is shorter than the
    // 80/40 px slot the spark logo used; center it vertically in that slot.
    {
        const int slot  = L.small_icons ? LOGO_SMALL_HEIGHT : LOGO_HEIGHT;
        const int art_h = L.small_icons ? CLAWD_STILL_SMALL_H : CLAWD_STILL_H;
        const int top   = L.logo_y + (slot - art_h) / 2;
#ifdef BOARD_HAS_PSRAM
        // Animated: idles, does acts, and takes walk-off/lurk trips.
        splash_mascot_create(scr, L.margin, top + art_h, L.small_icons ? 2 : 3);
#else
        logo_img = lv_image_create(scr);
        lv_image_set_src(logo_img, &logo_dsc);
        lv_obj_set_pos(logo_img, L.margin, top);
#endif
    }

    // OpenAI mark — swapped into the corner-logo slot while the Codex screen
    // is showing (the Clawd mascot stays on every other non-splash screen).
    {
        const int slot = L.small_icons ? LOGO_SMALL_HEIGHT : LOGO_HEIGHT;
        const int ow = L.small_icons ? ICON_OPENAI_SMALL_W : ICON_OPENAI_W;
        const int oh = L.small_icons ? ICON_OPENAI_SMALL_H : ICON_OPENAI_H;
        init_icon_dsc_rgb565a8(&openai_dsc, ow, oh,
            L.small_icons ? icon_openai_small_data : icon_openai_data);
        openai_img = lv_image_create(scr);
        lv_image_set_src(openai_img, &openai_dsc);
        // The mark is smaller than the logo slot — center it in the slot.
        lv_obj_set_pos(openai_img, L.margin + (slot - ow) / 2,
                       L.logo_y + (slot - oh) / 2);
        lv_obj_add_flag(openai_img, LV_OBJ_FLAG_HIDDEN);
    }

    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_obj_set_pos(battery_img, L.scr_w - L.batt_w - L.margin, L.batt_y);
    // Boards without battery telemetry never show the indicator (per the HAL
    // contract; previously every board drew the empty-battery glyph).
    if (!board_caps().has_battery) {
        lv_obj_del(battery_img);
        battery_img = nullptr;
    }
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;
    data_ok = data->ok;
    if (!data->ok) return;          // a {"ok":false} "no data" beat → fall through to idle, keep last numbers
    last_data_ms = lv_tick_get();   // a real usage update just landed
    data_received = true;

    if (data->clock_epoch > 0) {    // daemon supplied wall-clock time → drive the title clock
        clock_base_epoch = data->clock_epoch;
        clock_base_ms = last_data_ms;
        clock_fmt = data->clock_fmt;
    } else if (clock_base_epoch != 0) {   // clock turned off daemon-side → revert title to "Usage"
        clock_base_epoch = 0;
        clock_last_min = -1;
        lv_label_set_text(lbl_title, "Usage");
    }

    int s_pct = (int)(data->session_pct + 0.5f);

    if (data->enterprise) {
        // Spending box: big number-only label + small "%" symbol + desc + pace
        lv_obj_set_style_text_font(lbl_session_pct, L.ent_pct_font, 0);
        lv_label_set_text(lbl_session_label, "Spending");
        lv_obj_add_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status,   LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
        update_clock_marker(bar_session, -1, 0);
        update_clock_marker(bar_weekly, -1, 0);
    } else {
        lv_obj_set_style_text_font(lbl_session_pct, L.pct_font, 0);
        lv_label_set_text(lbl_session_label, "Current");
        lv_obj_clear_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
        update_clock_marker(bar_session, data->session_reset_mins, 300);
        update_clock_marker(bar_weekly, data->weekly_reset_mins, 10080);
    }

    char buf[48];

    // Pace vars used in both enterprise blocks below
    const char* pace_text = "Under pace";
    lv_color_t  pace_color = COL_GREEN;
    const char* pace_hex   = "788c5d";   // matches THEME_GREEN
    if (data->session_pct > (float)data->time_pct + 15.0f) {
        pace_text = "Over pace";  pace_color = COL_RED;   pace_hex = "c0392b";
    } else if (data->session_pct > (float)data->time_pct - 15.0f) {
        pace_text = "On pace";    pace_color = COL_AMBER; pace_hex = "d97757";
    }

    if (data->enterprise) {
        lv_label_set_text_fmt(lbl_session_pct, "%d", s_pct);
        lv_obj_align_to(lbl_session_pct_sym, lbl_session_pct,
                        LV_ALIGN_OUT_RIGHT_TOP, 4, 12);
    } else {
        lv_label_set_text_fmt(lbl_session_pct, "%d%%", s_pct);
        format_reset_time(data->session_reset_mins, buf, sizeof(buf));
        lv_label_set_text(lbl_session_reset, buf);
    }

    lv_bar_set_value(bar_session, s_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_session, pct_color(data->session_pct), LV_PART_INDICATOR);

    if (data->enterprise) {
        // Period box: time % + dynamic pace color + "Resets <date>" label
        lv_label_set_text(lbl_weekly_label, "Period");
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", data->time_pct);
        lv_bar_set_value(bar_weekly, data->time_pct, LV_ANIM_ON);
        lv_color_t bar_pace = (data->session_pct <= (float)data->time_pct) ? COL_GREEN :
                              (data->session_pct <= (float)data->time_pct + 15.0f) ? COL_AMBER :
                              COL_RED;
        lv_obj_set_style_bg_color(bar_weekly, bar_pace, LV_PART_INDICATOR);
        snprintf(buf, sizeof(buf), "#%s %s# - #faf9f5 Resets %s#",
                 pace_hex, pace_text, data->reset_date);
        lv_label_set_text(lbl_weekly_reset, buf);
    } else {
        int w_pct = (int)(data->weekly_pct + 0.5f);
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", w_pct);
        lv_bar_set_value(bar_weekly, w_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar_weekly, pct_color(data->weekly_pct), LV_PART_INDICATOR);
        format_reset_time(data->weekly_reset_mins, buf, sizeof(buf));
        lv_label_set_text(lbl_weekly_reset, buf);
    }

    // ---- Codex view ----
    codex_data_seen = data->codex_valid;
    if (!data->codex_valid) {
        update_clock_marker(bar_codex1, -1, 0);
        update_clock_marker(bar_codex2, -1, 0);
        // Host stopped sending Codex data while we're on its screen (config
        // flipped off, logs vanished) — retreat to the usage view.
        if (current_screen == SCREEN_CODEX) ui_show_screen(SCREEN_USAGE);
        return;
    }

    int c1 = (int)(data->codex_pct + 0.5f);
    lv_label_set_text(lbl_codex1_label, window_pill_text(data->codex_window_mins));
    lv_label_set_text_fmt(lbl_codex1_pct, "%d%%", c1);
    lv_bar_set_value(bar_codex1, c1, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_codex1, pct_color(data->codex_pct), LV_PART_INDICATOR);
    update_clock_marker(bar_codex1, data->codex_reset_mins, data->codex_window_mins);
    format_reset_time(data->codex_reset_mins, buf, sizeof(buf));
    lv_label_set_text(lbl_codex1_reset, buf);

    // Second panel slot: the plan's secondary rate window when it has one
    // (5h + weekly plans), otherwise the Daily panel — today's total tokens
    // measured against the 7-day daily average (Codex exposes no daily rate
    // limit, so the average is the only meaningful daily yardstick; bar full
    // = 2x the average day).
    if (data->codex_pct2 >= 0.0f) {
        int c2 = (int)(data->codex_pct2 + 0.5f);
        lv_label_set_text(lbl_codex2_label, window_pill_text(data->codex_window_mins2));
        lv_label_set_text_fmt(lbl_codex2_pct, "%d%%", c2);
        lv_bar_set_value(bar_codex2, c2, LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar_codex2, pct_color(data->codex_pct2), LV_PART_INDICATOR);
        update_clock_marker(bar_codex2, data->codex_reset_mins2, data->codex_window_mins2);
        format_reset_time(data->codex_reset_mins2, buf, sizeof(buf));
        lv_label_set_text(lbl_codex2_reset, buf);
        lv_obj_clear_flag(panel_codex2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(panel_codex_daily, LV_OBJ_FLAG_HIDDEN);
    } else {
        update_clock_marker(bar_codex2, -1, 0);
        long today_total = data->codex_tokens_in + data->codex_tokens_out;
        char tbuf[16];
        format_tokens(today_total, tbuf, sizeof(tbuf));
        lv_label_set_text(lbl_codex_daily_val, tbuf);
        int fill = 0;
        if (data->codex_day_avg > 0)
            fill = (int)((today_total * 50) / data->codex_day_avg);  // avg day = 50%
        if (fill > 100) fill = 100;
        lv_bar_set_value(bar_codex_daily, fill, LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar_codex_daily, pct_color((float)fill), LV_PART_INDICATOR);
        if (data->codex_day_avg > 0) {
            format_tokens(data->codex_day_avg, tbuf, sizeof(tbuf));
            snprintf(buf, sizeof(buf), "7-day avg %s/day", tbuf);
        } else {
            snprintf(buf, sizeof(buf), "No 7-day history");
        }
        lv_label_set_text(lbl_codex_daily_sub, buf);
        lv_obj_clear_flag(panel_codex_daily, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(panel_codex2, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_update_now_playing(const NowPlayingData* data) {
    if (!music_screen_supported || !data) return;
    music_last_update_ms = lv_tick_get();
    music_playing = data->playing;
    if (!music_playing) {
        if (current_screen == SCREEN_NOW_PLAYING) ui_show_screen(SCREEN_SPLASH);
        return;
    }

    music_duration = data->duration_sec > 0 ? data->duration_sec : 0;
    music_elapsed_base = data->elapsed_sec > 0 ? data->elapsed_sec : 0;
    music_progress_base_ms = music_last_update_ms;
    music_expected_generation = data->artwork_generation;
    lv_label_set_text(lbl_music_title, data->title);
    lv_label_set_text(lbl_music_artist, data->artist);

    // A changed track never displays the previous track's cover as if it were
    // current. Metadata appears immediately; the neutral panel remains until
    // the checked artwork transfer completes.
    if (music_art_generation != music_expected_generation) {
        lv_obj_add_flag(music_art, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(music_art_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
}

static uint32_t pluribus_age_now(void) {
    uint32_t elapsed = (lv_tick_get() - pluribus_received_ms) / 60000U;
    if (pluribus_received_age > UINT32_MAX - elapsed) return UINT32_MAX;
    return pluribus_received_age + elapsed;
}

static void render_pluribus_footer(uint32_t age) {
    if (!lbl_pluribus_footer || age == pluribus_last_rendered_age) return;
    pluribus_last_rendered_age = age;
    char age_text[28];
    if (age == 0) snprintf(age_text, sizeof(age_text), "JUST NOW");
    else if (age < 60) snprintf(age_text, sizeof(age_text), "%lu MIN AGO", (unsigned long)age);
    else if (age < 24 * 60) snprintf(age_text, sizeof(age_text), "%lu HR AGO", (unsigned long)(age / 60));
    else snprintf(age_text, sizeof(age_text), "%lu DAY AGO", (unsigned long)(age / (24 * 60)));

    char kind[sizeof(pluribus_kind)];
    size_t i = 0;
    for (; pluribus_kind[i] && i + 1 < sizeof(kind); ++i) {
        char c = pluribus_kind[i];
        kind[i] = (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
    }
    kind[i] = '\0';
    lv_label_set_text_fmt(lbl_pluribus_footer, "%s%s%s",
        kind, kind[0] ? "  \xC2\xB7  " : "", age_text);
}

void ui_update_pluribus(const PluribusActivityData* data) {
    if (!pluribus_screen_supported || !data) return;
    if (!data->present) {
        pluribus_present = false;
        pluribus_id = -1;
        if (current_screen == SCREEN_PLURIBUS) ui_show_screen(SCREEN_SPLASH);
        return;
    }

    // ``!=`` is intentional: a rebuilt/reset local database may restart ids.
    // Every valid receipt also refreshes the monotonic age latch.
    bool replacement = data->id != pluribus_id;
    pluribus_id = data->id;
    pluribus_received_ms = lv_tick_get();
    pluribus_received_age = data->age_minutes;
    pluribus_last_rendered_age = UINT32_MAX;
    pluribus_present = pluribus_received_age < PLURIBUS_EXPIRY_MINUTES;
    strlcpy(pluribus_kind, data->kind, sizeof(pluribus_kind));

    if (!pluribus_present) {
        if (current_screen == SCREEN_PLURIBUS) ui_show_screen(SCREEN_SPLASH);
        return;
    }
    (void)replacement;
    lv_label_set_text(lbl_pluribus_title, data->title);
    lv_label_set_text(lbl_pluribus_detail, data->detail);
    if (data->detail[0]) lv_obj_clear_flag(lbl_pluribus_detail, LV_OBJ_FLAG_HIDDEN);
    else                 lv_obj_add_flag(lbl_pluribus_detail, LV_OBJ_FLAG_HIDDEN);

    lv_color_t status_color;
    const char* status_text;
    if (strcmp(data->status, "needs_review") == 0) {
        status_color = lv_color_hex(0xe2a65d);
        status_text = "NEEDS REVIEW";
    } else if (strcmp(data->status, "failed") == 0) {
        status_color = lv_color_hex(0xe05b62);
        status_text = "NEEDS ATTENTION";
    } else {
        status_color = lv_color_hex(0x57bada);
        status_text = "COMPLETE";
    }
    lv_label_set_text(lbl_pluribus_status, status_text);
    lv_obj_set_style_text_color(lbl_pluribus_status, status_color, 0);
    lv_obj_set_style_bg_color(lbl_pluribus_status, status_color, 0);
    render_pluribus_footer(pluribus_received_age);
    // A taller replacement headline must not let LVGL's child-size refresh
    // carry a latent scroll offset into this fixed full-screen composition.
    lv_obj_scroll_to(pluribus_container, 0, 0, LV_ANIM_OFF);
}

void ui_update_artwork(const BleArtwork* artwork) {
    if (!music_screen_supported || !artwork || !artwork->pixels) return;
    if (artwork->generation != music_expected_generation) return;
    if (music_art_dsc.data) lv_image_cache_drop(&music_art_dsc);
    music_art_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    music_art_dsc.header.cf = artwork->encoding == BLE_ART_JPEG
        ? LV_COLOR_FORMAT_RAW : LV_COLOR_FORMAT_RGB565;
    music_art_dsc.header.w = artwork->width;
    music_art_dsc.header.h = artwork->height;
    music_art_dsc.header.stride = artwork->encoding == BLE_ART_JPEG
        ? 0 : artwork->width * 2;
    music_art_dsc.data_size = artwork->size;
    music_art_dsc.data = artwork->pixels;
    music_art_generation = artwork->generation;
    lv_image_set_src(music_art, &music_art_dsc);
    lv_obj_set_pos(music_art, (L.scr_w - artwork->width) / 2, 50);
    lv_obj_add_flag(music_art_placeholder, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(music_art, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(music_art);
}

static void update_music_progress(void) {
    if (!music_screen_supported || !music_playing) return;
    uint32_t now = lv_tick_get();
    if (now - music_last_update_ms >= MUSIC_FRESH_MS) {
        music_playing = false;
        if (current_screen == SCREEN_NOW_PLAYING) ui_show_screen(SCREEN_SPLASH);
        return;
    }
    int elapsed = music_elapsed_base + (int)((now - music_progress_base_ms) / 1000);
    if (music_duration > 0 && elapsed > music_duration) elapsed = music_duration;
    int value = music_duration > 0 ? (int)((int64_t)elapsed * 1000 / music_duration) : 0;
    lv_bar_set_value(bar_music, value, LV_ANIM_OFF);
    char buf[16];
    format_track_time(elapsed, buf, sizeof(buf));
    lv_label_set_text(lbl_music_elapsed, buf);
    format_track_time(music_duration - elapsed, buf, sizeof(buf));
    char remaining[18];
    snprintf(remaining, sizeof(remaining), "-%s", buf);
    lv_label_set_text(lbl_music_remaining, remaining);
}

static void update_pluribus_age(void) {
    if (!pluribus_screen_supported || !pluribus_present) return;
    if (lv_obj_get_scroll_y(pluribus_container) != 0)
        lv_obj_scroll_to_y(pluribus_container, 0, LV_ANIM_OFF);
    uint32_t age = pluribus_age_now();
    if (age >= PLURIBUS_EXPIRY_MINUTES) {
        pluribus_present = false;
        if (current_screen == SCREEN_PLURIBUS) ui_show_screen(SCREEN_SPLASH);
        return;
    }
    render_pluribus_footer(age);
}

// Pick the usage-view sub-screen: pairing hint (BLE down), the idle "Zzz" screen
// (connected but data has gone stale), or the live usage panels. Only re-lays-out
// on an actual change. The animated status line stays visible everywhere — it
// reads "Listening…" on the idle screen, keeping it alive rather than frozen.
static void update_view_state(void) {
    if (!usage_group || !pair_group || !idle_group) return;
    int v;
    if (!s_ble_connected) {
        v = 0;  // pairing hint
    } else if (data_received && data_ok && (lv_tick_get() - last_data_ms) < DATA_FRESH_MS) {
        v = 2;  // live usage
    } else {
        v = 1;  // idle / Zzz
    }
    if (v == view_state) return;
    view_state = v;
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(v == 0 ? pair_group : v == 1 ? idle_group : usage_group,
                      LV_OBJ_FLAG_HIDDEN);
}

void ui_tick_anim(void) {
    update_music_progress();
    update_pluribus_age();
    // The Codex view shows live numbers only: once the link drops or data goes
    // stale, retreat to the usage view, whose pair/idle sub-views own those
    // states. While live it runs the same animated status line as the usage
    // view (into its own white label — see the bottom of this function).
    if (current_screen == SCREEN_CODEX) {
        bool fresh = s_ble_connected && data_received && data_ok &&
                     (lv_tick_get() - last_data_ms) < DATA_FRESH_MS;
        if (!fresh) { ui_show_screen(SCREEN_USAGE); return; }
    } else if (current_screen != SCREEN_USAGE) {
        return;
    }
    update_view_state();
    if (view_state == 1) splash_mini_tick();   // animate the sleeping creature on the idle screen

    uint32_t now = lv_tick_get();

    // Title clock: once the daemon has sent wall-clock time, replace "Usage" with
    // the live time, advanced locally so it ticks every minute between payloads.
    if (clock_base_epoch > 0) {
        time_t cur = (time_t)(clock_base_epoch + (now - clock_base_ms) / 1000);
        struct tm tmv;
        gmtime_r(&cur, &tmv);   // epoch is already local wall-clock → gmtime keeps it as-is
        if (tmv.tm_min != clock_last_min) {   // only rewrite the title when the minute changes
            clock_last_min = tmv.tm_min;
            char tbuf[12];
            if (clock_fmt == 12) {
                int h12 = tmv.tm_hour % 12;
                if (h12 == 0) h12 = 12;
                snprintf(tbuf, sizeof(tbuf), "%d:%02d %s", h12, tmv.tm_min,
                         tmv.tm_hour < 12 ? "AM" : "PM");
            } else {
                snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
            }
            lv_label_set_text(lbl_title, tbuf);
        }
    }

    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }

    if (now - anim_last_ms < spinner_ms[anim_spinner_idx]) return;
    anim_last_ms = now;
    anim_phase = (anim_phase + 1) % SPINNER_PHASES;
    anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                    : (SPINNER_PHASES - anim_phase);

    // Status text by priority. Whimsical messages only when connected & settled.
    const char* text;
    if (!s_ble_connected) {
        text = "Waiting";              // advertising / waiting for a host connection
    } else if (view_state == 1) {      // idle — alternate so it reads as alive AND data-less
        text = (anim_msg_idx & 1) ? "No data" : "Listening";
    } else if (now - connected_at_ms < 5000) {
        text = "Connected";
    } else {
        text = anim_messages[anim_msg_idx];
    }

    // All states share the whimsical style: "<glyph> <Title-case word>…"
    static char buf[80];
    snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
             spinner_frames[anim_spinner_idx], text);
    lv_label_set_text(current_screen == SCREEN_CODEX ? lbl_codex_anim : lbl_anim,
                      buf);
}

static screen_t prev_non_splash_screen = SCREEN_USAGE;
static void apply_battery_visibility(void) {
    if (!battery_img) return;
    if (!battery_present || current_screen == SCREEN_SPLASH ||
        current_screen == SCREEN_NOW_PLAYING || current_screen == SCREEN_PLURIBUS)
        lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

static void advance_screen(void) {
    // Explicit gated cycle: Splash -> Usage -> Codex -> Music -> Pluribus.
    if (current_screen == SCREEN_SPLASH) {
        ui_show_screen(SCREEN_USAGE);
    } else if (current_screen == SCREEN_USAGE && codex_data_seen && view_state == 2) {
        ui_show_screen(SCREEN_CODEX);
    } else if ((current_screen == SCREEN_USAGE || current_screen == SCREEN_CODEX) &&
               music_screen_supported && music_playing) {
        ui_show_screen(SCREEN_NOW_PLAYING);
    } else if ((current_screen == SCREEN_USAGE || current_screen == SCREEN_CODEX ||
                current_screen == SCREEN_NOW_PLAYING) && pluribus_screen_supported &&
               pluribus_present) {
        ui_show_screen(SCREEN_PLURIBUS);
    } else {
        ui_show_screen(SCREEN_SPLASH);
    }
}

static void global_click_cb(lv_event_t* e) {
    (void)e;
    advance_screen();
}

void ui_tick_screen_rotation(void) {
    uint32_t now = lv_tick_get();
    if (now - last_screen_change_ms >= SCREEN_ROTATE_MS) advance_screen();
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(codex_container, LV_OBJ_FLAG_HIDDEN);
    if (music_container) lv_obj_add_flag(music_container, LV_OBJ_FLAG_HIDDEN);
    if (pluribus_container) lv_obj_add_flag(pluribus_container, LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:  splash_show(); break;
    case SCREEN_USAGE:   lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_CODEX:   lv_obj_clear_flag(codex_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_NOW_PLAYING:
        if (music_container && music_playing)
            lv_obj_clear_flag(music_container, LV_OBJ_FLAG_HIDDEN);
        else
            screen = SCREEN_SPLASH, splash_show();
        break;
    case SCREEN_PLURIBUS:
        if (pluribus_container && pluribus_present)
            lv_obj_clear_flag(pluribus_container, LV_OBJ_FLAG_HIDDEN);
        else
            screen = SCREEN_SPLASH, splash_show();
        break;
    default: break;
    }

    // Corner logo: Clawd mascot everywhere except splash (no corner art) and
    // the Codex screen, where the OpenAI mark takes the slot.
    splash_mascot_set_visible(screen != SCREEN_SPLASH && screen != SCREEN_CODEX &&
                              screen != SCREEN_NOW_PLAYING && screen != SCREEN_PLURIBUS);
    if (logo_img) {
        if (screen == SCREEN_SPLASH || screen == SCREEN_CODEX ||
            screen == SCREEN_NOW_PLAYING || screen == SCREEN_PLURIBUS)
            lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }
    if (screen == SCREEN_CODEX) lv_obj_clear_flag(openai_img, LV_OBJ_FLAG_HIDDEN);
    else                        lv_obj_add_flag(openai_img, LV_OBJ_FLAG_HIDDEN);

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    last_screen_change_ms = lv_tick_get();
    apply_battery_visibility();
}

void ui_toggle_splash(void) {
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    (void)name; (void)mac;
    bool was_connected = s_ble_connected;
    s_ble_connected = (state == BLE_STATE_CONNECTED);

    if (s_ble_connected && !was_connected) connected_at_ms = lv_tick_get();
    // pair / idle / usage — picked from connection + data freshness.
    update_view_state();
}

void ui_update_battery(int percent, bool charging) {
    if (!battery_img) return;

    // AXP2101 reports -1 when this battery-capable board has no battery
    // physically connected. Hide the indicator instead of presenting that
    // state as an alarming empty battery; it will reappear if one is added.
    battery_present = percent >= 0;
    if (!battery_present) {
        apply_battery_visibility();
        return;
    }

    int idx;
    if (charging) {
        idx = 4;
    } else if (percent <= 10) {
        idx = 0;
    } else if (percent <= 35) {
        idx = 1;
    } else if (percent <= 75) {
        idx = 2;
    } else {
        idx = 3;
    }
    lv_image_set_src(battery_img, &battery_dscs[idx]);
    apply_battery_visibility();
}
