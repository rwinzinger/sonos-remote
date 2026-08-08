// Screen 1 from specs.txt, on the 480x480 round panel:
//     12 o'clock = "Records"   9 o'clock = "Main"   3 o'clock = "Stereo"
//      6 o'clock = volume readout
//
// Buttons are toggles that SHOW state; "Records" is momentary per the agreed decision
// (tap selects line-in, there is no off) but still renders active while line-in is the
// current source.
//
// This file only draws and reports taps. It performs no Sonos calls — the sketch wires
// onTap() to actions, so the UI stays testable without speakers.

#pragma once

#include <Arduino.h>

namespace ui {

enum class Button { Records, Main, Stereo };

// Called on tap. The UI does NOT change the button's visual state itself: state is set by
// setActive() once the real Sonos state is known, so the screen can never claim something
// happened that did not.
typedef void (*TapHandler)(Button);

void build(TapHandler handler);

void setVolume(int volume);            // -1 = unknown, renders "--"
void setActive(Button b, bool active);
// Per-room levels shown small beneath the group number, as "main / stereo". Pass -1 for a
// room whose level is unknown. The big number remains the GROUP volume (Sonos's own average
// across members) because that is what the rotary actually changes.
void setRoomVolumes(int mainVol, int stereoVol);

void setStatus(const char *text);      // small line in the centre (room / errors)

// Spinner shown while a network action is in flight. Without it a tap looks ignored,
// because the real work happens off-thread and the buttons deliberately do not
// self-illuminate.
void setBusy(bool busy);

}  // namespace ui
