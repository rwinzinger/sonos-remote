#include "ui_screen.h"

#include <lvgl.h>
#include "sonos_icons.h"

namespace ui {
namespace {

// Round panel: everything sits on a radius rather than in corners. 150 px from centre keeps
// a 110 px button fully inside the glass with margin for the bezel.
const int RADIUS    = 150;
const int BTN_SIZE  = 110;
// Single source of truth: the same value is used for the border style AND for the maths
// that positions the split overlay, which must line up with the button's true centre.
const int BORDER_W  = 3;

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
const uint32_t BG          = 0x14141A;   // top of the background gradient
const uint32_t BG_DEEP     = 0x08080B;   // bottom — subtle depth without visible banding

// Gradient partners, kept CLOSE to their base colour on purpose: at 16-bit colour depth a
// long gradient bands visibly, so these are short-range and low-contrast.
const uint32_t IDLE_FILL_2   = 0x8A8A90;
const uint32_t ACCENT_FILL_2 = 0x63A9DC;
const uint32_t PRESS_FILL    = 0x7E7E85;

const int RING_SIZE  = 460;
const int RING_WIDTH = 8;

// Per-screen widgets that need updating from outside.
struct ScreenWidgets {
  lv_obj_t *screen  = nullptr;
  lv_obj_t *volume  = nullptr;
  lv_obj_t *rooms   = nullptr;
  lv_obj_t *arc     = nullptr;        // volume as a bezel ring
  lv_obj_t *spinner = nullptr;        // same geometry, shown instead while busy
  lv_obj_t *dots[3] = {nullptr, nullptr, nullptr};
};

ScreenWidgets home_;
ScreenWidgets vol_;
ScreenWidgets modes_;

lv_obj_t *btnVinyl   = nullptr;
lv_obj_t *btnMainS1  = nullptr;
lv_obj_t *btnStereoS1= nullptr;
lv_obj_t *btnSync    = nullptr;
lv_obj_t *btnMainS2  = nullptr;
lv_obj_t *btnStereoS2= nullptr;
lv_obj_t *btnHiFi    = nullptr;
lv_obj_t *btnRadio   = nullptr;
lv_obj_t *btnTV      = nullptr;
// Overlay showing the stereo pair SPLIT: left half stays the button face, right half is
// drawn over it. Clipped to the circle by the parent's clip_corner style.
struct SplitOverlay {
  lv_obj_t *rightHalf = nullptr;
  lv_obj_t *divider   = nullptr;
};
SplitOverlay splitHome_;
SplitOverlay splitVol_;
lv_obj_t *lblStatus  = nullptr;

// Animates the press feedback. transform_width/height (not transform_zoom): in LVGL 8.3
// zoom applies to images, while width/height insets work for any object.
const lv_style_prop_t PRESS_PROPS[] = {LV_STYLE_TRANSFORM_WIDTH, LV_STYLE_TRANSFORM_HEIGHT,
                                       LV_STYLE_BG_COLOR, LV_STYLE_PROP_INV};
lv_style_transition_dsc_t pressTransition_;

TapHandler          tapHandler    = nullptr;
ActionHandler       actionHandler = nullptr;
SceneHandler        sceneHandler  = nullptr;
ScreenChangeHandler screenHandler = nullptr;

Selection selection_   = Selection::None;
Screen    current_     = Screen::Home;
int       lastMainVol_  = -1;
int       lastStereoVol_= -1;

Button tagVinyl   = Button::Vinyl;
Button tagMainS1  = Button::Main;
Button tagStereoS1= Button::Stereo;
Action tagSync    = Action::Sync;
Action tagMainS2  = Action::SelectMain;
Action tagStereoS2= Action::SelectStereo;
Scene  tagHiFi    = Scene::HiFi;
Scene  tagRadio   = Scene::Radio;
Scene  tagTV      = Scene::TV;

void onTapEvent(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !tapHandler) return;
  Button *which = (Button *)lv_event_get_user_data(e);
  if (which) tapHandler(*which);
}

void onSceneEvent(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !sceneHandler) return;
  Scene *which = (Scene *)lv_event_get_user_data(e);
  if (which) sceneHandler(*which);
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
  int idx = (int)current_;
  if (dir == LV_DIR_LEFT)       idx++;
  else if (dir == LV_DIR_RIGHT) idx--;
  else return;
  // Clamp rather than wrap: on a 3-screen chain, wrapping makes it easy to overshoot and
  // lose track of where you are.
  if (idx < 0) idx = 0;
  if (idx > (int)Screen::Modes) idx = (int)Screen::Modes;
  showScreen((Screen)idx);
}

// Draw the right half of a circular button in a different colour, for the split stereo
// pair. Positioned with lv_obj_set_pos, NOT lv_obj_align: align works against the parent's
// CONTENT area (inside padding and border), which pushed the seam left of centre. Content
// origin sits BORDER_W in, so that offset is compensated explicitly to put the seam on the
// button's true middle. clip_corner on the parent trims the rectangle to a half-disc.
void attachSplitOverlay(lv_obj_t *btn, SplitOverlay &ov) {
  lv_obj_set_style_clip_corner(btn, true, LV_PART_MAIN);
  lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);

  ov.rightHalf = lv_obj_create(btn);
  lv_obj_set_size(ov.rightHalf, BTN_SIZE / 2, BTN_SIZE);
  lv_obj_set_pos(ov.rightHalf, BTN_SIZE / 2 - BORDER_W, -BORDER_W);
  lv_obj_set_style_radius(ov.rightHalf, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(ov.rightHalf, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(ov.rightHalf, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ov.rightHalf, lv_color_hex(ACCENT_FILL), LV_PART_MAIN);
  lv_obj_clear_flag(ov.rightHalf, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(ov.rightHalf, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_move_background(ov.rightHalf);
  lv_obj_add_flag(ov.rightHalf, LV_OBJ_FLAG_HIDDEN);

  ov.divider = lv_obj_create(btn);
  lv_obj_set_size(ov.divider, 2, BTN_SIZE);
  lv_obj_set_pos(ov.divider, BTN_SIZE / 2 - BORDER_W - 1, -BORDER_W);
  lv_obj_set_style_radius(ov.divider, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(ov.divider, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(ov.divider, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ov.divider, lv_color_hex(BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ov.divider, LV_OPA_40, LV_PART_MAIN);
  lv_obj_clear_flag(ov.divider, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(ov.divider, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_move_background(ov.divider);
  lv_obj_add_flag(ov.divider, LV_OBJ_FLAG_HIDDEN);
}


lv_obj_t *makeButton(lv_obj_t *parent, const char *text, int dx, int dy,
                     const lv_img_dsc_t *icon, const char *symbol,
                     lv_event_cb_t cb, void *tag) {
  lv_obj_t *btn = lv_btn_create(parent);
  lv_obj_set_size(btn, BTN_SIZE, BTN_SIZE);
  lv_obj_align(btn, LV_ALIGN_CENTER, dx, dy);

  lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn, BORDER_W, LV_PART_MAIN);
  lv_obj_set_style_border_color(btn, lv_color_hex(IDLE_BORDER), LV_PART_MAIN);

  // Slightly translucent, with a short vertical gradient for depth. Faces stay LIGHT: the
  // Sonos device icons are near-black, so a dark glassy treatment would erase them.
  lv_obj_set_style_bg_color(btn, lv_color_hex(IDLE_FILL), LV_PART_MAIN);
  lv_obj_set_style_bg_grad_color(btn, lv_color_hex(IDLE_FILL_2), LV_PART_MAIN);
  lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_VER, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(btn, 235, LV_PART_MAIN);

  lv_obj_set_style_bg_color(btn, lv_color_hex(ACCENT_FILL),
                            (lv_style_selector_t)LV_PART_MAIN | LV_STATE_CHECKED);
  lv_obj_set_style_bg_grad_color(btn, lv_color_hex(ACCENT_FILL_2),
                                 (lv_style_selector_t)LV_PART_MAIN | LV_STATE_CHECKED);
  lv_obj_set_style_border_color(btn, lv_color_hex(ACCENT),
                                (lv_style_selector_t)LV_PART_MAIN | LV_STATE_CHECKED);

  // Glow ONLY when active. Shadows are LVGL's most expensive draw op, so restricting them
  // to the one or two lit buttons keeps the cost bounded on a 480x480 panel.
  lv_obj_set_style_shadow_color(btn, lv_color_hex(ACCENT_FILL),
                                (lv_style_selector_t)LV_PART_MAIN | LV_STATE_CHECKED);
  lv_obj_set_style_shadow_width(btn, 22, (lv_style_selector_t)LV_PART_MAIN | LV_STATE_CHECKED);
  lv_obj_set_style_shadow_opa(btn, 90, (lv_style_selector_t)LV_PART_MAIN | LV_STATE_CHECKED);

  // Touch-down feedback: shrink and darken slightly. Worth more than it sounds — the real
  // action takes ~0.5 s of network, and this acknowledges the tap immediately.
  lv_obj_set_style_bg_color(btn, lv_color_hex(PRESS_FILL),
                            (lv_style_selector_t)LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_transform_width(btn, -5,
                                   (lv_style_selector_t)LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_transform_height(btn, -5,
                                    (lv_style_selector_t)LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_transition(btn, &pressTransition_, LV_PART_MAIN);

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

// The bezel ring. A round display should show volume as a ring, not only as digits — and
// the busy indicator reuses the SAME geometry and colours, so it reads as one ring changing
// behaviour rather than two competing elements.
void addBezelRing(ScreenWidgets &w) {
  w.arc = lv_arc_create(w.screen);
  lv_obj_set_size(w.arc, RING_SIZE, RING_SIZE);
  lv_obj_center(w.arc);
  lv_arc_set_range(w.arc, 0, 100);
  lv_arc_set_value(w.arc, 0);
  // Gap at the bottom, gauge style: the numerals live down there.
  lv_arc_set_bg_angles(w.arc, 135, 45);
  lv_arc_set_rotation(w.arc, 0);
  lv_obj_remove_style(w.arc, NULL, LV_PART_KNOB);      // display only, not draggable
  lv_obj_clear_flag(w.arc, LV_OBJ_FLAG_CLICKABLE);     // must never eat taps or swipes
  lv_obj_set_style_arc_width(w.arc, RING_WIDTH, LV_PART_MAIN);
  lv_obj_set_style_arc_width(w.arc, RING_WIDTH, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(w.arc, lv_color_hex(RING_TRACK), LV_PART_MAIN);
  lv_obj_set_style_arc_color(w.arc, lv_color_hex(RING_ACCENT), LV_PART_INDICATOR);

  w.spinner = lv_spinner_create(w.screen, 1100, 70);
  lv_obj_set_size(w.spinner, RING_SIZE, RING_SIZE);
  lv_obj_center(w.spinner);
  lv_obj_set_style_arc_width(w.spinner, RING_WIDTH, LV_PART_MAIN);
  lv_obj_set_style_arc_width(w.spinner, RING_WIDTH, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(w.spinner, lv_color_hex(RING_TRACK), LV_PART_MAIN);
  lv_obj_set_style_arc_color(w.spinner, lv_color_hex(RING_ACCENT), LV_PART_INDICATOR);
  lv_obj_clear_flag(w.spinner, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(w.spinner, LV_OBJ_FLAG_HIDDEN);
}

// Which of the three screens you are on. Without this the swipe chain is invisible.
void addPageDots(ScreenWidgets &w, int activeIndex) {
  for (int i = 0; i < 3; i++) {
    lv_obj_t *d = lv_obj_create(w.screen);
    lv_obj_set_size(d, 8, 8);
    lv_obj_align(d, LV_ALIGN_CENTER, (i - 1) * 18, RADIUS + 55);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(d, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(d, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(d, lv_color_hex(i == activeIndex ? TEXT_BRIGHT : 0x3A3A44),
                              LV_PART_MAIN);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
    w.dots[i] = d;
  }
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
  bool forward = (int)s > (int)current_;
  current_ = s;
  ScreenWidgets *w = (s == Screen::Home) ? &home_ : (s == Screen::Volume) ? &vol_ : &modes_;
  lv_scr_load_anim(w->screen,
                   forward ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT,
                   250, 0, false);
  if (screenHandler) screenHandler(s);
}

}  // namespace

void build(TapHandler taps, ActionHandler actions, SceneHandler scenes,
           ScreenChangeHandler onScreenChange) {
  lv_style_transition_dsc_init(&pressTransition_, PRESS_PROPS, lv_anim_path_ease_out,
                               120, 0, NULL);
  tapHandler    = taps;
  actionHandler = actions;
  sceneHandler  = scenes;
  screenHandler = onScreenChange;

  // ---- Screen 1: home -----------------------------------------------------------------
  home_.screen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(home_.screen, lv_color_hex(BG), LV_PART_MAIN);
  lv_obj_set_style_bg_grad_color(home_.screen, lv_color_hex(BG_DEEP), LV_PART_MAIN);
  lv_obj_set_style_bg_grad_dir(home_.screen, LV_GRAD_DIR_VER, LV_PART_MAIN);
  lv_obj_clear_flag(home_.screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(home_.screen, onGesture, LV_EVENT_GESTURE, NULL);

  btnVinyl    = makeButton(home_.screen, "Vinyl", 0, -RADIUS, nullptr, LV_SYMBOL_AUDIO,
                           onTapEvent, &tagVinyl);
  btnMainS1   = makeButton(home_.screen, "Main", -RADIUS, 0, &icon_era300, nullptr,
                           onTapEvent, &tagMainS1);
  btnStereoS1 = makeButton(home_.screen, "Stereo", RADIUS, 0, &icon_era100, nullptr,
                           onTapEvent, &tagStereoS1);
  addVolumeReadout(home_);
  addBezelRing(home_);
  addPageDots(home_, 0);
  addCentreLabel(home_.screen, "SONOS");

  lblStatus = lv_label_create(home_.screen);
  lv_label_set_text(lblStatus, "starting ...");
  lv_obj_set_style_text_font(lblStatus, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblStatus, lv_color_hex(TEXT_DIM), LV_PART_MAIN);
  lv_obj_align(lblStatus, LV_ALIGN_CENTER, 0, 28);

  // ---- Screen 2: per-room volume ------------------------------------------------------
  vol_.screen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(vol_.screen, lv_color_hex(BG), LV_PART_MAIN);
  lv_obj_set_style_bg_grad_color(vol_.screen, lv_color_hex(BG_DEEP), LV_PART_MAIN);
  lv_obj_set_style_bg_grad_dir(vol_.screen, LV_GRAD_DIR_VER, LV_PART_MAIN);
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
  addBezelRing(vol_);
  addPageDots(vol_, 1);
  addCentreLabel(vol_.screen, "VOLUME");

  // ---- Screen 3: one-tap modes --------------------------------------------------------
  modes_.screen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(modes_.screen, lv_color_hex(BG), LV_PART_MAIN);
  lv_obj_set_style_bg_grad_color(modes_.screen, lv_color_hex(BG_DEEP), LV_PART_MAIN);
  lv_obj_set_style_bg_grad_dir(modes_.screen, LV_GRAD_DIR_VER, LV_PART_MAIN);
  lv_obj_clear_flag(modes_.screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(modes_.screen, onGesture, LV_EVENT_GESTURE, NULL);

  btnRadio = makeButton(modes_.screen, "Radio", 0, -RADIUS, nullptr, LV_SYMBOL_WIFI,
                        onSceneEvent, &tagRadio);            // 12 o'clock
  btnHiFi  = makeButton(modes_.screen, "HiFi", -RADIUS, 0, nullptr, LV_SYMBOL_VOLUME_MAX,
                        onSceneEvent, &tagHiFi);             //  9 o'clock
  btnTV    = makeButton(modes_.screen, "TV", RADIUS, 0, nullptr, LV_SYMBOL_VIDEO,
                        onSceneEvent, &tagTV);               //  3 o'clock
  addVolumeReadout(modes_);
  addBezelRing(modes_);
  addPageDots(modes_, 2);
  addCentreLabel(modes_.screen, "MODES");

  attachSplitOverlay(btnStereoS1, splitHome_);
  attachSplitOverlay(btnStereoS2, splitVol_);

  applySelectionStyles();
  lv_scr_load(home_.screen);
  current_ = Screen::Home;
}

void setSceneActive(Scene s, bool active) {
  lv_obj_t *o = (s == Scene::HiFi) ? btnHiFi : (s == Scene::Radio) ? btnRadio : btnTV;
  if (!o) return;
  if (active) lv_obj_add_state(o, LV_STATE_CHECKED);
  else        lv_obj_clear_state(o, LV_STATE_CHECKED);
}

void setVolume(int volume) {
  for (ScreenWidgets *w : {&home_, &vol_, &modes_}) {
    if (w->volume) {
      if (volume < 0) lv_label_set_text(w->volume, "--");
      else            lv_label_set_text_fmt(w->volume, "%d", volume);
    }
    // Animate the ring so a turn glides instead of stepping. Short enough (120 ms) that a
    // fast spin still tracks the hand rather than lagging behind it.
    if (w->arc && volume >= 0) lv_arc_set_value(w->arc, volume);
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

  for (ScreenWidgets *w : {&home_, &vol_, &modes_}) {
    if (w->rooms) lv_label_set_text(w->rooms, text);
  }
}

void setActive(Button b, bool active) {
  lv_obj_t *o = (b == Button::Vinyl) ? btnVinyl
              : (b == Button::Main)  ? btnMainS1
                                     : btnStereoS1;
  if (!o) return;
  if (active) lv_obj_add_state(o, LV_STATE_CHECKED);
  else        lv_obj_clear_state(o, LV_STATE_CHECKED);
}

// Split view for the stereo pair. While separated the two speakers are independent, so a
// single on/off button cannot describe them: the left half keeps the idle face and the
// right half shows whether the right speaker is playing (the TV setup).
void setStereoSplit(bool split, bool rightActive) {
  for (SplitOverlay *ov : {&splitHome_, &splitVol_}) {
    if (!ov->rightHalf) continue;
    if (!split) {
      lv_obj_add_flag(ov->rightHalf, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(ov->divider, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    // Blue only when the right speaker is actually producing sound; otherwise both halves
    // stay grey and only the divider shows, so the button never claims silent audio.
    lv_obj_set_style_bg_color(ov->rightHalf,
                              lv_color_hex(rightActive ? ACCENT_FILL : IDLE_FILL),
                              LV_PART_MAIN);
    lv_obj_clear_flag(ov->rightHalf, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ov->divider, LV_OBJ_FLAG_HIDDEN);
  }
}

void setStatus(const char *text) {
  if (lblStatus) lv_label_set_text(lblStatus, text ? text : "");
}

void setBusy(bool busy) {
  // Swap the static volume ring for the sweeping one. Same radius, width and colour, so it
  // reads as the ring starting to move rather than a second element appearing.
  for (ScreenWidgets *w : {&home_, &vol_, &modes_}) {
    if (!w->spinner || !w->arc) continue;
    if (busy) {
      lv_obj_add_flag(w->arc, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(w->spinner, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_clear_flag(w->arc, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(w->spinner, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

void setSelection(Selection s) {
  selection_ = s;
  applySelectionStyles();
  setRoomVolumes(lastMainVol_, lastStereoVol_);   // re-tint the breakdown
}

Selection selection()    { return selection_; }
Screen    currentScreen(){ return current_; }

}  // namespace ui
