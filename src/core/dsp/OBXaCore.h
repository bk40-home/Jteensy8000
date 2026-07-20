// =============================================================================
// OBXaCore.h — OB-Xa/Xpander filter core for JT-8000 v2 (Pass 5.2)
// =============================================================================
//
// PROVENANCE
//   Ported from v1 AudioFilterOBXa_OBXf (itself derived from the OB-Xf
//   project's filter): the 2-pole SEM-style core with diode-pair feedback
//   nonlinearity and push/BP-blend options, and the 4-pole cascade with the
//   atan first-pole nonlinearity, multimode pole morph, and the 15-row
//   Xpander pole-mixing matrix — all constants byte-for-byte.
//
// PACKAGING (header-only, matching VAFilterCore/MoogDVCore)
//   Filter cores in this project are inline structs so per-sample process()
//   calls inline into the section's block loop — the documented pattern of
//   VAFilterCore.h.  The section (FilterSection.h/.cpp) carries the normal
//   .h/.cpp split.
//
// BLOCK-RATE COEFFICIENTS (flagged CPU improvement, v1-sanctioned)
//   v1's reference path called tanf() PER SAMPLE because its audio-rate
//   cutoff-mod bus could move every sample; v1 also shipped the block-rate
//   alternative behind JT_OPT_OBXA_BLOCKRATE_MOD.  v2 has block-rate cutoff
//   by construction, so g and lpc are computed by the CALLER once per
//   change and passed in — identical output, ~20 cycles/sample saved.
//
// DOUBLE PRECISION (kept for fidelity)
//   The first-pole update and the TPT helper use double intermediates,
//   exactly as the OB-Xf-validated v1 code did.  The M7 FPU executes
//   doubles in hardware (at reduced throughput); ~8 double ops/sample is
//   the price of not changing a validated resonance character.  Casts are
//   explicit so -Wdouble-promotion stays meaningful elsewhere.
//
// STABILITY (v1 lessons, both ported)
//   * Resonance ceiling 0.97 — the core runs away at exactly 1.0 (the
//     caller clamps; documented here because it is a CORE property).
//   * Cutoff ceiling 0.24·fs — the wrapper's stability margin.
//   * stateGuard(): block-rate NaN/runaway check that resets the poles —
//     v1's OBXA_STATE_GUARD, kept as cheap insurance (4 compares/block).
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>
#include <math.h>

#include "core/AudioConfig.h"   // JT_FLASH_DATA

namespace JT {

// -----------------------------------------------------------------------------
// Xpander pole-mixing matrix — verbatim v1/OB-Xf; DEFINED in OBXaCore.cpp.
// LINKAGE RULE (learned from a real linker error): flash tables must have
// PLAIN linkage.  An `inline constexpr` variable is a COMDAT symbol, and
// GCC gives a COMDAT different section flags than ordinary const data —
// two flavours in one named section ("​.progmem") is a hard "section type
// conflict".  Extern declaration + single plain definition sidesteps the
// whole class of problem, and matches the Crc32 table precedent.
// -----------------------------------------------------------------------------
extern const float kObxaPoleMix[15][5];

struct OBXaCore {
    static constexpr int   kNumXpanderModes = 15;
    static constexpr float kPi              = 3.14159265358979f;
    static constexpr float kResMax          = 0.97f;   // v1 OBXA_RES_MAX
    static constexpr float kHugeThreshold   = 1.0e6f;  // v1 runaway bound

    // Xpander pole-mixing matrix: defined at namespace scope below so the
    // flash-placement attribute rides an inline constexpr variable (the
    // same pattern the generated ParamTable uses).

    // --- filter state (poles) + derived resonance terms ---
    float pole1 = 0.0f, pole2 = 0.0f, pole3 = 0.0f, pole4 = 0.0f;
    float res2Pole = 1.0f;             // 2-pole damping: 1 - r
    float res4Pole = 0.0f;             // 4-pole feedback: 3.5 * r
    float resCorrection    = 1.0f;     // sample-rate warp of the atan NL
    float resCorrectionInv = 1.0f;

    // --- mode flags (mirrored from the section's decoded filter.mode) ---
    bool    useTwoPole   = false;
    bool    bpBlend2Pole = false;
    bool    push2Pole    = false;
    bool    xpander4Pole = false;
    uint8_t xpanderMode  = 0;

    // --- multimode morph (pre-split at control rate, exactly as v1) ---
    float multimode01    = 0.0f;
    int   multimodePole  = 0;
    float multimodeXfade = 0.0f;

    void reset()
    {
        pole1 = pole2 = pole3 = pole4 = 0.0f;
    }

    // v1 setSampleRate: the atan nonlinearity's drive is warped so the
    // resonance character survives sample-rate changes (OB-Xf heritage).
    void setSampleRate(float fs)
    {
        const float rcRate = sqrtf(44000.0f / fs);
        resCorrection    = (970.0f / 44000.0f) * rcRate;
        resCorrectionInv = 1.0f / resCorrection;
    }

    // Caller clamps to kResMax — see the header stability note.
    void setResonance(float r01)
    {
        res2Pole = 1.0f - r01;
        res4Pole = 3.5f * r01;
    }

    void setMultimode(float m01)
    {
        multimode01    = m01;
        multimodePole  = (int)(m01 * 3.0f);
        multimodeXfade = (m01 * 3.0f) - (float)multimodePole;
    }

    // -------------------------------------------------------------------------
    // Nonlinearities and helpers — verbatim v1.
    // -------------------------------------------------------------------------

    // OB-Xf diode-pair conductance approximation (the SEM feedback "give").
    static inline float diodePairResistanceApprox(float x)
    {
        return (((((0.0103592f) * x + 0.00920833f) * x + 0.185f) * x
                 + 0.05f) * x + 1.0f);
    }

    // 1-pole TPT with double intermediates — the OB-Xf-validated form
    // (see the header's precision note).  Casts explicit for -Wdouble-...
    static inline float tptScaled(float& state, float input, float lpc)
    {
        const double v   = (double)((input - state) * lpc);
        const double res = v + (double)state;
        state = (float)(res + v);
        return (float)res;
    }

    // -------------------------------------------------------------------------
    // 2-pole (SEM-style) — g precomputed by the caller at block rate.
    // -------------------------------------------------------------------------
    inline float process2Pole(float x, float g)
    {
        // ZDF solve with the diode pair riding the resonance path; 'push'
        // biases the pair slightly for the "2P Push" mode's extra bite.
        const float push = -1.0f - (push2Pole ? 0.035f : 0.0f);
        const float tCfb = diodePairResistanceApprox(pole1 * 0.0876f) + push;

        const float v = (x
                         - 2.0f * (pole1 * (res2Pole + tCfb))
                         - g * pole1
                         - pole2)
                        / (1.0f + g * (2.0f * (res2Pole + tCfb) + g));

        const float y1 = v * g + pole1;
        pole1 = v * g + y1;

        const float y2 = y1 * g + pole2;
        pole2 = y1 * g + y2;

        // Output tap: plain LP->input morph, or the BP-blend law that
        // sweeps LP -> BP -> input across the multimode knob.
        if (bpBlend2Pole) {
            return (multimode01 < 0.5f)
                 ? 2.0f * ((0.5f - multimode01) * y2 + multimode01 * y1)
                 : 2.0f * ((1.0f - multimode01) * y1 + (multimode01 - 0.5f) * v);
        }
        return (1.0f - multimode01) * y2 + multimode01 * v;
    }

    // -------------------------------------------------------------------------
    // 4-pole cascade — g and lpc precomputed by the caller at block rate.
    // -------------------------------------------------------------------------
    inline float process4Pole(float x, float g, float lpc)
    {
        // ZDF feedback solve across the whole cascade.
        const float ml = 1.0f / (1.0f + g);
        const float S  = (lpc * (lpc * (lpc * pole1 + pole2) + pole3) + pole4) * ml;
        const float G  = lpc * lpc * lpc * lpc;
        const float y0 = (x - res4Pole * S) / (1.0f + res4Pole * G);

        // First pole inline, with the OB-Xf atan nonlinearity bounding the
        // resonant current (double intermediates as validated — see header).
        const double v   = (double)((y0 - pole1) * lpc);
        const double res = v + (double)pole1;
        pole1 = (float)(res + v);
        pole1 = atanf(pole1 * resCorrection) * resCorrectionInv;

        const float y1 = (float)res;
        const float y2 = tptScaled(pole2, y1, lpc);
        const float y3 = tptScaled(pole3, y2, lpc);
        const float y4 = tptScaled(pole4, y3, lpc);

        float out;
        if (xpander4Pole) {
            const float* m = kObxaPoleMix[xpanderMode];
            out = y0 * m[0] + y1 * m[1] + y2 * m[2] + y3 * m[3] + y4 * m[4];
        } else {
            switch (multimodePole) {
                case 0:  out = (1.0f - multimodeXfade) * y4 + multimodeXfade * y3; break;
                case 1:  out = (1.0f - multimodeXfade) * y3 + multimodeXfade * y2; break;
                case 2:  out = (1.0f - multimodeXfade) * y2 + multimodeXfade * y1; break;
                case 3:  out = y1; break;
                default: out = 0.0f; break;
            }
        }

        // v1's resonance-dependent volume compensation.
        return out * (1.0f + res4Pole * 0.45f);
    }

    // -------------------------------------------------------------------------
    // Block-rate state guard — v1 OBXA_STATE_GUARD.  A NaN or runaway pole
    // poisons the IIR forever; four compares per block buy self-healing.
    // Returns true when a reset happened (caller may count it).
    // -------------------------------------------------------------------------
    inline bool stateGuard()
    {
        const bool bad = !(pole1 == pole1) || !(pole2 == pole2)       // NaN
                      || !(pole3 == pole3) || !(pole4 == pole4)
                      || fabsf(pole1) > kHugeThreshold
                      || fabsf(pole4) > kHugeThreshold;
        if (bad) reset();
        return bad;
    }
};

} // namespace JT
