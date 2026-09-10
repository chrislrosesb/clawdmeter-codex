#include "../../hal/sound_hal.h"
#include "board.h"

#include <Arduino.h>

// Chris's 2.16-inch device no longer plays notification chimes. Leave I2S
// unclaimed for the planned ES7210 microphone capture and keep the amp off.
void sound_hal_init(void) {
    pinMode(SND_PA_PIN, OUTPUT);
    digitalWrite(SND_PA_PIN, LOW);
}

void sound_hal_play_reset(void) {}
void sound_hal_tick(void) {}
