// Sonos rotary remote — all three specs.txt activities, verified on hardware.
//
// Volume (single and multi-group), grouping (join / detach with coordination handover),
// line-in, Screen 1, encoder, dial press, touch.
//
// Design rule to preserve: a tap NEVER sets its own button state. LVGL's automatic toggle is
// cleared immediately and the state comes only from real Sonos state via workerRefreshState,
// so the screen can never claim something happened that did not.
//
// Threading rule to preserve: no Sonos HTTP call may run on the Arduino loop — see CLAUDE.md.
#define SONOS_ALLOW_VOLUME_WRITES 1

#include <WiFi.h>
#include <esp_system.h>

#include "secrets.h"
#include "panel.h"
#include "ui_screen.h"
#include "sonos.h"

// Encoder A/B are plain GPIOs; the BUTTON is on PCF8574 P5 and is read through panel.
#define ENCODER_A_PIN  42
#define ENCODER_B_PIN  4

// Room names as Sonos reports them (verified: "Main" = Era 300, "Stereo" = Era 100 pair).
static const char *ROOM_MAIN   = "Main";
static const char *ROOM_STEREO = "Stereo";

// --- tuning -------------------------------------------------------------------------
static const int      VOLUME_STEP        = 2;     // 1 detent == 1 count (measured)
static const uint32_t FLUSH_INTERVAL_MS  = 80;    // coalesce fast turns
static const uint32_t POLL_VOLUME_MS     = 5000;  // catch app/Alexa/Airplay changes
static const uint32_t STATE_REFRESH_MS   = 10000; // re-read grouping/line-in for the toggles
static const uint32_t STATUS_HOLD_MS     = 2500;  // transient status messages expire
// Largest change any single flush may apply. Coalescing merges a fast spin into one call,
// which measured up to +/-14 — enough for one flick of the wrist to slam the system to 0 or
// 100. Capping keeps a fast spin fast (a flush every 80 ms, so up to ~75 points/second)
// while making the full range impossible to cross accidentally.
static const int      MAX_ADJUST_PER_FLUSH = 6;
static const uint32_t BUTTON_DEBOUNCE_MS = 50;
static const uint32_t WIFI_TIMEOUT_MS    = 20000;

// --- state ----------------------------------------------------------------------------
int      lastStateA   = 0;
int      pendingSteps = 0;
int      currentVolume = -1;
int      uiMainVol    = -1;   // UI-thread mirrors, for the optimistic echo
int      uiStereoVol  = -1;
uint32_t lastFlushMs  = 0;
uint32_t lastPollMs   = 0;
uint32_t lastStateMs  = 0;
uint32_t lastButtonMs = 0;
uint8_t  lastSW       = HIGH;

// --- worker task ------------------------------------------------------------------------
// EVERY Sonos HTTP call happens on this task, pinned to core 0. The Arduino loop (core 1)
// must never block on the network: while it waits, the encoder is not polled (detents are
// silently lost) and lv_timer_handler() does not run (frozen screen, ignored taps). That
// was the cause of the sluggishness.
enum class Cmd : uint8_t { VolumeDelta, RoomVolumeDelta, SyncStereoToMain,
                           ToggleMain, ToggleStereo, LineIn, RefreshState };

// Which room a RoomVolumeDelta targets.
enum class Room : uint8_t { None, Main, Stereo };

struct Job {
  Cmd cmd;
  int arg;
  bool user;    // true only for deliberate user actions -> drives the busy ring
  Room room;    // for RoomVolumeDelta
};

QueueHandle_t jobQueue = nullptr;

// Shared with the UI thread. 32-bit aligned scalars only — no Strings, no locks needed.
volatile int  shVolume      = -1;   // GROUP volume (Sonos's average across members)
volatile int  shMainVol     = -1;   // each room's OWN level
volatile int  shStereoVol   = -1;
volatile bool shMainActive  = false;
volatile bool shStereoActive= false;
volatile bool shLineIn      = false;
volatile bool shBusy        = false;
volatile bool shHaveCoord   = false;
volatile bool shTargetGone  = false;   // SONOS_TARGET_ROOM absent from the topology
volatile int  shStatusCode  = 0;   // see statusText()
volatile uint32_t shStateSeq = 0;  // bumped whenever the flags above change

// Outstanding USER-initiated jobs. The busy ring follows this, not shBusy — otherwise the
// 5 s / 10 s background refreshes would make it spin on their own every few seconds, which
// reads as the device doing something when nobody touched it.
volatile int  shUserJobs    = 0;

enum StatusCode : int {
  ST_NONE = 0, ST_CONNECTING, ST_FINDING, ST_READY, ST_NO_SONOS,
  ST_WORKING, ST_GROUPING, ST_DETACHING, ST_LINEIN_OK, ST_FAILED, ST_WIFI_FAILED,
  ST_TARGET_GONE, ST_SYNCED
};

RTC_DATA_ATTR static uint32_t bootCount = 0;

const char *resetReasonText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "POWERON";
    case ESP_RST_EXT:      return "EXT";
    case ESP_RST_SW:       return "SW (after flashing)";
    case ESP_RST_PANIC:    return "PANIC <-- crash";
    case ESP_RST_INT_WDT:  return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT <-- power supply";
    default:               return "UNKNOWN";
  }
}

// ------------------------------------------------------------------------------- WiFi

bool connectWiFi() {
  if (String(WIFI_SSID) == "FILL_ME_IN" || String(WIFI_SSID).isEmpty()) {
    Serial.println("[wifi] secrets.h still holds placeholders");
    ui::setStatus("no wifi config");
    return false;
  }

  Serial.printf("[wifi] connecting to \"%s\" ...\n", WIFI_SSID);
  ui::setStatus("connecting wifi ...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t deadline = millis() + WIFI_TIMEOUT_MS;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    delay(100);
    panel::loop();          // keep the screen alive while we wait
  }

  if (WiFi.status() != WL_CONNECTED) {
    // Most common cause here: a 5 GHz-only SSID — this radio is 2.4 GHz only, and the
    // failure is indistinguishable from a wrong password.
    Serial.printf("[wifi] FAILED (status=%d)\n", WiFi.status());
    ui::setStatus("wifi failed");
    return false;
  }
  Serial.printf("[wifi] connected: %s (%d dBm)\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

// --------------------------------------------------------------------------- discovery

void resolveAndReport() {
  ui::setStatus("finding sonos ...");
  sonos::ResolveResult r = sonos::resolveCoordinator();
  if (r != sonos::ResolveResult::Ok) {
    Serial.printf("[sonos] resolve failed: %s\n", sonos::resolveResultText(r));
    ui::setStatus(sonos::resolveResultText(r));
    return;
  }
  if (sonos::getGroupVolume(currentVolume)) {
    ui::setVolume(currentVolume);
    ui::setStatus(sonos::coordinatorRoom());
    Serial.printf("[sonos] %s @ %s volume=%d, %u zones\n", sonos::coordinatorRoom(),
                  sonos::coordinatorIp(), currentVolume, sonos::zoneCount());
    for (uint8_t i = 0; i < sonos::zoneCount(); i++) {
      const sonos::Zone *z = sonos::zoneAt(i);
      if (z) Serial.printf("   zone \"%s\" %s%s\n", z->room.c_str(), z->ip.c_str(),
                           z->invisible ? " [bonded]" : "");
    }
  } else {
    ui::setStatus("volume read failed");
  }
}

// Forward declarations — the worker task and its queue are defined further down, but the
// encoder path above needs to enqueue into them.
void enqueue(Cmd cmd, int arg, Room room);
void enqueueUser(Cmd cmd);
void workerRefreshState();

// ----------------------------------------------------------------------------- encoder

// Measured logic (CLAUDE.md): count BOTH edges of A, direction from B where B == A is CW.
void pollEncoder() {
  int stateA = digitalRead(ENCODER_A_PIN);
  if (stateA == lastStateA) return;
  int stateB = digitalRead(ENCODER_B_PIN);
  lastStateA = stateA;
  int dir = (stateB == stateA) ? 1 : -1;
  pendingSteps += dir;

  // Optimistic echo on THIS frame; the worker reconciles with the real value later.
  // With a room selected on screen 2, the dial moves only that room, so echo THAT number
  // instead of the group figure.
  ui::Selection sel = ui::selection();
  if (sel == ui::Selection::Main && uiMainVol >= 0) {
    uiMainVol = constrain(uiMainVol + dir * VOLUME_STEP, 0, 100);
    ui::setRoomVolumes(uiMainVol, uiStereoVol);
  } else if (sel == ui::Selection::Stereo && uiStereoVol >= 0) {
    uiStereoVol = constrain(uiStereoVol + dir * VOLUME_STEP, 0, 100);
    ui::setRoomVolumes(uiMainVol, uiStereoVol);
  } else if (currentVolume >= 0) {
    currentVolume = constrain(currentVolume + dir * VOLUME_STEP, 0, 100);
    ui::setVolume(currentVolume);
  }
}

void flushVolume() {
  if (pendingSteps == 0) return;
  if (millis() - lastFlushMs < FLUSH_INTERVAL_MS) return;

  int steps = pendingSteps;
  pendingSteps = 0;
  lastFlushMs = millis();

#if SONOS_ALLOW_VOLUME_WRITES
  switch (ui::selection()) {
    case ui::Selection::Main:
      enqueue(Cmd::RoomVolumeDelta, steps * VOLUME_STEP, Room::Main);   break;
    case ui::Selection::Stereo:
      enqueue(Cmd::RoomVolumeDelta, steps * VOLUME_STEP, Room::Stereo); break;
    default:
      enqueue(Cmd::VolumeDelta, steps * VOLUME_STEP, Room::None);       break;
  }
#else
  Serial.printf("[vol] DRY RUN: would send %+d\n", steps * VOLUME_STEP);
#endif
}

// ------------------------------------------------------------------------------ buttons

// ---- worker task: all network I/O lives here (core 0) ----------------------------------

// Re-read real Sonos state into the shared flags. Never set from a tap: if an action
// silently fails the screen must show the truth, not our intent.
void workerRefreshState() {
  sonos::refreshTopology();                       // cheap: one POST, no SSDP

  bool grouped = sonos::roomsGrouped(ROOM_MAIN, ROOM_STEREO);

  bool playing = false;
  bool mainActive = sonos::isPlaying(ROOM_MAIN, playing) ? playing : false;

  bool stereoActive = grouped;
  if (!stereoActive && sonos::isPlaying(ROOM_STEREO, playing)) stereoActive = playing;

  bool lineIn = false;
  sonos::isLineInActive(ROOM_MAIN, lineIn);

  int vol = shVolume;
  sonos::getGroupVolume(vol);

  // Per-room levels: GetGroupVolume is the AVERAGE across members, so it cannot show that
  // Main and Stereo are set far apart. Two extra calls per refresh, on the worker only.
  int mainVol = -1, stereoVol = -1;
  if (!sonos::getRoomVolume(ROOM_MAIN, mainVol))     mainVol = -1;
  if (!sonos::getRoomVolume(ROOM_STEREO, stereoVol)) stereoVol = -1;

  shMainActive   = mainActive;
  shStereoActive = stereoActive;
  shLineIn       = lineIn;
  shVolume       = vol;
  shMainVol      = mainVol;
  shStereoVol    = stereoVol;
  shHaveCoord    = sonos::haveCoordinator();
  shTargetGone   = sonos::targetRoomMissing();
  shStateSeq     = shStateSeq + 1;

  // Say so when the configured room is absent. Silently steering a different group while
  // the buttons look normal is worse than an honest message.
  if (!shHaveCoord)          shStatusCode = ST_NO_SONOS;
  else if (shTargetGone)     shStatusCode = ST_TARGET_GONE;
}

void workerTask(void *) {
  Job job;
  for (;;) {
    if (xQueueReceive(jobQueue, &job, portMAX_DELAY) != pdTRUE) continue;

    // Collapse any queued volume deltas into one call — a fast spin must not become a
    // backlog of HTTP requests that keeps moving the volume after the knob has stopped.
    if (job.cmd == Cmd::VolumeDelta) {
      Job next;
      while (uxQueueMessagesWaiting(jobQueue) > 0 &&
             xQueuePeek(jobQueue, &next, 0) == pdTRUE && next.cmd == Cmd::VolumeDelta) {
        xQueueReceive(jobQueue, &next, 0);
        job.arg += next.arg;
      }
    }

    shBusy = true;
    switch (job.cmd) {
      case Cmd::VolumeDelta: {
        if (job.arg == 0) break;

        // Cap AFTER merging — merging is what produces the huge values. The excess is
        // DISCARDED rather than carried into the next flush: carrying it over would make
        // the volume keep sliding after the knob has stopped, which feels broken on a
        // physical control. The effect is a rate limit (~6 per 80 ms flush), not a loss of
        // fine control — slow turns are never clamped.
        if (job.arg > MAX_ADJUST_PER_FLUSH || job.arg < -MAX_ADJUST_PER_FLUSH) {
          int capped = job.arg > 0 ? MAX_ADJUST_PER_FLUSH : -MAX_ADJUST_PER_FLUSH;
          Serial.printf("[vol] %+d capped to %+d (fast spin)\n", job.arg, capped);
          job.arg = capped;
        }

        int vol = shVolume;
        uint8_t rooms = sonos::adjustVolumeAllRooms(job.arg, vol);
        if (rooms > 0) {
          shVolume = vol;
          // Bump the sequence so the UI reconciles the optimistic echo right away. Without
          // this, a capped fast spin would leave the screen showing a number that outran
          // the speakers until the next 5 s poll.
          shStateSeq = shStateSeq + 1;
          shStatusCode = ST_READY;
        } else {
          shStatusCode = ST_FAILED;
          sonos::refreshTopology();
        }
        Serial.printf("[vol] %+d -> %d (%u rooms)\n", job.arg, vol, rooms);
        break;
      }
      case Cmd::RoomVolumeDelta: {
        if (job.arg == 0) break;
        if (job.arg > MAX_ADJUST_PER_FLUSH || job.arg < -MAX_ADJUST_PER_FLUSH) {
          job.arg = job.arg > 0 ? MAX_ADJUST_PER_FLUSH : -MAX_ADJUST_PER_FLUSH;
        }
        const char *room = (job.room == Room::Main) ? ROOM_MAIN : ROOM_STEREO;
        int nv = 0;
        if (sonos::setRelativeRoomVolume(room, job.arg, nv)) {
          if (job.room == Room::Main) shMainVol = nv; else shStereoVol = nv;
          // The GROUP figure is the average across members, so changing one room moves it
          // too — re-read rather than trying to predict it.
          int gv = shVolume;
          if (sonos::getGroupVolume(gv)) shVolume = gv;
          shStateSeq = shStateSeq + 1;
          Serial.printf("[vol] %s %+d -> %d\n", room, job.arg, nv);
        } else {
          shStatusCode = ST_FAILED;
          Serial.printf("[vol] %s %+d FAILED\n", room, job.arg);
        }
        break;
      }
      case Cmd::SyncStereoToMain: {
        shStatusCode = ST_WORKING;
        int mainVol = -1;
        if (!sonos::getRoomVolume(ROOM_MAIN, mainVol)) {
          shStatusCode = ST_FAILED;
          Serial.println("[sync] could not read Main's volume");
          break;
        }
        bool ok = sonos::setRoomVolume(ROOM_STEREO, mainVol);
        Serial.printf("[sync] Stereo := Main (%d): %s\n", mainVol, ok ? "ok" : "FAILED");
        vTaskDelay(pdMS_TO_TICKS(250));
        workerRefreshState();
        shStatusCode = ok ? ST_SYNCED : ST_FAILED;
        break;
      }
      case Cmd::ToggleMain: {
        bool grouped = sonos::roomsGrouped(ROOM_MAIN, ROOM_STEREO);
        shStatusCode = grouped ? ST_DETACHING : ST_GROUPING;
        // The room you tap is the one that MOVES.
        bool ok = grouped ? sonos::detachRoom(ROOM_MAIN)
                          : sonos::joinRoomTo(ROOM_MAIN, ROOM_STEREO);
        Serial.printf("[ui] Main -> %s: %s\n", grouped ? "detach" : "join",
                      ok ? "ok" : "FAILED");
        vTaskDelay(pdMS_TO_TICKS(400));           // let Sonos settle before re-reading
        workerRefreshState();
        shStatusCode = ok ? ST_READY : ST_FAILED;
        break;
      }
      case Cmd::ToggleStereo: {
        bool grouped = sonos::roomsGrouped(ROOM_MAIN, ROOM_STEREO);
        shStatusCode = grouped ? ST_DETACHING : ST_GROUPING;
        bool ok = grouped ? sonos::detachRoom(ROOM_STEREO)
                          : sonos::joinRoomTo(ROOM_STEREO, ROOM_MAIN);
        Serial.printf("[ui] Stereo -> %s: %s\n", grouped ? "detach" : "join",
                      ok ? "ok" : "FAILED");
        vTaskDelay(pdMS_TO_TICKS(400));
        workerRefreshState();
        shStatusCode = ok ? ST_READY : ST_FAILED;
        break;
      }
      case Cmd::LineIn: {
        shStatusCode = ST_WORKING;
        bool ok = sonos::playLineIn(ROOM_MAIN);
        Serial.printf("[ui] Records -> line-in: %s\n", ok ? "ok" : "FAILED");
        vTaskDelay(pdMS_TO_TICKS(400));
        workerRefreshState();
        shStatusCode = ok ? ST_LINEIN_OK : ST_FAILED;
        break;
      }
      case Cmd::RefreshState:
        workerRefreshState();
        if (shStatusCode == ST_FINDING || shStatusCode == ST_NONE) shStatusCode = ST_READY;
        break;
    }
    shBusy = false;
    if (job.user && shUserJobs > 0) shUserJobs = shUserJobs - 1;
  }
}

void enqueue(Cmd cmd, int arg, Room room = Room::None) {
  if (!jobQueue) return;
  Job job{cmd, arg, false, room};
  // Non-blocking on purpose: dropping a redundant refresh beats stalling the UI thread.
  xQueueSend(jobQueue, &job, 0);
}

// A deliberate user action: shows the busy ring until the worker finishes it.
void enqueueUser(Cmd cmd) {
  if (!jobQueue) return;
  Job job{cmd, 0, true, Room::None};
  shUserJobs = shUserJobs + 1;
  if (xQueueSend(jobQueue, &job, 0) != pdTRUE) {
    shUserJobs = shUserJobs - 1;      // never leave the ring spinning forever
  }
}

// Screen 2. Selection is mutually exclusive and toggles off when the same room is tapped
// again, so the dial always has one unambiguous target.
void onUiAction(ui::Action a) {
  switch (a) {
    case ui::Action::Sync:
      if (!shHaveCoord) { shStatusCode = ST_NO_SONOS; return; }
      ui::setBusy(true);
      shStatusCode = ST_WORKING;
      enqueueUser(Cmd::SyncStereoToMain);
      break;
    case ui::Action::SelectMain:
      ui::setSelection(ui::selection() == ui::Selection::Main ? ui::Selection::None
                                                             : ui::Selection::Main);
      Serial.printf("[ui] selection -> %s\n",
                    ui::selection() == ui::Selection::Main ? "Main" : "none");
      break;
    case ui::Action::SelectStereo:
      ui::setSelection(ui::selection() == ui::Selection::Stereo ? ui::Selection::None
                                                               : ui::Selection::Stereo);
      Serial.printf("[ui] selection -> %s\n",
                    ui::selection() == ui::Selection::Stereo ? "Stereo" : "none");
      break;
  }
}

// Leaving screen 2 clears the selection. Otherwise the dial would silently stay in
// single-room mode while screen 1 gives no hint that it is — a hidden mode is worse than
// an extra tap.
void onScreenChange(ui::Screen s) {
  Serial.printf("[ui] screen -> %s\n", s == ui::Screen::Home ? "home" : "volume");
  if (s == ui::Screen::Home && ui::selection() != ui::Selection::None) {
    ui::setSelection(ui::Selection::None);
    Serial.println("[ui] selection cleared (left volume screen)");
  }
}

void onUiTap(ui::Button b) {
  // LVGL already flipped the checkbox on click. Clear it at once — state is owned by the
  // worker and must reflect Sonos, not the tap.
  ui::setActive(b, false);

  if (!shHaveCoord) {
    shStatusCode = ST_NO_SONOS;
    return;
  }

  // Immediate feedback, then hand off. The screen updates on this frame; the network call
  // happens on core 0.
  ui::setBusy(true);
  shStatusCode = ST_WORKING;
  switch (b) {
    case ui::Button::Records: enqueueUser(Cmd::LineIn);       break;
    case ui::Button::Main:    enqueueUser(Cmd::ToggleMain);   break;
    case ui::Button::Stereo:  enqueueUser(Cmd::ToggleStereo); break;
  }
}

// Pressing the dial itself toggles Stereo — the same action as the on-screen Stereo
// button, reachable without looking at the display.
void pollButton() {
  // Rate-limited: this is an I2C transaction, and the loop now runs with no delay. Reading
  // it every iteration floods the bus that the touch controller shares, which makes both
  // the button and touch unreliable. 25 ms is far faster than any human press.
  static uint32_t lastReadMs = 0;
  uint32_t now = millis();
  if (now - lastReadMs < 25) return;
  lastReadMs = now;

  uint8_t sw = panel::readButtonRaw();      // active low, via I2C expander

  if (sw == LOW && lastSW == HIGH && (now - lastButtonMs) > BUTTON_DEBOUNCE_MS) {
    lastButtonMs = now;
    Serial.println("[btn] dial press -> toggle Stereo");
    if (shHaveCoord) {
      ui::setBusy(true);
      shStatusCode = ST_WORKING;
      enqueueUser(Cmd::ToggleStereo);
    } else {
      shStatusCode = ST_NO_SONOS;
    }
  }
  lastSW = sw;
}

// -------------------------------------------------------------------------------- main

void setup() {
  Serial.begin(115200);
  uint32_t waitStart = millis();
  while (!Serial && (millis() - waitStart) < 3000) delay(50);

  bootCount++;
  Serial.println();
  Serial.println("=========================================");
  Serial.println(" Sonos rotary remote — Phase 2 (Screen 1)");
  Serial.printf(" boot #%lu, reset: %s\n",
                (unsigned long)bootCount, resetReasonText(esp_reset_reason()));
  Serial.printf(" volume writes: %s\n", SONOS_ALLOW_VOLUME_WRITES ? "LIVE" : "DRY RUN");
  Serial.println("=========================================");
  Serial.flush();

  // Display first, so there is visible feedback during the slow WiFi/discovery steps.
  if (!panel::begin()) {
    Serial.println("[boot] panel::begin reported a problem — continuing headless");
  }
  ui::build(onUiTap, onUiAction, onScreenChange);
  panel::loop();

  pinMode(ENCODER_A_PIN, INPUT);     // external 10K pull-ups on both lines
  pinMode(ENCODER_B_PIN, INPUT);
  lastStateA = digitalRead(ENCODER_A_PIN);

  if (connectWiFi()) {
    sonos::begin(SONOS_COORDINATOR_IP, SONOS_TARGET_ROOM);
    resolveAndReport();
  }

  // Worker on core 0. The Arduino loop runs on core 1 and must stay free for LVGL and the
  // encoder; 8 KB stack covers HTTPClient plus the topology String parsing.
  jobQueue = xQueueCreate(12, sizeof(Job));
  xTaskCreatePinnedToCore(workerTask, "sonos", 8192, nullptr, 1, nullptr, 0);
  enqueue(Cmd::RefreshState, 0);

  lastPollMs = millis();
  lastStateMs = millis();
}

const char *statusText(int code) {
  switch (code) {
    case ST_CONNECTING:  return "connecting wifi ...";
    case ST_FINDING:     return "finding sonos ...";
    case ST_NO_SONOS:    return "no sonos";
    case ST_WORKING:     return "working ...";
    case ST_GROUPING:    return "grouping ...";
    case ST_DETACHING:   return "detaching ...";
    case ST_LINEIN_OK:   return "Plattenspieler";
    case ST_FAILED:      return "failed";
    case ST_WIFI_FAILED: return "wifi failed";
    case ST_TARGET_GONE: return "Main offline";
    case ST_SYNCED:      return "synced";
    // Idle shows the SONOS logo alone — no room name. Anything transient appears beneath
    // it and then expires back to this.
    case ST_READY:       return "";
    default:             return "";
  }
}

// Push worker results into LVGL. Runs on the UI thread only — LVGL is not thread-safe, so
// the worker never touches it.
void syncUiFromWorker() {
  static uint32_t lastSeq = 0xFFFFFFFF;
  static int lastStatus = -1;
  static bool lastBusy = false;
  static uint32_t statusSetAt = 0;

  // Ring follows USER jobs only — background refreshes must never make it spin.
  bool wantBusy = (shUserJobs > 0);
  if (wantBusy != lastBusy) {
    lastBusy = wantBusy;
    ui::setBusy(wantBusy);
  }

  if (shStateSeq != lastSeq) {
    lastSeq = shStateSeq;
    ui::setActive(ui::Button::Main,    shMainActive);
    ui::setActive(ui::Button::Stereo,  shStereoActive);
    ui::setActive(ui::Button::Records, shLineIn);
    if (shVolume >= 0) {
      currentVolume = shVolume;      // reconcile the optimistic echo with reality
      ui::setVolume(currentVolume);
    }
    uiMainVol   = shMainVol;
    uiStereoVol = shStereoVol;
    ui::setRoomVolumes(uiMainVol, uiStereoVol);
  }

  if (shStatusCode != lastStatus) {
    lastStatus = shStatusCode;
    statusSetAt = millis();
    ui::setStatus(statusText(lastStatus));
  } else if (statusSetAt && millis() - statusSetAt > STATUS_HOLD_MS && !shBusy) {
    // Transient messages must not linger: "detaching ..." shown minutes later is a lie.
    if (lastStatus != ST_READY && !(shTargetGone || !shHaveCoord)) {
      lastStatus = ST_READY;
      shStatusCode = ST_READY;
      ui::setStatus(statusText(ST_READY));
    }
    statusSetAt = 0;
  }
}

void loop() {
  pollEncoder();      // latency-critical: must run far more often than anything else
  flushVolume();
  pollButton();
  syncUiFromWorker();
  panel::loop();

  // Both timers only ENQUEUE — no network call ever runs on this thread.
  //
  // NOT gated on shHaveCoord. Gating recovery on already having a coordinator is a
  // deadlock: lose it once (a speaker powers off) and the refresh that would find it again
  // never runs. The refresh IS the recovery path, so it must always be allowed.
  if (millis() - lastStateMs >= STATE_REFRESH_MS) {
    lastStateMs = millis();
    if (pendingSteps == 0) enqueue(Cmd::RefreshState, 0);
  }

  if (millis() - lastPollMs >= POLL_VOLUME_MS) {
    lastPollMs = millis();
    if (pendingSteps == 0 && !shBusy) enqueue(Cmd::RefreshState, 0);
  }
}
