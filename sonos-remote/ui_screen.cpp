#include "ui_screen.h"

#include <lvgl.h>
#include "sonos_icons.h"

namespace ui {
namespace {

// Round panel: everything sits on a radius rather than in corners. 150 px from centre keeps
// a 110 px button fully inside the glass with margin for the bezel.
const int RADIUS    = 150;
const int BTN_SIZE  = 110;

// Active state is carried by HUE, not brightness — grey-on-grey was too subtle to read at a
// glance. Faces stay light because the Sonos device icons are near-black and cannot be
// recoloured convincingly.
const uint32_t IDLE_FILL   = 0xA0A0A0;
const uint32_t ACCENT_FILL = 0x86C8F5;   // light blue ACTIVE face (screen 1)
const uint32_t IDLE_BORDER = 0xD0D0D0;
const uint32_t ACCENT      = 0xFFFFFF;   // ACTIVE border (screen 1)
const uint32_t BTN_TEXT    = 0x14141A;

// Screen 2 selection: light red. Distinct from the blue "active" state on purpose — these
// mean different things (selected-for-editing vs speaker-is-on).
const uint32_t SELECT_COLOR = 0xF08080;

const uint32_t RING_ACCENT = 0x86C8F5;
const uint32_t RING_TRACK  = 0x22222A;
const uint32_t TEXT_BRIGHT = 0xF0F0F5;
const uint32_t TEXT_DIM    = 0x8A8A95;
const uint32_t BG          = 0x101014;

const int RING_SIZE  = 460;
const int RING_WIDTH = 8;

// Per-screen widgets that need updating from outside.
struct ScreenWidgets {
  lv_obj_t *screen  = nullptr;
  lv_obj_t *volume  = nullptr;
  lv_obj_t *rooms   = nullptr;
};

ScreenWidgets home_;
ScreenWidgets vol_;

lv_obj_t *btnRecords = nullptr;
lv_obj_t *btnMainS1  = nullptr;
lv_obj_t *btnStereoS1= nullptr;
lv_obj_t *btnSync    = nullptr;
lv_obj_t *btnMainS2  = nullptr;
lv_obj_t *btnStereoS2= nullptr;
lv_obj_t *lblStatus  = nullptr;
lv_obj_t *spinner    = nullptr;

TapHandler          tapHandler    = nullptr;
ActionHandler       actionHandler = nullptr;
ScreenChangeHandler screenHandler = nullptr;

Selection selection_   = Selection::None;
Screen    current_     = Screen::Home;
int       lastMainVol_  = -1;
int       lastStereoVol_= -1;

Button tagRecords = Button::Records;
Button tagMainS1  = Button::Main;
Button tagStereoS1= Button::Stereo;
Action tagSync    = Action::Sync;
Action tagMainS2  = Action::SelectMain;
Action tagStereoS2= Action::SelectStereo;

void onTapEvent(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !tapHandler) return;
  Button *which = (Button *)lv_event_get_user_data(e);
  if (which) tapHandler(*which);
}

void onActionEvent(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !actionHandler) return;
  Action *which = (Action *)lv_event_get_user_data(e);
  if (which) actionHandler(*which);
}

void showScreen(Screen s);

// Swipe handling. Gestures land on whatever is under the finger, so the BUTTONS must have
// LV_OBJ_FLAG_GESTURE_BUBBLE set or a swipe starting on one would be swallowed.
void onGesture(lv_event_t *e) {
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
  if (dir == LV_DIR_LEFT)       showScreen(Screen::Volume);
  else if (dir == LV_DIR_RIGHT) showScreen(Screen::Home);
}

lv_obj_t *makeButton(lv_obj_t *parent, const char *text, int dx, int dy,
                     const lv_img_dsc_t *icon, const char *symbol,
                     lv_event_cb_t cb, void *tag) {
  lv_obj_t *btn = lv_btn_create(parent);
  lv_obj_set_size(btn, BTN_SIZE, BTN_SIZE);
  lv_obj_align(btn, LV_ALIGN_CENTER, dx, dy);

  lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(btn, lv_color_hex(IDLE_FILL), LV_PART_MAIN);
  lv_obj_set_style_border_width(btn, 3, LV_PART_MAIN);
  lv_obj_set_style_border_color(btn, lv_color_hex(IDLE_BORDER), LV_PART_MAIN);
  lv_obj_set_style_bg_color(btn, lv_color_hex(ACCENT_FILL),
                            (lv_style_selector_t)LV_PART_MAIN | LV_STATE_CHECKED);
  lv_obj_set_style_border_color(btn, lv_color_hex(ACCENT),
                                (lv_style_selector_t)LV_PART_MAIN | LV_STATE_CHECKED);

  lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
  lv_obj_add_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE);   // let swipes pass through to the screen
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, tag);

  if (icon) {
    lv_obj_t *img = lv_img_create(btn);
    lv_img_set_src(img, icon);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, -18);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    // Icons stay as Sonos ships them (near-black); the light button face is the contrast.
  } else if (symbol) {
    lv_obj_t *sym = lv_label_create(btn);
    lv_label_set_text(sym, symbol);
    lv_obj_set_style_text_font(sym, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(sym, lv_color_hex(BTN_TEXT), LV_PART_MAIN);
    lv_obj_align(sym, LV_ALIGN_CENTER, 0, -16);
  }

  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(lbl, lv_color_hex(BTN_TEXT), LV_PART_MAIN);
  lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 28);
  return btn;
}

// Volume readout shared by both screens.
void addVolumeReadout(ScreenWidgets &w) {
  w.volume = lv_label_create(w.screen);
  lv_label_set_text(w.volume, "--");
  lv_obj_set_style_text_font(w.volume, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_obj_set_style_text_color(w.volume, lv_color_hex(TEXT_BRIGHT), LV_PART_MAIN);
  lv_obj_align(w.volume, LV_ALIGN_CENTER, 0, RADIUS - 10);

  w.rooms = lv_label_create(w.screen);
  // Recolour markup lets ONE side of "13 / 25" turn red without splitting it into
  // separate labels.
  lv_label_set_recolor(w.rooms, true);
  lv_label_set_text(w.rooms, "");
  lv_obj_set_style_text_font(w.rooms, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(w.rooms, lv_color_hex(TEXT_DIM), LV_PART_MAIN);
  lv_obj_align(w.rooms, LV_ALIGN_CENTER, 0, RADIUS + 26);
}

lv_obj_t *addCentreLabel(lv_obj_t *parent, const char *text) {
  lv_obj_t *lbl = lv_label_create(parent);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(lbl, lv_color_hex(TEXT_BRIGHT), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(lbl, 7, LV_PART_MAIN);
  // +4 optical nudge: all-caps text has no descenders, so true geometric centring reads high.
  lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 4);
  return lbl;
}

void applySelectionStyles() {
  // Border only — the face keeps showing on/off state, so the two meanings stay separable.
  lv_obj_set_style_border_color(
      btnMainS2,
      lv_color_hex(selection_ == Selection::Main ? SELECT_COLOR : IDLE_BORDER),
      LV_PART_MAIN);
  lv_obj_set_style_border_width(btnMainS2, selection_ == Selection::Main ? 5 : 3,
                                LV_PART_MAIN);
  lv_obj_set_style_border_color(
      btnStereoS2,
      lv_color_hex(selection_ == Selection::Stereo ? SELECT_COLOR : IDLE_BORDER),
      LV_PART_MAIN);
  lv_obj_set_style_border_width(btnStereoS2, selection_ == Selection::Stereo ? 5 : 3,
                                LV_PART_MAIN);
}

void showScreen(Screen s) {
  if (s == current_) return;
  current_ = s;
  lv_scr_load_anim(s == Screen::Home ? home_.screen : vol_.screen,
                   s == Screen::Home ? LV_SCR_LOAD_ANIM_MOVE_RIGHT
                                     : LV_SCR_LOAD_ANIM_MOVE_LEFT,
                   250, 0, false);
  if (screenHandler) screenHandler(s);
}

}  // namespace

void build(TapHandler taps, ActionHandler actions, ScreenChangeHandler onScreenChange) {
  tapHandler    = taps;
  actionHandler = actions;
  screenHandler = onScreenChange;

  // ---- Screen 1: home -----------------------------------------------------------------
  home_.screen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(home_.screen, lv_color_hex(BG), LV_PART_MAIN);
  lv_obj_clear_flag(home_.screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(home_.screen, onGesture, LV_EVENT_GESTURE, NULL);

  btnRecords  = makeButton(home_.screen, "Records", 0, -RADIUS, nullptr, LV_SYMBOL_AUDIO,
                           onTapEvent, &tagRecords);
  btnMainS1   = makeButton(home_.screen, "Main", -RADIUS, 0, &icon_era300, nullptr,
                           onTapEvent, &tagMainS1);
  btnStereoS1 = makeButton(home_.screen, "Stereo", RADIUS, 0, &icon_era100, nullptr,
                           onTapEvent, &tagStereoS1);
  addVolumeReadout(home_);
  addCentreLabel(home_.screen, "SONOS");

  lblStatus = lv_label_create(home_.screen);
  lv_label_set_text(lblStatus, "starting ...");
  lv_obj_set_style_text_font(lblStatus, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblStatus, lv_color_hex(TEXT_DIM), LV_PART_MAIN);
  lv_obj_align(lblStatus, LV_ALIGN_CENTER, 0, 28);

  spinner = lv_spinner_create(home_.screen, 1100, 70);
  lv_obj_set_size(spinner, RING_SIZE, RING_SIZE);
  lv_obj_center(spinner);
  lv_obj_set_style_arc_width(spinner, RING_WIDTH, LV_PART_MAIN);
  lv_obj_set_style_arc_width(spinner, RING_WIDTH, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(spinner, lv_color_hex(RING_TRACK), LV_PART_MAIN);
  lv_obj_set_style_arc_color(spinner, lv_color_hex(RING_ACCENT), LV_PART_INDICATOR);
  lv_obj_clear_flag(spinner, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);

  // ---- Screen 2: per-room volume ------------------------------------------------------
  vol_.screen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(vol_.screen, lv_color_hex(BG), LV_PART_MAIN);
  lv_obj_clear_flag(vol_.screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(vol_.screen, onGesture, LV_EVENT_GESTURE, NULL);

  // LV_SYMBOL_REFRESH is the conventional two-arrow sync glyph in LVGL's built-in font.
  btnSync     = makeButton(vol_.screen, "Sync", 0, -RADIUS, nullptr, LV_SYMBOL_REFRESH,
                           onActionEvent, &tagSync);
  btnMainS2   = makeButton(vol_.screen, "Main", -RADIUS, 0, &icon_era300, nullptr,
                           onActionEvent, &tagMainS2);
  btnStereoS2 = makeButton(vol_.screen, "Stereo", RADIUS, 0, &icon_era100, nullptr,
                           onActionEvent, &tagStereoS2);
  // Selection is ours to render, so these must not self-toggle on tap.
  lv_obj_clear_flag(btnMainS2, LV_OBJ_FLAG_CHECKABLE);
  lv_obj_clear_flag(btnStereoS2, LV_OBJ_FLAG_CHECKABLE);
  lv_obj_clear_flag(btnSync, LV_OBJ_FLAG_CHECKABLE);

  addVolumeReadout(vol_);
  addCentreLabel(vol_.screen, "VOLUME");

  applySelectionStyles();
  lv_scr_load(home_.screen);
  current_ = Screen::Home;
}

void setVolume(int volume) {
  for (ScreenWidgets *w : {&home_, &vol_}) {
    if (!w->volume) continue;
    if (volume < 0) lv_label_set_text(w->volume, "--");
    else            lv_label_set_text_fmt(w->volume, "%d", volume);
  }
}

void setRoomVolumes(int mainVol, int stereoVol) {
  lastMainVol_ = mainVol;
  lastStereoVol_ = stereoVol;

  char mainPart[32];
  char stereoPart[32];
  if (mainVol < 0) snprintf(mainPart, sizeof(mainPart), "--");
  else             snprintf(mainPart, sizeof(mainPart), "%d", mainVol);
  if (stereoVol < 0) snprintf(stereoPart, sizeof(stereoPart), "--");
  else               snprintf(stereoPart, sizeof(stereoPart), "%d", stereoVol);

  // Tint only the selected room's number, via LVGL's #RRGGBB text# recolour markup.
  char text[96];
  if (selection_ == Selection::Main) {
    snprintf(text, sizeof(text), "#f08080 %s# / %s", mainPart, stereoPart);
  } else if (selection_ == Selection::Stereo) {
    snprintf(text, sizeof(text), "%s / #f08080 %s#", mainPart, stereoPart);
  } else {
    snprintf(text, sizeof(text), "%s / %s", mainPart, stereoPart);
  }

  for (ScreenWidgets *w : {&home_, &vol_}) {
    if (w->rooms) lv_label_set_text(w->rooms, text);
  }
}

void setActive(Button b, bool active) {
  lv_obj_t *o = (b == Button::Records) ? btnRecords
              : (b == Button::Main)    ? btnMainS1
                                       : btnStereoS1;
  if (!o) return;
  if (active) lv_obj_add_state(o, LV_STATE_CHECKED);
  else        lv_obj_clear_state(o, LV_STATE_CHECKED);
}

void setStatus(const char *text) {
  if (lblStatus) lv_label_set_text(lblStatus, text ? text : "");
}

void setBusy(bool busy) {
  if (!spinner) return;
  if (busy) lv_obj_clear_flag(spinner, LV_OBJ_FLAG_HIDDEN);
  else      lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
}

void setSelection(Selection s) {
  selection_ = s;
  applySelectionStyles();
  setRoomVolumes(lastMainVol_, lastStereoVol_);   // re-tint the breakdown
}

Selection selection()    { return selection_; }
Screen    currentScreen(){ return current_; }

}  // namespace ui
