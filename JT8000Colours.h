/**
 * JT8000Colours.h  —  JT-8000 colour palette
 *
 * ALL VALUES ARE STANDARD RGB565
 * ================================
 * The ILI9341 panel powers on with colour inversion active (INVON).
 * UIManager_TFT::beginDisplay() sends INVOFF (0x20) immediately after
 * begin() + setRotation(), so the panel displays standard RGB565 correctly.
 *
 * Formula:  value = ((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3)
 *
 * DO NOT pre-swap R and B — inversion is handled in hardware, not here.
 */

#pragma once

// ---------------------------------------------------------------------------
// Core UI colours — standard RGB565
// Comment shows the target colour as #RRGGBB.
// ---------------------------------------------------------------------------

#define COLOUR_BACKGROUND   0x10A5  // #101428  deep charcoal-navy
#define COLOUR_TEXT         0xD6BC  // #D2D7E1  warm off-white
#define COLOUR_SYSTEXT      0xFD00  // #FFA000  amber orange (JP-8000 LED style)
#define COLOUR_TEXT_DIM     0x7BF1  // #787D8C  steel grey
#define COLOUR_SELECTED     0xFD00  // #FFA000  amber (selected row background)
#define COLOUR_ACCENT       0xF8E3  // #FF1C18  red
#define COLOUR_OSC          0x065F  // #00CBFF  bright cyan
#define COLOUR_FILTER       0xFDA5  // #FFB428  warm amber-yellow
#define COLOUR_ENV          0xD994  // #DC32A0  magenta-pink
#define COLOUR_LFO          0xFB60  // #FF6C00  orange
#define COLOUR_FX           0x06FF  // #00DCFF  light cyan
#define COLOUR_GLOBAL       0x8412  // #828291  neutral grey
#define COLOUR_HEADER_BG    0x1907  // #19233C  dark navy panel
#define COLOUR_BORDER       0x29AA  // #2D3750  blue-grey

// ---------------------------------------------------------------------------
// Scope / meter colours — standard RGB565
// ---------------------------------------------------------------------------
#define COLOUR_SCOPE_WAVE   0x07E7  // #00FF38  bright LCD green
#define COLOUR_SCOPE_ZERO   0x0322  // #006414  dim green
#define COLOUR_SCOPE_BG     0x0060  // #000C00  near-black green tint
#define COLOUR_METER_GREEN  0x16E2  // #14DC14  bright green
#define COLOUR_METER_YELLOW 0xFEE0  // #FFDC00  yellow
#define COLOUR_METER_RED    0xF8E3  // #FF1C18  red

// ---------------------------------------------------------------------------
// Named colours — standard RGB565 (these were always correct)
// ---------------------------------------------------------------------------
#define RED                 0xF800
#define PINK                0xF81F
#define YELLOW              0xFFE0
#define GREEN               0x07E0
#define MIDDLEGREEN         0x0400
#define DARKGREEN           0x02A0
#define DX_DARKCYAN         0x030D

// Grey shades — R==G==B so identical regardless of channel order
#define GREY1               0xC618
#define GREY2               0x52AA
#define GREY3               0x2104
#define GREY4               0x10A2

// Character cell dimensions (mirrors ILI9341_t3n.h)
#ifndef CHAR_width
  #define CHAR_width         12
  #define CHAR_height        17
  #define CHAR_width_small    6
  #define CHAR_height_small   8
#endif
