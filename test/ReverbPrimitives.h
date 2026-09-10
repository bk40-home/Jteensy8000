// =============================================================================
// ReverbPrimitives.h — inner-loop building blocks shared by every reverb
// =============================================================================
//
// WHY THESE MOVED OUT OF PlateReverb
//   Room (and Hall, and Gated after it) need the same circular delay line,
//   allpass and one-pole filters the plate uses.  Re-declaring them per
//   algorithm would be four copies of the same arithmetic to keep in step —
//   exactly the duplication the "standardise language" rule exists to stop.
//   Moved VERBATIM: not one operation changed, which is what lets the plate's
//   bit-identity test still pass across this refactor.
//
// WHY THEY ARE HEADER-INLINE
//   These are the per-sample inner loop.  They MUST inline into each
//   algorithm's processBlock or every sample pays a call.  Same rationale as
//   VAFilterCore.h.  The big static tables live in PlateReverb.cpp so only one
//   translation unit carries them.
//
// NAMESPACE
//   JT::Rv — short on purpose, since these names appear all over the inner
//   loops and `Rv::DelayLine` reads better there than a longer qualifier.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>
#include <string.h>   // memset
#include <math.h>     // fabsf, sqrtf

#include "core/AudioConfig.h"

namespace JT {

// Pitch-shifter lookup tables, defined once in PlateReverb.cpp.
namespace PitchTables {
extern const float kSemitoneRatios[37];   // -12..+24 semitones, 12-TET ratios
extern const float kFadeTable[257];       // raised-cosine crossfade, [256]=1.0
}

namespace Rv {

// Circular delay line, integer read/write, no modulo (M7 has no HW divide).
struct DelayLine {
    float*   buf      = nullptr;
    uint32_t len      = 0;
    uint32_t writeIdx = 0;

    inline void write(float s) {
        buf[writeIdx] = s;
        if (++writeIdx >= len) writeIdx = 0;
    }
    inline float read(uint32_t d) const {
        const uint32_t idx = (writeIdx >= d) ? (writeIdx - d)
                                             : (writeIdx + len - d);
        return buf[idx];
    }
    inline float readInterp(float d) const {
        const uint32_t i = (uint32_t)d;
        const float    f = d - (float)i;
        const float    s0 = read(i);
        const float    s1 = read(i + 1);
        return s0 + f * (s1 - s0);
    }
    void clear() { if (buf) memset(buf, 0, len * sizeof(float)); writeIdx = 0; }
};

// First-order allpass with feedback (Dattorro diffuser / tank APF).
struct Allpass {
    DelayLine dl;
    float     gain = 0.0f;

    inline float process(float x) {
        const float delayed = dl.read(dl.len - 1);
        const float y       = -gain * x + delayed;
        dl.write(x + gain * y);
        return y;
    }
    // Modulated tap for tank chorusing; delay clamped in-bounds.
    inline float processModulated(float x, float modSamples) {
        float d = (float)(dl.len - 1) + modSamples;
        if (d < 1.0f)                d = 1.0f;
        if (d > (float)(dl.len - 1)) d = (float)(dl.len - 1);
        const float delayed = dl.readInterp(d);
        const float y       = -gain * x + delayed;
        dl.write(x + gain * y);
        return y;
    }
    void clear() { dl.clear(); }
};

// One-pole LP: y = x + coeff*(y_prev - x).  coeff 0 => transparent.
struct OnePole_LP {
    float state = 0.0f;
    float coeff = 0.0f;
    inline float process(float x) { state = x + coeff * (state - x); return state; }
    void clear() { state = 0.0f; }
};

// One-pole HP: y = x - LP(x).  coeff<1e-3 => transparent bypass (MANDATORY
// guard — without it, coeff=0 makes state track x and output = SILENCE).
struct OnePole_HP {
    float state = 0.0f;
    float coeff = 0.0f;
    inline float process(float x) {
        if (coeff < 0.001f) return x;          // bypass guard
        state = x + coeff * (state - x);
        return x - state;
    }
    void clear() { state = 0.0f; }
};

// Doppler delay-line pitch shifter (hexefx AudioBasicPitch port).  Two read
// pointers 180 deg apart, raised-cosine crossfade over the splice.  ZERO
// cost when mix==0 or pitch==unity (returns input, no buffer touch).
struct PitchShifter {
    float*   buf       = nullptr;
    uint32_t readAddr  = 0;    // 16.16 fixed point into a 4096 buffer
    uint16_t writeAddr = 0;
    uint32_t readAdder = 0;    // phase increment per sample (pitch)
    float    mix       = 0.0f;
    float    lpState   = 0.0f; // output smoothing (softens splice aliasing)

    // Were PlateTank::kPitchBufBits / kPitchBufSize; now owned here, since the
    // shifter no longer lives inside that class.  PlateTank still exposes
    // kPitchBufSize for its pool arithmetic and simply forwards to these.
    static constexpr uint32_t BUF_BITS  = 12u;
    static constexpr uint32_t BUF_SIZE  = 1u << BUF_BITS;     // 4096
    static constexpr uint32_t BUF_MASK  = BUF_SIZE - 1u;
    static constexpr uint32_t FRAC_BITS = 32u - BUF_BITS;
    static constexpr uint32_t FRAC_MASK = (1u << FRAC_BITS) - 1u;
    static constexpr uint32_t DELTA_0   = 1u << FRAC_BITS;    // unity pitch
    static constexpr float    LP_COEFF  = 0.26f;             // ~6 kHz rolloff

    // Tables live at NAMESPACE scope (PitchTables below), not as class
    // statics: PlateTank is a template, so class statics would be
    // duplicated per instantiation — 294 floats of flash for every variant.

    void assign(float* p) { buf = p; clear(); }
    void setPitch(float ratio)     { readAdder = (uint32_t)((float)DELTA_0 * ratio); }
    void setPitchSemitones(int8_t st) {
        if (st < -12) st = -12; else if (st > 24) st = 24;
        setPitch(PitchTables::kSemitoneRatios[st + 12]);
    }
    void setMix(float m) { mix = (m < 0.0f) ? 0.0f : (m > 1.0f ? 1.0f : m); }
    void clear() {
        if (buf) memset(buf, 0, BUF_SIZE * sizeof(float));
        readAddr = 0; writeAddr = 0; readAdder = DELTA_0; lpState = 0.0f;
    }

    inline float process(float input) {
        // Bypass — no buffer touched (the CPU win; see banner).
        if (mix == 0.0f || readAdder == DELTA_0) return input;

        buf[writeAddr] = input;
        readAddr += readAdder;

        const uint32_t idx1   = (readAddr >> FRAC_BITS) & BUF_MASK;
        const float    kFrac1 = (float)(readAddr & FRAC_MASK) * (1.0f / (float)FRAC_MASK);
        const float    sMain  = buf[idx1] * (1.0f - kFrac1)
                              + buf[(idx1 + 1u) & BUF_MASK] * kFrac1;

        const uint32_t readAddr2 = readAddr + 0x80000000u;      // 180 deg
        const uint32_t idx2   = (readAddr2 >> FRAC_BITS) & BUF_MASK;
        const float    kFrac2 = (float)(readAddr2 & FRAC_MASK) * (1.0f / (float)FRAC_MASK);
        const float    sHalf  = buf[idx2] * (1.0f - kFrac2)
                              + buf[(idx2 + 1u) & BUF_MASK] * kFrac2;

        const uint32_t distAcc  = readAddr - ((uint32_t)writeAddr << FRAC_BITS);
        const uint32_t fadeIdx  = (distAcc >> (32u - 9u)) & 0x1FFu;
        const float    fadeFrac = (float)(distAcc & ((1u << 23u) - 1u))
                                * (1.0f / (float)((1u << 23u) - 1u));
        const uint32_t tblIdx = fadeIdx & 0xFFu;
        const float    xf0    = PitchTables::kFadeTable[tblIdx];
        const float    xf1    = PitchTables::kFadeTable[tblIdx + 1u];   // [256]=1.0 guard
        float          blend  = xf0 * (1.0f - fadeFrac) + xf1 * fadeFrac;
        if (fadeIdx > 0xFFu) blend = 1.0f - blend;

        float pitched = sMain * blend + sHalf * (1.0f - blend);
        lpState += LP_COEFF * (pitched - lpState);
        pitched = lpState;

        writeAddr = (writeAddr + 1u) & BUF_MASK;
        return pitched * mix + input * (1.0f - mix);
    }
};

} // namespace Rv
} // namespace JT
