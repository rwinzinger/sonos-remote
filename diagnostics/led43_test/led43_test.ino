// Diagnostic: is the onboard LED a single PWM LED on GPIO 43?
//
// RESULT: NO. Keep this sketch only as a record of the negative result.
//
// This board has NO MCU-controllable LED at all. The firmware side of this test works
// perfectly — ledcAttach succeeds, the PWM duty sweeps, no crash, no reboot — and
// nothing lights up, because GPIO 43 is U0TXD routed to the FPC/UART header J10, not an
// LED. Confirmed in Eagle_SCH&PCB/ESP32_Display_2.1(K)_Main_V1.0.sch, which contains
// exactly one LED part: `PWR`, a green power indicator hardwired to the 5 V rail on no
// GPIO. Elecrow's comment at RotaryScreen_2_1.ino:391 calling GPIO 43 an onboard LED
// (板载LED) is wrong, and RGB_CODE's WS2812 on GPIO 48 is a different board entirely
// (GPIO 48 is an LCD blue data line here).
//
// The only light this firmware can control is the LCD backlight, BL_PWM = GPIO 6.
//
// Still useful as a board-health probe: it prints PSRAM size (expect 8388608, proving
// PSRAM=opi took effect), free heap, uptime, and an RTC-backed boot counter, which
// together distinguish a healthy sketch from a reboot loop.
//
// Visible-pattern design (kept for reuse): 3 slow breaths -> 5 fast blinks -> 2 s off,
// so a static power LED can never be mistaken for a controlled one.

#include <esp_system.h>

#define BREATH_LED_PIN 43
#define PWM_FREQ       5000
#define PWM_RES        8      // 8-bit: duty 0..255

RTC_DATA_ATTR static uint32_t bootCount = 0;

static const char *resetReasonText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "POWERON (fresh power-up)";
    case ESP_RST_EXT:      return "EXT (reset pin)";
    case ESP_RST_SW:       return "SW (esp_restart / after flashing)";
    case ESP_RST_PANIC:    return "PANIC <-- CRASH, this is a bug";
    case ESP_RST_INT_WDT:  return "INT_WDT <-- interrupt watchdog";
    case ESP_RST_TASK_WDT: return "TASK_WDT <-- task watchdog";
    case ESP_RST_WDT:      return "WDT <-- other watchdog";
    case ESP_RST_BROWNOUT: return "BROWNOUT <-- power supply problem";
    case ESP_RST_SDIO:     return "SDIO";
    case ESP_RST_DEEPSLEEP:return "DEEPSLEEP";
    default:               return "UNKNOWN";
  }
}

// Reprinted every cycle, not just in setup(): attaching a serial reader to /dev/cu.*
// does not reset the board, so a setup()-only banner is missed on every late attach.
void printBanner() {
  Serial.println();
  Serial.println("=========================================");
  Serial.println(" GPIO 43 onboard-LED diagnostic");
  Serial.println("=========================================");
  Serial.printf("boot count (survives soft reset): %lu\n", (unsigned long)bootCount);
  Serial.printf("reset reason: %s\n", resetReasonText(esp_reset_reason()));
  Serial.printf("uptime: %lu ms  (keeps climbing => no reboot loop)\n",
                (unsigned long)millis());
  Serial.printf("free heap: %lu bytes\n", (unsigned long)ESP.getFreeHeap());
  Serial.printf("PSRAM size: %lu bytes  (0 means PSRAM is NOT enabled)\n",
                (unsigned long)ESP.getPsramSize());
  Serial.printf("driving GPIO %d with %d Hz PWM, %d-bit\n",
                BREATH_LED_PIN, PWM_FREQ, PWM_RES);
  Serial.println("-----------------------------------------");
}

void setup() {
  Serial.begin(115200);
  // USB CDC needs a moment to enumerate; without this the first lines are lost.
  uint32_t waitStart = millis();
  while (!Serial && (millis() - waitStart) < 3000) {
    delay(50);
  }

  bootCount++;
  printBanner();

  // ESP32 core 3.x API: attach pin directly, no manual channel bookkeeping.
  if (!ledcAttach(BREATH_LED_PIN, PWM_FREQ, PWM_RES)) {
    Serial.println("ERROR: ledcAttach failed on GPIO 43");
  }
  ledcWrite(BREATH_LED_PIN, 0);
}

void breathePhase() {
  Serial.println("[phase 1] 3 slow breaths (fade up, fade down)");
  for (uint8_t cycle = 0; cycle < 3; cycle++) {
    for (int duty = 0; duty <= 255; duty++) {
      ledcWrite(BREATH_LED_PIN, duty);
      delay(4);
    }
    for (int duty = 255; duty >= 0; duty--) {
      ledcWrite(BREATH_LED_PIN, duty);
      delay(4);
    }
    Serial.printf("  breath %u/3 done at %lu ms\n", cycle + 1, (unsigned long)millis());
  }
}

void blinkPhase() {
  Serial.println("[phase 2] 5 fast blinks at full brightness");
  for (uint8_t i = 0; i < 5; i++) {
    ledcWrite(BREATH_LED_PIN, 255);
    delay(120);
    ledcWrite(BREATH_LED_PIN, 0);
    delay(120);
  }
}

void loop() {
  printBanner();
  breathePhase();
  blinkPhase();

  Serial.println("[phase 3] fully OFF for 2 s, then repeating");
  ledcWrite(BREATH_LED_PIN, 0);
  delay(2000);
}
