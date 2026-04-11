// PresetBrowser.h
// =============================================================================
// Full-screen preset browser modal for the JT-8000.
//
// Layout (320 × 240 px):
//   ┌──────────────────────────────────────┐
//   │  PRESET BROWSER            [CANCEL]  │  ← header  28 px
//   ├──────────────────────────────────────┤
//   │► 00 PORTAPAD                         │  ← selected (highlighted)
//   │  01 CHROME PD                        │
//   │  02 LYRE                             │
//   │  03 WISHFISH                         │  ← 7 visible rows × 26 px
//   │  04 PULSAR                           │
//   │  05 TAPESTORM                        │
//   │  06 TIBEPIUM                         │
//   ├──────────────────────────────────────┤
//   │  [◄ PREV]              [NEXT ►]      │  ← footer  30 px
//   └──────────────────────────────────────┘
//
// Interaction:
//   Encoder delta  → scroll cursor up/down (wraps)
//   Encoder press  → confirm and close
//   Tap CANCEL     → close without loading
//   Tap PREV/NEXT  → page up/down (7 patches at a time)
//   Tap row once   → move cursor
//   Tap row twice  → confirm and close
//
// Usage:
//   PresetBrowser _browser;
//   _browser.open(synth, currentIdx);        // open
//   if (_browser.isOpen()) {
//       _browser.draw(tft);                  // call every frame
//       _browser.onEncoder(delta);           // encoder input
//       _browser.onTouch(tx, ty);            // touch input
//   }
// =============================================================================

#pragma once
#include "ILI9341_t3n.h"
#include "Presets.h"
#include "SynthEngine.h"
#include "JT8000Colours.h"

// ─────────────────────────────────────────────────────────────────────────────
// Layout constants
// ─────────────────────────────────────────────────────────────────────────────
namespace PBLayout {
    static constexpr uint16_t W            = 320;
    static constexpr uint16_t H            = 240;
    static constexpr uint16_t HDR_H        = 28;
    static constexpr uint16_t FTR_H        = 30;
    static constexpr uint16_t ROW_H        = 26;
    static constexpr int      VISIBLE_ROWS = 7;
    static constexpr uint16_t LIST_Y       = HDR_H;
    static constexpr uint16_t LIST_H       = VISIBLE_ROWS * ROW_H;   // 182 px
    static constexpr uint16_t FTR_Y        = LIST_Y + LIST_H;        // 210
    static constexpr uint16_t BTN_W        = 80;
    static constexpr uint16_t BTN_H        = FTR_H - 4;
    static constexpr uint16_t CANCEL_W     = 70;
    static constexpr uint16_t CANCEL_X     = W - CANCEL_W - 4;
    static constexpr uint16_t CANCEL_Y     = 2;
    static constexpr uint16_t CANCEL_H     = HDR_H - 4;
}

// ─────────────────────────────────────────────────────────────────────────────
// Colour palette — BGR565 pre-swapped for ILI9341 BGR panel
// Formula: send = ((B>>3)<<11)|((G>>2)<<5)|(R>>3)
// ─────────────────────────────────────────────────────────────────────────────
namespace PBColour {
    // All values are BGR565 — R and B are pre-swapped to compensate for MADCTL_BGR.
    static constexpr uint16_t BG        = COLOUR_BACKGROUND;      // #101428 bg
    static constexpr uint16_t HDR_BG    = 0x30E2;  // #141C32 dark navy header
    static constexpr uint16_t HDR_TEXT  = COLOUR_TEXT;
    static constexpr uint16_t ROW_BG    = COLOUR_HEADER_BG;       // dark panel rows
    static constexpr uint16_t ROW_ALT   = 0x30E2;  // #161C30 alt row slightly lighter
    static constexpr uint16_t SEL_BG    = 0xCCA0;  // #0096C8 teal-cyan selection
    static constexpr uint16_t SEL_TEXT  = 0xFFFF;  // white text on teal
    static constexpr uint16_t ROW_TEXT  = 0xD617;  // #BEC3D2 light grey preset name
    static constexpr uint16_t IDX_TEXT  = COLOUR_TEXT_DIM;        // index number
    static constexpr uint16_t FTR_BG    = 0x30E2;  // #141C32 dark navy footer
    static constexpr uint16_t BTN_BG    = 0x6206;  // #324164 dark blue-grey button
    static constexpr uint16_t BTN_TEXT  = COLOUR_TEXT;
    static constexpr uint16_t CANCEL_BG = COLOUR_ACCENT;          // red cancel
    static constexpr uint16_t BORDER    = 0x51A5;  // #2D3750 steel separator
}

// =============================================================================
class PresetBrowser {
public:
    // Callback: called when user confirms a selection
    using LoadCallback = void(*)(int globalIndex);

    PresetBrowser() = default;

    // -------------------------------------------------------------------------
    // open() — show the browser.
    // startIdx  — currently loaded patch index (pre-selects cursor)
    // loadCb    — called on confirm; if nullptr, presets_loadByGlobalIndex used
    // -------------------------------------------------------------------------
    void open(SynthEngine* synth, int startIdx = 0, LoadCallback loadCb = nullptr);
    void close();

    bool isOpen()   const;
    int  selected() const;

    // Call every frame while isOpen() — only repaints changed rows
    void draw(ILI9341_t3n& tft);

    // delta = +1 (down) or -1 (up); wraps around
    void onEncoder(int delta);

    // Confirm selected preset and close
    void onEncoderPress();

    // Returns true if the browser consumed the touch
    bool onTouch(int tx, int ty);

private:
    // ---- Draw helpers ----
    void _drawHeader(ILI9341_t3n& tft);
    void _drawFooter(ILI9341_t3n& tft);
    void _drawRow(ILI9341_t3n& tft, int row);          // row = slot 0..VISIBLE_ROWS-1
    void _drawRowForIdx(ILI9341_t3n& tft, int idx);    // by global preset index

    // ---- Helpers ----
    int  _clampScrollTop(int st) const;
    void _loadPreset(int idx);

    // ---- Members ----
    SynthEngine*  _synth       = nullptr;
    LoadCallback  _loadCb      = nullptr;
    bool          _open        = false;
    bool          _dirty       = false;     // true → full redraw needed
    int           _totalCount  = 0;
    int           _cursorIdx   = 0;
    int           _scrollTop   = 0;
    int           _prevCursor  = -1;        // for partial redraw
    int           _prevScroll  = -1;
};
