/* MiniEnvelope — visual ADSR envelope widget for the JT-8000 TFT
 *
 * Copyright (c) 2025, Kris Bishop
 * See MiniEnvelope.h for description.
 */

#include "MiniEnvelope.h"
#include "Mapping.h"
#include "JT8000Colours.h"

using namespace MiniEnvLayout;
using namespace JT8000Map;

// ===========================================================================
//  CC mapping tables — one per flavour, indexed by EnvPoint constants
// ===========================================================================

static const uint8_t kAmpCCs[] = {
    CC::AMP_ATTACK,          // ATK_TIME   (0)
    CC::AMP_ATTACK_CURVE,    // ATK_CURVE  (1)
    255,                     // DEPTH      (2) — not used for amp
    CC::AMP_DECAY,           // DEC_TIME   (3)
    CC::AMP_DECAY_CURVE,     // DEC_CURVE  (4)
    CC::AMP_SUSTAIN,         // SUS_LEVEL  (5)
    CC::AMP_RELEASE,         // REL_TIME   (6)
    CC::AMP_RELEASE_CURVE    // REL_CURVE  (7)
};

static const uint8_t kFilterCCs[] = {
    CC::FILTER_ENV_ATTACK,       // ATK_TIME
    CC::FILTER_ATTACK_CURVE,     // ATK_CURVE
    CC::FILTER_ENV_AMOUNT,       // DEPTH
    CC::FILTER_ENV_DECAY,        // DEC_TIME
    CC::FILTER_DECAY_CURVE,      // DEC_CURVE
    CC::FILTER_ENV_SUSTAIN,      // SUS_LEVEL
    CC::FILTER_ENV_RELEASE,      // REL_TIME
    CC::FILTER_RELEASE_CURVE     // REL_CURVE
};

static const uint8_t kPitchCCs[] = {
    CC::PITCH_ENV_ATTACK,        // ATK_TIME
    CC::PITCH_ATTACK_CURVE,      // ATK_CURVE
    CC::PITCH_ENV_DEPTH,         // DEPTH
    CC::PITCH_ENV_DECAY,         // DEC_TIME
    CC::PITCH_DECAY_CURVE,       // DEC_CURVE
    CC::PITCH_ENV_SUSTAIN,       // SUS_LEVEL
    CC::PITCH_ENV_RELEASE,       // REL_TIME
    CC::PITCH_RELEASE_CURVE      // REL_CURVE
};


// ===========================================================================
//  pointToCC — look up the CC number for a point index and flavour
// ===========================================================================
uint8_t MiniEnvelope::pointToCC(EnvFlavour flavour, int8_t pointIdx)
{
    if (pointIdx < 0 || pointIdx > 7) return 255;
    switch (flavour) {
        case EnvFlavour::AMP:    return kAmpCCs[pointIdx];
        case EnvFlavour::FILTER: return kFilterCCs[pointIdx];
        case EnvFlavour::PITCH:  return kPitchCCs[pointIdx];
        default:                 return 255;
    }
}


// ===========================================================================
//  computeLayout — turn raw CC values into pixel coordinates
// ===========================================================================
MiniEnvelope::StageLayout MiniEnvelope::computeLayout(
    int16_t x, int16_t y, int16_t w,
    EnvFlavour flavour,
    uint8_t (*getCC)(uint8_t cc))
{
    StageLayout L;
    L.drawW = w - 2 * ENV_DRAW_PAD_X;
    L.drawH = ENV_CELL_H - 2 * ENV_DRAW_PAD_Y;
    L.x0    = x + ENV_DRAW_PAD_X;
    L.y0    = y + ENV_DRAW_PAD_Y;

    // --- read raw CC values for this flavour ---
    const uint8_t* ccs = (flavour == EnvFlavour::AMP)    ? kAmpCCs
                       : (flavour == EnvFlavour::FILTER) ? kFilterCCs
                       :                                   kPitchCCs;

    uint8_t ccAtk  = getCC(ccs[EnvPoint::ATK_TIME]);
    uint8_t ccDec  = getCC(ccs[EnvPoint::DEC_TIME]);
    uint8_t ccRel  = getCC(ccs[EnvPoint::REL_TIME]);
    uint8_t ccSus  = getCC(ccs[EnvPoint::SUS_LEVEL]);

    // convert time CCs to milliseconds for proportional width allocation
    float msAtk = cc_to_time_ms(ccAtk);
    float msDec = cc_to_time_ms(ccDec);
    float msRel = cc_to_time_ms(ccRel);

    // sustain gets a fixed visual width (not time-based — it's indefinite)
    float msSus = (msAtk + msDec + msRel) * 0.3f;
    if (msSus < 50.0f) msSus = 50.0f;

    float total = msAtk + msDec + msSus + msRel;
    if (total < 1.0f) total = 1.0f;

    // proportional widths, clamped to min/max
    auto stageW = [&](float ms) -> int16_t {
        int16_t px = (int16_t)((ms / total) * (float)L.drawW);
        if (ms > 0.0f && px < ENV_MIN_STAGE_W) px = ENV_MIN_STAGE_W;
        if (ms == 0.0f) px = ENV_MIN_ZERO_W;
        if (px > ENV_MAX_STAGE_W) px = ENV_MAX_STAGE_W;
        return px;
    };

    int16_t wAtk = stageW(msAtk);
    int16_t wDec = stageW(msDec);
    int16_t wSus = stageW(msSus);
    int16_t wRel = stageW(msRel);

    // normalise so they fit exactly in drawW
    int16_t wTotal = wAtk + wDec + wSus + wRel;
    if (wTotal != L.drawW && wTotal > 0) {
        float scale = (float)L.drawW / (float)wTotal;
        wAtk = (int16_t)(wAtk * scale);
        wDec = (int16_t)(wDec * scale);
        wSus = (int16_t)(wSus * scale);
        wRel = L.drawW - wAtk - wDec - wSus;  // remainder avoids rounding gap
    }

    L.xAtk = L.x0 + wAtk;
    L.xDec = L.xAtk + wDec;
    L.xSus = L.xDec + wSus;
    L.xEnd = L.xSus + wRel;

    // --- vertical positions ---
    float susNorm = (float)ccSus / 127.0f;

    if (flavour == EnvFlavour::AMP) {
        // unipolar: bottom = zero, top = unity
        L.yBase    = L.y0 + L.drawH;               // bottom
        L.yPeak    = L.y0;                          // top (unity)
        L.ySustain = L.y0 + (int16_t)((1.0f - susNorm) * L.drawH);
    } else {
        // bipolar: full height available.
        // positive depth → envelope rises from bottom (like amp)
        // negative depth → envelope drops from top (inverted)
        // |depth| scales how far the peak reaches toward the opposite edge
        uint8_t ccDepth = getCC(ccs[EnvPoint::DEPTH]);
        float depthNorm = ((float)ccDepth - 64.0f) / 63.0f;  // -1..+1
        if (depthNorm >  1.0f) depthNorm =  1.0f;
        if (depthNorm < -1.0f) depthNorm = -1.0f;

        if (depthNorm >= 0.0f) {
            // positive: baseline at bottom, peak rises toward top
            L.yBase    = L.y0 + L.drawH;
            L.yPeak    = L.y0 + (int16_t)((1.0f - depthNorm) * L.drawH);
            int16_t peakExtent = L.yBase - L.yPeak;
            L.ySustain = L.yBase - (int16_t)(susNorm * (float)peakExtent);
        } else {
            // negative: baseline at top, peak drops toward bottom
            L.yBase    = L.y0;
            L.yPeak    = L.y0 + (int16_t)((-depthNorm) * L.drawH);
            int16_t peakExtent = L.yPeak - L.yBase;  // positive distance downward
            L.ySustain = L.yBase + (int16_t)(susNorm * (float)peakExtent);
        }
    }

    return L;
}


// ===========================================================================
//  drawCurvedSegment — polyline approximation of a geometric-series curve
//
//  Uses the same alpha/factor math as the envelope engine so the visual
//  shape matches what the ear hears.  Cost: ~8 drawLine() calls.
// ===========================================================================
void MiniEnvelope::drawCurvedSegment(
    ILI9341_t3n& tft,
    int16_t x0, int16_t y0, int16_t x1, int16_t y1,
    float curve, uint16_t colour)
{
    const int N = ENV_CURVE_SEGS;

    // convert curve exponent to alpha (same as envelope engine)
    float alpha;
    if (curve <= 1.0f) alpha = -6.0f * (1.0f - curve) / 0.85f;
    else               alpha =  6.0f * (curve - 1.0f) / 4.0f;

    int16_t prevX = x0, prevY = y0;

    if (fabsf(alpha) < 0.01f) {
        // linear — single straight line
        tft.drawLine(x0, y0, x1, y1, colour);
        return;
    }

    // geometric series: compute cumulative normalised position at each step
    float r  = expf(alpha / (float)N);
    float rN = powf(r, (float)N);
    float denom = rN - 1.0f;
    if (fabsf(denom) < 1e-6f) {
        // degenerate — straight line
        tft.drawLine(x0, y0, x1, y1, colour);
        return;
    }

    // inc_fraction at step 0, then multiply by r each step
    // sum of all fractions = 1.0 (normalised range)
    float inc = (r - 1.0f) / denom;
    float cumulative = 0.0f;

    for (int i = 1; i <= N; i++) {
        cumulative += inc;
        inc *= r;

        float t = (float)i / (float)N;       // linear time position
        float v = cumulative;                 // curved amplitude position

        int16_t px = x0 + (int16_t)(t * (float)(x1 - x0));
        int16_t py = y0 + (int16_t)(v * (float)(y1 - y0));

        tft.drawLine(prevX, prevY, px, py, colour);
        prevX = px;
        prevY = py;
    }
}


// ===========================================================================
//  curveDotPosition — where to place the curve adjustment dot
//
//  Positioned at the visual midpoint of the curved segment.
//  When curve = 1.0 (linear), the dot is exactly at the segment midpoint.
//  When curved, the dot sits on the curve at t=0.5.
// ===========================================================================
void MiniEnvelope::curveDotPosition(
    int16_t x0, int16_t y0, int16_t x1, int16_t y1,
    float curve,
    int16_t& dotX, int16_t& dotY)
{
    // x is always at the segment midpoint (t = 0.5 along time axis)
    dotX = (x0 + x1) / 2;

    // convert curve exponent to alpha
    float alpha;
    if (curve <= 1.0f) alpha = -6.0f * (1.0f - curve) / 0.85f;
    else               alpha =  6.0f * (curve - 1.0f) / 4.0f;

    if (fabsf(alpha) < 0.01f) {
        // linear — dot at exact midpoint
        dotY = (y0 + y1) / 2;
        return;
    }

    // compute the cumulative amplitude at t=0.5 using geometric series
    const int halfN = ENV_CURVE_SEGS / 2;
    const int N     = ENV_CURVE_SEGS;
    float r  = expf(alpha / (float)N);
    float rN = powf(r, (float)N);
    float denom = rN - 1.0f;
    if (fabsf(denom) < 1e-6f) { dotY = (y0 + y1) / 2; return; }

    float inc = (r - 1.0f) / denom;
    float cumulative = 0.0f;
    for (int i = 0; i < halfN; i++) {
        cumulative += inc;
        inc *= r;
    }

    dotY = y0 + (int16_t)(cumulative * (float)(y1 - y0));
}


// ===========================================================================
//  draw — main entry point, called from HomeScreen::_drawControl()
// ===========================================================================
void MiniEnvelope::draw(
    ILI9341_t3n& tft, int16_t x, int16_t y, int16_t w,
    EnvFlavour flavour,
    uint8_t (*getCC)(uint8_t cc),
    int8_t selectedPt)
{
    // --- clear widget area ---
    tft.fillRect(x, y, w, ENV_CELL_H, COLOUR_SURFACE);

    // --- compute layout ---
    StageLayout L = computeLayout(x, y, w, flavour, getCC);

    // --- read curve values for this flavour ---
    const uint8_t* ccs = (flavour == EnvFlavour::AMP)    ? kAmpCCs
                       : (flavour == EnvFlavour::FILTER) ? kFilterCCs
                       :                                   kPitchCCs;

    float crvAtk = cc_to_curve(getCC(ccs[EnvPoint::ATK_CURVE]));
    float crvDec = cc_to_curve(getCC(ccs[EnvPoint::DEC_CURVE]));
    float crvRel = cc_to_curve(getCC(ccs[EnvPoint::REL_CURVE]));

    // --- all segments drawn in bright orange ---
    const uint16_t colLine = COLOUR_ACCENT_ORANGE;

    // --- draw envelope segments ---

    // attack: start → peak
    drawCurvedSegment(tft, L.x0, L.yBase, L.xAtk, L.yPeak, crvAtk, colLine);

    // decay: peak → sustain
    drawCurvedSegment(tft, L.xAtk, L.yPeak, L.xDec, L.ySustain, crvDec, colLine);

    // sustain: flat line at sustain level
    tft.drawLine(L.xDec, L.ySustain, L.xSus, L.ySustain, colLine);

    // release: sustain → baseline
    drawCurvedSegment(tft, L.xSus, L.ySustain, L.xEnd, L.yBase, crvRel, colLine);

    // --- draw control point dots ---
    struct DotInfo { int16_t px, py; int8_t ptIdx; };
    DotInfo dots[8];
    int dotCount = 0;

    // attack time dot: at the peak vertex
    dots[dotCount++] = { L.xAtk, L.yPeak, EnvPoint::ATK_TIME };

    // attack curve dot: on the attack segment
    {
        int16_t dx, dy;
        curveDotPosition(L.x0, L.yBase, L.xAtk, L.yPeak, crvAtk, dx, dy);
        dots[dotCount++] = { dx, dy, EnvPoint::ATK_CURVE };
    }

    // depth dot (bipolar only): at the peak vertex (shares position with atk time)
    if (flavour != EnvFlavour::AMP) {
        dots[dotCount++] = { L.xAtk, L.yPeak, EnvPoint::DEPTH };
    }

    // decay time dot: at the end of decay (start of sustain flat line)
    dots[dotCount++] = { L.xDec, L.ySustain, EnvPoint::DEC_TIME };

    // decay curve dot: on the decay segment
    {
        int16_t dx, dy;
        curveDotPosition(L.xAtk, L.yPeak, L.xDec, L.ySustain, crvDec, dx, dy);
        dots[dotCount++] = { dx, dy, EnvPoint::DEC_CURVE };
    }

    // sustain level dot: midpoint of the sustain flat line
    dots[dotCount++] = { (int16_t)((L.xDec + L.xSus) / 2), L.ySustain,
                         EnvPoint::SUS_LEVEL };

    // release time dot: at the end of release
    dots[dotCount++] = { L.xEnd, L.yBase, EnvPoint::REL_TIME };

    // release curve dot: on the release segment
    {
        int16_t dx, dy;
        curveDotPosition(L.xSus, L.ySustain, L.xEnd, L.yBase, crvRel, dx, dy);
        dots[dotCount++] = { dx, dy, EnvPoint::REL_CURVE };
    }

    // --- render dots: all same colour, selected dot is larger ---
    for (int i = 0; i < dotCount; i++) {
        int16_t r = (dots[i].ptIdx == selectedPt) ? ENV_DOT_R_SEL : ENV_DOT_R_UNSEL;
        tft.fillCircle(dots[i].px, dots[i].py, r, COLOUR_ACCENT_ORANGE);
    }
}