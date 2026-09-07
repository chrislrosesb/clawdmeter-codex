#pragma once
#include <stdint.h>

enum ble_state_t {
    BLE_STATE_INIT,
    BLE_STATE_ADVERTISING,
    BLE_STATE_CONNECTED,
    BLE_STATE_DISCONNECTED,
};

void ble_init(void);
void ble_tick(void);
ble_state_t ble_get_state(void);
const char* ble_get_device_name(void);
const char* ble_get_mac_address(void);
void ble_clear_bonds(void);
bool ble_has_bonds(void);
bool ble_has_data(void);
const char* ble_get_data(void);
void ble_send_ack(void);
void ble_send_nack(void);
void ble_request_refresh(void);

enum BleArtworkEncoding : uint8_t {
    BLE_ART_RGB565 = 0,
    BLE_ART_JPEG = 1,
};

// Album artwork arrives as a checked, chunked transfer over the same encrypted
// RX characteristic as usage JSON. The completed buffer remains valid until a
// later call returns a newer frame.
struct BleArtwork {
    const uint8_t* pixels;
    uint32_t size;
    uint16_t width;
    uint16_t height;
    uint16_t generation;
    BleArtworkEncoding encoding;
};
bool ble_take_artwork(BleArtwork* out);

void ble_set_battery_level(int pct);

// BLE HID keyboard
void ble_keyboard_press(uint8_t key, uint8_t modifier);
void ble_keyboard_release(void);
