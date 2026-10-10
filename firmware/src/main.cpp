#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include "data.h"
#include "ui.h"
#include "ble.h"
#include "splash.h"
#include "usage_rate.h"
#include "idle.h"
#include "idle_cfg.h"
#include "brightness.h"
#include "audio_test.h"

#include "hal/board_caps.h"
#include "hal/display_hal.h"
#include "hal/touch_hal.h"
#include "hal/input_hal.h"
#include "hal/power_hal.h"
#include "hal/imu_hal.h"
#include "hal/sound_hal.h"

static UsageData usage = {};
static NowPlayingData now_playing = {};
static AgentMessagesData agent_messages = {};

// ---- LVGL draw buffers (partial render mode) ----
// PSRAM-equipped boards (S3) can comfortably hold larger strips. PSRAM-free
// boards (e.g. ESP32-C6) allocate from internal SRAM, so we shrink the strip
// — 480×20 RGB565 = 19 KB × 2 buffers = 38 KB, fits beside everything else.
#ifdef BOARD_HAS_PSRAM
#define BUF_LINES 40
#define LV_BUF_CAPS (MALLOC_CAP_SPIRAM)
#else
#define BUF_LINES 20
#define LV_BUF_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif
static uint16_t* buf1 = nullptr;
static uint16_t* buf2 = nullptr;

static uint32_t my_tick(void) { return millis(); }

static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    display_hal_draw_bitmap(area->x1, area->y1, w, h, (uint16_t*)px_map);
    lv_display_flush_ready(disp);
}

static void rounder_cb(lv_event_t* e) {
    lv_area_t* area = (lv_area_t*)lv_event_get_param(e);
    display_hal_round_area(&area->x1, &area->y1, &area->x2, &area->y2);
}

// Touch policy is driven by IDLE_WAKE_ON_TOUCH:
//   true  → a press edge while asleep wakes the device and the first touch is
//           swallowed (mirrors the button wake-consumption); a press while
//           awake counts as activity.
//   false → touch never counts as activity and is fully swallowed while the
//           panel is dark, so pets/sleeves can't wake it overnight and LVGL
//           can't quietly toggle splash<->usage on a black panel.
static void my_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    uint16_t x, y;
    bool pressed;
    touch_hal_read(&x, &y, &pressed);
    const bool raw_pressed = pressed;

    if (IDLE_WAKE_ON_TOUCH) {
        static bool touch_was = false;
        static bool touch_wake_swallowed = false;
        if (raw_pressed && !touch_was) {
            // Press edge — consume as wake if asleep.
            if (idle_consume_wake_press()) {
                touch_wake_swallowed = true;
                pressed = false;
            }
        } else if (!raw_pressed && touch_was) {
            // Release edge.
            if (touch_wake_swallowed) {
                touch_wake_swallowed = false;
                pressed = false;
            }
        } else if (raw_pressed && touch_wake_swallowed) {
            // Held finger through wake — keep hiding until release.
            pressed = false;
        }
        touch_was = raw_pressed;
    } else if (idle_is_asleep()) {
        pressed = false;
    }

    if (pressed) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// Parse a JSON line into UsageData.
static bool parse_json(const char* json, UsageData* out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    out->session_pct = doc["s"] | 0.0f;
    out->session_reset_mins = doc["sr"] | -1;
    out->weekly_pct = doc["w"] | 0.0f;
    out->weekly_reset_mins = doc["wr"] | -1;
    strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
    out->chime = doc["c"] | false;   // absent (old daemon / chime off) → stay silent
    const char* acct = doc["acct"] | "pro";
    out->enterprise = (strcmp(acct, "ent") == 0);
    out->time_pct = doc["tp"] | 0;
    out->period_days = doc["pd"] | 30;
    strlcpy(out->reset_date, doc["rd"] | "", sizeof(out->reset_date));
    out->clock_epoch = doc["t"] | 0L;
    out->clock_fmt = doc["tf"] | 24;

    JsonVariantConst x = doc["x"];   // optional Codex (OpenAI) usage object
    out->codex_valid = !x.isNull();
    if (out->codex_valid) {
        out->codex_pct          = x["p"]   | 0.0f;
        out->codex_reset_mins   = x["rm"]  | -1;
        out->codex_window_mins  = x["wm"]  | 0;
        out->codex_pct2         = x["p2"]  | -1.0f;
        out->codex_reset_mins2  = x["rm2"] | -1;
        out->codex_window_mins2 = x["wm2"] | 0;
        out->codex_tokens_in    = x["ti"]  | 0L;
        out->codex_tokens_out   = x["to"]  | 0L;
        out->codex_day_avg      = x["da"]  | 0L;
    }

    out->ok = doc["ok"] | false;
    out->valid = true;
    return true;
}

// Parse a standalone now-playing update. ``recognized`` distinguishes a
// malformed music update from an ordinary usage payload, preserving backward
// compatibility with every existing daemon payload.
static bool parse_now_playing_json(const char* json, NowPlayingData* out,
                                   bool* recognized) {
    *recognized = false;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) return false;
    JsonVariantConst np = doc["np"];
    if (np.isNull()) return true;
    *recognized = true;
    out->playing = (np["p"] | 0) != 0;
    strlcpy(out->title, np["t"] | "", sizeof(out->title));
    strlcpy(out->artist, np["a"] | "", sizeof(out->artist));
    strlcpy(out->album, np["l"] | "", sizeof(out->album));
    out->duration_sec = np["d"] | 0;
    out->elapsed_sec = np["e"] | 0;
    out->artwork_generation = np["g"] | 0;
    return true;
}

// Parse a standalone agent-messages update ({"am":...}). The retired
// Pluribus activity frame ({"pb":...}) is still recognized and acknowledged
// so an older host can never fall through to the usage parser.
static bool parse_agent_messages_json(const char* json, AgentMessagesData* out,
                                      bool* recognized, bool* legacy) {
    *recognized = false;
    *legacy = false;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) return false;
    if (!doc["pb"].isUnbound()) {
        *recognized = true;
        *legacy = true;
        return true;
    }
    JsonVariantConst rd = doc["rd"];
    if (!rd.isUnbound()) {   // read in Pluribus: clear these ids
        *recognized = true;
        *legacy = true;
        const char* ids[24];
        int n = 0;
        for (JsonVariantConst v : rd.as<JsonArrayConst>())
            if (n < 24 && v.is<const char*>()) ids[n++] = v.as<const char*>();
        ui_agents_dismiss_ids(ids, n);
        return true;
    }
    JsonVariantConst rm = doc["rm"];
    if (!rm.isUnbound()) {   // one chat/Room reply, queued by the UI
        *recognized = true;
        AgentMessage reply = {};
        const char* id = rm["i"] | "";
        const char* agent = rm["g"] | "";
        if (!id[0] || !agent[0] || !rm["a"].is<uint32_t>()) return false;
        strlcpy(reply.id, id, sizeof(reply.id));
        strlcpy(reply.agent, agent, sizeof(reply.agent));
        strlcpy(reply.name, rm["w"] | agent, sizeof(reply.name));
        strlcpy(reply.room, rm["r"] | "", sizeof(reply.room));
        strlcpy(reply.text, rm["t"] | "", sizeof(reply.text));
        reply.age_minutes = rm["a"].as<uint32_t>();
        if (ui_add_room_reply(&reply)) idle_note_activity();
        *legacy = true;   // handled here; leave the check-in snapshot untouched
        return true;
    }
    JsonVariantConst am = doc["am"];
    if (am.isUnbound()) return true;
    *recognized = true;
    AgentMessagesData parsed = {};
    if (!am.isNull()) {
        if (!am.is<JsonObjectConst>()) return false;
        parsed.total = am["n"] | 0;
        for (JsonVariantConst m : am["m"].as<JsonArrayConst>()) {
            if (parsed.count >= AGENT_MSG_MAX) break;
            const char* id = m["i"] | "";
            const char* agent = m["g"] | "";
            if (!id[0] || !agent[0] || !m["a"].is<uint32_t>()) return false;
            AgentMessage& dst = parsed.msgs[parsed.count++];
            strlcpy(dst.id, id, sizeof(dst.id));
            strlcpy(dst.agent, agent, sizeof(dst.agent));
            strlcpy(dst.name, m["w"] | agent, sizeof(dst.name));
            strlcpy(dst.text, m["t"] | "", sizeof(dst.text));
            dst.age_minutes = m["a"].as<uint32_t>();
        }
        if (parsed.total < parsed.count) parsed.total = parsed.count;
    }
    *out = parsed;
    return true;
}

// Sample agent messages for checking the screen over USB (serial
// "agentdemo N", N = 0..3). Made-up captions; never sent by a host.
static void agent_demo(int n) {
    static const char* const ids[]    = {"demo00000001", "demo00000002", "demo00000003"};
    static const char* const agents[] = {"champion", "skeptic", "sales-marketing"};
    static const char* const names[]  = {"Tori", "Sloane", "Mila"};
    static const char* const texts[]  = {"Proud of today's pitch", "Check the Q4 margin math",
                                         "Silver Air pricing note"};
    static const uint32_t ages[] = {0, 1, 2};
    AgentMessagesData demo = {};
    if (n < 0) n = 0;
    if (n > AGENT_MSG_MAX) n = AGENT_MSG_MAX;
    for (int i = 0; i < n; ++i) {
        AgentMessage& m = demo.msgs[demo.count++];
        strlcpy(m.id, ids[i], sizeof(m.id));
        strlcpy(m.agent, agents[i], sizeof(m.agent));
        strlcpy(m.name, names[i], sizeof(m.name));
        strlcpy(m.text, texts[i], sizeof(m.text));
        m.age_minutes = ages[i];
    }
    demo.total = demo.count;
    agent_messages = demo;
    if (ui_update_agents(&agent_messages)) idle_note_activity();
    Serial.printf("agentdemo: %d sample message(s)\n", n);
}

// ---- Serial lines: commands and the USB data link ----
// Lines starting with '{' are display JSON and '@' lines are hex-encoded
// binary packets (artwork), both answered "#ok"/"#err" so the Mac can pace
// itself. "clawdmeter?" is the Mac's handshake/keepalive. Anything else is a
// debug command. Sized for a 1 KB artwork chunk in hex plus its header.
#define LINE_BUF_SIZE 2304
static char line_buf[LINE_BUF_SIZE];
static int line_pos = 0;
static bool line_overflow = false;

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decode "@<hex>" in place; returns the byte count, or -1 if malformed.
static int decode_hex_line(char* line, int len) {
    if (len < 3 || (len - 1) % 2) return -1;
    uint8_t* out = (uint8_t*)line;
    for (int i = 1, o = 0; i < len; i += 2, ++o) {
        int hi = hex_nibble(line[i]), lo = hex_nibble(line[i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[o] = (uint8_t)(hi << 4 | lo);
    }
    return (len - 1) / 2;
}

static void send_screenshot() {
#ifndef BOARD_HAS_PSRAM
    // A full RGB565 framebuffer doesn't fit in internal SRAM on PSRAM-free
    // boards (e.g. 480×480×2 = 460 KB). Capture is unsupported there.
    Serial.println("SCREENSHOT_UNSUPPORTED");
    return;
#else
    const uint32_t w = board_caps().width;
    const uint32_t h = board_caps().height;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf) {
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);

    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) {
        heap_caps_free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    Serial.printf("SCREENSHOT_START %lu %lu %lu\n",
        (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");
    heap_caps_free(sbuf);
#endif
}

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c != '\n' && c != '\r') {
            if (line_pos < LINE_BUF_SIZE - 1) line_buf[line_pos++] = c;
            else line_overflow = true;
            continue;
        }
        if (line_overflow) {
            if (line_buf[0] == '{' || line_buf[0] == '@') Serial.println("#err");
            line_pos = 0;
            line_overflow = false;
            continue;
        }
        if (line_pos == 0) continue;
        line_buf[line_pos] = '\0';
        const int len = line_pos;
        line_pos = 0;
        if (line_buf[0] == '{') {
            Serial.println(ble_usb_packet((const uint8_t*)line_buf, len) ? "#ok" : "#err");
            return;   // let loop() consume this payload before the next one
        } else if (line_buf[0] == '@') {
            int n = decode_hex_line(line_buf, len);
            int r = n > 0 ? ble_usb_packet((const uint8_t*)line_buf, n) : 0;
            Serial.println(r == 2 ? "#have" : r ? "#ok" : "#err");
        } else if (strcmp(line_buf, "clawdmeter?") == 0) {
            ble_usb_note_alive();
            Serial.println("CLAWDMETER usb1");
        }
        else if (audio_test_command(line_buf)) { /* handled, including busy guard */ }
        else if (strcmp(line_buf, "screenshot") == 0) send_screenshot();
        else if (strcmp(line_buf, "buzz") == 0)  sound_hal_play_reset();
        else if (strncmp(line_buf, "agentdemo", 9) == 0) agent_demo(atoi(line_buf + 9));
        else if (strcmp(line_buf, "agentclear") == 0) ui_agents_dismiss_all();   // = a tap
    }
}

// Each board provides this. Must bring up the shared I2C bus (Wire.begin
// with the board's SDA/SCL pins) and any board-private hardware that has
// to settle before display/touch (e.g. an IO expander gating the LCD
// reset line). Called exactly once at the start of setup().
extern "C" void board_init(void);

void setup() {
    Serial.setRxBufferSize(8192);   // USB data link bursts (artwork chunks)
    Serial.begin(115200);
    audio_test_init();
    delay(300);
    Serial.println("{\"ready\":true}");

    board_init();

    display_hal_init();
    display_hal_begin();
    idle_init();        // takes over panel brightness and starts the idle timer
    brightness_init();  // load the user's saved brightness level and apply via idle

    power_hal_init();
    imu_hal_init();
    sound_hal_init();
    touch_hal_init();

    // ---- LVGL ----
    const int W = board_caps().width;
    const int H = board_caps().height;

    lv_init();
    lv_tick_set_cb(my_tick);

    buf1 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);
    buf2 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);

    lv_display_t* disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, W * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    ble_init();
    input_hal_init();

    ui_init();
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());
    ui_update_battery(power_hal_battery_pct(), power_hal_is_charging());
    ui_show_screen(SCREEN_SPLASH);

    Serial.printf("Dashboard ready (%s, %dx%d), waiting for data on BLE...\n",
        board_caps().name, W, H);
}

static ble_state_t last_ble_state = BLE_STATE_INIT;

// Hold-to-pair gesture: hold the PWR button ~3s, then RELEASE → clear all BLE
// bonds and re-advertise. Clearing on *release* (not while held) is deliberate:
// holding to power the device OFF (AXP hardware shutdown at 8s) must not wipe
// the bond — a power-off hold never releases before shutdown. To stop a
// "chicken-out" release just before 8s from pairing, the gesture disarms at 6s.
//
//   ~1.5s long-press edge → PENDING
//   3.0s (+1500)          → ARMED   (release from here clears bonds)
//   6.0s (+4500)          → DISARMED (no clear; AXP powers off at 8s)
#define PAIR_ARM_AFTER_LONG_MS    1500   // 3.0s total
#define PAIR_DISARM_AFTER_LONG_MS 4500   // 6.0s total
enum pair_state_t { PAIR_IDLE, PAIR_PENDING, PAIR_ARMED };
static pair_state_t pair_state        = PAIR_IDLE;
static uint32_t     pair_long_seen_ms = 0;

static void pair_tick(void) {
    if (pair_state == PAIR_IDLE && power_hal_pwr_long_pressed()) {
        pair_state = PAIR_PENDING;
        pair_long_seen_ms = millis();
        (void)power_hal_pwr_released();  // drain any stale release edge
        Serial.println("PWR long-press: hold to ~3s then release to pair");
        return;
    }
    if (pair_state == PAIR_IDLE) return;

    if (power_hal_pwr_released()) {
        if (pair_state == PAIR_ARMED) {
            Serial.println("Pair: released in window — clearing bonds, advertising");
            ble_clear_bonds();
        } else {
            Serial.println("Pair: released too early — cancelled");
        }
        pair_state = PAIR_IDLE;
        return;
    }

    uint32_t held = millis() - pair_long_seen_ms;
    if (pair_state == PAIR_PENDING && held >= PAIR_ARM_AFTER_LONG_MS) {
        pair_state = PAIR_ARMED;
        Serial.println("Pair: armed — release to pair");
    } else if (pair_state == PAIR_ARMED && held >= PAIR_DISARM_AFTER_LONG_MS) {
        pair_state = PAIR_IDLE;  // power-off territory; don't pair
        Serial.println("Pair: disarmed (holding toward power-off)");
    }
}

void loop() {
    idle_tick();
    lv_timer_handler();
    ui_tick_anim();
    ui_tick_screen_rotation();
    ble_tick();
    power_hal_tick();
    imu_hal_tick();
    sound_hal_tick();
    splash_tick();
    splash_mascot_tick();
    // Rotation transition (blank + ramp) would fight the idle fade — skip
    // ticks while the panel is dark. A rotation that happens during sleep
    // is detected by the next tick after wake and ramped in then.
    if (!idle_is_asleep()) display_hal_tick();

    // ---- Physical buttons ----
    //   PRIMARY   → HID Space  (Claude Code voice-mode PTT)
    //   SECONDARY → HID Shift+Tab  (mode toggle; only if the board has one)
    //   PWR       → on splash: cycle animations; on usage: cycle brightness;
    //               hold ~3s + release: pairing mode
    // First press from sleep is consumed as a wake-only event by
    // idle_consume_wake_press(); the normal action fires from the second
    // press. Activity bookkeeping happens inside idle_consume_wake_press
    // so no separate idle_note_activity() call is needed here.
    {
        static bool primary_was = false;
        static bool primary_wake_swallowed = false;
        bool primary_now = input_hal_is_held(INPUT_BTN_PRIMARY);
        if (primary_now != primary_was) {
            if (primary_now) {
                if (idle_consume_wake_press()) primary_wake_swallowed = true;
                else                            ble_keyboard_press(0x2C, 0);  // HID Space, no mods
            } else {
                if (primary_wake_swallowed) primary_wake_swallowed = false;
                else                        ble_keyboard_release();
            }
            primary_was = primary_now;
        }

        if (board_caps().button_count >= 2) {
            static bool secondary_was = false;
            static bool secondary_wake_swallowed = false;
            bool secondary_now = input_hal_is_held(INPUT_BTN_SECONDARY);
            if (secondary_now != secondary_was) {
                if (secondary_now) {
                    if (idle_consume_wake_press()) secondary_wake_swallowed = true;
                    else                            ble_keyboard_press(0x2B, 0x02);  // HID Tab + LEFT_SHIFT
                } else {
                    if (secondary_wake_swallowed) secondary_wake_swallowed = false;
                    else                          ble_keyboard_release();
                }
                secondary_was = secondary_now;
            }
        }

        if (power_hal_pwr_pressed()) {
            if (!idle_consume_wake_press()) {
                // On splash: cycle animations. Elsewhere: cycle
                // screen brightness (single non-splash view, no more screens).
                if (ui_get_current_screen() == SCREEN_SPLASH) splash_next();
                else                                          brightness_cycle();
            }
        }

        pair_tick();
    }

    // A Mac on the USB cable counts as a live link for the UI's pairing hint.
    ble_state_t bs = ble_usb_active() ? BLE_STATE_CONNECTED : ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    static int  last_pct      = -2;
    static bool last_charging = false;
    int  pct      = power_hal_battery_pct();
    bool charging = power_hal_is_charging();
    if (pct != last_pct || charging != last_charging) {
        if (pct != last_pct) ble_set_battery_level(pct);
        last_pct = pct;
        last_charging = charging;
        ui_update_battery(pct, charging);
    }

    check_serial_cmd();
    audio_test_tick();

    // Taps on the agent screen go back to the Mac, which marks them read in
    // Pluribus (clearing the phone too): a "#tap" line over USB, and the
    // readable TX value over BLE.
    {
        char ids[8 * 13 + 8];
        uint16_t seq = 0;
        if (ui_take_agent_taps(ids, sizeof(ids), &seq)) {
            Serial.printf("#tap %s\n", ids);
            char report[160];
            snprintf(report, sizeof(report), "{\"tap\":\"%s\",\"s\":%u}", ids, seq);
            ble_set_tap_report(report);
        }
    }

    BleArtwork artwork;
    if (ble_take_artwork(&artwork)) ui_update_artwork(&artwork);

    if (ble_has_data()) {
        const char* incoming = ble_get_data();
        bool music_message = false;
        bool agents_message = false;
        bool legacy_message = false;
        if (!parse_agent_messages_json(incoming, &agent_messages, &agents_message,
                                       &legacy_message)) {
            ble_send_nack();
        } else if (agents_message) {
            // A new message wakes the panel; the UI takes over the screen.
            if (!legacy_message && ui_update_agents(&agent_messages)) idle_note_activity();
            ble_send_ack();
        } else if (!parse_now_playing_json(incoming, &now_playing, &music_message)) {
            ble_send_nack();
        } else if (music_message) {
            ui_update_now_playing(&now_playing);
            ble_send_ack();
        } else if (parse_json(incoming, &usage)) {
            int g_before = usage_rate_group();
            bool session_reset = usage_rate_sample(usage.session_pct);
            int g_after = usage_rate_group();
            // 5-hour session limit refilled → chime so the user knows they can
            // use Claude again (no-op on boards without a buzzer). Gated on the
            // daemon's opt-in `chime` config; the `buzz` serial cmd ignores it.
            if (session_reset && usage.chime) {
                Serial.println("session reset detected — chime");
                sound_hal_play_reset();
            }
            if (g_after != g_before) {
                Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
                    g_before, g_after, usage.session_pct);
                if (splash_is_active()) splash_pick_for_current_rate();
            }
            ui_update(&usage);
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }

    delay(5);
}
