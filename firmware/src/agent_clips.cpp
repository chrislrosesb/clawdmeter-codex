#include "agent_clips.h"

#include <Arduino.h>
#include <ctype.h>
#include <string.h>

#if defined(BOARD_HAS_PSRAM) && LV_USE_TJPGD
#include <esp_heap_caps.h>
#include <src/libs/tjpgd/tjpgd.h>
#include <src/misc/cache/instance/lv_image_cache.h>

// Start: A6 01 | crc u32 | total u32 | frames u16 | fps u8 | w u16 | h u16 |
//        agent[16] | name[12]   (45 bytes; crc is the clip's identity)
// Chunk: A6 02 | crc u32 | offset u32 | data...
// Data:  frames x (<u32 length><baseline JPEG>)
#define CLIP_START 0x01
#define CLIP_CHUNK 0x02
#define CLIP_SLOTS 6
#define CLIP_MAX_BYTES (2u * 1024 * 1024)
#define CLIP_MAX_SIDE 320
#define CLIP_MAX_FRAMES 400

struct Clip {
    uint8_t* data;
    uint32_t* offsets;        // frame start (past its length prefix)
    uint32_t* lengths;
    uint32_t crc, total, received;
    uint16_t frames, w, h;
    uint8_t fps;
    char agent[17];
    char name[13];
    uint32_t born;            // eviction order
    volatile bool ready;
};

static Clip clips[CLIP_SLOTS] = {};
static int receiving = -1;
static uint32_t births = 0;

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

static uint32_t crc32_of(const uint8_t* d, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= d[i];
        for (int b = 0; b < 8; ++b) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(c & 1));
    }
    return ~c;
}

static void free_clip(Clip& c) {
    c.ready = false;
    heap_caps_free(c.data);
    free(c.offsets);
    free(c.lengths);
    c = {};
}

static int playing_slot = -1;

// Index the frames once the bytes are complete and checked.
static bool index_clip(Clip& c) {
    c.offsets = (uint32_t*)malloc(c.frames * sizeof(uint32_t));
    c.lengths = (uint32_t*)malloc(c.frames * sizeof(uint32_t));
    if (!c.offsets || !c.lengths) return false;
    uint32_t off = 0;
    for (uint16_t i = 0; i < c.frames; ++i) {
        if (off + 4 > c.total) return false;
        uint32_t n = rd32(c.data + off);
        off += 4;
        if (n < 4 || off + n > c.total || c.data[off] != 0xFF || c.data[off + 1] != 0xD8) return false;
        c.offsets[i] = off;
        c.lengths[i] = n;
        off += n;
    }
    return off == c.total;
}

int agent_clip_packet(const uint8_t* d, size_t len) {
    if (len < 10 || d[0] != CLIP_MAGIC) return 0;
    const uint32_t crc = rd32(d + 2);
    if (d[1] == CLIP_START) {
        if (len != 45) return 0;
        for (auto& c : clips) if (c.ready && c.crc == crc) return 2;
        const uint32_t total = rd32(d + 6);
        const uint16_t frames = rd16(d + 10);
        const uint8_t fps = d[12];
        const uint16_t w = rd16(d + 13), h = rd16(d + 15);
        if (!total || total > CLIP_MAX_BYTES || !frames || frames > CLIP_MAX_FRAMES ||
            !fps || fps > 30 || !w || !h || w > CLIP_MAX_SIDE || h > CLIP_MAX_SIDE) return 0;
        if (receiving >= 0 && !clips[receiving].ready) free_clip(clips[receiving]);
        receiving = -1;
        // A free slot, else evict the oldest clip that is not on screen.
        int slot = -1;
        for (int i = 0; i < CLIP_SLOTS && slot < 0; ++i) if (!clips[i].data) slot = i;
        uint8_t* buf = nullptr;
        while (true) {
            if (slot >= 0) buf = (uint8_t*)heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
            if (buf) break;
            int victim = -1;
            for (int i = 0; i < CLIP_SLOTS; ++i)
                if (clips[i].data && i != playing_slot && (victim < 0 || clips[i].born < clips[victim].born))
                    victim = i;
            if (victim < 0) return 0;
            free_clip(clips[victim]);
            slot = victim;
        }
        Clip& c = clips[slot];
        c.data = buf;
        c.crc = crc;
        c.total = total;
        c.frames = frames;
        c.fps = fps;
        c.w = w;
        c.h = h;
        memcpy(c.agent, d + 17, 16);
        c.agent[16] = '\0';
        memcpy(c.name, d + 33, 12);
        c.name[12] = '\0';
        c.born = ++births;
        receiving = slot;
        return 1;
    }
    if (d[1] != CLIP_CHUNK || receiving < 0) return 0;
    Clip& c = clips[receiving];
    const uint32_t off = rd32(d + 6);
    const size_t n = len - 10;
    if (c.crc != crc || off != c.received || off + n > c.total) return 0;
    memcpy(c.data + off, d + 10, n);
    c.received += n;
    if (c.received == c.total) {
        receiving = -1;
        if (crc32_of(c.data, c.total) != c.crc || !index_clip(c)) {
            Serial.println("clip: rejected (crc/frames)");
            free_clip(c);
            return 0;
        }
        c.ready = true;
        Serial.printf("clip: %s ready (%u frames, %lu KB)\n", c.name, c.frames,
                      (unsigned long)(c.total / 1024));
    }
    return 1;
}

// ---- Playback ----

static lv_obj_t* root = nullptr;
static lv_obj_t* img = nullptr;
static lv_obj_t* label = nullptr;
static uint16_t* fb[2] = {nullptr, nullptr};
static lv_image_dsc_t dsc[2] = {};
static int front = 0;
static uint16_t frame_no = 0;
static uint32_t next_due = 0;
static int rotation = 0;
static uint8_t work[4096];

struct JpegSrc { const uint8_t* p; size_t n, pos; uint16_t* out; uint16_t w; };

static size_t jd_in(JDEC* jd, uint8_t* buf, size_t n) {
    JpegSrc* s = (JpegSrc*)jd->device;
    if (s->pos + n > s->n) n = s->n - s->pos;
    if (buf) memcpy(buf, s->p + s->pos, n);
    s->pos += n;
    return n;
}

static int jd_out(JDEC* jd, void* bitmap, JRECT* r) {   // 24-bit block -> RGB565
    JpegSrc* s = (JpegSrc*)jd->device;
    const uint8_t* px = (const uint8_t*)bitmap;
    for (unsigned y = r->top; y <= r->bottom; ++y) {
        uint16_t* row = s->out + y * s->w + r->left;
        for (unsigned x = r->left; x <= r->right; ++x, px += 3)
            *row++ = (uint16_t)((px[2] & 0xF8) << 8 | (px[1] & 0xFC) << 3 | px[0] >> 3);   // LVGL tjpgd emits B,G,R
    }
    return 1;
}

static bool decode(const Clip& c, uint16_t i, uint16_t* out) {
    JDEC jd;
    JpegSrc src = {c.data + c.offsets[i], c.lengths[i], 0, out, c.w};
    if (jd_prepare(&jd, jd_in, work, sizeof(work), &src) != JDR_OK) return false;
    if (jd.width != c.w || jd.height != c.h) return false;
    return jd_decomp(&jd, jd_out, 0) == JDR_OK;
}

void agent_clips_create(lv_obj_t* parent) {
    fb[0] = (uint16_t*)heap_caps_malloc(CLIP_MAX_SIDE * CLIP_MAX_SIDE * 2, MALLOC_CAP_SPIRAM);
    fb[1] = (uint16_t*)heap_caps_malloc(CLIP_MAX_SIDE * CLIP_MAX_SIDE * 2, MALLOC_CAP_SPIRAM);
    root = lv_obj_create(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    img = lv_image_create(root);
    lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE);
    label = lv_label_create(root);
    LV_FONT_DECLARE(font_mono_18);
    lv_obj_set_style_text_font(label, &font_mono_18, 0);
    lv_obj_set_style_text_letter_space(label, 2, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xa8a397), 0);
    lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* agent_clips_root(void) { return root; }

int agent_clips_ready(void) {
    int n = 0;
    for (auto& c : clips) n += c.ready ? 1 : 0;
    return fb[0] && fb[1] ? n : 0;
}

bool agent_clips_playing(void) { return playing_slot >= 0; }

static void show_frame(const Clip& c) {
    int back = 1 - front;
    if (!decode(c, frame_no, fb[back])) return;
    lv_image_cache_drop(&dsc[back]);
    dsc[back].header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc[back].header.cf = LV_COLOR_FORMAT_RGB565;
    dsc[back].header.w = c.w;
    dsc[back].header.h = c.h;
    dsc[back].header.stride = c.w * 2;
    dsc[back].data_size = c.w * c.h * 2;
    dsc[back].data = (const uint8_t*)fb[back];
    lv_image_set_src(img, &dsc[back]);
    front = back;
}

bool agent_clips_start(void) {
    if (!root || agent_clips_ready() == 0) return false;
    for (int k = 0; k < CLIP_SLOTS; ++k) {
        int i = (rotation + k) % CLIP_SLOTS;
        if (!clips[i].ready) continue;
        rotation = i + 1;
        playing_slot = i;
        const Clip& c = clips[i];
        frame_no = 0;
        lv_obj_set_size(img, c.w, c.h);
        lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 64);
        char upper[13];
        size_t j = 0;
        for (; c.name[j] && j < 12; ++j) upper[j] = (char)toupper((unsigned char)c.name[j]);
        upper[j] = '\0';
        lv_label_set_text(label, upper);
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 64 + c.h + 22);
        show_frame(c);
        next_due = millis() + 1000 / c.fps;
        lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
        return true;
    }
    return false;
}

bool agent_clips_tick(void) {
    if (playing_slot < 0) return false;
    const Clip& c = clips[playing_slot];
    if (!c.ready) { agent_clips_stop(); return false; }
    const uint32_t now = millis();
    if ((int32_t)(now - next_due) < 0) return true;
    if (++frame_no >= c.frames) return false;
    next_due += 1000 / c.fps;
    if ((int32_t)(now - next_due) > 500) next_due = now;   // fell behind: don't sprint
    show_frame(c);
    return true;
}

void agent_clips_stop(void) {
    playing_slot = -1;
    if (root) lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
}

#else   // no PSRAM or no JPEG decoder: clips are refused, nothing plays

int agent_clip_packet(const uint8_t*, size_t) { return 0; }
void agent_clips_create(lv_obj_t*) {}
lv_obj_t* agent_clips_root(void) { return nullptr; }
int agent_clips_ready(void) { return 0; }
bool agent_clips_start(void) { return false; }
bool agent_clips_tick(void) { return false; }
void agent_clips_stop(void) {}
bool agent_clips_playing(void) { return false; }

#endif
