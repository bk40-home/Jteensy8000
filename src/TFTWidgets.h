/* Audio Library for Teensy
 * Copyright (c) 2025, Kris Bishop, bishopkris40@hotmail.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
// TFTWidgets.h
// =============================================================================
// JT-8000 TFT widget library — stateless mini widgets + numeric entry overlay.
//
// Two categories:
//
//   Stateless draw functions (MiniKnob, MiniSelect, MiniToggle, etc.)
//     The accordion HomeScreen owns layout and scroll position. It calls these
//     draw functions at calculated screen coordinates each frame. Only visible
//     controls are drawn — draw cost is proportional to what's on screen.
//
//   TFTNumericEntry — full-screen value editor overlay
//     MODE_NUMBER: 10-key numeric pad with value display box.
//     MODE_ENUM:   scrollable list picker.
//     HomeScreen owns one instance. While open it takes all touch/encoder input.
//
// Design rules (enforced everywhere):
//   - No heap allocation (no new/delete, no Arduino String)
//   - Never call fillScreen() inside a widget — clear own rect only
//   - All string buffers are fixed-size stack arrays
//   - Audio must never be blocked — draws must complete in < 1 ms
//
// Colour: everything sources from JT8000Colours.h via local constexpr aliases.
// Single source of truth — no intermediate theme struct.
// =============================================================================

#pragma once
#include <Arduino.h>
#include "ILI9341_t3n.h"
#include "JT8000Colours.h"
#include "JT8000_Sections.h"


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
    static constexpr int16_t KNOB_RADIUS    = 12;    // arc radius (24 px diameter)
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
    static constexpr int16_t SGRID_SLIDER_W = 14;    // per-step slider column width
    static constexpr int16_t SGRID_SLIDER_H = 60;    // slider track height
    static constexpr int16_t SGRID_THUMB_H  = 4;     // thumb height
    static constexpr int16_t SGRID_NUM_H    = 8;     // step number text height
    static constexpr int16_t SGRID_TOTAL_H  = SGRID_SLIDER_H + SGRID_NUM_H + 4;

    // Legacy alias for HomeScreen layout calculations
    static constexpr int16_t GRID_BAR_H     = SGRID_SLIDER_H;
    static constexpr int16_t GRID_NUM_H     = SGRID_NUM_H;

    // ---- Spacing ----
    static constexpr int16_t CTRL_GAP_X     = 2;     // horizontal gap between controls
    static constexpr int16_t GROUP_GAP_X    = 6;     // horizontal gap between groups
    static constexpr int16_t BODY_PAD_X     = 6;     // left/right padding inside section body
    static constexpr int16_t BODY_PAD_Y     = 4;     // top/bottom padding inside section body

}  // namespace MiniLayout


// =============================================================================
// MiniKnob — compact 270-degree arc knob with label and value
//
//   ┌────────┐
//   │  ╭──╮  │  ← 24 px diameter arc, orange fill proportional to value
//   │  ╰──╯  │
//   │ LABEL  │  ← 7 px uppercase text
//   │  val   │  ← 8 px orange value text
//   └────────┘
// =============================================================================
namespace MiniKnob {

    void draw(ILI9341_t3n& d, int16_t x, int16_t y,
              uint8_t value, const char* label, const char* valText,
              bool selected);

    void drawArcOnly(ILI9341_t3n& d, int16_t x, int16_t y,
                     uint8_t value, bool selected);

    inline bool hitTest(int16_t cellX, int16_t cellY, int16_t tx, int16_t ty) {
        return (tx >= cellX && tx < cellX + MiniLayout::KNOB_CELL_W &&
                ty >= cellY && ty < cellY + MiniLayout::KNOB_CELL_H);
    }
}


// =============================================================================
// MiniSelect — compact dropdown box with label
//
//   ┌──────────────┐
//   │ LABEL        │
//   │ ┌──────────┐ │
//   │ │ value  ▼ │ │
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
// MiniToggle — on/off pill switch with label
//
//   ┌──────────┐
//   │  LABEL   │
//   │  [  ON ] │
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
//   ┌─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┐
//   │ │ │█│ │ │█│ │ │ │█│ │ │ │ │ │ │  ← fill from bottom
//   │ │█│█│ │█│█│█│ │ │█│ │ │ │ │ │ │
//   │█│█│█│█│█│█│█│█│ │█│ │ │ │ │ │ │
//   ├─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┤
//   │1│2│3│4│5│6│7│8│9│…│ │ │ │ │ │ │
//   └─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┘
// =============================================================================
namespace MiniSliderGrid {

    void draw(ILI9341_t3n& d, int16_t x, int16_t y, int16_t w,
              uint8_t stepCount, const uint8_t values[16],
              int8_t selectedStep, int8_t playingStep);

    int8_t hitTestStep(int16_t gridX, int16_t gridY, int16_t gridW,
                       uint8_t stepCount, int16_t tx, int16_t ty);
}


// =============================================================================
// SectionHeader — collapsible section header bar
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
// GroupHeader — small group label with underline
//
//   WAVE & TUNING
//   ─────────────
// =============================================================================
namespace GroupHeader {

    void draw(ILI9341_t3n& d, int16_t x, int16_t y, int16_t w,
              const char* label);
}


// =============================================================================
// Capacity constants for TFTNumericEntry
// =============================================================================
static constexpr int ENTRY_MAX_DIGITS   = 7;    // typed digits in numeric entry
static constexpr int ENTRY_MAX_ENUM     = 64;   // options in list picker
static constexpr int ENTRY_TITLE_LEN    = 24;   // title bar string length
static constexpr int ENTRY_UNIT_LEN     = 8;    // unit string length (Hz, ms …)


// =============================================================================
// TFTNumericEntry — full-screen value editor overlay
//
// Two modes:
//   MODE_NUMBER — 10-key numeric pad with value display box
//   MODE_ENUM   — scrollable list picker
//
// isOpen() returns false once the user confirms or cancels.
// The callback fires ONLY on Confirm (not Cancel).
// =============================================================================
class TFTNumericEntry {
public:
    using Callback = void (*)(int value);

    enum Mode : uint8_t { MODE_CLOSED = 0, MODE_NUMBER, MODE_ENUM };

    TFTNumericEntry();

    void setDisplay(ILI9341_t3n* d);

    // Open numeric keypad
    // title, unit — must outlive the call (stored by pointer, not copied)
    void openNumeric(const char* title, const char* unit,
                     int minVal, int maxVal, int currentVal, Callback cb);

    // Open list picker
    // labels[] array must outlive the call
    void openEnum(const char* title, const char* const* labels, int count,
                  int currentIdx, Callback cb);

    // Call every frame while isOpen()
    void draw();

    // Route all touches here while isOpen() — consumes every touch
    bool onTouch(int16_t x, int16_t y);

    // Scroll the enum list by delta steps (positive = scroll down).
    // No-op when mode is not MODE_ENUM.
    void onEncoderDelta(int delta);

    bool isOpen()  const;
    Mode getMode() const;
    void close();           // close without firing callback (Cancel)

private:
    // ---- Layout (320 x 240 screen) ----
    static constexpr int SW            = 320;
    static constexpr int SH            = 240;
    static constexpr int TB_HEIGHT     = 30;                        // title bar
    static constexpr int VB_Y          = TB_HEIGHT + 4;             // value box top
    static constexpr int VB_HEIGHT     = 36;                        // value box height
    static constexpr int KP_Y          = VB_Y + VB_HEIGHT + 8;     // keypad top
    static constexpr int KP_X          = 10;
    static constexpr int KP_WIDTH      = 300;
    static constexpr int KEY_WIDTH     = 94;
    static constexpr int KEY_HEIGHT    = 36;
    static constexpr int KEY_GAP       = 4;
    static constexpr int BR_Y          = KP_Y + 3 * (KEY_HEIGHT + KEY_GAP);

    // Bottom row without sign key (minVal >= 0): [0:90] [<-:90] [OK:106]
    static constexpr int BR0_WIDTH     = 90;
    static constexpr int BRBK_WIDTH    = 90;
    static constexpr int BRCO_WIDTH    = 106;

    // Bottom row with sign key (minVal < 0): [0:60] [±:60] [<-:72] [OK:96]
    static constexpr int BRS_0_WIDTH   = 60;
    static constexpr int BRS_S_WIDTH   = 60;
    static constexpr int BRS_BK_WIDTH  = 72;
    static constexpr int BRS_CO_WIDTH  = 96;

    static constexpr int CANCEL_X      = 240;
    static constexpr int CANCEL_Y      = 4;
    static constexpr int CANCEL_WIDTH  = 75;
    static constexpr int CANCEL_HEIGHT = 22;
    static constexpr int EN_ROW_HEIGHT = 32;
    static constexpr int EN_ROWS       = (SH - TB_HEIGHT - 40) / EN_ROW_HEIGHT;
    static constexpr int EN_BTN_Y      = SH - 36;

    // ---- Draw helpers ----
    void _drawFull();
    void _drawCancelButton(bool pressed);
    void _drawValueBox();
    void _drawKeypad();
    void _drawKey(int16_t kx, int16_t ky, int16_t kw, int16_t kh,
                  const char* label, uint16_t bgCol, bool pressed);
    static const char* _digitStr(int d);

    // ---- Touch handlers ----
    void _handleNumericTouch(int16_t x, int16_t y);
    void _handleEnumTouch(int16_t x, int16_t y);

    // ---- Numeric actions ----
    void _appendDigit(int d);
    void _backspace();
    void _toggleSign();
    void _confirm();

    // ---- Enum helpers ----
    void _drawEnumList();
    void _drawEnumButtons();
    void _scrollToSelection();

    // ---- Members ----
    ILI9341_t3n* _display;
    Mode         _mode;

    int  _minVal, _maxVal, _currentVal;
    char _digitBuf[ENTRY_MAX_DIGITS];
    int  _digitCount;
    bool _editing;          // false = showing hint; true = user has typed
    bool _negative;         // true when the entered value should be negated

    int         _selectedEnum;
    int         _numEnumOptions;
    const char* _enumLabels[ENTRY_MAX_ENUM];
    int         _scrollOffset;

    char     _titleBuf[ENTRY_TITLE_LEN];
    char     _unitBuf[ENTRY_UNIT_LEN];
    Callback _callback;

    bool _fullRedraw;
    bool _valueDirty;
};
