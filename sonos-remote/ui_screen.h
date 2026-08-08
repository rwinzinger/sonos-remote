// Two screens on the 480x480 round panel, swipe left/right between them.
//
// SCREEN 1 (from specs.txt):
//     12 o'clock = "Records"   9 = "Main"   3 = "Stereo"   6 = volume   centre = SONOS
//
// SCREEN 2 (per-room volume):
//     12 o'clock = "Sync"      9 = "Main"   3 = "Stereo"   6 = volume   centre = VOLUME
//   Tapping Main or Stereo SELECTS that room: its border and its number in the breakdown
//   turn light red, and the dial then moves only that room. Tapping again deselects.
//   Selection is mutually exclusive — tapping the other room moves the selection.
//
// This file only draws and reports intent. It performs no Sonos calls, and buttons never
// set their own state: state is pushed in from real Sonos state.

#pragma once

#include <Arduino.h>

namespace ui {

enum class Button { Records, Main, Stereo };          // screen 1
enum class Action { Sync, SelectMain, SelectStereo }; // screen 2
enum class Selection { None, Main, Stereo };
enum class Screen { Home, Volume };

typedef void (*TapHandler)(Button);
typedef void (*ActionHandler)(Action);
typedef void (*ScreenChangeHandler)(Screen);

void build(TapHandler taps, ActionHandler actions, ScreenChangeHandler onScreenChange);

// Volume readout — applied to BOTH screens so swiping never shows a stale number.
void setVolume(int volume);                             // group volume, -1 = unknown
void setRoomVolumes(int mainVol, int stereoVol);        // "main / stereo" breakdown

void setActive(Button b, bool active);                  // screen 1 toggle state
void setStatus(const char *text);
void setBusy(bool busy);

// Screen 2 selection. Also re-tints the breakdown, so call setRoomVolumes() after this or
// let the next refresh do it.
void setSelection(Selection s);
Selection selection();

Screen currentScreen();

}  // namespace ui
