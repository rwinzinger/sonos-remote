# CLAUDE.md — Sonos Rotary Controller (Elecrow CrowPanel 2.1")

Guidance for Claude Code working in this repo. Read this fully before writing or
compiling anything. Placeholders in `<...>` must be filled in by the user.

## What we are building

A physical volume knob for a Sonos system. A rotary encoder on the Elecrow
CrowPanel 2.1" controls the **group volume** of a Sonos group; the round display
shows the current volume (and, later, now-playing). The firmware talks to Sonos
over the LAN via the local UPnP API — no cloud, no Home Assistant.

## Hardware (verified against the Elecrow repo)

- Board: Elecrow CrowPanel 2.1" HMI ESP32-S3 Rotary Display
- MCU: ESP32-S3R8 — 8 MB PSRAM (OPI), 16 MB flash, 2.4 GHz WiFi + BLE
- Display: 480×480 round IPS, controller **ST7701S** (RGB-parallel, NOT SPI),
  driven via **Arduino_GFX** (`GFX_Library_for_Arduino` -> `Arduino_ESP32RGBPanel`)
- Touch: capacitive, **CST-series** (`Adafruit_CST8XX` / CST328), on I2C
- LEDs: **there is NO MCU-controllable LED on this board.** The schematic
  (`Eagle_SCH&PCB/*.sch`, the authority) contains exactly one LED part: `PWR`, a green
  power indicator hardwired between GND and the 5 V rail, on no GPIO. No WS2812, no
  ambient RGB LED. The only light the firmware can control is the **LCD backlight**
  (`BL_PWM` = GPIO 6). Ignore the readme's "LED ambient light" spec line — it does not
  match this hardware revision. Verified empirically: see `diagnostics/led43_test/`.
- Input: rotary encoder + push button; capacitive touch
- Power: USB-C 5 V (always tethered)

### Pin map (verified against the schematic, not just the demo code)

- I2C: SDA = GPIO 38, SCL = GPIO 39
- Encoder: A = GPIO 42, B = GPIO 4
- Backlight: GPIO 6 (net `BL_PWM`) — the ONLY controllable light
- GPIO 43 / 44 = **U0TXD / U0RXD**, routed through level-shift MOSFETs to the 12-pin
  FPC connector `J10`. This is the UART/programming header — **not** an LED.
- **PCF8574 I2C IO-expander @ address 0x21** — several signals live here, NOT on GPIOs:
  - P0 = touch reset, P2 = touch interrupt, P3 = LCD power, P4 = LCD reset,
    P5 = **encoder button** (INPUT_PULLUP)
- LCD RGB bus pins (handled by Arduino_ESP32RGBPanel):
  1,2,3,5,7,8,9,10,11,12,13,14,15,16,17,18,21,40,41,45,46,47,48

**Gotcha:** the encoder BUTTON is read through the PCF8574 over I2C, not with
`digitalRead`. LCD power/reset and touch reset/interrupt are also on the expander,
so the expander must be initialised before the display.

**Gotcha: RATE-LIMIT the button read.** The touch controller and the PCF8574 share one I2C
bus. Polling the button every `loop()` iteration (the loop has no delay since the async
refactor) floods that bus and the presses stop registering — the dial click silently
"disappeared" this way. Reading it every 25 ms fixed it, and is still far faster than any
human press. If touch or the button ever get flaky, look at I2C traffic volume first.

**Gotcha:** GPIO 48 is an LCD **blue** data line — schematic net `B3`, passed to
`Arduino_ESP32RGBPanel` as its `B2` argument (the schematic's B1..B5 labels are offset by
one from the driver's B0..B4 argument names; schematic `B2` = GPIO 45). Never drive
GPIO 48 directly — anything bit-banging it is fighting the display bus.

**Authority order when sources disagree:** schematic (`Eagle_SCH&PCB/*.sch`, XML and
greppable) > demo code > readme. All three conflict about the LEDs, and the schematic
was right. Elecrow's own comment in `RotaryScreen_2_1.ino` calls GPIO 43 an "onboard
LED" (板载LED) — it is not; that pin is UART0 TX.

## Toolchain — use the CLI, never the GUI

Build / flash / serial all happen through `arduino-cli` so you can iterate
autonomously.

- ESP32 core: **`esp32:esp32` @ 2.0.17 — NOT 3.x.** This is mandatory, not a preference:
  the bundled `Arduino_GFX` 1.3.1 (the ST7701S RGB driver) does not compile against core
  3.x / ESP-IDF 5. On 3.3.11 it fails in the LIBRARY, not the sketch:
  - `Arduino_DataBus.h:161` — `LIST_HEAD` / `i80_device_list` undeclared (IDF list macros
    changed)
  - `Arduino_ESP32RGBPanel.h:43` — `esp_lcd_rgb_panel_frame_trans_done_cb_t` renamed to
    `esp_lcd_rgb_panel_frame_buf_complete_cb_t`
  Core 3.x also removed `ledcSetup`/`ledcAttachPin` (used at 4 sites in the demo).
  Corroboration: Elecrow's own setup screenshot shows **Arduino IDE 1.8.19**, which core
  3.x does not support. Core 2.0.17 has everything the Sonos work needs.
  **`arduino-cli` keeps only ONE version per platform** — installing 2.0.17 silently
  uninstalled 3.3.11. Reverse with `arduino-cli core install esp32:esp32@3.3.11`.

### The build/flash/monitor loop

First verify the exact option keys for the installed core version:
`arduino-cli board details -b esp32:esp32:esp32s3`

```
FQBN="esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=huge_app,CDCOnBoot=cdc"
PORT="/dev/cu.usbmodem21401"   # NOT STABLE — re-check with: arduino-cli board list

arduino-cli compile --fqbn "$FQBN" --build-property upload.maximum_size=10485760 .
arduino-cli upload  -p "$PORT" --fqbn "$FQBN" .
```

Verified end-to-end on this hardware: compiles, flashes, runs, and reports
`PSRAM size = 8388608` (proof OPI PSRAM took effect). Note `PartitionScheme=custom` does
NOT exist in core 2.x — see the partition section below for how custom tables work.

Elecrow's own tested Tools settings (from `Snipaste_2025-08-18_18-37-15.png`): 16 MB
flash, **Huge APP**, OPI PSRAM, QIO 80 MHz, 921600 upload, USB Mode "Hardware CDC and
JTAG", **USB CDC On Boot = Disabled**. We deliberately deviate on that last one and use
`CDCOnBoot=cdc`: with it disabled, `Serial` goes to UART0 (GPIO 43/44 -> FPC header) and
you get no USB serial output. Verified: with `cdc` we get output on `/dev/cu.usbmodem21401`.

### Partitions — the demo does NOT fit any stock scheme

`RotaryScreen_2_1` links to **~6.36 MB**. The largest stock app partition is 3 MB
(`huge_app`), so it overflows by 202%. Cause: the bundled `UI` library is **40 MB of
SquareLine image arrays** (`ui_img_*.c`, several 3.3 MB each), and `lv_conf.h` enables
`LV_USE_DEMO_WIDGETS`, `LV_USE_DEMO_BENCHMARK`, `LV_BUILD_EXAMPLES` and every Montserrat
font 10-48.

Fix (used by `diagnostics/display_baseline/`) — flash is confirmed 16 MB via
`esptool flash_id`, so give the app 10 MB:

- Drop a `partitions.csv` in the sketch folder. Per `platform.txt:156` the core resolves
  partitions **source > variant > build.partitions**, so a sketch-local file silently
  overrides whatever `PartitionScheme=` says. No `custom` option needed.
- The size *check* still uses the selected scheme's ceiling, so add
  `--build-property upload.maximum_size=10485760` or the build fails with "text section
  exceeds available space" even though the real partition is fine.
- Always verify the table that actually got built before flashing a >3 MB app, or it will
  overrun into SPIFFS:
  `python3 ~/Library/Arduino15/packages/esp32/hardware/esp32/2.0.17/tools/gen_esp32part.py <build>/<sketch>.ino.partitions.bin`

### Reading serial non-interactively (agents: read this before concluding "no output")

`arduino-cli monitor` **produces nothing when its stdout is a pipe** — it buffers, and
killing it discards the buffer. A silent capture is an artifact of the tool, NOT a dead
board. This wasted real debugging time once; do not repeat it. Use instead:

```
stty -f /dev/cu.usbmodem21401 115200 raw -echo clocal
timeout 20 cat /dev/cu.usbmodem21401 > /tmp/serial.log ; cat /tmp/serial.log
```

- Opening `/dev/cu.*` does **not** reset the board, so a banner printed only in
  `setup()` is missed on every late attach. **Reprint diagnostics periodically from
  `loop()`** instead of once in `setup()`.
- To force a reset without reflashing:
  `~/Library/Arduino15/packages/esp32/tools/esptool_py/5.3.1/esptool --port
  /dev/cu.usbmodem21401 --after hard-reset --no-stub read-mac`
  Do not hold `cat` open on the port while doing this — the two contend and the reset
  silently does not happen.
- `esp_reset_reason()` reports `UNKNOWN` on this board after an esptool reset. Don't
  rely on it as a crash detector; use an `RTC_DATA_ATTR` boot counter plus `millis()`.

### Mandatory build options

- **PSRAM = OPI** — required for the 480×480 framebuffer; without it you run out of RAM.
- **Flash = 16 MB**, partition scheme with enough app space (LVGL builds are large).
- **USB CDC On Boot = Enabled** — otherwise `Serial` output never reaches USB and you
  are blind during debugging.

## Libraries — policy (read before installing ANYTHING)

Elecrow ships the **tested** set under `example/libraries/`. Copy its contents into
the Arduino libraries folder (`~/Documents/Arduino/libraries/`). Note that
`lv_conf.h` sits at `example/libraries/lv_conf.h` — one level ABOVE the `lvgl/`
folder, which is exactly where LVGL needs it.

**Status: already done.** The bundle is copied into `~/Documents/Arduino/libraries/`
(incl. `lv_conf.h`), so this step does not need repeating.

**Do NOT replace the bundled versions of the critical stack:**
- `lvgl` — the bundle is **8.3.6**, NOT the 8.3.11 the readme's table claims. 8.3.6 is
  what Elecrow actually tested and what is installed. Do not "correct" it to 8.3.11 and
  do not go to v9 (v9 breaks the API). Keep the bundled `lv_conf.h` with it.
- `GFX_Library_for_Arduino` — bundle is **1.3.1**. It only builds on core 2.x (see the
  toolchain section). Newer 1.6.x supports core 3.x but changes the panel constructor and
  the `st7701_type5_init_operations` symbol, i.e. exactly the init we must not touch.
- the CST touch library (`Adafruit_CST8XX` / CST328) — confirmed working: the demo streams
  real `Touch ID #0 (x, y) Event: PRESS/TOUCHING/RELEASE` lines over serial.
- `PCF8574` (IO-expander)

**Before any library install, check the triggering `#include`:**
- Real, un-bundled dependency -> install it.
- A second copy of LVGL or Arduino_GFX, or a newer LVGL -> STOP, use the bundled one.

`Adafruit_NeoPixel` (1.15.5) is installed but **unnecessary** — it was pulled in for
`RGB_CODE.ino`, which does not match this board. Harmless; do not build on it.
`FastLED.rar` is likewise unused. No addressable-LED library is needed: there is no
addressable LED (see Hardware above).

## Examples in the repo (real paths)

- `example/RotaryScreen_2_1/RotaryScreen_2_1.ino` — **full integrated demo**: display
  (Arduino_GFX) + encoder + touch + LVGL UI. The display bring-up baseline and the
  skeleton to copy into `sonos-remote/`. Keep this copy pristine; build from
  `diagnostics/display_baseline/` instead. Note it also drives hardware we do NOT have
  (a UDP LED strip and a servo on a remote "Advance" device) — strip that, don't debug it.
- `diagnostics/` — our own probes, not Elecrow's:
  - `display_baseline/` — the working baseline (verbatim demo + `partitions.csv`).
  - `led43_test/` — records the NEGATIVE result that no controllable LED exists; also a
    handy PSRAM / heap / reboot-loop probe.
  - `encoder_test/` — encoder + button probe on the schematic-correct pins. Written from
    scratch because Elecrow's `Encoder_code` targets another board.
- `example/Simple example/Encoder_code/Encoder_code.ino` — **DOES NOT MATCH THIS BOARD.
  Do not use it.** It declares `A=45, B=42, SW=41`; here GPIO 45 = LCD `B2`, GPIO 41 = LCD
  `PCLK`, and the button is not a GPIO at all. It would poll display data lines and attach
  an interrupt to the pixel clock — and it half-works, because its "B" (42) really is
  encoder A, which makes it more misleading than a clean failure. Use
  `diagnostics/encoder_test/` instead.
- `example/Simple example/RGB_CODE/RGB_CODE.ino` — **DOES NOT MATCH THIS BOARD. Do not
  use it.** It drives a 5-pixel WS2812 on GPIO 48, but GPIO 48 is the LCD's B2 line and
  the board has no onboard WS2812. It compiles and flashes cleanly and then does
  nothing — a silent dead end. The only WS2812 in the Elecrow demo is a 15-LED strip on
  a **separate remote device** ("Advance"), driven over **UDP**
  (`sendLedStripCommand`, `REMOTE_NUM_LEDS` in `RotaryScreen_2_1.ino`).
- `factory_firmware/` — prebuilt binary for a zero-code hardware sanity check.
- `factory_soucecode/ESP32_Display_2_1-1/` — source of the factory firmware.
- `example/esphome/` — alternative ESPHome path (not our route).
- `3D file/` — enclosure STP models (useful for mounting).

## Workflow rules (follow in this order)

1. **Prove the display first — DONE.** The baseline builds, flashes and runs from
   `diagnostics/display_baseline/`: a byte-identical copy of `RotaryScreen_2_1.ino` plus a
   `partitions.csv`, so the git-tracked `example/` tree stays pristine. Source code is
   unmodified; only the partition layout and build options differ. Touch is confirmed
   live over serial. Rebuild it any time as a known-good reference point.
2. **Never hand-write the ST7701S init or RGB timing.** Reuse Elecrow's Arduino_GFX
   panel init exactly. A wrong init = black screen with no error, and hours lost.
3. Prove inputs: encoder (GPIO 42/4) and button (PCF8574 P5) via `Encoder_code`.
4. Only then create/populate `sonos-remote/` from a COPY of `RotaryScreen_2_1`, strip
   the demo UI, and add — in this order — WiFi connect -> Sonos volume call -> LVGL UI.
5. Compile after every change; read `arduino-cli monitor` output before concluding.
6. Pin all versions in `sketch.yaml`. Do not bump the ESP32 core or LVGL.

## Sonos integration

- Network is **2.4 GHz only** (FRITZ!Box 7510). The board's 2.4 GHz WiFi is fine.
- Control **group volume** via the local UPnP API on the group COORDINATOR (port 1400):
  - Service `GroupRenderingControl`, action `SetRelativeGroupVolume`
  - Endpoint: `POST http://192.168.187.73:1400/MediaRenderer/GroupRenderingControl/Control`
  - SOAPAction: `urn:schemas-upnp-org:service:GroupRenderingControl:1#SetRelativeGroupVolume`
  - Body: `InstanceID=0`, `Adjustment=<±step>` (e.g. +2 / -2 per detent)
  - Read the level for the display: action `GetGroupVolume` on the same service.
- Rooms: **"Main"** = Era 300 (group head), **"Stereo"** = Era 100 pair.
  Saved group = **"Berlin"**. **VERIFIED on the home LAN (2026-08-07):**

| Room | Model | IP | UUID | Role |
|---|---|---|---|---|
| Main | Era 300 | `192.168.178.26` | `RINCON_804AF280F3E801400` | coordinator of its group |
| Stereo | Era 100 SL | `192.168.178.104` | `RINCON_74CA60A2313C01400` | LF, **coordinator** of the pair |
| Stereo | Era 100 SL | `192.168.178.103` | `RINCON_74CA60A22E4401400` | RF, **bonded — `Invisible="1"`** |

Home LAN is `192.168.178.0/24`, FRITZ!Box gateway `.1`. IPs are DHCP and observed once —
treat them as reference, not config. The firmware discovers at runtime.

**At the time of checking, Main and Stereo were in TWO SEPARATE groups**, not joined as
"Berlin". This changes what the knob controls: grouped, the coordinator is Main and volume
moves both; ungrouped, it moves only the Era 300. The coordinator must therefore be
re-resolved rather than cached across sessions.

**The bonded RF speaker is a live example of the two traps below:** `192.168.178.103` is
marked `Invisible="1"` in the topology and answers `GetGroupVolume` with an EMPTY body.
Targeting it would silently do nothing.

### Protocol validated (read-only, against a different Sonos system)

The transport was proven end-to-end from the dev Mac with `curl`. Confirmed working
exactly as written above: the endpoint path, the quoted `SOAPAction` header, the
`InstanceID=0` envelope, and the reply shape `<CurrentVolume>N</CurrentVolume>`.
`GetZoneGroupState` on `/ZoneGroupTopology/Control` also works and returns
`Coordinator="RINCON_..."` plus one `<ZoneGroupMember>` per player with `ZoneName`,
`UUID` and `Location=http://<ip>:1400/...`.

`192.168.187.73` was never real — it was a placeholder. It is gone from the config; the
real system is in the table above. **Still resolve the coordinator at runtime rather than
hardcoding even the verified IPs** — they are DHCP, and the coordinator moves when the
group changes.

An earlier bring-up session ran on a different network entirely (`192.168.4.0/22`, a Beam +
Move in `Wohnzimmer`/`Küche`). That is where the multi-household finding below came from —
useful, but unrelated to this system.

- Discovery: SSDP `M-SEARCH` for `urn:schemas-upnp-org:device:ZonePlayer:1` on
  `239.255.255.250:1900` works and returns a `LOCATION:` URL per player.
- **Gotcha: SSDP can return players from MULTIPLE Sonos households on one LAN.** Observed
  live: a third player advertised itself and served `device_description.xml` with
  `roomName=Wohnzimmer`, yet was absent from that room's `GetZoneGroupState` and returned an
  EMPTY reply to `GetGroupVolume` — a separate household. So never treat the SSDP list as
  one system: pick a player, read `GetZoneGroupState`, and trust only members listed there.
- A non-coordinator answers `GetGroupVolume` with an empty body — useful as the signal to
  go re-resolve the coordinator rather than as an error to retry blindly.
- `ZoneGroupTopology` -> `GetZoneGroupState` gives the current coordinator; Sonos can
  reassign it, so re-resolve periodically rather than caching forever.
- The local Sonos API is **unofficial/undocumented** but stable (SoCo, node-sonos use
  it). Keep all Sonos calls behind ONE module so a future change is a one-place patch.

## Functional spec (`sonos-remote/specs.txt`) — decisions

The spec defines three day-to-day actions and one screen. Requirements verified against the
live system on 2026-08-07; the ambiguities were resolved with the user as follows.

**Screen 1 layout** (positions given as clock face on the round 480x480):
`12:00` = "Records", `9:00` = "Main", `3:00` = "Stereo", `6:00` = volume readout,
centre = SONOS wordmark. Buttons are toggles showing current state. The rotary ALWAYS
controls volume; turning it shows the current volume (plain digits for now, gauge later).

**Visual treatment (added after the functional safepoint):**
- **The bezel ring is the volume**, drawn as a 270° arc with a gap at the bottom for the
  numerals. The busy indicator REUSES the same geometry and colours, so the ring simply
  starts sweeping instead of a second element appearing. Present on ALL three screens —
  the old spinner existed only on screen 1, so mode taps gave no feedback.
- **Page dots** at the bottom; without them the three-screen swipe chain is invisible.
- **Gradients are short-range and low-contrast on purpose.** 16-bit colour bands visibly
  over a long ramp. `LV_DITHER_GRADIENT` exists if more range is ever wanted, at a RAM cost.
- **Button faces stay LIGHT.** A dark glassy treatment looks better in the abstract but
  erases the near-black Sonos device icons — the same lesson as the white-recolour attempt.
- **Glow only in `LV_STATE_CHECKED`.** Shadows are LVGL's most expensive draw op; applying
  one to all six buttons every frame costs real fill rate on a 480x480 RGB panel.
- **Press feedback uses `transform_width/height`, NOT `transform_zoom`** — in LVGL 8.3 zoom
  applies to images, the width/height insets work on any object.

**UI conventions established during build — keep these:**
- **One accent colour** (`ACCENT` / `ACCENT_FILL` in `ui_screen.cpp`, muted violet). Do not
  scatter hex values; active buttons and the busy ring share it.
- **Busy ring** is a full-width spinner at the bezel (460 px, 8 px arc), non-clickable so it
  cannot swallow taps. It follows `shUserJobs` — **user actions only**. Never drive it from
  `shBusy`, or the 5 s/10 s background refreshes make it spin unprompted. A counter, not a
  bool: a bool strands the ring "on" if a job starts and finishes between two UI frames.
- **Volume turns do not spin the ring** — the number is the feedback.
- **Centre is the logo, not a room name.** Transient status appears under it and expires
  after `STATUS_HOLD_MS`. The logo is letter-spaced text, not the real Sonos artwork; to
  embed the genuine mark, convert a PNG to an LVGL C array.
- Logo carries a **+4 px optical nudge**: LVGL centres the label's box, but all-caps text
  with no descenders reads high at true geometric centre.

**Decisions:**

1. **Volume with several active groups** -> send the SAME relative adjustment to EVERY active
   group's coordinator. Each group keeps its own absolute level, so the balance between rooms
   is preserved. Costs one HTTP call per active group per flush.
2. **"Main" toggled OFF** -> genuinely ungroup, leaving Stereo playing. **Use
   `DelegateGroupCoordinationTo` when Main is the coordinator**, otherwise the stream dies
   with it: hand coordination to Stereo first (`RejoinGroup=false`), then Main is out and
   playback continues. Only use `BecomeCoordinatorOfStandaloneGroup` on a NON-coordinator.
   Caveat to expect: sources anchored to Main (line-in, Airplay, voice) cannot survive on
   Stereo no matter how the handover is done.
3. **"Records" is MOMENTARY, not a toggle** -> tap switches Main to line-in and plays. There
   is no off-state. The button renders as active while line-in is the current source.

**Out of scope — cannot be built as specified:** spec line 24 wants the RIGHT speaker of
"Stereo" alone for TV audio. Blocked twice: that Era 100 is **bonded** (`Invisible="1"`,
`ChannelMapSet ...RF,RF`), so it is not an addressable zone while the pair exists, and
un-bonding is an app-level config change, not a runtime command. The TV path is also
**Bluetooth**, which is outside the Sonos UPnP API entirely. Do not sink time into this.

**Line-in verified:** Main exposes `AudioIn:1` + `VirtualLineIn:1`, input name
`"Plattenspieler"` (matches the spec). Select it with
`SetAVTransportURI(x-rincon-stream:RINCON_804AF280F3E801400)` then `Play`.

**Grouping calls:** join a player to a coordinator with
`SetAVTransportURI(x-rincon:RINCON_<coordinatorUuid>)`; detach with
`BecomeCoordinatorOfStandaloneGroup`; hand over coordination with
`DelegateGroupCoordinationTo`.

**"Active" is not a Sonos concept.** Sonos only knows *grouped with X* and *playing*. The
three toggles are our own state model, derived from `GetZoneGroupState` plus
`GetTransportInfo` — do not expect a single API field to report it.

## Resilience — what breaks when a speaker goes offline

Learned the hard way when the Era 300 powered off mid-session. It stayed LISTED in
`GetZoneGroupState` (Sonos remembers it, and it was still shown as a group member) while
answering **nothing** over HTTP. Assume at any moment that a zone in the topology may be
unreachable.

Rules the firmware now follows — do not regress these:

1. **Never gate the periodic refresh on already having a coordinator.** `if (shHaveCoord)
   enqueue(RefreshState)` is a DEADLOCK: lose the coordinator once and the refresh that
   would recover it never runs. The display then freezes showing every button inactive,
   forever. The refresh IS the recovery path.
2. **A missing target room must not disable everything.** `SONOS_TARGET_ROOM = "Main"`
   with Main offline used to yield `RoomNotFound` -> no coordinator -> dead volume knob,
   even though Stereo was playing happily. `applyCoordinator()` now falls back to any
   available group and sets `targetRoomMissing()`, which the UI shows as "Main offline".
3. **Try every known player before SSDP.** A refresh that only knows one cached IP dies
   when that one speaker is the one that went away. Any player can serve the topology.
4. **SSDP multicast is NOT dependable.** On this network it stopped answering entirely —
   from the board *and* from a laptop — while every speaker was reachable by direct HTTP.
   So: player IPs are cached in **NVS** (`Preferences`, namespace `sonos`, key `ips`) after
   every successful topology read, and `SONOS_COORDINATOR_IP` acts as a **seed** for the
   very first boot. Neither is a hardcoded coordinator — topology remains authoritative.
5. **Expect `HTTP -1` in the log** for calls aimed at an offline player. It is handled, not
   a bug — but it is the signal that a speaker is down.

## Volume: use PER-ROOM relative calls, never SetRelativeGroupVolume

**Sonos stores a group's internal volume BALANCE separately from the members' actual
levels.** `SetVolume` on one member changes that member but does NOT update the stored
balance, so the next `SetRelativeGroupVolume` recomputes members as
*target average x stored ratio* and re-imposes the OLD imbalance.

Measured on this system:

```
Sync sets both rooms to 18/18        <- correct
one group +2  ->  Main 18 / Stereo 22 <- Main did not move; old 18/22 ratio restored
```

One detent after a sync undid the sync completely. Note this hides itself well: group
adjustments DO preserve an existing spread exactly (verified over 8 adjustments holding -4),
so it reads as slow drift rather than an instant snap-back.

**Therefore `adjustVolumeAllRooms()` sends the same relative delta to EACH ROOM via
`RenderingControl SetRelativeVolume`.** That path never consults the stored balance:
equal stays equal, and a deliberate offset is preserved by plain arithmetic. Verified over
five up/down adjustments holding `spread=0`. It also makes grouped and split behave
identically — one rule instead of two.

Costs one call per room rather than one per group (skip `Invisible="1"` satellites, or a
bonded pair gets double-adjusted). Consequence accepted deliberately: the knob no longer
matches the Sonos app's group slider, which still uses the proportional model — so dragging
that slider can reassert the app's balance.

## Screen 3 — one-tap modes, and the traps behind them

`Radio` 12 o'clock, `HiFi` 9, `TV` 3. Scenes exist so the grouping model never has to be
reasoned about — each is one intent, applied idempotently.

| Mode | Does |
|---|---|
| HiFi | rebuild pair if split, stop any Bluetooth on Stereo, join Stereo -> Main, sync volumes |
| Radio | rebuild pair, detach + stop Stereo, play `SONOS_RADIO_URI` on Main |
| TV | split the pair, silence everything except the right speaker (toggle: tap again rebuilds) |

`Vinyl` on Screen 1 is also a scene: rebuild pair -> join Stereo to Main -> Main to line-in -> play.

**Trap 1 — room NAMES are not stable identifiers.** `SeparateStereoPair` makes Sonos revert
both halves to their PRE-PAIR names (measured: "Stereo" became "Wohnzimmer"), and
`CreateStereoPair` does NOT restore the name. After one TV mode, `ROOM_STEREO = "Stereo"`
matched nothing and every lookup for it failed permanently — HiFi could not find the room to
join, which looked like "re-pairing is broken" but was not. `zoneByRoom()` therefore falls
back to the pair's **LF UUID** (from the cached `ChannelMapSet`) when the configured stereo
name matches nothing. Never assume a room name persists; it is user-editable AND
Sonos-editable.

**Trap 2 — a speaker holding a live LOCAL SOURCE cannot become a follower.** While Stereo
streamed the TV over Bluetooth, `joinRoomTo(Stereo -> Main)` silently did nothing. Scenes
that group Stereo under Main must STOP its source first. Bluetooth/virtual line-in shows up
as `x-sonos-vli:<uuid>:<n>,bluetooth:<n>`; note the RIGHT speaker hosts that stream even
while the pair is bonded, so the pair plays TV audio without being split.

**Trap 3 — "active" must mean the same thing for both rooms.** An earlier version derived
Main from playback but Stereo from grouping, which showed Stereo lit and Main dark while
neither was playing together. Both now mean *this room's group is producing sound*.

**Mode highlighting is derived from real state, and stays honest.** TV = Bluetooth source
present OR pair split. HiFi = grouped, no Bluetooth. Radio = Main alone and playing, no
Bluetooth. A state matching none of them (e.g. Stereo alone on Bluetooth before the fix)
highlights NOTHING rather than picking the nearest mode and lying.

**Decision: Main is ALWAYS the master.** HiFi joins Stereo to Main, never the reverse — so it
deliberately stops TV audio. Chosen over "join toward whatever is playing" because a
predictable master beats preserving whatever happened to be on.

**Stereo pair split/rebuild works over UPnP** — `DeviceProperties` exposes `CreateStereoPair`
and `SeparateStereoPair`, taking the `ChannelMapSet`
(`UUID_LF:LF,LF;UUID_RF:RF,RF`). That string only appears in the topology WHILE paired, so it
is cached in NVS (`sonos`/`cms`) — without it a split pair could not be rebuilt after a
reboot, stranding the speakers.

**Discovery: mDNS, not a fixed DHCP lease.** Sonos players advertise `_sonos._tcp` with
instance names `RINCON_<uuid>@<roomName>` — verified from both a laptop and the board. Order
is now **cached NVS IPs -> mDNS -> SSDP**, with SSDP last because it is the one that failed
here. `SONOS_COORDINATOR_IP` is consequently EMPTY; it exists only as a manual override.
(The user's FRITZ!Box does not offer fixed-lease settings for these devices, so this had to
work without one.)

**The pair's NAME is restored automatically.** `createStereoPair()` calls
`renameStereoPair()` internally — at the call site it would eventually be forgotten — reading
the current attributes and writing back the wanted name with icon and configuration
preserved (`SetZoneAttributes` takes all three; omitting them blanks them). It skips the
write when the name is already right.

**Split-pair indicator.** While separated, the Stereo button on the home AND volume screens
draws as two halves: left idle, right blue *only when the right speaker is actually playing*.
Position it with `lv_obj_set_pos`, NOT `lv_obj_align` — align works against the parent's
CONTENT area (inside padding and border), which puts the seam at ~35% instead of 50%.
`BORDER_W` is shared between the border style and that maths so they cannot drift.

**Bluetooth to the split-off right speaker is confirmed working** for TV audio. Selecting the
Bluetooth source itself is still done from the phone; only the pairing/grouping is automated.

## Screen 2 — per-room volume

Swipe LEFT for Screen 2, RIGHT to return. Layout mirrors Screen 1 with `Sync`
(`LV_SYMBOL_REFRESH`) at 12 o'clock; centre reads `VOLUME` instead of `SONOS` so the screens
are distinguishable.

- Tapping Main/Stereo SELECTS a room: light-red border (3->5 px) and its number in the
  breakdown recoloured via LVGL `#RRGGBB text#` markup. The dial then drives only that room.
- Selection is **mutually exclusive**, and **cleared when leaving Screen 2** — a dial silently
  stuck in single-room mode while Screen 1 shows no hint of it is worse than an extra tap.
- Red is border-only: the face still shows blue on/off state, so "selected for editing" and
  "speaker is on" stay separable.
- Buttons need `LV_OBJ_FLAG_GESTURE_BUBBLE`, or a swipe starting on one is swallowed.
- `Sync` sets Stereo to Main's level. After the change above it now STICKS.

## Idle blanking

The display blanks after `DISPLAY_SLEEP_MS` (2 min) without interaction and wakes on any
input. Only the BACKLIGHT is switched; LCD power stays up so waking is instant and needs no
panel re-init (which would flash and cost ~500 ms). Sonos polling continues while dark, so
the screen shows current state the moment it lights rather than a stale snapshot.

**Inputs are treated differently on purpose:**

| Input while asleep | Behaviour |
|---|---|
| knob turn | wakes AND adjusts volume |
| touch | wakes only — the tap is swallowed |
| dial press | wakes only — no Stereo toggle |

A volume knob must work in the dark, and volume is trivially reversible. Taps and the dial
press trigger grouping and stereo-pair changes, so a blind press must not fire one.

**The wake-up tap is swallowed inside `panel`, in the LVGL input driver** — not in the
sketch. By the time the sketch sees it LVGL has already dispatched the press to a button.

## Threading — the rule that keeps the UI responsive

**NEVER make a Sonos HTTP call from the Arduino `loop()`.** While it blocks, `pollEncoder()`
does not run (detents are silently LOST, so the knob feels erratic) and `lv_timer_handler()`
does not run (frozen screen, taps ignored). This was measured, not theorised: the first
version felt badly sluggish because of it.

Structure:
- **Core 1 (Arduino loop):** encoder polling, LVGL, reading shared flags. Never blocks.
- **Core 0 (`workerTask`):** every SOAP/HTTP call, driven by a FreeRTOS queue of `Job`s.
- Shared state is plain 32-bit `volatile` scalars plus a `shStateSeq` counter — no Strings
  across threads, no mutex needed. **LVGL is not thread-safe: only core 1 touches it.**

What was actually slow, and the fixes:

| Cause | Fix |
|---|---|
| `resolveCoordinator()` (1.5-3 s **SSDP**) on a 10 s timer in `loop()` | `refreshTopology()` — one POST (~30 ms) to a cached player; SSDP only on first resolve or when that player vanishes |
| A tap did action + `delay(400)` + SSDP + 3-4 POSTs, all blocking (~5-8 s) | tap only enqueues; worker does the rest |
| `HTTP_TIMEOUT_MS = 5000` | 2000 — a dead player must not stall the UI |
| Volume number only updated after the HTTP round-trip | optimistic echo in `pollEncoder()`, reconciled from the worker's real value |
| Fast spins queued one HTTP call per detent | worker collapses consecutive `VolumeDelta` jobs into one |
| Status text never cleared ("detaching ..." shown minutes later) | `STATUS_HOLD_MS` (2.5 s) expiry back to the room name |
| A tap looked ignored while work happened off-thread | `ui::setBusy()` spinner |

## Encoder -> volume mapping

### The reading logic (measured on hardware — copy this, don't re-derive it)

```c
// Poll A; on ANY edge (rising OR falling), take direction from B:
int stateA = digitalRead(ENCODER_A_PIN);      // GPIO 42
if (stateA != lastStateA) {
  int stateB = digitalRead(ENCODER_B_PIN);    // GPIO 4
  bool cw = (stateB == stateA);               // B == A -> CW, B != A -> CCW
  ...
  lastStateA = stateA;
}
```

Two traps, both hit and measured during bring-up:

1. **Count BOTH edges of A, not just rising.** This encoder rests alternately at
   `(A=0,B=0)` and `(A=1,B=1)`, so rising edges occur only every SECOND detent. Elecrow's
   demo counts rising only and therefore throws away half the knob's resolution — verified:
   10 detents produced 10 A-edges but only 5 rising ones.
2. **The direction rule is `B == A`, independent of edge polarity.** Do NOT "simplify" it to
   `B == HIGH`: that is correct on rising edges and inverts on falling ones, so a one-way
   turn nets to zero movement. Verified CW gives `(1,1)`/`(0,0)` and CCW gives `(1,0)`/`(0,1)`.

- One detent CW -> `Adjustment=+STEP`, CCW -> `Adjustment=-STEP` (STEP = 2, and 1 detent is
  exactly 1 count, so a click moves volume by 2).
- No debounce needed on A/B (clean signals). The BUTTON still needs its 50 ms debounce.
- Coalesce fast turns (accumulate, send at most ~every 80 ms) so Sonos isn't flooded.
- Poll `GetGroupVolume` occasionally so the knob/display reflect changes made
  elsewhere (Sonos app, AirPlay).
- **Dial press (PCF8574 P5) = toggle "Stereo"** — identical to the on-screen Stereo button
  (join to / detach from Main's group), so the pair can be grouped without looking at the
  screen. Decided 2026-08-08, replacing the earlier play/pause idea. Play/pause has no
  control surface now; if it is wanted later it needs a new gesture (long-press) or a
  second screen.

## Secrets

- Put WiFi SSID/password (and optionally `COORDINATOR_IP`) in `secrets.h`.
- Add `secrets.h` to `.gitignore`. Never commit credentials.

## ESP32-S3 gotchas

- If auto-reset into flashing fails (e.g. after a crash): hold **BOOT**, tap
  **RESET**, release BOOT -> download mode.
- Native USB-CDC port re-enumerates after flashing/reset — reconnect the monitor.
- **The port name changes when the board moves to a different USB socket** — seen live:
  `/dev/cu.usbmodem21401` became `/dev/cu.usbmodem21201`, and upload failed with "the port
  doesn't exist". Always re-run `arduino-cli board list` rather than trusting a documented
  port. It may also report Board Name "Unknown" — harmless, uploads work regardless.
- Serial baud: 115200.

## Bring-up status

Done and verified on hardware:

- Port `/dev/cu.usbmodem21401`; chip ESP32-S3 rev v0.2; flash **16 MB** (`esptool flash_id`)
- FQBN + build options (see toolchain section) — compile, flash, run all confirmed
- OPI PSRAM live at runtime: `PSRAM size = 8388608`
- Elecrow library bundle copied to `~/Documents/Arduino/libraries/` (incl. `lv_conf.h`)
- Core pinned to 2.0.17 (3.x cannot build the display driver)
- Display baseline runs from `diagnostics/display_baseline/`: **pixels confirmed on the
  panel, demo UI renders and responds as expected, touch confirmed working.** This is the
  known-good reference — if a later build shows a black screen, diff against it first.
- No controllable LED exists (schematic-verified) — that avenue is closed, not pending

- Encoder verified and **calibrated** on **GPIO 42 (A) / GPIO 4 (B)** — 20 detents gave 20
  events, none missed, none spurious, exact return to zero. **1 detent = 1 count**, so
  `Adjustment = ±STEP` per count and `STEP = 2` means 2 volume points per click.
  No debounce needed on A/B: the quadrature levels are perfectly clean.
- Encoder button verified on **PCF8574 P5 over I2C** (active low, polled). Single presses
  register reliably.

- **Phase 1 firmware WORKS on hardware** (`sonos-remote/`): WiFi join (board gets
  `192.168.178.113`, RSSI -55), SSDP discovery of all 3 players, coordinator resolution to
  Main, `GetGroupVolume`, and **live `SetRelativeGroupVolume` from the knob** — 65
  consecutive writes, every one exactly ±2, no failures and no re-resolves.

- **Phase 2: Screen 1 renders and touch works.** `panel.cpp` carries Elecrow's ST7701S
  bring-up verbatim (expander -> LCD power/reset -> touch reset -> gfx -> LVGL -> backlight
  -> `P3 LOW`), `ui_screen.cpp` draws the clock-face layout. A touch tap at 9 o'clock was
  logged as the Main button, so hit-testing maps correctly. Build is 1.09 MB (34%).
  - `gfx->begin()` returns **void** in Arduino_GFX 1.3.1 — do not test it as a bool.
  - LVGL buffers: two full 480x480 frames in PSRAM (460,800 bytes each).
  - `panel` owns the PCF8574 because LCD power, touch reset and the encoder button share it.

**All three specs.txt activities verified on hardware (2026-08-08), zero errors in the run:**
- Volume across ONE group and across **TWO separate groups**: both coordinators moved +10 in
  lockstep and the inter-room difference was preserved exactly (26/34 -> 36/44). That is the
  whole point of a RELATIVE per-group adjustment rather than a shared absolute level.
- Grouping: `Stereo` join + detach, `Main` join, and the dial press — all OK.
- Line-in: `Records` -> `Plattenspieler` OK.

- **Coalescing verified** with a fast spin: merged adjustments up to ±14 appeared, delivering
  ~43 detents in 13 HTTP flushes (~3.3 detents per request) with no steps lost.
- **Two-group fan-out verified under controlled conditions**: rooms split first and the
  topology confirmed stable for the whole test. Main 28->46 and Stereo 22->40 — both moved
  exactly +18, matching the firmware's 9 × +2, with the 6-point inter-room difference intact.
  **Lesson: verify the topology did not change during a test before believing its result.**
  An earlier run appeared to show Stereo being skipped; the real cause was Alexa/the app
  regrouping mid-test, which the before/after reads straddled.
- **Fast-spin volume slam fixed**: a merged flush could reach ±14 and drive volume to 0/100 in
  one flick. Capped at `MAX_ADJUST_PER_FLUSH = 6`, applied AFTER merging. Excess is
  DISCARDED, not carried over — carrying it would keep the volume sliding after the knob
  stops, which feels broken on a physical control. Net effect is a rate limit (~75
  points/second while spinning); slow turns are never clamped.
- A volume flush now bumps `shStateSeq`, so the optimistic display echo reconciles on the
  next frame. Without that, a capped fast spin left the screen showing a number that had
  outrun the speakers until the next 5 s poll.

Still open:
- `sonos-remote/specs.txt` line 24 ("right speaker of Stereo only, via Bluetooth") is now
  DONE — TV mode splits the pair and the Bluetooth path is confirmed. The spec text itself
  still reads as an open wish; update it if the file is ever revised.