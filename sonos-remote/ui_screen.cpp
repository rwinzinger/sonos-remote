#include "ui_screen.h"

#include <lvgl.h>
#include "sonos_icons.h"

namespace ui {
namespace {

// Round panel: everything must stay inside a 480 px circle, so the buttons sit on a
// radius rather than in screen corners. 150 px from centre keeps a 110 px button fully
// inside the glass with margin for the bezel.
const int RADIUS      = 150;
const int BTN_SIZE    = 110;

// One accent colour for the whole UI — active buttons and the busy ring. Muted violet,
// deliberately low-saturation so an active button reads as "on" without shouting.
// The device icons are near-black and cannot be recoloured convincingly, so the BUTTONS
// carry the contrast: light faces throughout, with active state shown by a brighter face
// and a white border rather than by hue.
// Active state is carried by HUE, not brightness — grey-on-grey was too subtle to read at
// a glance. The blue stays light enough for the near-black icons and labels to hold up.
const uint32_t IDLE_FILL       = 0xA0A0A0;   // darker grey when idle
const uint32_t ACCENT_FILL     = 0x86C8F5;   // light blue ACTIVE face
const uint32_t IDLE_BORDER     = 0xD0D0D0;
const uint32_t ACCENT          = 0xFFFFFF;   // ACTIVE border
const uint32_t BTN_TEXT        = 0x14141A;   // dark label, to match the dark icons

// Ring shares the active-button blue so transient activity and persistent state read as
// one palette. Still its own constant: the ring sits on the near-black background, the
// button face does not, so they may need to diverge again.
const uint32_t RING_ACCENT     = 0x86C8F5;
const uint32_t RING_TRACK      = 0x22222A;

lv_obj_t *screen      = nullptr;
lv_obj_t *btnRecords  = nullptr;
lv_obj_t *btnMain     = nullptr;
lv_obj_t *btnStereo   = nullptr;
lv_obj_t *lblVolume   = nullptr;
lv_obj_t *lblStatus   = nullptr;
lv_obj_t *lblRooms    = nullptr;
lv_obj_t *lblLogo     = nullptr;
lv_obj_t *spinner     = nullptr;

// Busy ring: a full-width spinner tracking the bezel, which is what a round display is
// for. Buttons reach 205 px from centre (150 radius + half of 110), so a 460 px ring at
// 230 px clears them without crowding.
const int RING_SIZE  = 460;
const int RING_WIDTH = 8;

TapHandler tapHandler = nullptr;

void onButtonEvent(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  if (!tapHandler) return;
  Button *which = (Button *)lv_event_get_user_data(e);
  if (which) tapHandler(*which);
}

// icon: the speaker's own product image, or nullptr to fall back to `symbol` text.
lv_obj_t *makeButton(lv_obj_t *parent, const char *text, int dx, int dy, Button *tag,
                     const lv_img_dsc_t *icon, const char *symbol) {
  lv_obj_t *btn = lv_btn_create(parent);
  lv_obj_set_size(btn, BTN_SIZE, BTN_SIZE);
  lv_obj_align(btn, LV_ALIGN_CENTER, dx, dy);

  // Circular buttons suit the round display better than rectangles.
  lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(btn, lv_color_hex(IDLE_FILL), LV_PART_MAIN);
  lv_obj_set_style_border_width(btn, 3, LV_PART_MAIN);
  lv_obj_set_style_border_color(btn, lv_color_hex(IDLE_BORDER), LV_PART_MAIN);

  // Active/checked styling — this is how the user sees which speakers are on.
  lv_obj_set_style_bg_color(btn, lv_color_hex(ACCENT_FILL), (lv_style_selector_t)LV_PART_MAIN | LV_STATE_CHECKED);
  lv_obj_set_style_border_color(btn, lv_color_hex(ACCENT), (lv_style_selector_t)LV_PART_MAIN | LV_STATE_CHECKED);

  lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
  lv_obj_add_event_cb(btn, onButtonEvent, LV_EVENT_CLICKED, tag);

  // Icon above, name below. The image is 48 px inside a 110 px button, which leaves room
  // for the label without either crowding the circular border.
  if (icon) {
    lv_obj_t *img = lv_img_create(btn);
    lv_img_set_src(img, icon);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, -18);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);   // let taps reach the button
    // Icons stay as Sonos ships them (near-black). The BUTTON is light instead — see the
    // fill colours above. Recolouring the icons white was tried and looked wrong.
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
  lv_obj_align(lbl, LV_ALIGN_CENTER, 0, (icon || symbol) ? 28 : 0);

  return btn;
}

// Static tags so the event user_data stays valid for the life of the screen.
Button tagRecords = Button::Records;
Button tagMain    = Button::Main;
Button tagStereo  = Button::Stereo;

lv_obj_t *objFor(Button b) {
  switch (b) {
    case Button::Records: return btnRecords;
    case Button::Main:    return btnMain;
    case Button::Stereo:  return btnStereo;
  }
  return nullptr;
}

}  // namespace

void build(TapHandler handler) {
  tapHandler = handler;

  screen = lv_scr_act();
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x101014), LV_PART_MAIN);
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

  // Main and Stereo use the speakers' OWN product icons (see sonos_icons.h). Records has
  // no device of its own, so it uses LVGL's built-in audio glyph.
  btnRecords = makeButton(screen, "Records", 0, -RADIUS, &tagRecords,
                          nullptr, LV_SYMBOL_AUDIO);                     // 12 o'clock
  btnMain    = makeButton(screen, "Main",    -RADIUS, 0,  &tagMain,
                          &icon_era300, nullptr);                        //  9 o'clock
  btnStereo  = makeButton(screen, "Stereo",  RADIUS,  0,  &tagStereo,
                          &icon_era100, nullptr);                        //  3 o'clock

  // 6 o'clock: volume. Big digits — legible across the room, which is the whole point of
  // a physical knob.
  lblVolume = lv_label_create(screen);
  lv_label_set_text(lblVolume, "--");
  lv_obj_set_style_text_font(lblVolume, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblVolume, lv_color_hex(0xF0F0F5), LV_PART_MAIN);
  lv_obj_align(lblVolume, LV_ALIGN_CENTER, 0, RADIUS - 10);

  // Centre: the SONOS wordmark. Rendered as letter-spaced type rather than a bitmap —
  // see the note in setStatus() about embedding the real logo.
  lblLogo = lv_label_create(screen);
  lv_label_set_text(lblLogo, "SONOS");
  lv_obj_set_style_text_font(lblLogo, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblLogo, lv_color_hex(0xF0F0F5), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(lblLogo, 7, LV_PART_MAIN);
  // +4 is an OPTICAL correction, not a layout offset. LVGL centres the label's bounding
  // box, but "SONOS" is all caps with no descenders, so the glyphs sit in the upper part
  // of that box and geometric centring reads as too high. Nudging down by roughly half
  // the descender space centres what the eye actually sees.
  lv_obj_align(lblLogo, LV_ALIGN_CENTER, 0, 4);

  // Status now sits UNDER the logo and is empty when idle, so the resting screen is just
  // logo + volume rather than a room name.
  // Per-room breakdown under the group number. GetGroupVolume returns the AVERAGE across
  // members, so the big number alone cannot show that e.g. Main sits at 13 while Stereo is
  // at 25. Kept small and dim: the group figure is what the knob moves and stays primary.
  lblRooms = lv_label_create(screen);
  lv_label_set_text(lblRooms, "");
  lv_obj_set_style_text_font(lblRooms, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblRooms, lv_color_hex(0x8A8A95), LV_PART_MAIN);
  lv_obj_align(lblRooms, LV_ALIGN_CENTER, 0, RADIUS + 26);

  lblStatus = lv_label_create(screen);
  lv_label_set_text(lblStatus, "starting ...");
  lv_obj_set_style_text_font(lblStatus, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblStatus, lv_color_hex(0x8A8A95), LV_PART_MAIN);
  lv_obj_align(lblStatus, LV_ALIGN_CENTER, 0, 28);

  // Busy ring at the bezel. Hidden when idle.
  spinner = lv_spinner_create(screen, 1100, 70);
  lv_obj_set_size(spinner, RING_SIZE, RING_SIZE);
  lv_obj_center(spinner);
  lv_obj_set_style_arc_width(spinner, RING_WIDTH, LV_PART_MAIN);
  lv_obj_set_style_arc_width(spinner, RING_WIDTH, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(spinner, lv_color_hex(RING_TRACK), LV_PART_MAIN);
  lv_obj_set_style_arc_color(spinner, lv_color_hex(RING_ACCENT), LV_PART_INDICATOR);
  lv_obj_clear_flag(spinner, LV_OBJ_FLAG_CLICKABLE);   // must never steal taps
  lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
}

void setBusy(bool busy) {
  if (!spinner) return;
  if (busy) {
    lv_obj_clear_flag(spinner, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
  }
}

void setVolume(int volume) {
  if (!lblVolume) return;
  if (volume < 0) {
    lv_label_set_text(lblVolume, "--");
  } else {
    lv_label_set_text_fmt(lblVolume, "%d", volume);
  }
}

void setActive(Button b, bool active) {
  lv_obj_t *o = objFor(b);
  if (!o) return;
  if (active) {
    lv_obj_add_state(o, LV_STATE_CHECKED);
  } else {
    lv_obj_clear_state(o, LV_STATE_CHECKED);
  }
}

// Rooms whose own level differs from the group average. -1 = unknown, which renders blank
// rather than a misleading placeholder.
void setRoomVolumes(int mainVol, int stereoVol) {
  if (!lblRooms) return;
  if (mainVol < 0 && stereoVol < 0) {
    lv_label_set_text(lblRooms, "");
  } else if (mainVol < 0) {
    lv_label_set_text_fmt(lblRooms, "-- / %d", stereoVol);
  } else if (stereoVol < 0) {
    lv_label_set_text_fmt(lblRooms, "%d / --", mainVol);
  } else {
    lv_label_set_text_fmt(lblRooms, "%d / %d", mainVol, stereoVol);
  }
}

void setStatus(const char *text) {
  if (lblStatus) lv_label_set_text(lblStatus, text ? text : "");
}

}  // namespace ui
