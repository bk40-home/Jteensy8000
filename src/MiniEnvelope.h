/* MiniEnvelope — visual ADSR envelope widget for the JT-8000 TFT
 *
 * Copyright (c) 2025, Kris Bishop
 *
 * Draws a polyline ADSR shape with selectable control points.
 * Encoder navigates between points; value adjustment via CC dispatch.
 *
 * Three flavours:
 *   AMP    — unipolar (peak locked at top, 7 points)
 *   FILTER — bipolar  (depth controls peak height, 8 points)
 *   PITCH  — bipolar  (depth controls peak height, 8 points)
 *
 * No numeric labels on TFT — just the visual shape and dots.
 */

#pragma once
#include <Arduino.h>
#include "ILI9341_t3n.h"
#include "CCDefs.h"

// ---------------------------------------------------------------------------
//  Envelope flavour — determines CC mapping and bipolar behaviour
// ---------------------------------------------------------------------------
enum class EnvFlavour : uint8_t {
    AMP    = 0,   // unipolar, no depth point
    FILTER = 1,   // bipolar, depth = FILTER_ENV_AMOUNT
    PITCH  = 2    // bipolar, depth = PITCH_ENV_DEPTH
};

// ---------------------------------------------------------------------------
//  Layout constants
// ---------------------------------------------------------------------------
namespace MiniEnvLayout {
    static constexpr int16_t ENV_CELL_H       = 70;   // total widget height
    static constexpr int16_t ENV_DRAW_PAD_X   = 4;    // left/right padding within widget
    static constexpr int16_t ENV_DRAW_PAD_Y   = 6;    // top/bottom padding within widget
    static constexpr int16_t ENV_DOT_R_SEL    = 4;    // selected dot radius
    static constexpr int16_t ENV_DOT_R_UNSEL  = 2;    // unselected dot radius
    static constexpr int16_t ENV_CURVE_SEGS   = 8;    // polyline segments per curved stage
    static constexpr int16_t ENV_MIN_STAGE_W  = 20;   // minimum px width for any non-zero stage
    static constexpr int16_t ENV_MIN_ZERO_W   = 4;    // minimum px width for a zero-time stage
    static constexpr int16_t ENV_MAX_STAGE_W  = 140;  // maximum px width for any stage

    // pitch envelope depth: ± this many semitones at CC extremes
    static constexpr int     PITCH_ENV_MAX_ST = 24;
}

// ---------------------------------------------------------------------------
//  Point indices — order matches encoder navigation (left to right)
// ---------------------------------------------------------------------------
namespace EnvPoint {
    static constexpr int8_t ATK_TIME   = 0;
    static constexpr int8_t ATK_CURVE  = 1;
    static constexpr int8_t DEPTH      = 2;   // filter/pitch only; amp skips this
    static constexpr int8_t DEC_TIME   = 3;
    static constexpr int8_t DEC_CURVE  = 4;
    static constexpr int8_t SUS_LEVEL  = 5;
    static constexpr int8_t REL_TIME   = 6;
    static constexpr int8_t REL_CURVE  = 7;

    // amp uses indices 0,1, 3,4,5,6,7 (skips 2)
    static constexpr int8_t AMP_COUNT    = 7;
    static constexpr int8_t BIPOLAR_COUNT = 8;

    // map amp sequential index (0-6) to the global point index
    inline int8_t ampToGlobal(int8_t ampIdx) {
        // 0→0, 1→1, 2→3, 3→4, 4→5, 5→6, 6→7
        return (ampIdx < 2) ? ampIdx : ampIdx + 1;
    }

    // map global point index to amp sequential index (-1 if not applicable)
    inline int8_t globalToAmp(int8_t globalIdx) {
        if (globalIdx == DEPTH) return -1;
        return (globalIdx < 2) ? globalIdx : globalIdx - 1;
    }
}

// ---------------------------------------------------------------------------
//  MiniEnvelope — static draw class (no instance state, like MiniKnob)
// ---------------------------------------------------------------------------
class MiniEnvelope {
public:
    // ---- primary draw function ----
    //  x, y:        top-left of the widget area
    //  w:           widget width (typically SW - 2*BODY_PAD_X)
    //  flavour:     AMP / FILTER / PITCH
    //  getCC:       callback to read a CC value (typically HomeScreen::_getCC)
    //  selectedPt:  which point is selected (-1 = none), using EnvPoint indices
    static void draw(ILI9341_t3n& tft, int16_t x, int16_t y, int16_t w,
                     EnvFlavour flavour,
                     uint8_t (*getCC)(uint8_t cc),
                     int8_t selectedPt);

    // ---- get the CC number for a given point index and flavour ----
    static uint8_t pointToCC(EnvFlavour flavour, int8_t pointIdx);

    // ---- number of selectable points for a flavour ----
    static int8_t pointCount(EnvFlavour flavour) {
        return (flavour == EnvFlavour::AMP)
            ? EnvPoint::AMP_COUNT
            : EnvPoint::BIPOLAR_COUNT;
    }

    // ---- convert sequential cursor (0..N-1) to global point index ----
    static int8_t cursorToPoint(EnvFlavour flavour, int8_t cursor) {
        if (flavour == EnvFlavour::AMP) return EnvPoint::ampToGlobal(cursor);
        return cursor;
    }

    // ---- convert global point index to sequential cursor ----
    static int8_t pointToCursor(EnvFlavour flavour, int8_t pointIdx) {
        if (flavour == EnvFlavour::AMP) return EnvPoint::globalToAmp(pointIdx);
        return pointIdx;
    }

private:
    // ---- internal: compute x positions for each stage boundary ----
    struct StageLayout {
        int16_t xAtk;      // x at end of attack (= peak)
        int16_t xDec;      // x at end of decay (= start of sustain)
        int16_t xSus;      // x at end of sustain (= start of release)
        int16_t xEnd;      // x at end of release (right edge)
        int16_t yBase;     // y baseline (bottom for amp, centre for bipolar)
        int16_t yPeak;     // y at peak (top for amp, scaled by depth for bipolar)
        int16_t ySustain;  // y at sustain level
        int16_t drawW;     // usable drawing width
        int16_t drawH;     // usable drawing height
        int16_t x0;        // left edge of drawing area
        int16_t y0;        // top edge of drawing area
    };

    static StageLayout computeLayout(int16_t x, int16_t y, int16_t w,
                                     EnvFlavour flavour,
                                     uint8_t (*getCC)(uint8_t cc));

    // ---- internal: draw one curved segment as a polyline ----
    static void drawCurvedSegment(ILI9341_t3n& tft,
                                  int16_t x0, int16_t y0,
                                  int16_t x1, int16_t y1,
                                  float curve, uint16_t colour);

    // ---- internal: compute curve dot position (midpoint with offset) ----
    static void curveDotPosition(int16_t x0, int16_t y0,
                                 int16_t x1, int16_t y1,
                                 float curve,
                                 int16_t& dotX, int16_t& dotY);
};
