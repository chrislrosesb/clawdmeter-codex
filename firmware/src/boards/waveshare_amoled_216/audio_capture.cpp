#ifdef CLAWDMETER_AUDIO_TEST
#include "../../hal/audio_capture_hal.h"
#include "board.h"
#include "es7210_vendor/es7210.h"
#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s_std.h>

static i2s_chan_handle_t rx;
static bool enabled;
static unsigned slot;
static uint32_t overruns;

static bool IRAM_ATTR overflow(i2s_chan_handle_t, i2s_event_data_t*, void*) {
    __atomic_fetch_add(&overruns, 1u, __ATOMIC_RELAXED);
    return false;
}

uint32_t audio_capture_hal_overruns() {
    return __atomic_load_n(&overruns, __ATOMIC_RELAXED);
}

bool audio_capture_hal_end() {
    bool ok = true;
    if (rx) {
        if (enabled) ok = i2s_channel_disable(rx) == ESP_OK;
        ok = (i2s_del_channel(rx) == ESP_OK) && ok;
        rx = nullptr;
        enabled = false;
    }
    // Powers down ADC inputs and clocks; does not reset the shared I2C bus.
    ok = (es7210_adc_ctrl_state(AUDIO_HAL_CODEC_MODE_ENCODE,
                              AUDIO_HAL_CTRL_STOP) == ESP_OK) && ok;
    ok = (es7210_read_reg(ES7210_CLOCK_OFF_REG01) == 0x7f &&
          es7210_read_reg(ES7210_ANALOG_REG40) == 0xc0 &&
          es7210_read_reg(ES7210_POWER_DOWN_REG06) == 0x07 &&
          es7210_read_reg(ES7210_MIC12_POWER_REG4B) == 0xff) && ok;
    digitalWrite(SND_PA_PIN, LOW);
    return ok;
}

bool audio_capture_hal_begin(unsigned microphone) {
    if (rx || (microphone != 1 && microphone != 2)) return false;
    // Exact-board schematic: ES7210 @0x40, physical MIC1/2 on SDOUT1,
    // standard 16-bit I2S. Receive BOTH slots and explicitly pick one; mono
    // hardware slot packing differs across ESP32 generations.
    // MIC3 is the speaker-reference/AEC input, not an ambient microphone.
    slot = microphone - 1;
    __atomic_store_n(&overruns, 0u, __ATOMIC_RELAXED);
    audio_hal_codec_config_t codec = {};
    codec.adc_input = AUDIO_HAL_ADC_INPUT_ALL;
    codec.codec_mode = AUDIO_HAL_CODEC_MODE_ENCODE;
    codec.i2s_iface.mode = AUDIO_HAL_MODE_SLAVE;
    codec.i2s_iface.fmt = AUDIO_HAL_I2S_NORMAL;
    codec.i2s_iface.samples = AUDIO_HAL_16K_SAMPLES;
    codec.i2s_iface.bits = AUDIO_HAL_BIT_LENGTH_16BITS;
    // Wire's built-in per-transaction mutex also protects touch/PMU/IMU.
    // Never start a second IDF I2C driver on this bus.
    if (es7210_adc_init(&Wire, &codec) != ESP_OK ||
        es7210_adc_config_i2s(codec.codec_mode, &codec.i2s_iface) != ESP_OK ||
        es7210_mic_select((es7210_input_mics_t)(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2)) != ESP_OK ||
        es7210_adc_set_gain((es7210_input_mics_t)(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2),
                           GAIN_37_5DB) != ESP_OK ||
        es7210_adc_ctrl_state(codec.codec_mode, AUDIO_HAL_CTRL_START) != ESP_OK ||
        es7210_read_reg(ES7210_ANALOG_REG40) != 0x43 ||
        es7210_read_reg(ES7210_MIC1_GAIN_REG43) != 0x1e ||
        es7210_read_reg(ES7210_MIC2_GAIN_REG44) != 0x1e ||
        es7210_read_reg(ES7210_SDP_INTERFACE1_REG11) != 0x60 ||
        es7210_read_reg(ES7210_SDP_INTERFACE2_REG12) != 0x00) {
        audio_capture_hal_end();
        return false;
    }
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan.dma_desc_num = 8;
    chan.dma_frame_num = 256;
    i2s_std_config_t cfg = {};
    cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000);
    cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_STEREO);
    cfg.gpio_cfg.mclk = (gpio_num_t)SND_I2S_MCLK;
    cfg.gpio_cfg.bclk = (gpio_num_t)SND_I2S_BCLK;
    cfg.gpio_cfg.ws = (gpio_num_t)SND_I2S_WS;
    cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    cfg.gpio_cfg.din = (gpio_num_t)SND_I2S_DIN;
    i2s_event_callbacks_t callbacks = {};
    callbacks.on_recv_q_ovf = overflow;
    if (i2s_new_channel(&chan, nullptr, &rx) != ESP_OK ||
        i2s_channel_init_std_mode(rx, &cfg) != ESP_OK ||
        i2s_channel_register_event_callback(rx, &callbacks, nullptr) != ESP_OK ||
        i2s_channel_enable(rx) != ESP_OK) {
        audio_capture_hal_end();
        return false;
    }
    enabled = true;
    // Discard 100 ms of settling input, actively draining DMA rather than sleeping.
    int16_t discard[256];
    for (size_t count = 0; count < 1600;) {
        size_t got = 0;
        if (!audio_capture_hal_read(discard, min((size_t)256, 1600 - count), &got)) {
            audio_capture_hal_end();
            return false;
        }
        count += got;
    }
    return true;
}

bool audio_capture_hal_read(int16_t* mono, size_t capacity, size_t* samples) {
    *samples = 0;
    if (!enabled || capacity == 0) return false;
    int16_t stereo[512];
    size_t bytes = 0;
    const size_t frames = min(capacity, (size_t)256);
    const esp_err_t err = i2s_channel_read(rx, stereo, frames * 4, &bytes, 250);
    if (err != ESP_OK || !bytes || bytes % 4) return false;
    *samples = bytes / 4;
    for (size_t i = 0; i < *samples; ++i) mono[i] = stereo[i * 2 + slot];
    return true;
}
#endif
