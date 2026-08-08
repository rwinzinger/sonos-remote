// Three screens on the 480x480 round panel: Home -> Volume -> Modes.
// Swipe LEFT to advance, RIGHT to go back (a linear chain, not a carousel).
//
// SCREEN 1 (from specs.txt):
//     12 o'clock = "Vinyl"     9 = "Main"   3 = "Stereo"   6 = volume   centre = SONOS
//   "Vinyl" is a SCENE, not just a source select: join Stereo to Main, switch Main to
//   the Plattenspieler line-in, and play — the whole record-player ritual in one tap.
//
// SCREEN 3 (one-tap modes): 12 = "Radio"   9 = "HiFi"   3 = "TV"
//   Scenes hide the grouping model entirely: each is a single intent, so a guest never has
//   to reason about join/detach.
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

enum class Button { Vinyl, Main, Stereo };            // screen 1
enum class Action { Sync, SelectMain, SelectStereo }; // screen 2
enum class Selection { None, Main, Stereo };
enum class Scene  { HiFi, Radio, TV };                // screen 3
enum class Screen { Home, Volume, Modes };

typedef void (*TapHandler)(Button);
typedef void (*ActionHandler)(Action);
typedef void (*SceneHandler)(Scene);
typedef void (*ScreenChangeHandler)(Screen);

void build(TapHandler taps, ActionHandler actions, SceneHandler scenes,
           ScreenChangeHandler onScreenChange);

// Volume readout — applied to ALL screens so swiping never shows a stale number.
void setVolume(int volume);                             // group volume, -1 = unknown
void setRoomVolumes(int mainVol, int stereoVol);        // "main / stereo" breakdown

void setActive(Button b, bool active);                  // screen 1 toggle state
void setSceneActive(Scene s, bool active);             // screen 3 highlight

// Draw the Stereo button as two halves while the pair is SPLIT (TV mode): left half idle,
// right half blue when the right speaker is playing. A single on/off state cannot describe
// two independent speakers.
void setStereoSplit(bool split, bool rightActive);
void setStatus(const char *text);
void setBusy(bool busy);

// Screen 2 selection. Also re-tints the breakdown, so call setRoomVolumes() after this or
// let the next refresh do it.
void setSelection(Selection s);
Selection selection();

Screen currentScreen();

}  // namespace ui
