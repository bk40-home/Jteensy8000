/**
 * JT8000Colours.h  —  JT-8000 unified orange colour palette
 *
 * ALL VALUES ARE STANDARD RGB565
 * ================================
 * The ILI9341 panel powers on with colour inversion active (INVON).
 * UIManager_TFT::beginDisplay() sends INVOFF (0x20) immediately after
 * begin() + setRotation(), so the panel displays standard RGB565 correctly.
 *
 * Formula:  value = ((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3)
 *
 * DESIGN: Single orange accent throughout. No per-section colours.
 * Scope waveform = orange. Status indicators (CPU, meter) keep their
 * functional traffic-light colours. Cancel/alert = red.
 */

#pragma once

// ---------------------------------------------------------------------------
// Core UI colours — standard RGB565
// Comment shows the target colour as #RRGGBB.
// ---------------------------------------------------------------------------

// ---- Background tones (dark navy/charcoal) ----
#define COLOUR_BACKGROUND   0x10A5  // #101428  deep charcoal-navy
#define COLOUR_HEADER_BG    0x1907  // #19233C  dark navy panel
#define COLOUR_SURFACE      0x1148  // #111620  section body fill
#define COLOUR_SURFACE2     0x1989  // #161C28  group / card background
#define COLOUR_SURFACE3     0x21EB  // #1C2436  hover / pressed highlight

// ---- Text ----
#define COLOUR_TEXT         0xD6BC  // #D2D7E1  warm off-white (primary)
#define COLOUR_TEXT_HI      0xCF3E  // #C8D8F4  bright white (headers, selected)
#define COLOUR_TEXT_DIM     0x7BF1  // #787D8C  steel grey (secondary / hints)
#define COLOUR_TEXT_MUTED   0x3A6E  // #3E5070  very dim (group labels, chevrons)

// ---- Orange accent — unified for ALL sections, widgets, knobs ----
#define COLOUR_ACCENT_ORANGE  0xFD00  // #FFA000  amber orange (primary accent)
#define COLOUR_ACCENT_HI      0xFD20  // #FFA040  bright orange (hover / active)
#define COLOUR_ACCENT_DIM     0x6A80  // #6A3508  dim orange (inactive tracks)

// ---- Backward-compatible aliases ----
// Every section colour now resolves to the same orange.
// Existing code referencing COLOUR_OSC, COLOUR_FILTER, etc. auto-unifies.
#define COLOUR_SYSTEXT      COLOUR_ACCENT_ORANGE
#define COLOUR_SELECTED     COLOUR_ACCENT_ORANGE
#define COLOUR_OSC          COLOUR_ACCENT_ORANGE
#define COLOUR_FILTER       COLOUR_ACCENT_ORANGE
#define COLOUR_ENV          COLOUR_ACCENT_ORANGE
#define COLOUR_LFO          COLOUR_ACCENT_ORANGE
#define COLOUR_FX           COLOUR_ACCENT_ORANGE
#define COLOUR_GLOBAL       COLOUR_ACCENT_ORANGE
#define COLOUR_SEQ          COLOUR_ACCENT_ORANGE

// ---- Functional colours (not decorative — keep distinct) ----
#define COLOUR_ACCENT_RED   0xF8E3  // #FF1C18  cancel / alert / destructive
#define COLOUR_ACCENT       COLOUR_ACCENT_RED  // backward compat for TFTWidgets
#define COLOUR_BORDER       0x29AA  // #2D3750  blue-grey borders

// ---------------------------------------------------------------------------
// Scope — orange waveform, background-matched erase (no ghost artefacts)
// ---------------------------------------------------------------------------
#define COLOUR_SCOPE_WAVE   COLOUR_ACCENT_ORANGE  // orange waveform trace
#define COLOUR_SCOPE_ZERO   COLOUR_ACCENT_DIM     // dim orange centre line
#define COLOUR_SCOPE_BG     COLOUR_BACKGROUND     // erase colour = background

// Status meters keep traffic-light scheme (functional indication)
#define COLOUR_METER_GREEN  0x16E2  // #14DC14  bright green (safe)
#define COLOUR_METER_YELLOW 0xFEE0  // #FFDC00  yellow (caution)
#define COLOUR_METER_RED    0xF8E3  // #FF1C18  red (clipping)

// ---------------------------------------------------------------------------
// Named colours — standard RGB565
// ---------------------------------------------------------------------------
#define RED                 0xF800
#define PINK                0xF81F
#define YELLOW              0xFFE0
#define GREEN               0x07E0
#define MIDDLEGREEN         0x0400
#define DARKGREEN           0x02A0
#define DX_DARKCYAN         0x030D

// Grey shades — R==G==B so channel order irrelevant
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
