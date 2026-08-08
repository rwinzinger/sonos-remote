// Sonos product icons for the on-screen buttons.
//
// These are NOT redrawn approximations: each speaker publishes its own product icon over
// the LAN. device_description.xml carries an <iconList> whose <url> points at
// /img/icon-<model>.png on port 1400 — Era 300 = icon-S41.png, Era 100 SL = icon-S62.png,
// both 48x48 8-bit RGBA.
//
// Regenerate (with the speakers reachable):
//   curl -o icon-era300.png http://<main-ip>:1400/img/icon-S41.png
//   curl -o icon-era100.png http://<stereo-ip>:1400/img/icon-S62.png
//   python3 ../tools/png2lvgl.py icon-era300.png icon-era100.png sonos_icons.c
//
// Encoded as LV_IMG_CF_TRUE_COLOR_ALPHA for LV_COLOR_DEPTH 16 / LV_COLOR_16_SWAP 0:
// RGB565 little-endian plus one alpha byte per pixel, 6912 bytes each.
//
// A different Sonos model reports a different icon URL — read it from the device
// description rather than assuming these filenames.

#pragma once

#include <lvgl.h>

extern const lv_img_dsc_t icon_era300;   // "Main"
extern const lv_img_dsc_t icon_era100;   // "Stereo"
