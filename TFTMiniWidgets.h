// TFTMiniWidgets.h
// =============================================================================
// Compact widget drawing functions for the JT-8000 scrollable accordion UI.
//
// These are NOT TFTWidget subclasses. The accordion HomeScreen owns layout and
// scroll position; it calls these stateless draw functions at calculated screen
// coordinates each frame. This avoids allocating hundreds of widget objects for
// 120+ controls — instead, only visible controls are drawn, and the draw cost
// is proportional to what's on screen.
//
// Design rules (same as TFTWidgets.h):
//   - No heap allocation
//   - No fillScreen() — each function clears only its own rect
//   - All string buffers are caller-owned (const char* pointers)
//   - Draw functions complete in < 0.5 ms (measured on ILI9341_t3n @ 30 MHz)
//
// Colour: everything uses COLOUR_ACCENT_ORANGE from JT8000Colours.h.
// =============================================================================

#pragma once
#include <Arduino.h>
#include "ILI9341_t3n.h"
#include "JT8000Colours.h"

// =============================================================================
// Layout constants — pixel dimensions for each mini widget cell
// =============================================================================
namespace MiniLayout {

    // ---- Section header (collapsible bar) ----
    static constexpr int16_t SEC_HDR_H      = 22;    // collapsed section height
    static constexpr int16_t SEC_LED_R      = 3;     // LED dot radius
    static constexpr int16_t SEC_PAD_X      = 8;     // horizontal padding

    // ---- Group header (label above controls) ----
    static constexpr int16_t GRP_HDR_H      = 12;    // group label bar height
    static constexpr int16_t GRP_PAD_BOTTOM = 3;     // gap below controls before next group

    // ---- Mini knob ----
    static constexpr int16_t KNOB_CELL_W    = 42;    // total cell width per knob
    static constexpr int16_t KNOB_CELL_H    = 48;    // total cell height (arc + label + value)
    static constexpr int16_t KNOB_RADIUS    = 12;    // arc radius (24px diameter)
    static constexpr int16_t KNOB_ARC_W     = 2;     // arc stroke thickness
    static constexpr int16_t KNOB_DOT_R     = 2;     // pointer dot radius

    // ---- Mini select (dropdown) ----
    static constexpr int16_t SEL_CELL_W     = 70;    // total cell width
    static constexpr int16_t SEL_CELL_H     = 28;    // total cell height (label + box)
    static constexpr int16_t SEL_BOX_H      = 16;    // dropdown box height

    // ---- Mini toggle (pill) ----
    static constexpr int16_t TOG_CELL_W     = 38;    // total cell width
    static constexpr int16_t TOG_CELL_H     = 28;    // total cell height (label + pill)
    static constexpr int16_t TOG_PILL_W     = 28;    // pill width
    static constexpr int16_t TOG_PILL_H     = 14;    // pill height
    static constexpr int16_t TOG_PILL_R     = 7;     // pill corner radius

    // ---- Seq slider grid ----
    static constexpr int16_t SGRID_SLIDER_W   = 14;    // per-step slider column width
    static constexpr int16_t SGRID_SLIDER_H   = 60;    // slider track height
    static constexpr int16_t SGRID_THUMB_H    = 4;     // thumb height
    static constexpr int16_t SGRID_NUM_H      = 8;     // step number text height
    static constexpr int16_t SGRID_TOTAL_H    = SGRID_SLIDER_H + SGRID_NUM_H + 4; // total grid height

    // Legacy alias for HomeScreen layout calculations
    static constexpr int16_t GRID_BAR_H       = SGRID_SLIDER_H;
    static constexpr int16_t GRID_NUM_H       = SGRID_NUM_H;

    // ---- Spacing ----
    static constexpr int16_t CTRL_GAP_X     = 2;     // horizontal gap between controls
    static constexpr int16_t GROUP_GAP_X    = 6;     // horizontal gap between groups
    static constexpr int16_t BODY_PAD_X     = 6;     // left/right padding inside section body
    static constexpr int16_t BODY_PAD_Y     = 4;     // top/bottom padding inside section body

}  // namespace MiniLayout


// =============================================================================
// MiniKnob — draw a compact 270° arc knob with label and value
//
//   ┌────────┐
//   │  ╭──╮  │  ← 24px diameter arc, orange fill proportional to value
//   │  ╰──╯  │
//   │ LABEL  │  ← 7px uppercase text
//   │  val   │  ← 8px orange value text
//   └────────┘
//
// x,y = top-left of cell. value = 0-127. selected = highlight ring.
// =============================================================================
namespace MiniKnob {

    // Draw the complete knob cell, clearing its rect first.
    void draw(ILI9341_t3n& d, int16_t x, int16_t y,
              uint8_t value, const char* label, const char* valText,
              bool selected);

    // Draw only the arc + pointer (skip label/value) — for value-change repaints.
    // Caller must clear the arc area before calling if background is not uniform.
    void drawArcOnly(ILI9341_t3n& d, int16_t x, int16_t y,
                     uint8_t value, bool selected);

    // Hit-test: is (tx,ty) inside this knob's cell?
    inline bool hitTest(int16_t cellX, int16_t cellY, int16_t tx, int16_t ty) {
        return (tx >= cellX && tx < cellX + MiniLayout::KNOB_CELL_W &&
                ty >= cellY && ty < cellY + MiniLayout::KNOB_CELL_H);
    }
}


// =============================================================================
// MiniSelect — draw a compact dropdown box with label
//
//   ┌──────────────┐
//   │ LABEL        │  ← 7px uppercase dim text
//   │ ┌──────────┐ │
//   │ │ value  ▼ │ │  ← 8px orange text in bordered box
//   │ └──────────┘ │
//   └──────────────┘
// =============================================================================
namespace MiniSelect {

    void draw(ILI9341_t3n& d, int16_t x, int16_t y,
              const char* label, const char* valText,
              bool selected);

    inline bool hitTest(int16_t cellX, int16_t cellY, int16_t tx, int16_t ty) {
        return (tx >= cellX && tx < cellX + MiniLayout::SEL_CELL_W &&
                ty >= cellY && ty < cellY + MiniLayout::SEL_CELL_H);
    }
}


// =============================================================================
// MiniToggle — draw an on/off pill switch with label
//
//   ┌──────────┐
//   │  LABEL   │  ← 7px uppercase dim text
//   │  [  ON ] │  ← pill: filled orange when on, dim border when off
//   └──────────┘
// =============================================================================
namespace MiniToggle {

    void draw(ILI9341_t3n& d, int16_t x, int16_t y,
              const char* label, bool isOn, bool selected);

    inline bool hitTest(int16_t cellX, int16_t cellY, int16_t tx, int16_t ty) {
        return (tx >= cellX && tx < cellX + MiniLayout::TOG_CELL_W &&
                ty >= cellY && ty < cellY + MiniLayout::TOG_CELL_H);
    }
}


// =============================================================================
// MiniSliderGrid — step sequencer as 16 vertical sliders (unipolar 0-127)
//
// Each step is a vertical slider: fill from bottom proportional to value.
// Tap a slider to select that step for editing (encoder R adjusts value).
// Playing step gets a bright border. Selected step gets an orange border.
// Values are unipolar: 0 = empty (bottom), 127 = full (top).
//
//   ┌─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┐
//   │ │ │█│ │ │█│ │ │ │█│ │ │ │ │ │ │  ← fill from bottom
//   │ │█│█│ │█│█│█│ │ │█│ │ │ │ │ │ │
//   │█│█│█│█│█│█│█│█│ │█│ │ │ │ │ │ │
//   ├─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┤
//   │1│2│3│4│5│6│7│8│9│…│ │ │ │ │ │ │  ← step numbers
//   └─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┘
// =============================================================================
namespace MiniSliderGrid {

    // Draw the complete slider grid. stepCount = active steps (1-16).
    // values[] has 16 entries (0-127 each, unipolar).
    // selectedStep = currently selected for editing (-1 = none).
    // playingStep = currently playing (-1 = none).
    void draw(ILI9341_t3n& d, int16_t x, int16_t y, int16_t w,
              uint8_t stepCount, const uint8_t values[16],
              int8_t selectedStep, int8_t playingStep);

    // Hit-test: returns step index (0-15) at touch position, or -1 if miss.
    int8_t hitTestStep(int16_t gridX, int16_t gridY, int16_t gridW,
                       uint8_t stepCount, int16_t tx, int16_t ty);
}


// =============================================================================
// SectionHeader — draw a collapsible section header bar
//
//   ┌──────────────────────────────────────┐
//   │ ● Oscillator 1                    ▼  │
//   └──────────────────────────────────────┘
// =============================================================================
namespace SectionHeader {

    void draw(ILI9341_t3n& d, int16_t x, int16_t y, int16_t w,
              const char* label, bool expanded, bool highlighted);

    inline bool hitTest(int16_t barX, int16_t barY, int16_t barW,
                        int16_t tx, int16_t ty) {
        return (tx >= barX && tx < barX + barW &&
                ty >= barY && ty < barY + MiniLayout::SEC_HDR_H);
    }
}


// =============================================================================
// GroupHeader — draw a small group label
//
//   WAVE & TUNING
//   ─────────────
// =============================================================================
namespace GroupHeader {

    void draw(ILI9341_t3n& d, int16_t x, int16_t y, int16_t w,
              const char* label);
}
