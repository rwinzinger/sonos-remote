// Display + touch + LVGL bring-up for the CrowPanel 2.1".
//
// The ST7701S init, RGB timings and power-up ORDER are copied VERBATIM from Elecrow's
// RotaryScreen_2_1 demo. CLAUDE.md rule #2: never hand-write them — a wrong init is a
// black screen with no error message.
//
// This module also owns the PCF8574, because LCD power (P3), LCD reset (P4), touch reset
// (P0), touch interrupt (P2) and the encoder BUTTON (P5) all live on that one expander.
// Two owners would mean two Wire/PCF init paths fighting each other.

#pragma once

#include <Arduino.h>

namespace panel {

// Full bring-up: Wire -> PCF8574 -> LCD power/reset -> touch reset -> gfx -> LVGL -> touch
// driver -> backlight. Returns false if the expander or touch controller did not answer;
// the display may still work, so check the serial log for which part failed.
bool begin();

// Drive LVGL. Call often from loop(); LVGL uses millis() as its tick (LV_TICK_CUSTOM=1),
// so no separate tick feeding is required.
void loop();

// Encoder push button, read over I2C from PCF8574 P5. Active low, returns raw level.
// Not a GPIO — digitalRead() can never see this.
uint8_t readButtonRaw();

bool expanderOk();
bool touchOk();

void setBacklight(uint8_t duty);   // 0-255

// --- idle blanking --------------------------------------------------------------------
// Turning the display off is just the backlight: LCD power stays up, so waking is instant
// and needs no panel re-init (which would flash and take ~500 ms).
void setDisplayOn(bool on);
bool displayOn();

// True if the touchscreen was touched since the last call, and clears the flag. Reported
// even while the display is off — but in that state the touch is NOT forwarded to LVGL, so
// the tap that wakes the screen cannot also press whatever happens to be under the finger.
bool consumeTouchActivity();

}  // namespace panel
