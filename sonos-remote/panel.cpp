#include "panel.h"

#include <Wire.h>
#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include <Adafruit_CST8XX.h>
#include "PCF8574.h"

namespace panel {
namespace {

#define I2C_SDA_PIN          38
#define I2C_SCL_PIN          39
#define SCREEN_BACKLIGHT_PIN 6
#define I2C_TOUCH_ADDR       0x15

const uint16_t SCREEN_W = 480;
const uint16_t SCREEN_H = 480;

// Backlight PWM. Channel 0 is the demo's choice; keep it clear of anything else using ledc.
const int BL_PWM_FREQ    = 5000;
const int BL_PWM_CHANNEL = 0;
const int BL_PWM_RES     = 8;

// Elecrow's empirical touch calibration — reported Y sits ~20 px below the visual hit
// point on this panel. Taken from the demo; do not "clean up" without re-measuring.
const int TOUCH_Y_OFFSET = 20;

PCF8574 pcf8574(0x21);
Adafruit_CST8XX tsPanel = Adafruit_CST8XX();

bool expanderOk_ = false;
bool touchOk_    = false;
bool displayOn_  = true;
bool touchSeen_  = false;

const uint8_t BACKLIGHT_DUTY = 204;   // the value Elecrow's demo uses

// ---- VERBATIM from RotaryScreen_2_1.ino: pin mapping and panel timings. Do not edit. ----
Arduino_ESP32RGBPanel *bus = new Arduino_ESP32RGBPanel(
  16 /* CS */, 2 /* SCK */, 1 /* SDA */,
  40 /* DE */, 7 /* VSYNC */, 15 /* HSYNC */, 41 /* PCLK */,
  46 /* R0 */, 3 /* R1 */, 8 /* R2 */, 18 /* R3 */, 17 /* R4 */,
  14 /* G0/P22 */, 13 /* G1/P23 */, 12 /* G2/P24 */, 11 /* G3/P25 */, 10 /* G4/P26 */, 9 /* G5 */,
  5 /* B0 */, 45 /* B1 */, 48 /* B2 */, 47 /* B3 */, 21 /* B4 */
);

Arduino_ST7701_RGBPanel *gfx = new Arduino_ST7701_RGBPanel(
  bus, GFX_NOT_DEFINED /* RST */, 0 /* rotation */,
  false /* IPS */, 480 /* width */, 480 /* height */,
  st7701_type5_init_operations, sizeof(st7701_type5_init_operations),
  true /* BGR */,
  10 /* hsync_front_porch */, 4 /* hsync_pulse_width */, 20 /* hsync_back_porch */,
  10 /* vsync_front_porch */, 4 /* vsync_pulse_width */, 20 /* vsync_back_porch */);
// ---------------------------------- end verbatim ----------------------------------------

lv_disp_draw_buf_t draw_buf;
lv_color_t *buf1 = nullptr;
lv_color_t *buf2 = nullptr;

void dispFlush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
#if (LV_COLOR_16_SWAP != 0)
  gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#else
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#endif
  lv_disp_flush_ready(disp);
}

void touchpadRead(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
  if (touchOk_ && tsPanel.touched()) {
    touchSeen_ = true;
    if (!displayOn_) {
      // Swallow the wake-up tap. The buttons do consequential things — splitting a stereo
      // pair, regrouping rooms — so a blind tap on a dark screen must never trigger one.
      data->state = LV_INDEV_STATE_REL;
      return;
    }
    CST_TS_Point p = tsPanel.getPoint(0);
    data->point.x = p.x;
    data->point.y = p.y - TOUCH_Y_OFFSET;
    data->state = LV_INDEV_STATE_PR;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

}  // namespace

bool begin() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setTimeOut(100);            // a stuck bus must fail loudly, not hang the boot

  // Expander pin roles — see CLAUDE.md pin map.
  pcf8574.pinMode(P0, OUTPUT);        // touch reset
  pcf8574.pinMode(P2, OUTPUT);        // touch interrupt
  pcf8574.pinMode(P3, OUTPUT);        // LCD power (P-MOSFET gate: LOW = on)
  pcf8574.pinMode(P4, OUTPUT);        // LCD reset
  pcf8574.pinMode(P5, INPUT_PULLUP);  // encoder button

  expanderOk_ = pcf8574.begin();
  Serial.printf("[panel] PCF8574 @ 0x21: %s\n", expanderOk_ ? "OK" : "FAILED");

  pcf8574.digitalWrite(P3, HIGH);
  delay(100);

  // LCD reset pulse
  pcf8574.digitalWrite(P4, HIGH);
  delay(100);
  pcf8574.digitalWrite(P4, LOW);
  delay(120);
  pcf8574.digitalWrite(P4, HIGH);
  delay(120);

  // Touch reset pulse
  pcf8574.digitalWrite(P0, HIGH);
  delay(100);
  pcf8574.digitalWrite(P0, LOW);
  delay(120);
  pcf8574.digitalWrite(P0, HIGH);
  delay(120);
  pcf8574.digitalWrite(P2, HIGH);
  delay(120);

  // Arduino_GFX 1.3.1: Arduino_ST7701_RGBPanel::begin() returns void — there is no status
  // to check. A failed panel init shows up as a black screen, not as an error code.
  gfx->begin();
  gfx->fillScreen(BLACK);
  Serial.println("[panel] gfx ready");

  touchOk_ = tsPanel.begin(&Wire, I2C_TOUCH_ADDR);
  Serial.printf("[panel] touch CST @ 0x%02X: %s\n", I2C_TOUCH_ADDR,
                touchOk_ ? "OK" : "not found");

  lv_init();

  // Two full-screen buffers live in PSRAM (2 x 460800 bytes). They will not fit in
  // internal RAM — this is why PSRAM=opi is mandatory.
  size_t bufferSize = sizeof(lv_color_t) * SCREEN_W * SCREEN_H;
  buf1 = (lv_color_t *)heap_caps_malloc(bufferSize, MALLOC_CAP_SPIRAM);
  buf2 = (lv_color_t *)heap_caps_malloc(bufferSize, MALLOC_CAP_SPIRAM);
  if (!buf1 || !buf2) {
    Serial.printf("[panel] LVGL buffer alloc FAILED (%u bytes each)\n",
                  (unsigned)bufferSize);
    return false;
  }
  lv_disp_draw_buf_init(&draw_buf, buf1, buf2, SCREEN_W * SCREEN_H);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res       = SCREEN_W;
  disp_drv.ver_res       = SCREEN_H;
  disp_drv.flush_cb      = dispFlush;
  disp_drv.draw_buf      = &draw_buf;
  disp_drv.direct_mode   = 0;
  disp_drv.full_refresh  = 0;
  disp_drv.sw_rotate     = 0;
  disp_drv.screen_transp = 0;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type    = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touchpadRead;
  lv_indev_drv_register(&indev_drv);

  // Backlight on, then LCD power. Order matters — the demo does exactly this.
  ledcSetup(BL_PWM_CHANNEL, BL_PWM_FREQ, BL_PWM_RES);
  ledcAttachPin(SCREEN_BACKLIGHT_PIN, BL_PWM_CHANNEL);
  ledcWrite(BL_PWM_CHANNEL, BACKLIGHT_DUTY);
  pcf8574.digitalWrite(P3, LOW);

  Serial.println("[panel] ready");
  return true;
}

void loop() {
  lv_timer_handler();
}

uint8_t readButtonRaw() {
  if (!expanderOk_) return HIGH;
  return pcf8574.digitalRead(P5, true);
}

bool expanderOk() { return expanderOk_; }
bool touchOk()    { return touchOk_; }

void setBacklight(uint8_t duty) { ledcWrite(BL_PWM_CHANNEL, duty); }

void setDisplayOn(bool on) {
  if (on == displayOn_) return;
  displayOn_ = on;
  if (on) {
    // Instant on: waking must feel immediate.
    ledcWrite(BL_PWM_CHANNEL, BACKLIGHT_DUTY);
  } else {
    // Short fade out — nothing is waiting on the loop while going idle, and an abrupt
    // cut is startling in a dark room.
    for (int duty = BACKLIGHT_DUTY; duty >= 0; duty -= 8) {
      ledcWrite(BL_PWM_CHANNEL, duty < 0 ? 0 : duty);
      delay(12);
    }
    ledcWrite(BL_PWM_CHANNEL, 0);
  }
}

bool displayOn() { return displayOn_; }

bool consumeTouchActivity() {
  bool seen = touchSeen_;
  touchSeen_ = false;
  return seen;
}

}  // namespace panel
