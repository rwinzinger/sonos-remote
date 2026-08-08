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
// Same size as a button, deliberately: the four circles sit on one ring and any size
// difference reads as a mistake rather than emphasis. Derived from BTN_SIZE so they cannot
// drift apart.
const int VOL_SIZE  = BTN_SIZE;

// The volume circle's outline and the layout ring are the same stroke, deliberately: they
// read as one construction line. Shared constants so they cannot drift apart.
const uint32_t RING_LINE_COLOR = 0x4A4A54;
const int      RING_LINE_W     = BORDER_W;

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

// DERIVED from ACCENT_FILL, not a second copy of the same literal: the volume ring and an
// active button face are meant to be the same blue, and two independent constants holding
// 0x86C8F5 would silently diverge the first time one is tweaked. It keeps its own name
// because the ring sits on the near-black background while the button face does not, so a
// deliberate divergence stays possible — it just has to be explicit.
//
// Only arc layer 0 shows this pure; the rest are the same hue at falling opacity. It matches
// the TOP of the active button's gradient, which darkens to ACCENT_FILL_2.
const uint32_t RING_ACCENT = ACCENT_FILL;
const uint32_t RING_TRACK  = 0x22222A;
const uint32_t TEXT_BRIGHT = 0xF0F0F5;
const uint32_t TEXT_DIM    = 0x8A8A95;

// Now-playing line. Width is capped because the Main and Stereo buttons close in to +/-95 px.
#define NP_FONT (&lv_font_montserrat_16)
const int NP_WIDTH  = 180;
const uint32_t BG          = 0x14141A;   // top of the background gradient
const uint32_t BG_DEEP     = 0x08080B;   // bottom — subtle depth without visible banding

// Gradient partners, kept CLOSE to their base colour on purpose: at 16-bit colour depth a
// long gradient bands visibly, so these are short-range and low-contrast.
const uint32_t IDLE_FILL_2   = 0x8A8A90;
const uint32_t ACCENT_FILL_2 = 0x63A9DC;
const uint32_t PRESS_FILL    = 0x7E7E85;

const int RING_SIZE  = 460;
const int RING_WIDTH = 8;      // legacy single-width bezel; layers below supersede it

struct ScreenWidgets;
void setArcLayers(void *screenWidgets, int32_t value);

// A wide ring that fades inward. LVGL 8.3 has NO radial gradient for arcs — bg_grad_dir is
// linear and applies to backgrounds, not arc strokes — so the fade is built from concentric
// arcs with a falling opacity ramp. Six layers of 6 px stepping inward by 5 px overlap
// slightly, which hides the seams; the band spans radius 199..230, i.e. ~32 px.
const int ARC_LAYERS      = 6;
const int ARC_LAYER_WIDTH = 6;
const int ARC_LAYER_STEP  = 10;    // diameter step; radius step is half of this
// Gamma-shaped, NOT linear. Perceived brightness follows roughly alpha^0.43, so an evenly
// spaced alpha ramp looks top-heavy: the outer layers read as one solid band and then the
// fade falls off a cliff. Falling faster early gives visually even steps.
const lv_opa_t ARC_LAYER_OPA[ARC_LAYERS] = {255, 170, 105, 60, 30, 12};

// Per-screen widgets that need updating from outside.
struct ScreenWidgets {
  lv_obj_t *screen  = nullptr;
  lv_obj_t *volume  = nullptr;
  lv_obj_t *rooms   = nullptr;
  lv_obj_t *arcLayers[6] = {nullptr};  // volume ring, outermost first
  lv_obj_t *spinner = nullptr;        // same geometry, shown instead while busy
  lv_obj_t *dots[3] = {nullptr, nullptr, nullptr};
  lv_obj_t *nowPlaying = nullptr;
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

// Animation target: sets every layer of one screen's ring from a single animation.
void setArcLayers(void *screenWidgets, int32_t value) {
  ScreenWidgets *w = (ScreenWidgets *)screenWidgets;
  for (int i = 0; i < ARC_LAYERS; i++) {
    if (w->arcLayers[i]) lv_arc_set_value(w->arcLayers[i], value);
  }
}

// A faint circle through the centres of the four elements at 12/9/3/6, drawn behind them
// so they read as beads on one ring rather than four independent blobs. The busy sweep
// lives here too — see below.
void addLayoutRing(ScreenWidgets &w) {
  lv_obj_t *ring = lv_obj_create(w.screen);
  // + RING_LINE_W, not just RADIUS*2: LVGL draws the border INWARD from the bounding box,
  // so a 2*RADIUS box puts the stroke's centreline at RADIUS - w/2 and the ring runs
  // visibly inside the button centres. Growing the box by one stroke width lands the
  // centreline exactly on RADIUS.
  lv_obj_set_size(ring, RADIUS * 2 + RING_LINE_W, RADIUS * 2 + RING_LINE_W);
  lv_obj_center(ring);
  lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(ring, RING_LINE_W, LV_PART_MAIN);
  lv_obj_set_style_border_color(ring, lv_color_hex(RING_LINE_COLOR), LV_PART_MAIN);
  lv_obj_set_style_pad_all(ring, 0, LV_PART_MAIN);
  lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);

  // The busy sweep runs ON the layout ring rather than the outer bezel, which belongs to
  // the volume. Its track is transparent so the static ring shows through and only the
  // moving segment is drawn — it reads as a light travelling around the construction line.
  w.spinner = lv_spinner_create(w.screen, 1100, 70);
  lv_obj_set_size(w.spinner, RADIUS * 2 + RING_LINE_W, RADIUS * 2 + RING_LINE_W);
  lv_obj_center(w.spinner);
  lv_obj_set_style_arc_width(w.spinner, RING_LINE_W, LV_PART_MAIN);
  lv_obj_set_style_arc_width(w.spinner, RING_LINE_W, LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(w.spinner, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_color(w.spinner, lv_color_hex(RING_ACCENT), LV_PART_INDICATOR);
  lv_obj_clear_flag(w.spinner, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(w.spinner, LV_OBJ_FLAG_HIDDEN);

  // Z-order: spinner to the back first, then the ring, so both land BELOW the buttons
  // (created earlier) while the sweep still draws on top of the static ring.
  lv_obj_move_background(w.spinner);
  lv_obj_move_background(ring);
}


// The outer bezel ring: volume only. The busy sweep used to share this band and had to
// hide the volume while working — it now lives on the inner layout ring instead, so both
// can be shown at once.
void addBezelRing(ScreenWidgets &w) {
  for (int i = 0; i < ARC_LAYERS; i++) {
    lv_obj_t *a = lv_arc_create(w.screen);
    int size = RING_SIZE - i * ARC_LAYER_STEP;
    lv_obj_set_size(a, size, size);
    lv_obj_center(a);
    lv_arc_set_range(a, 0, 100);
    lv_arc_set_value(a, 0);
    // Gap at the bottom, gauge style: the numerals live down there.
    lv_arc_set_bg_angles(a, 135, 45);
    lv_arc_set_rotation(a, 0);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);      // display only, not draggable
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);     // must never eat taps or swipes
    lv_obj_set_style_arc_width(a, ARC_LAYER_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, ARC_LAYER_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, lv_color_hex(RING_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(RING_ACCENT), LV_PART_INDICATOR);
    // The fade: full opacity at the rim, almost nothing by the innermost layer.
    lv_obj_set_style_arc_opa(a, ARC_LAYER_OPA[i], LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(a, (lv_opa_t)(ARC_LAYER_OPA[i] / 3), LV_PART_MAIN);
    w.arcLayers[i] = a;
  }

}

// Which of the three screens you are on. Without this the swipe chain is invisible.
void addPageDots(ScreenWidgets &w, int activeIndex) {
  for (int i = 0; i < 3; i++) {
    lv_obj_t *d = lv_obj_create(w.screen);
    lv_obj_set_size(d, 8, 8);
    lv_obj_align(d, LV_ALIGN_CENTER, (i - 1) * 18, 40);
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

// Now playing, under the screen's title. Same dim treatment as the per-room volume figures,
// so it reads as secondary information rather than competing with the wordmark. Width is
// capped at 180 px because the Main and Stereo buttons close in to +/-95 px; longer titles
// scroll instead of colliding with them.
void addNowPlaying(ScreenWidgets &w) {
  w.nowPlaying = lv_label_create(w.screen);
  lv_label_set_long_mode(w.nowPlaying, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_width(w.nowPlaying, NP_WIDTH);
  lv_label_set_text(w.nowPlaying, "");
  lv_obj_set_style_text_font(w.nowPlaying, NP_FONT, LV_PART_MAIN);
  lv_obj_set_style_text_color(w.nowPlaying, lv_color_hex(TEXT_DIM), LV_PART_MAIN);
  lv_obj_set_style_text_align(w.nowPlaying, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(w.nowPlaying, LV_ALIGN_CENTER, 0, 12);
}

// Volume lives in its own circle at 6 o'clock, completing the 12/9/3/6 ring. Outline only,
// not a filled face like the buttons: it is a READOUT, and a face identical to the buttons
// would imply it can be tapped. Both figures live inside it — group large, per-room small.
void addVolumeReadout(ScreenWidgets &w) {
  lv_obj_t *circle = lv_obj_create(w.screen);
  lv_obj_set_size(circle, VOL_SIZE, VOL_SIZE);
  lv_obj_align(circle, LV_ALIGN_CENTER, 0, RADIUS);
  lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(circle, lv_color_hex(BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(circle, 160, LV_PART_MAIN);
  lv_obj_set_style_border_width(circle, RING_LINE_W, LV_PART_MAIN);
  lv_obj_set_style_border_color(circle, lv_color_hex(RING_LINE_COLOR), LV_PART_MAIN);
  lv_obj_set_style_pad_all(circle, 0, LV_PART_MAIN);
  lv_obj_clear_flag(circle, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(circle, LV_OBJ_FLAG_SCROLLABLE);

  w.volume = lv_label_create(circle);
  lv_label_set_text(w.volume, "--");
  // 40 rather than 48: inside a 110 px circle the larger size left almost no margin around
  // a three-digit value, and the circle now matches the buttons so it cannot grow instead.
  lv_obj_set_style_text_font(w.volume, &lv_font_montserrat_40, LV_PART_MAIN);
  lv_obj_set_style_text_color(w.volume, lv_color_hex(TEXT_BRIGHT), LV_PART_MAIN);
  lv_obj_align(w.volume, LV_ALIGN_CENTER, 0, -10);

  w.rooms = lv_label_create(circle);
  // Recolour markup lets ONE side of "13 / 25" turn red without splitting it into
  // separate labels.
  lv_label_set_recolor(w.rooms, true);
  lv_label_set_text(w.rooms, "");
  lv_obj_set_style_text_font(w.rooms, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(w.rooms, lv_color_hex(TEXT_DIM), LV_PART_MAIN);
  lv_obj_align(w.rooms, LV_ALIGN_CENTER, 0, 26);
}

lv_obj_t *addCentreLabel(lv_obj_t *parent, const char *text) {
  lv_obj_t *lbl = lv_label_create(parent);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(lbl, lv_color_hex(TEXT_BRIGHT), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(lbl, 7, LV_PART_MAIN);
  // +4 optical nudge: all-caps text has no descenders, so true geometric centring reads high.
  // Sits above centre: the now-playing line lives underneath, and the transient status
  // line under that.
  lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -16);
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
  addLayoutRing(home_);
  addVolumeReadout(home_);
  addBezelRing(home_);
  addPageDots(home_, 0);
  addCentreLabel(home_.screen, "SONOS");
  addNowPlaying(home_);

  lblStatus = lv_label_create(home_.screen);
  lv_label_set_text(lblStatus, "starting ...");
  lv_obj_set_style_text_font(lblStatus, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblStatus, lv_color_hex(TEXT_DIM), LV_PART_MAIN);
  // Below the dots: the dots are permanent, the status is transient, so the fixed element
  // sits nearer the text it belongs to.
  lv_obj_align(lblStatus, LV_ALIGN_CENTER, 0, 64);

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

  addLayoutRing(vol_);
  addVolumeReadout(vol_);
  addBezelRing(vol_);
  addPageDots(vol_, 1);
  addCentreLabel(vol_.screen, "VOLUME");
  addNowPlaying(vol_);

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
  addLayoutRing(modes_);
  addVolumeReadout(modes_);
  addBezelRing(modes_);
  addPageDots(modes_, 2);
  addCentreLabel(modes_.screen, "MODES");
  addNowPlaying(modes_);

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
    // Glide rather than step. ONE animation drives all six layers via setArcLayers, rather
    // than six animations racing each other. Any running one is deleted first, so a fast
    // spin re-aims at the newest value instead of queueing a backlog.
    if (w->arcLayers[0] && volume >= 0) {
      lv_anim_del(w, setArcLayers);
      lv_anim_t a;
      lv_anim_init(&a);
      lv_anim_set_var(&a, w);
      lv_anim_set_exec_cb(&a, setArcLayers);
      lv_anim_set_values(&a, lv_arc_get_value(w->arcLayers[0]), volume);
      lv_anim_set_time(&a, 120);
      lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
      lv_anim_start(&a);
    }
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

// Empty text simply clears the line; the wordmark stays put on every screen.
void setNowPlaying(const char *text) {
  // Four spaces each side. Padding is added HERE, not in the sonos module: it is a display
  // concern and the module should keep returning clean metadata.
  //
  // Symmetric on purpose. Short text stays visually CENTRED (equal padding cancels out) and
  // simply does not scroll, while long text overflows and scrolls — and at the wrap the
  // trailing four meet the leading four, giving an eight-space break without a lopsided
  // offset. An earlier version measured the text and padded it enough to force even short
  // strings to scroll; that was consistent but left a long blank sweep after a short name.
  static char padded[160];
  if (text && *text) {
    snprintf(padded, sizeof(padded), "    %s    ", text);
  } else {
    padded[0] = '\0';
  }

  for (ScreenWidgets *w : {&home_, &vol_, &modes_}) {
    if (!w->nowPlaying) continue;
    // Only re-set when it actually changed: otherwise every refresh restarts the scroll
    // animation and a long title never gets read to the end.
    if (strcmp(lv_label_get_text(w->nowPlaying), padded) != 0) {
      lv_label_set_text(w->nowPlaying, padded);
    }
  }
}

void setStatus(const char *text) {
  if (lblStatus) lv_label_set_text(lblStatus, text ? text : "");
}

void setBusy(bool busy) {
  // Swap the static volume ring for the sweeping one. Same radius, width and colour, so it
  // reads as the ring starting to move rather than a second element appearing.
  for (ScreenWidgets *w : {&home_, &vol_, &modes_}) {
    if (!w->spinner) continue;
    // The volume ring STAYS VISIBLE while busy: the sweep now lives on the inner layout
    // ring, so the two no longer compete for the same band.
    if (busy) lv_obj_clear_flag(w->spinner, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(w->spinner, LV_OBJ_FLAG_HIDDEN);
  }
}

void setSelection(Selection s) {
  selection_ = s;
  applySelectionStyles();
  setRoomVolumes(lastMainVol_, lastStereoVol_);   // re-tint the breakdown
}

void goHome() {
  // showScreen() picks MOVE_RIGHT for a backwards jump, so this is exactly the swipe-back
  // animation — including from Modes, which slides two screens in one motion.
  showScreen(Screen::Home);
}

Selection selection()    { return selection_; }
Screen    currentScreen(){ return current_; }

}  // namespace ui
