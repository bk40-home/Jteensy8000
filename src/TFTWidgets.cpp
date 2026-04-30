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
// TFTWidgets.cpp
// =============================================================================
// Implementations for all JT-8000 TFT widgets.
// See TFTWidgets.h for class/namespace documentation.
//
// PERFORMANCE NOTES:
//   - Each draw clears only its own cell rect (no fillScreen)
//   - Arc drawing uses fillCircle dots at 6-degree steps (45 dots for 270 deg)
//     -> ~45 SPI transactions for a full knob redraw
//   - At 30 MHz SPI each fillCircle(r=2) is ~0.02 ms -> full knob < 1 ms
//   - Text uses built-in 6x8 font (textSize=1), no external fonts loaded
// =============================================================================

#include "TFTWidgets.h"
#include <math.h>


// =============================================================================
// Colour aliases — single source from JT8000Colours.h
//
// Every widget in this file reads colour through these constexprs.
// To retheme: change values here (or in JT8000Colours.h). Nothing else to touch.
// =============================================================================
static constexpr float PI_F     = 3.14159265f;
static constexpr float D_TO_RAD = PI_F / 180.0f;

// Background tones (darkest to lightest)
static constexpr uint16_t COL_BACKGROUND = COLOUR_BACKGROUND;    // #101428  deep charcoal-navy
static constexpr uint16_t COL_BG         = COLOUR_SURFACE;       // #111620  section body fill
static constexpr uint16_t COL_BG2        = COLOUR_SURFACE2;      // #161C28  group / card bg
static constexpr uint16_t COL_SURFACE3   = COLOUR_SURFACE3;      // #1C2436  hover / pressed
static constexpr uint16_t COL_HEADER     = COLOUR_HEADER_BG;     // #19233C  dark navy panel

// Text
static constexpr uint16_t COL_TEXT       = COLOUR_TEXT;           // #D2D7E1  warm off-white
static constexpr uint16_t COL_TEXT_HI    = COLOUR_TEXT_HI;        // #C8D8F4  bright white
static constexpr uint16_t COL_TEXT_DIM   = COLOUR_TEXT_DIM;       // #787D8C  steel grey
static constexpr uint16_t COL_TEXT_MUT   = COLOUR_TEXT_MUTED;     // #3E5070  very dim

// Accent
static constexpr uint16_t COL_ACCENT     = COLOUR_ACCENT_ORANGE; // #FFA000  amber orange
static constexpr uint16_t COL_ACCENT_H   = COLOUR_ACCENT_HI;     // #FFA040  bright orange
static constexpr uint16_t COL_ACCENT_D   = COLOUR_ACCENT_DIM;    // #6A3508  dim orange

// Functional (not decorative — keep distinct)
static constexpr uint16_t COL_RED        = COLOUR_ACCENT_RED;    // #FF1C18  cancel / alert
static constexpr uint16_t COL_CONFIRM    = 0x1C06;               // #1A8030  confirm green (RGB565)
static constexpr uint16_t COL_BORDER     = COLOUR_BORDER;        // #2D3750  blue-grey borders

using namespace MiniLayout;


// =============================================================================
// Shared arc drawing — used by MiniKnob
// =============================================================================

// Draw a thick arc from startDeg over sweepDeg (degrees, knob-relative).
// 0 deg = 7 o'clock (start of 270 deg sweep). Positive = clockwise.
// Screen CW convention: 0 deg = 12 o'clock, 90 deg = 3 o'clock.
static void drawArcSegment(ILI9341_t3n& d, int16_t cx, int16_t cy,
                           float startDeg, float sweepDeg,
                           int16_t radius, int16_t thickness,
                           uint16_t colour)
{
    const float baseAngle = 225.0f;  // 7 o'clock in screen-CW degrees from 12
    const float step      = 6.0f;
    const float r         = (float)(radius - thickness);

    for (float a = startDeg; a <= sweepDeg + 0.1f; a += step) {
        const float screenDeg = baseAngle + a;
        const float rad = screenDeg * D_TO_RAD;
        const int16_t px = cx + (int16_t)(r * sinf(rad));
        const int16_t py = cy - (int16_t)(r * cosf(rad));
        d.fillCircle(px, py, thickness, colour);
    }
}

// Draw the pointer dot at the current value position on the arc.
static void drawPointerDot(ILI9341_t3n& d, int16_t cx, int16_t cy,
                           uint8_t value, int16_t radius, int16_t arcW,
                           uint16_t colour)
{
    const float screenDeg = 225.0f + (value / 127.0f) * 270.0f;
    const float rad = screenDeg * D_TO_RAD;
    const float r   = (float)(radius - arcW);
    const int16_t px = cx + (int16_t)(r * sinf(rad));
    const int16_t py = cy - (int16_t)(r * cosf(rad));
    d.fillCircle(px, py, KNOB_DOT_R, colour);
}


// =============================================================================
// MiniKnob
// =============================================================================

void MiniKnob::draw(ILI9341_t3n& d, int16_t x, int16_t y,
                    uint8_t value, const char* label, const char* valText,
                    bool selected)
{
    // Clear cell
    d.fillRect(x, y, KNOB_CELL_W, KNOB_CELL_H, COL_BG);

    // Knob centre — horizontally centred, pushed to top of cell
    const int16_t cx = x + KNOB_CELL_W / 2;
    const int16_t cy = y + 4 + KNOB_RADIUS;

    // Knob body circle (dark fill + border)
    d.fillCircle(cx, cy, KNOB_RADIUS, COL_HEADER);
    d.drawCircle(cx, cy, KNOB_RADIUS, selected ? COL_ACCENT : COL_BORDER);

    // Arc track (full 270 deg, dim)
    drawArcSegment(d, cx, cy, 0.0f, 270.0f, KNOB_RADIUS, KNOB_ARC_W, COL_ACCENT_D);

    // Arc fill (proportional to value)
    if (value > 0) {
        const float fillSweep = (value / 127.0f) * 270.0f;
        const uint16_t arcCol = selected ? COL_ACCENT_H : COL_ACCENT;
        drawArcSegment(d, cx, cy, 0.0f, fillSweep, KNOB_RADIUS, KNOB_ARC_W, arcCol);
    }

    // Pointer dot at arc end
    drawPointerDot(d, cx, cy, value, KNOB_RADIUS, KNOB_ARC_W,
                   selected ? COL_ACCENT_H : COL_ACCENT);

    // Label (centred below knob, dim uppercase)
    if (label && label[0]) {
        const int16_t labelY = cy + KNOB_RADIUS + 2;
        const int16_t labelW = (int16_t)(strlen(label) * 6);
        const int16_t labelX = cx - labelW / 2;
        d.setTextSize(1);
        d.setTextColor(selected ? COL_ACCENT : COL_TEXT_DIM, COL_BG);
        d.setCursor(labelX, labelY);
        d.print(label);
    }

    // Value text (centred below label, orange)
    if (valText && valText[0]) {
        const int16_t valY = cy + KNOB_RADIUS + 12;
        const int16_t valW = (int16_t)(strlen(valText) * 6);
        const int16_t valX = cx - valW / 2;
        d.setTextSize(1);
        d.setTextColor(COL_ACCENT, COL_BG);
        d.setCursor(valX, valY);
        d.print(valText);
    }

    // Selection highlight border
    if (selected) {
        d.drawRect(x, y, KNOB_CELL_W, KNOB_CELL_H, COL_ACCENT);
    }
}

void MiniKnob::drawArcOnly(ILI9341_t3n& d, int16_t x, int16_t y,
                           uint8_t value, bool selected)
{
    const int16_t cx = x + KNOB_CELL_W / 2;
    const int16_t cy = y + 4 + KNOB_RADIUS;

    // Clear the arc ring region
    const int16_t clearR = KNOB_RADIUS + 2;
    d.fillRect(cx - clearR, cy - clearR, clearR * 2, clearR * 2, COL_HEADER);
    d.drawCircle(cx, cy, KNOB_RADIUS, selected ? COL_ACCENT : COL_BORDER);

    // Full dim track
    drawArcSegment(d, cx, cy, 0.0f, 270.0f, KNOB_RADIUS, KNOB_ARC_W, COL_ACCENT_D);

    // Fill
    if (value > 0) {
        const float fillSweep = (value / 127.0f) * 270.0f;
        drawArcSegment(d, cx, cy, 0.0f, fillSweep, KNOB_RADIUS, KNOB_ARC_W,
                       selected ? COL_ACCENT_H : COL_ACCENT);
    }

    // Pointer dot
    drawPointerDot(d, cx, cy, value, KNOB_RADIUS, KNOB_ARC_W,
                   selected ? COL_ACCENT_H : COL_ACCENT);
}


// =============================================================================
// MiniSelect
// =============================================================================

void MiniSelect::draw(ILI9341_t3n& d, int16_t x, int16_t y,
                      const char* label, const char* valText,
                      bool selected)
{
    // Clear cell
    d.fillRect(x, y, SEL_CELL_W, SEL_CELL_H, COL_BG);

    // Label (top, dim uppercase)
    if (label && label[0]) {
        d.setTextSize(1);
        d.setTextColor(selected ? COL_ACCENT : COL_TEXT_DIM, COL_BG);
        d.setCursor(x + 2, y + 1);
        d.print(label);
    }

    // Dropdown box
    const int16_t boxY = y + SEL_CELL_H - SEL_BOX_H;
    const uint16_t boxBorder = selected ? COL_ACCENT : COL_BORDER;
    const uint16_t boxBg     = COL_BACKGROUND;

    d.fillRect(x + 1, boxY, SEL_CELL_W - 2, SEL_BOX_H, boxBg);
    d.drawRect(x + 1, boxY, SEL_CELL_W - 2, SEL_BOX_H, boxBorder);

    // Value text inside box (orange, left-aligned with padding)
    if (valText && valText[0]) {
        d.setTextSize(1);
        d.setTextColor(COL_ACCENT, boxBg);
        d.setCursor(x + 4, boxY + 4);

        // Truncate if text is too wide for box
        const int16_t maxChars = (SEL_CELL_W - 18) / 6;
        char truncBuf[16];
        strncpy(truncBuf, valText, sizeof(truncBuf) - 1);
        truncBuf[sizeof(truncBuf) - 1] = '\0';
        if ((int)strlen(truncBuf) > maxChars && maxChars > 0) {
            truncBuf[maxChars] = '\0';
        }
        d.print(truncBuf);
    }

    // Dropdown arrow (right side of box, dim)
    d.setTextSize(1);
    d.setTextColor(COL_TEXT_MUT, boxBg);
    d.setCursor(x + SEL_CELL_W - 12, boxY + 4);
    d.print("\x19");  // down-arrow character

    // Selection highlight
    if (selected) {
        d.drawRect(x, y, SEL_CELL_W, SEL_CELL_H, COL_ACCENT);
    }
}


// =============================================================================
// MiniToggle
// =============================================================================

void MiniToggle::draw(ILI9341_t3n& d, int16_t x, int16_t y,
                      const char* label, bool isOn, bool selected)
{
    // Clear cell
    d.fillRect(x, y, TOG_CELL_W, TOG_CELL_H, COL_BG);

    // Label (centred, dim uppercase)
    if (label && label[0]) {
        const int16_t labelW = (int16_t)(strlen(label) * 6);
        const int16_t labelX = x + (TOG_CELL_W - labelW) / 2;
        d.setTextSize(1);
        d.setTextColor(selected ? COL_ACCENT : COL_TEXT_DIM, COL_BG);
        d.setCursor(labelX, y + 1);
        d.print(label);
    }

    // Pill shape
    const int16_t pillX = x + (TOG_CELL_W - TOG_PILL_W) / 2;
    const int16_t pillY = y + TOG_CELL_H - TOG_PILL_H - 1;

    if (isOn) {
        // Active: filled orange pill
        d.fillRect(pillX, pillY, TOG_PILL_W, TOG_PILL_H, COL_ACCENT_D);
        d.drawRect(pillX, pillY, TOG_PILL_W, TOG_PILL_H, COL_ACCENT);

        d.setTextSize(1);
        d.setTextColor(COL_ACCENT_H, COL_ACCENT_D);
        d.setCursor(pillX + (TOG_PILL_W - 12) / 2, pillY + 3);
        d.print("ON");
    } else {
        // Inactive: dim border pill
        d.fillRect(pillX, pillY, TOG_PILL_W, TOG_PILL_H, COL_BACKGROUND);
        d.drawRect(pillX, pillY, TOG_PILL_W, TOG_PILL_H, COL_BORDER);

        d.setTextSize(1);
        d.setTextColor(COL_TEXT_MUT, COL_BACKGROUND);
        d.setCursor(pillX + (TOG_PILL_W - 18) / 2, pillY + 3);
        d.print("OFF");
    }

    // Selection highlight
    if (selected) {
        d.drawRect(x, y, TOG_CELL_W, TOG_CELL_H, COL_ACCENT);
    }
}


// =============================================================================
// MiniSliderGrid — step sequencer as vertical sliders (unipolar 0-127)
// =============================================================================

void MiniSliderGrid::draw(ILI9341_t3n& d, int16_t x, int16_t y, int16_t w,
                          uint8_t stepCount, const uint8_t values[16],
                          int8_t selectedStep, int8_t playingStep)
{
    if (stepCount == 0) stepCount = 1;
    if (stepCount > 16) stepCount = 16;

    const int16_t totalH = SGRID_TOTAL_H;

    // Clear grid area
    d.fillRect(x, y, w, totalH, COL_BG);

    // Per-step column width — fill available width evenly
    const int16_t cellW = (w - 2) / stepCount;
    if (cellW < 4) return;  // too narrow to draw

    const int16_t trackH   = SGRID_SLIDER_H;
    const int16_t trackTop = y;

    for (uint8_t i = 0; i < stepCount; ++i) {
        const int16_t cx      = x + 1 + i * cellW;
        const int16_t sliderX = cx + 1;
        const int16_t sliderW = cellW - 2;

        // Slider track background
        d.fillRect(sliderX, trackTop, sliderW, trackH, COL_BACKGROUND);
        d.drawRect(sliderX, trackTop, sliderW, trackH, COL_BORDER);

        // Fill from bottom — unipolar: 0 = empty, 127 = full
        if (values[i] > 0) {
            const int16_t fillH  = (int16_t)((int32_t)values[i] * (trackH - 2) / 127);
            const int16_t fillY  = trackTop + trackH - 1 - fillH;
            const uint16_t fillCol = (i == selectedStep) ? COL_ACCENT_H : COL_ACCENT;
            d.fillRect(sliderX + 1, fillY, sliderW - 2, fillH, fillCol);

            // Thumb indicator at top of fill
            const int16_t thumbY = fillY - SGRID_THUMB_H / 2;
            if (thumbY >= trackTop) {
                d.fillRect(sliderX, thumbY, sliderW, SGRID_THUMB_H, fillCol);
            }
        }

        // Playing step: bright top/bottom border lines
        if (i == playingStep) {
            d.drawFastHLine(sliderX, trackTop, sliderW, COL_ACCENT_H);
            d.drawFastHLine(sliderX, trackTop + trackH - 1, sliderW, COL_ACCENT_H);
        }

        // Selected step: orange border around the whole slider
        if (i == selectedStep) {
            d.drawRect(sliderX - 1, trackTop - 1, sliderW + 2, trackH + 2, COL_ACCENT);
        }

        // Step number below slider
        char numBuf[4];
        snprintf(numBuf, sizeof(numBuf), "%d", i + 1);
        const int16_t numW = (int16_t)(strlen(numBuf) * 6);
        const int16_t numX = cx + (cellW - numW) / 2;
        const int16_t numY = trackTop + trackH + 2;
        d.setTextSize(1);
        const uint16_t numCol = (i == playingStep)  ? COL_ACCENT_H :
                                (i == selectedStep) ? COL_ACCENT    : COL_TEXT_MUT;
        d.setTextColor(numCol, COL_BG);
        d.setCursor(numX, numY);
        d.print(numBuf);
    }
}

int8_t MiniSliderGrid::hitTestStep(int16_t gridX, int16_t gridY, int16_t gridW,
                                   uint8_t stepCount, int16_t tx, int16_t ty)
{
    if (stepCount == 0) return -1;
    if (stepCount > 16) stepCount = 16;

    const int16_t totalH = SGRID_TOTAL_H;

    if (ty < gridY || ty >= gridY + totalH) return -1;
    if (tx < gridX || tx >= gridX + gridW)  return -1;

    const int16_t cellW = (gridW - 2) / stepCount;
    if (cellW <= 0) return -1;

    const int16_t relX = tx - gridX - 1;
    const int8_t step  = (int8_t)(relX / cellW);
    return (step >= 0 && step < (int8_t)stepCount) ? step : -1;
}


// =============================================================================
// SectionHeader — collapsible section bar
// =============================================================================

void SectionHeader::draw(ILI9341_t3n& d, int16_t x, int16_t y, int16_t w,
                         const char* label, bool expanded, bool highlighted)
{
    const uint16_t bgCol = highlighted ? COL_BG2 : COL_HEADER;

    // Background
    d.fillRect(x, y, w, SEC_HDR_H, bgCol);

    // Bottom border
    d.drawFastHLine(x, y + SEC_HDR_H - 1, w, COL_BORDER);

    // Orange LED dot (left side)
    const int16_t ledX = x + SEC_PAD_X;
    const int16_t ledY = y + SEC_HDR_H / 2;
    d.fillCircle(ledX, ledY, SEC_LED_R, COL_ACCENT);

    // Section title text
    if (label && label[0]) {
        d.setTextSize(1);
        d.setTextColor(highlighted ? COL_ACCENT : COL_TEXT_HI, bgCol);
        d.setCursor(ledX + SEC_LED_R + 6, y + (SEC_HDR_H - 8) / 2);
        d.print(label);
    }

    // Chevron (right side)
    d.setTextSize(1);
    d.setTextColor(COL_TEXT_MUT, bgCol);
    d.setCursor(x + w - SEC_PAD_X - 8, y + (SEC_HDR_H - 8) / 2);
    d.print(expanded ? "\x19" : "\x1A");
}


// =============================================================================
// GroupHeader — small group label with underline
// =============================================================================

void GroupHeader::draw(ILI9341_t3n& d, int16_t x, int16_t y, int16_t w,
                       const char* label)
{
    // Clear
    d.fillRect(x, y, w, GRP_HDR_H, COL_BG);

    // Label text (dim, uppercase, small)
    if (label && label[0]) {
        d.setTextSize(1);
        d.setTextColor(COL_TEXT_MUT, COL_BG);
        d.setCursor(x + 2, y + 1);
        d.print(label);
    }

    // Subtle underline
    d.drawFastHLine(x, y + GRP_HDR_H - 1, w, COL_BACKGROUND);
}


// =============================================================================
// TFTNumericEntry
// =============================================================================

TFTNumericEntry::TFTNumericEntry()
    : _display(nullptr), _mode(MODE_CLOSED)
    , _minVal(0), _maxVal(127), _currentVal(0)
    , _digitCount(0), _editing(false), _negative(false)
    , _selectedEnum(0), _numEnumOptions(0)
    , _scrollOffset(0)
    , _callback(nullptr)
    , _fullRedraw(false)
    , _valueDirty(false)
{
    _titleBuf[0] = '\0';
    _unitBuf[0]  = '\0';
    _digitBuf[0] = '\0';
    for (int i = 0; i < ENTRY_MAX_ENUM; ++i) _enumLabels[i] = nullptr;
}

void TFTNumericEntry::setDisplay(ILI9341_t3n* d) { _display = d; }

void TFTNumericEntry::openNumeric(const char* title, const char* unit,
                                  int minVal, int maxVal, int currentVal,
                                  Callback cb) {
    if (!_display) return;

    _mode       = MODE_NUMBER;
    _minVal     = minVal;
    _maxVal     = maxVal;
    _currentVal = currentVal;
    _callback   = cb;

    strncpy(_titleBuf, title ? title : "", ENTRY_TITLE_LEN - 1);
    _titleBuf[ENTRY_TITLE_LEN - 1] = '\0';
    strncpy(_unitBuf, unit ? unit : "", ENTRY_UNIT_LEN - 1);
    _unitBuf[ENTRY_UNIT_LEN - 1] = '\0';

    // Start in hint mode: show current value dimmed until user types
    _digitBuf[0] = '\0';
    _digitCount  = 0;
    _editing     = false;
    _negative    = (currentVal < 0);

    _fullRedraw  = true;
    _valueDirty  = false;
}

void TFTNumericEntry::openEnum(const char* title, const char* const* labels,
                               int count, int currentIdx, Callback cb) {
    if (!_display) return;

    _mode           = MODE_ENUM;
    _callback       = cb;
    _numEnumOptions = (count < ENTRY_MAX_ENUM) ? count : ENTRY_MAX_ENUM;
    _selectedEnum   = constrain(currentIdx, 0, _numEnumOptions - 1);
    _scrollOffset   = 0;

    strncpy(_titleBuf, title ? title : "", ENTRY_TITLE_LEN - 1);
    _titleBuf[ENTRY_TITLE_LEN - 1] = '\0';

    for (int i = 0; i < _numEnumOptions; ++i) _enumLabels[i] = labels[i];

    _scrollToSelection();

    _fullRedraw = true;
    _valueDirty = false;
}

void TFTNumericEntry::draw() {
    if (_mode == MODE_CLOSED || !_display) return;
    if (_fullRedraw) {
        _drawFull();
        _fullRedraw = false;
        _valueDirty = false;
    } else if (_valueDirty) {
        _drawValueBox();
        _valueDirty = false;
    }
}

bool TFTNumericEntry::onTouch(int16_t x, int16_t y) {
    if (_mode == MODE_CLOSED) return false;
    if (_mode == MODE_NUMBER) _handleNumericTouch(x, y);
    else                      _handleEnumTouch(x, y);
    return true;    // consume all touches while open
}

bool TFTNumericEntry::isOpen()  const { return _mode != MODE_CLOSED; }
TFTNumericEntry::Mode TFTNumericEntry::getMode() const { return _mode; }

// ---------------------------------------------------------------------------
// onEncoderDelta — scroll the enum list while it is open.
// delta > 0 = later items, delta < 0 = earlier items.
// ---------------------------------------------------------------------------
void TFTNumericEntry::onEncoderDelta(int delta) {
    if (_mode != MODE_ENUM || delta == 0 || _numEnumOptions == 0) return;

    const int newSel = constrain(_selectedEnum + delta, 0, _numEnumOptions - 1);
    if (newSel == _selectedEnum) return;

    _selectedEnum = newSel;
    _scrollToSelection();
    _drawEnumList();    // partial redraw — list region only
}

void TFTNumericEntry::close() { _mode = MODE_CLOSED; }


// ---- Full-screen draw -------------------------------------------------------

void TFTNumericEntry::_drawFull() {
    _display->fillScreen(COL_BACKGROUND);

    // Title bar
    _display->fillRect(0, 0, SW, TB_HEIGHT, COL_HEADER);
    _display->setTextSize(2);
    _display->setTextColor(COL_TEXT, COL_HEADER);
    _display->setCursor(6, 7);
    _display->print(_titleBuf);

    _drawCancelButton(false);

    if (_mode == MODE_NUMBER) {
        _drawValueBox();
        _drawKeypad();
    } else {
        _drawEnumList();
        _drawEnumButtons();
    }
}

void TFTNumericEntry::_drawCancelButton(bool pressed) {
    const uint16_t bg = pressed ? COL_ACCENT_H : COL_RED;
    _display->fillRect(CANCEL_X, CANCEL_Y, CANCEL_WIDTH, CANCEL_HEIGHT, bg);
    _display->setTextSize(1);
    _display->setTextColor(COL_TEXT, bg);
    const int16_t lx = CANCEL_X + (CANCEL_WIDTH - 6 * 6) / 2;
    _display->setCursor(lx, 11);
    _display->print("Cancel");
}

void TFTNumericEntry::_drawValueBox() {
    _display->fillRect(KP_X, VB_Y, KP_WIDTH, VB_HEIGHT, COL_BG);
    _display->drawRect(KP_X, VB_Y, KP_WIDTH, VB_HEIGHT, COL_BORDER);

    // Build display string: digits (or hint) + unit
    char dispBuf[ENTRY_MAX_DIGITS + ENTRY_UNIT_LEN + 2];

    if (!_editing || _digitCount == 0) {
        // Hint mode: show current value dimmed
        snprintf(dispBuf, sizeof(dispBuf), "%d %s", _currentVal, _unitBuf);
        _display->setTextColor(COL_TEXT_DIM, COL_BG);
    } else {
        // Editing: show sign prefix + digits + unit
        snprintf(dispBuf, sizeof(dispBuf), "%s%s %s",
                 (_negative ? "-" : ""), _digitBuf, _unitBuf);
        _display->setTextColor(COL_TEXT_HI, COL_BG);
    }

    _display->setTextSize(2);
    const int16_t tw = (int16_t)(strlen(dispBuf) * 12);
    const int16_t tx = KP_X + (KP_WIDTH - tw) / 2;
    const int16_t ty = VB_Y + (VB_HEIGHT - 14) / 2;
    _display->setCursor(tx, ty);
    _display->print(dispBuf);
}

void TFTNumericEntry::_drawKeypad() {
    // Rows [7,8,9], [4,5,6], [1,2,3]
    const int digits[3][3] = { {7,8,9}, {4,5,6}, {1,2,3} };
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const int16_t kx = KP_X + col * (KEY_WIDTH + KEY_GAP);
            const int16_t ky = KP_Y + row * (KEY_HEIGHT + KEY_GAP);
            _drawKey(kx, ky, KEY_WIDTH, KEY_HEIGHT, _digitStr(digits[row][col]),
                     COL_SURFACE3, false);
        }
    }

    // Bottom row: layout depends on whether negative values are possible.
    if (_minVal < 0) {
        // Highlight +/- key in orange when currently negative
        const uint16_t signBg = _negative ? COL_ACCENT : COL_SURFACE3;
        _drawKey(KP_X,                                               BR_Y, BRS_0_WIDTH,  KEY_HEIGHT, "0",   COL_SURFACE3, false);
        _drawKey(KP_X + BRS_0_WIDTH + KEY_GAP,                      BR_Y, BRS_S_WIDTH,  KEY_HEIGHT, "+/-", signBg,       false);
        _drawKey(KP_X + BRS_0_WIDTH + BRS_S_WIDTH + 2*KEY_GAP,      BR_Y, BRS_BK_WIDTH, KEY_HEIGHT, "<-",  COL_BG2,      false);
        _drawKey(KP_X + BRS_0_WIDTH + BRS_S_WIDTH + BRS_BK_WIDTH + 3*KEY_GAP,
                                                                     BR_Y, BRS_CO_WIDTH, KEY_HEIGHT, "OK",  COL_CONFIRM,  false);
    } else {
        _drawKey(KP_X,                                       BR_Y, BR0_WIDTH,  KEY_HEIGHT, "0",  COL_SURFACE3, false);
        _drawKey(KP_X + BR0_WIDTH + KEY_GAP,                 BR_Y, BRBK_WIDTH, KEY_HEIGHT, "<-", COL_BG2,      false);
        _drawKey(KP_X + BR0_WIDTH + BRBK_WIDTH + 2*KEY_GAP, BR_Y, BRCO_WIDTH, KEY_HEIGHT, "OK", COL_CONFIRM,  false);
    }
}

void TFTNumericEntry::_drawKey(int16_t kx, int16_t ky, int16_t kw, int16_t kh,
                               const char* label, uint16_t bgCol, bool pressed) {
    const uint16_t bg = pressed ? COL_ACCENT_H : bgCol;
    _display->fillRect(kx, ky, kw, kh, bg);
    _display->drawRect(kx, ky, kw, kh, COL_BORDER);
    _display->setTextSize(1);
    _display->setTextColor(COL_TEXT, bg);
    const int16_t tw = (int16_t)(strlen(label) * 6);
    _display->setCursor(kx + (kw - tw) / 2, ky + (kh - 8) / 2);
    _display->print(label);
}

/*static*/ const char* TFTNumericEntry::_digitStr(int d) {
    static char buf[2] = "0";
    buf[0] = '0' + (char)d;
    return buf;
}


// ---- Numeric touch handler --------------------------------------------------

void TFTNumericEntry::_handleNumericTouch(int16_t x, int16_t y) {
    // Cancel button
    if (x >= CANCEL_X && x < CANCEL_X + CANCEL_WIDTH &&
        y >= CANCEL_Y && y < CANCEL_Y + CANCEL_HEIGHT) {
        close();
        return;
    }

    // Digit rows [7,8,9] [4,5,6] [1,2,3]
    const int digits[3][3] = { {7,8,9}, {4,5,6}, {1,2,3} };
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const int16_t kx = KP_X + col * (KEY_WIDTH + KEY_GAP);
            const int16_t ky = KP_Y + row * (KEY_HEIGHT + KEY_GAP);
            if (x >= kx && x < kx + KEY_WIDTH && y >= ky && y < ky + KEY_HEIGHT) {
                _appendDigit(digits[row][col]);
                return;
            }
        }
    }

    // Bottom row — layout depends on whether negative entry is allowed
    if (_minVal < 0) {
        const int16_t x0Start   = KP_X;
        const int16_t xSignStart = x0Start   + BRS_0_WIDTH  + KEY_GAP;
        const int16_t xBkStart   = xSignStart + BRS_S_WIDTH  + KEY_GAP;
        const int16_t xOkStart   = xBkStart   + BRS_BK_WIDTH + KEY_GAP;

        if (y >= BR_Y && y < BR_Y + KEY_HEIGHT) {
            if (x >= x0Start && x < x0Start + BRS_0_WIDTH)     { _appendDigit(0);  return; }
            if (x >= xSignStart && x < xSignStart + BRS_S_WIDTH){ _toggleSign(); _valueDirty = true; return; }
            if (x >= xBkStart && x < xBkStart + BRS_BK_WIDTH)  { _backspace();     return; }
            if (x >= xOkStart)                                   { _confirm();       return; }
        }
    } else {
        if (y >= BR_Y && y < BR_Y + KEY_HEIGHT) {
            if (x >= KP_X && x < KP_X + BR0_WIDTH) {
                _appendDigit(0);
                return;
            }
            if (x >= KP_X + BR0_WIDTH + KEY_GAP &&
                x < KP_X + BR0_WIDTH + KEY_GAP + BRBK_WIDTH) {
                _backspace();
                return;
            }
            if (x >= KP_X + BR0_WIDTH + BRBK_WIDTH + 2*KEY_GAP) {
                _confirm();
                return;
            }
        }
    }
}

void TFTNumericEntry::_toggleSign() {
    if (_minVal >= 0) return;
    _negative   = !_negative;
    _valueDirty = true;
}

void TFTNumericEntry::_appendDigit(int d) {
    if (_digitCount >= ENTRY_MAX_DIGITS - 1) return;

    // First keypress: clear hint and start fresh
    if (!_editing) {
        _digitBuf[0] = '\0';
        _digitCount  = 0;
        _editing     = true;
    }

    // Allow lone zero, but block leading zeros on multi-digit numbers
    if (_digitCount == 0 && d == 0) {
        _digitBuf[0] = '0';
        _digitBuf[1] = '\0';
        _digitCount  = 1;
        _valueDirty  = true;
        return;
    }

    _digitBuf[_digitCount++] = '0' + (char)d;
    _digitBuf[_digitCount]   = '\0';
    _valueDirty = true;
}

void TFTNumericEntry::_backspace() {
    if (_digitCount > 0) {
        _digitBuf[--_digitCount] = '\0';
        if (_digitCount == 0) _editing = false;
        _valueDirty = true;
    }
}

void TFTNumericEntry::_confirm() {
    int val;
    if (_editing && _digitCount > 0) {
        val = atoi(_digitBuf);
        if (_negative) val = -val;
        val = constrain(val, _minVal, _maxVal);
    } else {
        val = _currentVal;  // no digits typed -> keep current value
    }
    close();
    if (_callback) _callback(val);
}


// ---- Enum list helpers ------------------------------------------------------

void TFTNumericEntry::_drawEnumList() {
    const int listY = TB_HEIGHT + 2;
    const int listH = EN_BTN_Y - listY - 2;

    _display->fillRect(0, listY, SW, listH, COL_BACKGROUND);

    for (int r = 0; r < EN_ROWS; ++r) {
        const int idx = _scrollOffset + r;
        if (idx >= _numEnumOptions) break;

        const int16_t ry  = listY + r * EN_ROW_HEIGHT;
        const bool    sel = (idx == _selectedEnum);

        _display->fillRect(0, ry, SW, EN_ROW_HEIGHT - 1,
                           sel ? COL_ACCENT : COL_BACKGROUND);

        if (_enumLabels[idx]) {
            _display->setTextSize(2);
            _display->setTextColor(sel ? COL_BACKGROUND : COL_TEXT,
                                   sel ? COL_ACCENT     : COL_BACKGROUND);
            _display->setCursor(10, ry + (EN_ROW_HEIGHT - 14) / 2);
            _display->print(_enumLabels[idx]);
        }
    }
}

void TFTNumericEntry::_drawEnumButtons() {
    // Confirm (right)
    _display->fillRect(180, EN_BTN_Y, 130, 30, COL_CONFIRM);
    _display->setTextSize(1);
    _display->setTextColor(COL_TEXT, COL_CONFIRM);
    _display->setCursor(212, EN_BTN_Y + 11);
    _display->print("Confirm");

    // Cancel (left)
    _display->fillRect(10, EN_BTN_Y, 130, 30, COL_RED);
    _display->setTextColor(COL_TEXT, COL_RED);
    _display->setCursor(42, EN_BTN_Y + 11);
    _display->print("Cancel");
}

void TFTNumericEntry::_handleEnumTouch(int16_t x, int16_t y) {
    // Confirm button (right half of footer)
    if (x >= 180 && y >= EN_BTN_Y && y < EN_BTN_Y + 30) {
        const int confirmed = _selectedEnum;
        close();
        if (_callback) _callback(confirmed);
        return;
    }
    // Cancel button (left half of footer)
    if (x < 140 && y >= EN_BTN_Y && y < EN_BTN_Y + 30) {
        close();
        return;
    }

    // List rows — tap to select
    const int listY = TB_HEIGHT + 2;
    for (int r = 0; r < EN_ROWS; ++r) {
        const int16_t ry = listY + r * EN_ROW_HEIGHT;
        if (y >= ry && y < ry + EN_ROW_HEIGHT) {
            const int idx = _scrollOffset + r;
            if (idx < _numEnumOptions && idx != _selectedEnum) {
                _selectedEnum = idx;
                _drawEnumList();   // immediate partial redraw of list only
            }
            return;
        }
    }
}

void TFTNumericEntry::_scrollToSelection() {
    if (_selectedEnum < _scrollOffset) {
        _scrollOffset = _selectedEnum;
    } else if (_selectedEnum >= _scrollOffset + EN_ROWS) {
        _scrollOffset = _selectedEnum - EN_ROWS + 1;
    }
    if (_scrollOffset < 0) _scrollOffset = 0;
}
