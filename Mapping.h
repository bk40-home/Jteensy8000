// Mapping.h
//
// Centralised CC (0..127) <-> internal unit mappings for the JT-8000.
//
// All functions are inline and header-only — no .cpp required.
// Callers include: SynthEngine.cpp, Patch.cpp, UIManager.cpp.
//
// Naming convention:
//   cc_to_*   : CC byte (0..127) -> engineering unit (Hz, ms, normalised 0..1)
//   *_to_cc   : engineering unit -> CC byte
//
// Filter resonance has two separate paths depending on which engine is active:
//   OBXa  : 0..OBXA_RES_MAX  (see cc_to_obxa_res01 / obxa_res01_to_cc)
//   VA    : 0..1.0            (see cc_to_va_res01   / va_res01_to_cc)
//
// The engine-aware helpers cc_to_resonance() / resonance_to_cc() should be
// used at the CC dispatch site so the correct range is applied automatically.

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

// OBXa cutoff is clamped to the general ceiling; the bilinear tan() inside
// the core handles the full 20 kHz range at 44100 Hz sample rate.
static constexpr float OBXA_CUTOFF_MAX_HZ = CUTOFF_MAX_HZ;
static constexpr float OBXA_CUTOFF_MIN_HZ = CUTOFF_MIN_HZ;

// ---------------------------------------------------------------------------
// Preset byte->CC transform descriptor
// Used by the preset import path (Patch.cpp / preset loader).
// ---------------------------------------------------------------------------
enum class Xform : uint8_t {
    Raw_0_127,
    Bool_0_127,
    Enum_Direct,
    Scale_0_99_to_127,
    EnumMap_Osc1Wave,
    EnumMap_Osc2Wave,
    EnumMap_LFOWave,
    EnumMap_LFO1Dest,
};

// Apply an Xform to a raw preset byte, producing a CC value.
inline uint8_t toCC(uint8_t raw, Xform xf) {
    switch (xf) {
        case Xform::Bool_0_127:        return raw ? 127 : 0;
        case Xform::Enum_Direct:       return raw;
        case Xform::Scale_0_99_to_127: return (raw > 99) ? 127 : (raw * 127) / 99;
        default:                       return raw;
    }
}

// ---------------------------------------------------------------------------
// Preset byte->CC mapping slot
//
// byte1 : 1-indexed byte position in the 64-byte JP-8000 SysEx patch block.
// cc    : JT-8000 CC number (see CCDefs.h).
// xf    : transform applied before dispatching as a CC.
//
// Derived from JT8000_Parsed_Microsphere_Bank_CHUNKED_ENUMS.csv.
// Bytes not listed are unused or carry non-CC data (patch name, category).
// ---------------------------------------------------------------------------
struct Slot { uint8_t byte1; uint8_t cc; Xform xf; };

inline constexpr Slot kSlots[] = {
    // --- Oscillators -------------------------------------------------------
    {  1, CC::OSC1_WAVE,          Xform::Enum_Direct        }, // OSC1 waveform index
    {  2, CC::OSC2_WAVE,          Xform::Scale_0_99_to_127  }, // OSC2 waveform (0-99 -> 0-127)
    {  3, CC::OSC1_MIX,           Xform::Scale_0_99_to_127  }, // OSC1 mix / PWM / detune
    {  4, CC::OSC2_MIX,           Xform::Scale_0_99_to_127  }, // OSC2 mix
    {  5, CC::OSC1_PITCH_OFFSET,  Xform::Scale_0_99_to_127  }, // OSC1 coarse (semitones)
    {  6, CC::OSC1_FINE_TUNE,     Xform::Scale_0_99_to_127  }, // OSC1 fine tune (cents)
    {  7, CC::OSC2_PITCH_OFFSET,  Xform::Scale_0_99_to_127  }, // OSC2 coarse
    {  8, CC::OSC2_FINE_TUNE,     Xform::Scale_0_99_to_127  }, // OSC2 fine tune
    {  9, CC::OSC_MIX_BALANCE,    Xform::Scale_0_99_to_127  }, // OSC1/2 balance

    // --- VCF envelope ------------------------------------------------------
    { 15, CC::FILTER_ENV_ATTACK,  Xform::Scale_0_99_to_127  }, // VCF attack
    { 16, CC::FILTER_ENV_DECAY,   Xform::Scale_0_99_to_127  }, // VCF decay
    { 17, CC::FILTER_ENV_SUSTAIN, Xform::Scale_0_99_to_127  }, // VCF sustain
    { 18, CC::FILTER_ENV_RELEASE, Xform::Scale_0_99_to_127  }, // VCF release

    // --- VCA envelope ------------------------------------------------------
    { 19, CC::AMP_ATTACK,         Xform::Scale_0_99_to_127  }, // VCA attack
    { 20, CC::AMP_DECAY,          Xform::Scale_0_99_to_127  }, // VCA decay
    { 21, CC::AMP_SUSTAIN,        Xform::Scale_0_99_to_127  }, // VCA sustain
    { 22, CC::AMP_RELEASE,        Xform::Scale_0_99_to_127  }, // VCA release

    // --- VCF amount --------------------------------------------------------
    { 23, CC::FILTER_ENV_AMOUNT,  Xform::Scale_0_99_to_127  }, // VCF env mod depth

    // --- Ring modulator ----------------------------------------------------
    { 44, CC::RING1_MIX,          Xform::Bool_0_127         }, // ring switch on/off
    { 45, CC::RING2_MIX,          Xform::Scale_0_99_to_127  }, // ring mix amount

    // --- Portamento --------------------------------------------------------
    { 46, CC::GLIDE_ENABLE,       Xform::Bool_0_127         }, // porta/gliss enable
    { 47, CC::GLIDE_TIME,         Xform::Scale_0_99_to_127  }, // porta time

    // --- LFO 1 -------------------------------------------------------------
    { 48, CC::LFO1_WAVEFORM,      Xform::Enum_Direct        }, // LFO1 waveform
    { 50, CC::LFO1_FREQ,          Xform::Scale_0_99_to_127  }, // LFO1 speed
    { 51, CC::LFO1_DEPTH,         Xform::Scale_0_99_to_127  }, // LFO1 amount
    { 54, CC::LFO1_DESTINATION,   Xform::Enum_Direct        }, // LFO1 destination

    // --- LFO 2 -------------------------------------------------------------
    { 49, CC::LFO2_WAVEFORM,      Xform::Enum_Direct        }, // LFO2 waveform
    { 52, CC::LFO2_FREQ,          Xform::Scale_0_99_to_127  }, // LFO2 speed
    { 53, CC::LFO2_DEPTH,         Xform::Scale_0_99_to_127  }, // LFO2 amount
    // Note: LFO2 destination byte not present in preset CSV — omitted.
};

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

// Apply the current taper to a normalised [0..1] CC value.
inline float applyTaper(float t) {
    switch (cutoffTaperMode) {
        case TAPER_LOW:  return powf(t, 0.5f);   // sqrt — more resolution at low end
        case TAPER_HIGH: return powf(t, 2.0f);   // square — more resolution at high end
        default:         return t;               // linear
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
    // Invert the taper
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
//
// OBXA_RES_MAX < 1.0 keeps the OBXa core stable.  The entire CC range 0..127
// is mapped across 0..OBXA_RES_MAX so the knob feels full-range to the player.
// ---------------------------------------------------------------------------
inline float cc_to_obxa_res01(uint8_t cc) {
    // Scale so CC 127 -> OBXA_RES_MAX (not hard 1.0)
    return cc_to_norm(cc) * OBXA_RES_MAX;
}

inline uint8_t obxa_res01_to_cc(float r) {
    if (r < 0.0f)         r = 0.0f;
    if (r > OBXA_RES_MAX) r = OBXA_RES_MAX;
    // Invert: divide by OBXA_RES_MAX to get a 0..1 fraction, then encode
    const float n = (OBXA_RES_MAX > 0.0f) ? (r / OBXA_RES_MAX) : 0.0f;
    return norm_to_cc(n);
}

// ---------------------------------------------------------------------------
// CC <-> VA filter resonance (0..1.0, full range allowed)
// ---------------------------------------------------------------------------
inline float cc_to_va_res01(uint8_t cc) {
    return cc_to_norm(cc);   // full 0..1
}

inline uint8_t va_res01_to_cc(float r) {
    return norm_to_cc(r);
}

// ---------------------------------------------------------------------------
// Engine-aware resonance dispatch
//
// Call these at the CC receive site (SynthEngine::handleCC) and at the preset
// capture/restore site (Patch::captureFrom / applyTo) so the correct ceiling
// is applied for whichever engine is currently selected.
//
// engine: CC::FILTER_ENGINE_OBXA or CC::FILTER_ENGINE_VA  (see CCDefs.h)
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
const float msMin = 1.0f;       // minimum envelope time
const float msMax = 11880.0f;   // maximum envelope time

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
    return (mode >= OBXA_NUM_XPANDER_MODES) ? (OBXA_NUM_XPANDER_MODES - 1) : (uint8_t)mode;
}

inline uint8_t obxa_xpander_mode_to_cc(uint8_t mode) {
    if (mode >= OBXA_NUM_XPANDER_MODES) mode = OBXA_NUM_XPANDER_MODES - 1;
    const uint16_t start = (uint16_t)mode       * 128u / (uint16_t)OBXA_NUM_XPANDER_MODES;
    const uint16_t end   = (uint16_t)(mode + 1) * 128u / (uint16_t)OBXA_NUM_XPANDER_MODES;
    return (uint8_t)((start + end) / 2u);
}

// ---------------------------------------------------------------------------
// OBXa 2-pole feature toggles
// ---------------------------------------------------------------------------
inline bool cc_to_obxa_two_pole(uint8_t cc)      { return cc_to_bool(cc); }
inline bool cc_to_obxa_bpblend_2pole(uint8_t cc) { return cc_to_bool(cc); }
inline bool cc_to_obxa_push_2pole(uint8_t cc)    { return cc_to_bool(cc); }

} // namespace JT8000Map
