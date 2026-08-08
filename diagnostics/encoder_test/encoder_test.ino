// Encoder + button test for the CrowPanel 2.1" rotary.
//
// NOT a copy of example/Simple example/Encoder_code — that example is for a DIFFERENT
// board and is wrong on all three pins for this one:
//     it declares A=45 (here: LCD B2), B=42 (here: encoder A), SW=41 (here: LCD PCLK).
// Flashing it would poll display data lines and hang an interrupt on the pixel clock,
// while half-working (its "B" really is encoder A) — misleading rather than failing.
//
// Pins below are from the schematic (Eagle_SCH&PCB/*.sch), cross-checked against the
// working RotaryScreen_2_1 demo:
//     ENCODER_A -> J1.A -> GPIO 42   (external 10K pull-up R19)
//     ENCODER_B -> J1.B -> GPIO 4    (external 10K pull-up R20)
//     ENCODER_SW ------> PCF8574 P5  (I2C expander @ 0x21, NOT a GPIO; R21 pull-up)
// The button MUST be polled over I2C. digitalRead() can never see it.
//
// Deliberately does not touch PCF8574 P0/P2/P3/P4 (touch reset/int, LCD power/reset), so
// the panel stays dark for this test. That is expected, not a fault.
//
// Goal beyond "does it work": establish how many edge events one physical detent yields,
// which sets the Sonos volume STEP mapping.
//
// MEASURED: counting only RISING edges on A gives 1 event per 2 detents — half the knob's
// resolution is discarded. This encoder rests alternately at (A=0,B=0) and (A=1,B=1), so
// only every second detent produces a rising edge. Counting BOTH edges of A yields 1 event
// per detent. Both counters run side by side below so one turn measures both.

#include <Wire.h>
#include "PCF8574.h"

#define I2C_SDA_PIN    38
#define I2C_SCL_PIN    39
#define ENCODER_A_PIN  42
#define ENCODER_B_PIN  4

PCF8574 pcf8574(0x21);

static const unsigned long DEBOUNCE_MS     = 50;
static const unsigned long DOUBLECLICK_MS  = 300;
static const unsigned long HEARTBEAT_MS    = 3000;

int  lastStateA   = 0;

// OLD logic: rising edges of A only -> measured at 1 event per 2 detents.
long position     = 0;     // net: + = CW, - = CCW
long cwCount      = 0;
long ccwCount     = 0;

// NEW logic: both edges of A -> should be 1 event per detent.
long positionBoth = 0;
long cwBoth       = 0;
long ccwBoth      = 0;

long buttonPresses = 0;
long doubleClicks  = 0;

uint8_t lastSW           = HIGH;
unsigned long lastPressMs = 0;
int  pendingClicks       = 0;
unsigned long lastHeartbeat = 0;
bool pcfOk = false;

void setup() {
  Serial.begin(115200);
  uint32_t waitStart = millis();
  while (!Serial && (millis() - waitStart) < 3000) {
    delay(50);
  }

  Serial.println();
  Serial.println("=========================================");
  Serial.println(" Encoder + button test (A=42, B=4, SW=PCF8574 P5)");
  Serial.println("=========================================");

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  pcf8574.pinMode(P5, INPUT_PULLUP);   // encoder push button
  pcfOk = pcf8574.begin();
  Serial.printf("PCF8574 @ 0x21: %s\n", pcfOk ? "OK" : "FAILED (button cannot be read)");

  pinMode(ENCODER_A_PIN, INPUT);       // external 10K pull-ups, no INPUT_PULLUP needed
  pinMode(ENCODER_B_PIN, INPUT);
  lastStateA = digitalRead(ENCODER_A_PIN);

  Serial.printf("initial levels: A=%d B=%d\n",
                digitalRead(ENCODER_A_PIN), digitalRead(ENCODER_B_PIN));
  Serial.println("Turn the knob and press the button. Heartbeat every 3 s.");
  Serial.println("-----------------------------------------");
  lastHeartbeat = millis();
}

void pollEncoder() {
  int stateA = digitalRead(ENCODER_A_PIN);
  if (stateA == lastStateA) return;               // no edge on A

  int stateB = digitalRead(ENCODER_B_PIN);

  // Direction rule, MEASURED on this board and independent of edge polarity:
  //     B == A -> CW        B != A -> CCW
  // Verified: turning CW yields (A=1,B=1) on rising edges and (A=0,B=0) on falling ones,
  // i.e. A == B at every edge in one direction. Matches Elecrow's demo, which treats
  // B != A as CCW. Do NOT "simplify" this to (B == HIGH) — that is correct on rising
  // edges only and silently inverts on falling ones, netting position to zero.
  bool cw = (stateB == stateA);

  // NEW: both edges -> one count per detent.
  if (cw) { cwBoth++;  positionBoth++; }
  else    { ccwBoth++; positionBoth--; }

  // OLD: rising edges only, kept for direct comparison in the same run.
  if (stateA == HIGH) {
    if (cw) { cwCount++;  position++; }
    else    { ccwCount++; position--; }
  }

  Serial.printf("%-4s edge=%-7s  both=%-4ld rising=%-4ld  (A=%d B=%d)\n",
                cw ? "CW" : "CCW",
                stateA == HIGH ? "rising" : "falling",
                positionBoth, position, stateA, stateB);

  lastStateA = stateA;
}

void pollButton() {
  if (!pcfOk) return;

  uint8_t sw = pcf8574.digitalRead(P5, true);   // active low
  unsigned long now = millis();

  if (sw == LOW && lastSW == HIGH && (now - lastPressMs) > DEBOUNCE_MS) {
    lastPressMs = now;
    buttonPresses++;
    pendingClicks++;
    Serial.printf("BUTTON PRESS  (total=%ld)\n", buttonPresses);
  } else if (sw == HIGH && lastSW == LOW) {
    Serial.println("BUTTON RELEASE");
  }
  lastSW = sw;

  // Resolve single vs double click once the double-click window has expired.
  if (pendingClicks > 0 && (now - lastPressMs) > DOUBLECLICK_MS) {
    if (pendingClicks == 1) {
      Serial.println("  -> CLICK");
    } else {
      doubleClicks++;
      Serial.printf("  -> DOUBLE CLICK (x%d)\n", pendingClicks);
    }
    pendingClicks = 0;
  }
}

void loop() {
  pollEncoder();
  pollButton();

  unsigned long now = millis();
  if (now - lastHeartbeat >= HEARTBEAT_MS) {
    lastHeartbeat = now;
    Serial.printf("[hb] uptime=%lus  BOTH: pos=%ld cw=%ld ccw=%ld | RISING: pos=%ld cw=%ld"
                  " ccw=%ld | presses=%ld dbl=%ld  A=%d B=%d SW=%d\n",
                  now / 1000,
                  positionBoth, cwBoth, ccwBoth,
                  position, cwCount, ccwCount,
                  buttonPresses, doubleClicks,
                  digitalRead(ENCODER_A_PIN), digitalRead(ENCODER_B_PIN),
                  pcfOk ? pcf8574.digitalRead(P5, true) : -1);
  }

  delay(1);   // fast enough for a hand-turned knob, slow enough not to flood I2C
}
