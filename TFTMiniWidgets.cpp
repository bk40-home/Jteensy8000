// TFTMiniWidgets.cpp
// =============================================================================
// Stateless draw functions for the JT-8000 accordion UI mini widgets.
// See TFTMiniWidgets.h for layout diagrams and design notes.
//
// PERFORMANCE NOTES:
//   - Each draw clears only its own cell rect (no fillScreen)
//   - Arc drawing uses fillCircle dots at 6° steps (45 dots for full 270°)
//     → ~45 SPI transactions for a full knob redraw
//   - At 30 MHz SPI each fillCircle(r=2) is ~0.02 ms → full knob < 1 ms
//   - Text uses built-in 6×8 font (textSize=1), no external fonts loaded
// =============================================================================

#include "TFTMiniWidgets.h"
#include <math.h>

// ---------------------------------------------------------------------------
// Shared constants
// ---------------------------------------------------------------------------
static constexpr float PI_F       = 3.14159265f;
static constexpr float D_TO_RAD = PI_F / 180.0f;

// Shorthand for the theme colours — avoids long macro names in draw code
static constexpr uint16_t COL_BG       = COLOUR_SURFACE;
static constexpr uint16_t COL_BG2      = COLOUR_SURFACE2;
static constexpr uint16_t COL_ACCENT   = COLOUR_ACCENT_ORANGE;
static constexpr uint16_t COL_ACCENT_H = COLOUR_ACCENT_HI;
static constexpr uint16_t COL_ACCENT_D = COLOUR_ACCENT_DIM;
static constexpr uint16_t COL_TEXT     = COLOUR_TEXT;
static constexpr uint16_t COL_TEXT_DIM = COLOUR_TEXT_DIM;
static constexpr uint16_t COL_TEXT_MUT = COLOUR_TEXT_MUTED;
static constexpr uint16_t COL_BORDER   = COLOUR_BORDER;
static constexpr uint16_t COL_HEADER   = COLOUR_HEADER_BG;

using namespace MiniLayout;


// =============================================================================
// MiniKnob
// =============================================================================

// Internal: draw a thick arc from startAngle over sweepAngle (degrees).
// Uses SCREEN coordinates where Y increases downward.
// Angle convention: 0° = 7 o'clock (start of 270° sweep). Positive = clockwise.
// In screen-space math: 7 o'clock = +135° from 12-o'clock = +135° from -Y axis.
// We convert to standard trig using: px = cx + r*sin(angle), py = cy + r*cos(angle)
// where angle is measured clockwise from 12-o'clock (top-dead-centre).
static void drawArcSegment(ILI9341_t3n& d, int16_t cx, int16_t cy,
                           float startDeg, float sweepDeg,
                           int16_t radius, int16_t thickness,
                           uint16_t colour)
{
    // Screen CW convention: 0° = 12 o'clock, 90° = 3 o'clock, etc.
    // 7 o'clock = 225° CW from 12. Our knob arc starts there.
    // startDeg/sweepDeg are relative: 0° start = 7 o'clock, 270° = full travel to 5 o'clock.
    const float baseAngle = 225.0f;  // 7 o'clock in screen-CW degrees from 12
    const float step      = 6.0f;
    const float r         = (float)(radius - thickness);

    for (float a = startDeg; a <= sweepDeg + 0.1f; a += step) {
        const float screenDeg = baseAngle + a;   // CW from 12 o'clock
        const float rad = screenDeg * D_TO_RAD;
        // sin/cos for screen CW from top: x = sin(angle), y = cos(angle) [note: +cos = down]
        const int16_t px = cx + (int16_t)(r * sinf(rad));
        const int16_t py = cy - (int16_t)(r * cosf(rad));
        d.fillCircle(px, py, thickness, colour);
    }
}

// Internal: draw the pointer dot at the arc-end position
static void drawPointerDot(ILI9341_t3n& d, int16_t cx, int16_t cy,
                           uint8_t value, int16_t radius, int16_t arcW,
                           uint16_t colour)
{
    // Same screen-CW convention as drawArcSegment:
    // 225° base + proportional sweep through 270°
    const float screenDeg = 225.0f + (value / 127.0f) * 270.0f;
    const float rad = screenDeg * D_TO_RAD;
    const float r   = (float)(radius - arcW);
    const int16_t px = cx + (int16_t)(r * sinf(rad));
    const int16_t py = cy - (int16_t)(r * cosf(rad));
    d.fillCircle(px, py, KNOB_DOT_R, colour);  // small dot — no +1
}

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

    // Arc track (full 270°, dim)
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
        const int16_t labelY  = cy + KNOB_RADIUS + 2;
        const int16_t labelW  = (int16_t)(strlen(label) * 6);
        const int16_t labelX  = cx - labelW / 2;
        d.setTextSize(1);
        d.setTextColor(selected ? COL_ACCENT : COL_TEXT_DIM, COL_BG);
        d.setCursor(labelX, labelY);
        d.print(label);
    }

    // Value text (centred below label, orange)
    if (valText && valText[0]) {
        const int16_t valY  = cy + KNOB_RADIUS + 12;
        const int16_t valW  = (int16_t)(strlen(valText) * 6);
        const int16_t valX  = cx - valW / 2;
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

    // Redraw arc area only — clear the arc ring region
    // Clear a square around the arc (slightly larger than knob diameter)
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
    const uint16_t boxBg     = COLOUR_BACKGROUND;

    d.fillRect(x + 1, boxY, SEL_CELL_W - 2, SEL_BOX_H, boxBg);
    d.drawRect(x + 1, boxY, SEL_CELL_W - 2, SEL_BOX_H, boxBorder);

    // Value text inside box (orange, left-aligned with padding)
    if (valText && valText[0]) {
        d.setTextSize(1);
        d.setTextColor(COL_ACCENT, boxBg);
        d.setCursor(x + 4, boxY + 4);

        // Truncate display if text is too wide for box
        const int16_t maxChars = (SEL_CELL_W - 18) / 6;  // leave room for arrow
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
    d.print("\x19");  // down-arrow character (or use "v")

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

    // Pill shape — drawn as a rounded rect
    const int16_t pillX = x + (TOG_CELL_W - TOG_PILL_W) / 2;
    const int16_t pillY = y + TOG_CELL_H - TOG_PILL_H - 1;

    if (isOn) {
        // Active: filled orange pill
        d.fillRect(pillX, pillY, TOG_PILL_W, TOG_PILL_H,
                         COL_ACCENT_D);
        d.drawRect(pillX, pillY, TOG_PILL_W, TOG_PILL_H,
                         COL_ACCENT);

        // "ON" text centred in pill
        d.setTextSize(1);
        d.setTextColor(COL_ACCENT_H, COL_ACCENT_D);
        d.setCursor(pillX + (TOG_PILL_W - 12) / 2, pillY + 3);
        d.print("ON");
    } else {
        // Inactive: dim border pill
        d.fillRect(pillX, pillY, TOG_PILL_W, TOG_PILL_H,
                         COLOUR_BACKGROUND);
        d.drawRect(pillX, pillY, TOG_PILL_W, TOG_PILL_H,
                         COL_BORDER);

        // "OFF" text centred in pill
        d.setTextSize(1);
        d.setTextColor(COL_TEXT_MUT, COLOUR_BACKGROUND);
        d.setCursor(pillX + (TOG_PILL_W - 18) / 2, pillY + 3);
        d.print("OFF");
    }

    // Selection highlight
    if (selected) {
        d.drawRect(x, y, TOG_CELL_W, TOG_CELL_H, COL_ACCENT);
    }
}


// =============================================================================
// MiniGrid — step sequencer tap-to-select bar grid
// =============================================================================

void MiniGrid::draw(ILI9341_t3n& d, int16_t x, int16_t y, int16_t w,
                    uint8_t stepCount, const uint8_t values[16],
                    int8_t selectedStep, int8_t playingStep)
{
    if (stepCount == 0) stepCount = 1;
    if (stepCount > 16) stepCount = 16;

    const int16_t totalH = GRID_BAR_H + GRID_NUM_H + 4;

    // Clear grid area
    d.fillRect(x, y, w, totalH, COL_BG);

    // Calculate per-step width to fill available width
    const int16_t cellW   = (w - 2) / stepCount;
    const int16_t midBarY = y + GRID_BAR_H / 2;  // centre line for bipolar display

    for (uint8_t i = 0; i < stepCount; ++i) {
        const int16_t cx = x + 1 + i * cellW;
        const int16_t barX = cx + 1;
        const int16_t barW = cellW - 2;

        // Bar background
        d.fillRect(barX, y, barW, GRID_BAR_H, COLOUR_BACKGROUND);
        d.drawRect(barX, y, barW, GRID_BAR_H, COL_BORDER);

        // Centre line (zero reference for bipolar values)
        d.drawFastHLine(barX, midBarY, barW, COL_TEXT_MUT);

        // Value bar — treat 64 as zero, 0-63 negative, 65-127 positive
        const int8_t bipolar = (int8_t)values[i] - 64;
        if (bipolar > 0) {
            // Positive: bar grows upward from centre
            const int16_t barH = (int16_t)((int32_t)bipolar * (GRID_BAR_H / 2) / 63);
            const uint16_t col = (i == selectedStep) ? COL_ACCENT_H : COL_ACCENT;
            d.fillRect(barX + 1, midBarY - barH, barW - 2, barH, col);
        } else if (bipolar < 0) {
            // Negative: bar grows downward from centre
            const int16_t barH = (int16_t)((int32_t)(-bipolar) * (GRID_BAR_H / 2) / 64);
            const uint16_t col = (i == selectedStep) ? COL_ACCENT_H : 0xE2A0;  // dim red-orange
            d.fillRect(barX + 1, midBarY, barW - 2, barH, col);
        }

        // Playing step highlight: bright border
        if (i == playingStep) {
            d.drawRect(barX, y, barW, GRID_BAR_H, COL_ACCENT_H);
        }

        // Selected step: orange border
        if (i == selectedStep) {
            d.drawRect(barX - 1, y - 1, barW + 2, GRID_BAR_H + 2, COL_ACCENT);
        }

        // Step number below bar
        char numBuf[4];
        snprintf(numBuf, sizeof(numBuf), "%d", i + 1);
        const int16_t numW = (int16_t)(strlen(numBuf) * 6);
        const int16_t numX = cx + (cellW - numW) / 2;
        const int16_t numY = y + GRID_BAR_H + 2;
        d.setTextSize(1);
        const uint16_t numCol = (i == playingStep) ? COL_ACCENT_H :
                                (i == selectedStep) ? COL_ACCENT : COL_TEXT_MUT;
        d.setTextColor(numCol, COL_BG);
        d.setCursor(numX, numY);
        d.print(numBuf);
    }
}

int8_t MiniGrid::hitTestStep(int16_t gridX, int16_t gridY, int16_t gridW,
                             uint8_t stepCount, int16_t tx, int16_t ty)
{
    if (stepCount == 0) return -1;
    if (stepCount > 16) stepCount = 16;

    const int16_t totalH = GRID_BAR_H + GRID_NUM_H + 4;

    // Check vertical bounds
    if (ty < gridY || ty >= gridY + totalH) return -1;
    if (tx < gridX || tx >= gridX + gridW) return -1;

    // Determine which step column was tapped
    const int16_t cellW = (gridW - 2) / stepCount;
    if (cellW <= 0) return -1;

    const int16_t relX = tx - gridX - 1;
    const int8_t step = (int8_t)(relX / cellW);
    return (step >= 0 && step < (int8_t)stepCount) ? step : -1;
}


// =============================================================================
// SectionHeader — collapsible section bar
// =============================================================================

void SectionHeader::draw(ILI9341_t3n& d, int16_t x, int16_t y, int16_t w,
                         const char* label, bool expanded, bool highlighted)
{
    const uint16_t bgCol = highlighted ? COL_BG2 : COL_HEADER;

    // Background with subtle gradient effect (darker left, lighter right)
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
        d.setTextColor(highlighted ? COL_ACCENT : COLOUR_TEXT_HI, bgCol);
        d.setCursor(ledX + SEC_LED_R + 6, y + (SEC_HDR_H - 8) / 2);
        d.print(label);
    }

    // Chevron (right side) — rotated when collapsed
    d.setTextSize(1);
    d.setTextColor(COL_TEXT_MUT, bgCol);
    d.setCursor(x + w - SEC_PAD_X - 8, y + (SEC_HDR_H - 8) / 2);
    d.print(expanded ? "\x19" : "\x1A");  // down-arrow or right-arrow
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
    d.drawFastHLine(x, y + GRP_HDR_H - 1, w, COLOUR_BACKGROUND);
}