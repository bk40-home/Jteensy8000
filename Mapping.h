/*
 * Mapping.h
 *
 * Centralised CC (0..127) <-> internal unit mappings for the JT-8000.
 *
 * All functions are inline and header-only — no .cpp required.
 * Callers include: SynthEngine.cpp, Patch.cpp, HomeScreen.cpp.
 *
 * Naming convention:
 *   cc_to_*   : CC byte (0..127) -> engineering unit (Hz, ms, normalised 0..1)
 *   *_to_cc   : engineering unit -> CC byte
 *
 * Filter resonance has two separate paths depending on which engine is active:
 *   OBXa  : 0..OBXA_RES_MAX  (see cc_to_obxa_res01 / obxa_res01_to_cc)
 *   VA    : 0..1.0            (see cc_to_va_res01   / va_res01_to_cc)
 *
 * The engine-aware helpers cc_to_resonance() / resonance_to_cc() should be
 * used at the CC dispatch site so the correct range is applied automatically.
 *
 * REMOVED in factory bank refactor:
 *   - Xform enum, toCC(), kSlots[] — SysEx byte→CC import path.
 *     JUCE app now handles SysEx import; hardware uses flat CC arrays.
 */

#pragma once
#include <math.h>
#include <Arduino.h>
#include "CCDefs.h"
#include "LFOBlock.h"   // for NUM_LFO_DESTS

namespace JT8000Map {

// ---------------------------------------------------------------------------
// Global frequency range constants (shared by all filter engines)
// ---------------------------------------------------------------------------
static constexpr float CUTOFF_MIN_HZ = 20.0f;
static constexpr float CUTOFF_MAX_HZ = 20000.0f;

// ---------------------------------------------------------------------------
// OBXa-specific constants
// ---------------------------------------------------------------------------
static constexpr int   OBXA_NUM_XPANDER_MODES = 15;

// OBXa resonance ceiling.
// The OBXa core becomes unstable when resonance reaches exactly 1.0.
// Keeping a small safety margin prevents NaN/runaway without
// audibly limiting the range.  Tune between 0.97 and 1.0 as needed.
static constexpr float OBXA_RES_MAX = 0.97f;

static constexpr float OBXA_CUTOFF_MAX_HZ = CUTOFF_MAX_HZ;
static constexpr float OBXA_CUTOFF_MIN_HZ = CUTOFF_MIN_HZ;

// ---------------------------------------------------------------------------
// Utility: clamp to 0..1
// ---------------------------------------------------------------------------
inline float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// ---------------------------------------------------------------------------
// CC <-> normalised 0..1  (generic; use for non-resonance parameters)
// ---------------------------------------------------------------------------
inline float   cc_to_norm(uint8_t cc) { if (cc > 127) cc = 127; return cc / 127.0f; }
inline uint8_t norm_to_cc(float n)    { n = clamp01(n); return (uint8_t)constrain(lroundf(n * 127.0f), 0, 127); }

// ---------------------------------------------------------------------------
// CC <-> boolean  (threshold: cc >= 64 -> true)
// ---------------------------------------------------------------------------
inline bool    cc_to_bool(uint8_t cc) { return cc >= 64; }
inline uint8_t bool_to_cc(bool b)     { return b ? 127 : 0; }

// ---------------------------------------------------------------------------
// Cutoff taper  (adjustable response curve for the cutoff knob)
// ---------------------------------------------------------------------------
enum CutoffTaper { TAPER_NEUTRAL = 0, TAPER_LOW, TAPER_HIGH };
static CutoffTaper cutoffTaperMode = TAPER_LOW;   // default: square-root feel

inline float applyTaper(float t) {
    switch (cutoffTaperMode) {
        case TAPER_LOW:  return powf(t, 0.5f);   // sqrt — more resolution at low end
        case TAPER_HIGH: return powf(t, 2.0f);   // square — more resolution at high end
        default:         return t;                // linear
    }
}

// ---------------------------------------------------------------------------
// CC <-> filter cutoff frequency (Hz), shared exponential curve
// ---------------------------------------------------------------------------
inline float cc_to_cutoff_hz(uint8_t cc) {
    const float t = applyTaper(cc_to_norm(cc));
    return CUTOFF_MIN_HZ * powf(CUTOFF_MAX_HZ / CUTOFF_MIN_HZ, t);
}

inline uint8_t cutoff_hz_to_cc(float hz) {
    hz = fmaxf(CUTOFF_MIN_HZ, fminf(hz, CUTOFF_MAX_HZ));
    float t = logf(hz / CUTOFF_MIN_HZ) / logf(CUTOFF_MAX_HZ / CUTOFF_MIN_HZ);
    if (cutoffTaperMode == TAPER_LOW)  t = powf(t, 2.0f);
    if (cutoffTaperMode == TAPER_HIGH) t = powf(t, 0.5f);
    return (uint8_t)constrain(lroundf(t * 127.0f), 0, 127);
}

// ---------------------------------------------------------------------------
// CC <-> OBXa cutoff (applies engine-specific Hz ceiling)
// ---------------------------------------------------------------------------
inline float cc_to_obxa_cutoff_hz(uint8_t cc) {
    float hz = cc_to_cutoff_hz(cc);
    if (hz < OBXA_CUTOFF_MIN_HZ) hz = OBXA_CUTOFF_MIN_HZ;
    if (hz > OBXA_CUTOFF_MAX_HZ) hz = OBXA_CUTOFF_MAX_HZ;
    return hz;
}

inline uint8_t obxa_cutoff_hz_to_cc(float hz) {
    if (hz < OBXA_CUTOFF_MIN_HZ) hz = OBXA_CUTOFF_MIN_HZ;
    if (hz > OBXA_CUTOFF_MAX_HZ) hz = OBXA_CUTOFF_MAX_HZ;
    return cutoff_hz_to_cc(hz);
}

// ---------------------------------------------------------------------------
// CC <-> OBXa resonance (0..OBXA_RES_MAX)
// ---------------------------------------------------------------------------
inline float cc_to_obxa_res01(uint8_t cc) {
    return cc_to_norm(cc) * OBXA_RES_MAX;
}

inline uint8_t obxa_res01_to_cc(float r) {
    if (r < 0.0f)         r = 0.0f;
    if (r > OBXA_RES_MAX) r = OBXA_RES_MAX;
    const float n = (OBXA_RES_MAX > 0.0f) ? (r / OBXA_RES_MAX) : 0.0f;
    return norm_to_cc(n);
}

// ---------------------------------------------------------------------------
// CC <-> VA filter resonance (0..1.0, full range allowed)
// ---------------------------------------------------------------------------
inline float cc_to_va_res01(uint8_t cc) {
    return cc_to_norm(cc);
}

inline uint8_t va_res01_to_cc(float r) {
    return norm_to_cc(r);
}

// ---------------------------------------------------------------------------
// Engine-aware resonance dispatch
// ---------------------------------------------------------------------------
inline float cc_to_resonance(uint8_t cc, uint8_t engine) {
    return (engine == CC::FILTER_ENGINE_VA)
        ? cc_to_va_res01(cc)
        : cc_to_obxa_res01(cc);
}

inline uint8_t resonance_to_cc(float r, uint8_t engine) {
    return (engine == CC::FILTER_ENGINE_VA)
        ? va_res01_to_cc(r)
        : obxa_res01_to_cc(r);
}

// ---------------------------------------------------------------------------
// CC <-> envelope time (ms), logarithmic curve
// ---------------------------------------------------------------------------
const float msMin = 1.0f;
const float msMax = 11880.0f;

inline float cc_to_time_ms(uint8_t cc) {
    const float t = (float)cc / 127.0f;
    return msMin * powf(msMax / msMin, t);
}

inline uint8_t time_ms_to_cc(float ms) {
    if (ms <= msMin) return 0;
    if (ms >= msMax) return 127;
    const float cc = 127.0f * logf(ms / msMin) / logf(msMax / msMin);
    return (uint8_t)constrain(lroundf(cc), 0, 127);
}

// ---------------------------------------------------------------------------
// CC <-> LFO frequency (Hz), logarithmic curve  0.03..39 Hz
// ---------------------------------------------------------------------------
inline float cc_to_lfo_hz(uint8_t cc) {
    return 0.03f * powf(1300.0f, cc_to_norm(cc));
}

inline uint8_t lfo_hz_to_cc(float hz) {
    if (hz <= 0.03f)           return 0;
    if (hz >= 0.03f * 1300.0f) return 127;
    const float n = logf(hz / 0.03f) / logf(1300.0f);
    return norm_to_cc(n);
}

// ---------------------------------------------------------------------------
// CC <-> LFO destination (integer enum, spread evenly across 0..127)
// ---------------------------------------------------------------------------
inline uint8_t ccFromLfoDest(int dest) {
    if (dest < 0)              dest = 0;
    if (dest >= NUM_LFO_DESTS) dest = NUM_LFO_DESTS - 1;
    const uint16_t start = ((uint16_t)dest       * 128u) / NUM_LFO_DESTS;
    const uint16_t end   = ((uint16_t)(dest + 1) * 128u) / NUM_LFO_DESTS;
    return (uint8_t)((start + end) / 2u);
}

inline int lfoDestFromCC(uint8_t cc) {
    uint8_t idx = (uint16_t(cc) * NUM_LFO_DESTS) / 128u;
    if (idx >= NUM_LFO_DESTS) idx = NUM_LFO_DESTS - 1;
    return (int)idx;
}

// ---------------------------------------------------------------------------
// OBXa multimode  (0..1 blend between pole outputs)
// ---------------------------------------------------------------------------
inline float   cc_to_obxa_multimode(uint8_t cc) { return cc_to_norm(cc); }
inline uint8_t obxa_multimode_to_cc(float m)    { return norm_to_cc(m); }

// ---------------------------------------------------------------------------
// OBXa Xpander mode  (0..14, spread evenly across 0..127)
// ---------------------------------------------------------------------------
inline uint8_t cc_to_obxa_xpander_mode(uint8_t cc) {
    const uint16_t mode = (uint16_t)cc * (uint16_t)OBXA_NUM_XPANDER_MODES / 128u;
    return (mode >= OBXA_NUM_XPANDER_MODES)
        ? (uint8_t)(OBXA_NUM_XPANDER_MODES - 1)
        : (uint8_t)mode;
}

inline uint8_t obxa_xpander_mode_to_cc(uint8_t mode) {
    if (mode >= OBXA_NUM_XPANDER_MODES) mode = OBXA_NUM_XPANDER_MODES - 1;
    const uint16_t start = ((uint16_t)mode       * 128u) / OBXA_NUM_XPANDER_MODES;
    const uint16_t end   = ((uint16_t)(mode + 1) * 128u) / OBXA_NUM_XPANDER_MODES;
    return (uint8_t)((start + end) / 2u);
}

} // namespace JT8000Map
