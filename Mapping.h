// Mapping.h
//
// Centralized forward + inverse mappings between 7-bit CC (0..127) and
// internal units (Hz, ms, normalized) used by the JT-8000 project.

#pragma once
#include <math.h>
#include <Arduino.h>
#include "CCDefs.h"
#include "LFOBlock.h"  // for NUM_LFO_DESTS used in LFO destination binning

namespace JT8000Map {

    static constexpr float CUTOFF_MIN_HZ = 20.0f;
    static constexpr float CUTOFF_MAX_HZ = 20000.0f;

    static constexpr int   OBXA_NUM_XPANDER_MODES = 15;

    // ----- Optional JT byte -> CC helpers (kept for preset import paths) -----
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

    // Minimal example LUTs used by your preset loader path (unchanged)
    inline uint8_t toCC(uint8_t raw, Xform xf) {
        switch (xf) {
            case Xform::Bool_0_127:        return raw ? 127 : 0;
            case Xform::Enum_Direct:       return raw;
            case Xform::Scale_0_99_to_127: return (raw > 99) ? 127 : (raw * 127) / 99;
            default:                       return raw;
        }
    }

 
    // ---------------------------------------------------------------------------
    // Preset byte→CC mapping slot
    //
    // byte1 : 1-indexed byte position in the 64-byte JP-8000 SysEx patch block.
    // cc    : JT-8000 CC number to dispatch (see CCDefs.h).
    // xf    : transform applied to the raw byte before sending as CC.
    //
    // Derived from JT8000_Parsed_Microsphere_Bank_CHUNKED_ENUMS.csv.
    // Bytes not listed here are either unused or carry non-CC data (patch name,
    // category).  All bytes are 0-99 in the Microsphere bank; Scale_0_99_to_127
    // is used for any parameter where the raw range is 0..99 and needs mapping
    // to full 0..127 CC range.  Enum bytes use Enum_Direct (value sent as-is).
    // ---------------------------------------------------------------------------
    struct Slot { uint8_t byte1; uint8_t cc; Xform xf; };

    inline constexpr Slot kSlots[] = {
        // --- Oscillators -------------------------------------------------------
        {  1, CC::OSC1_WAVE,          Xform::Enum_Direct        }, // OSC1 waveform index
        {  2, CC::OSC2_WAVE,          Xform::Scale_0_99_to_127  }, // OSC2 wave (raw 0-99 → CC 0-127)
        {  3, CC::OSC1_MIX,           Xform::Scale_0_99_to_127  }, // OSC1 additional (PWM/Detune/FM)
        {  4, CC::OSC2_MIX,           Xform::Scale_0_99_to_127  }, // OSC2 additional
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
        { 23, CC::FILTER_ENV_AMOUNT,  Xform::Scale_0_99_to_127  }, // VCF env amount

        // --- Ring modulator ----------------------------------------------------
        // Byte 44: ring switch — treat as boolean (0=off, non-zero=on)
        // Byte 45: ring mix amount
        { 44, CC::RING1_MIX,          Xform::Bool_0_127         }, // ring switch on/off
        { 45, CC::RING2_MIX,          Xform::Scale_0_99_to_127  }, // ring amount

        // --- Portamento --------------------------------------------------------
        // Byte 46: mode enum 0=Portamento 1=Glissando → bool enable (>0 = glide on)
        // Byte 47: amount (0=off, 1-99=glide time)
        { 46, CC::GLIDE_ENABLE,       Xform::Bool_0_127         }, // porta/gliss enable
        { 47, CC::GLIDE_TIME,         Xform::Scale_0_99_to_127  }, // porta amount

        // --- LFO 1 -------------------------------------------------------------
        { 48, CC::LFO1_WAVEFORM,      Xform::Enum_Direct        }, // LFO1 waveform
        { 50, CC::LFO1_FREQ,          Xform::Scale_0_99_to_127  }, // LFO1 speed
        { 51, CC::LFO1_DEPTH,         Xform::Scale_0_99_to_127  }, // LFO1 amount
        { 54, CC::LFO1_DESTINATION,   Xform::Enum_Direct        }, // LFO1 destination

        // --- LFO 2 -------------------------------------------------------------
        { 49, CC::LFO2_WAVEFORM,      Xform::Enum_Direct        }, // LFO2 waveform
        { 52, CC::LFO2_FREQ,          Xform::Scale_0_99_to_127  }, // LFO2 speed
        { 53, CC::LFO2_DEPTH,         Xform::Scale_0_99_to_127  }, // LFO2 amount
        // Note: LFO2 destination byte not present in CSV — omitted.
    };

    // ----- Shared curves -----
    enum CutoffTaper { TAPER_NEUTRAL = 0, TAPER_LOW, TAPER_HIGH };
    static CutoffTaper cutoffTaperMode = TAPER_LOW;

    inline float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
    inline float cc_to_norm(uint8_t cc) { if (cc>127) cc=127; return cc/127.0f; }
    inline uint8_t norm_to_cc(float n) { n = clamp01(n); return (uint8_t)constrain(lroundf(n*127.0f),0,127); }

    inline float applyTaper(float t) {
        switch (cutoffTaperMode) { case TAPER_LOW: return powf(t,0.5f);
                                   case TAPER_HIGH:return powf(t,2.0f);
                                   default:        return t; }
    }

    inline float cc_to_cutoff_hz(uint8_t cc) {
        float t = applyTaper(cc_to_norm(cc));
        return CUTOFF_MIN_HZ * powf(CUTOFF_MAX_HZ / CUTOFF_MIN_HZ, t);
    }
    inline uint8_t cutoff_hz_to_cc(float hz) {
        hz = fmaxf(CUTOFF_MIN_HZ, fminf(hz, CUTOFF_MAX_HZ));
        float t = logf(hz / CUTOFF_MIN_HZ) / logf(CUTOFF_MAX_HZ / CUTOFF_MIN_HZ);
        if (cutoffTaperMode==TAPER_LOW)  t = powf(t,2.0f);
        if (cutoffTaperMode==TAPER_HIGH) t = powf(t,0.5f);
        return (uint8_t)constrain(lroundf(t*127.0f),0,127);
    }

    const float msMin = 1.0f;      // <<< set your desired minimum (e.g. 1 ms)
    const float msMax = 11880.0f;  // <<< set your desired maximum (e.g. 30 s)

    inline float cc_to_time_ms(uint8_t cc) {
    const float t = (float)cc / 127.0f;
    return msMin * powf(msMax / msMin, t);
}
    // =================== OBXa (OB-Xf) helpers ===================
    //
    // The OBXa core becomes numerically fragile near:
    //  - very high cutoff (bilinear tan() gets extreme)
    //  - resonance exactly 1.0 (full feedback)
    //
    // In your log, failure occurred at:
    //   cutoff=10584 Hz, res=1.000, fs=44100
    // 10584 is ~0.24 * 44100, which matches the usual practical cutoff clamp.
    //
    // Use THESE mappings for the OBXa filter block / CC mapping.

    // Clamp cutoff for OBXa core stability (0.24 * 44100 = 10584)
    static constexpr float OBXA_CUTOFF_MAX_HZ = 18000.0f;
    static constexpr float OBXA_CUTOFF_MIN_HZ = CUTOFF_MIN_HZ;

    inline float cc_to_obxa_cutoff_hz(uint8_t cc)
    {
        float hz = cc_to_cutoff_hz(cc); // reuse your exponential cutoff curve
        if (hz < OBXA_CUTOFF_MIN_HZ) hz = OBXA_CUTOFF_MIN_HZ;
        if (hz > OBXA_CUTOFF_MAX_HZ) hz = OBXA_CUTOFF_MAX_HZ;
        return hz;
    }

    inline uint8_t obxa_cutoff_hz_to_cc(float hz)
    {
        if (hz < OBXA_CUTOFF_MIN_HZ) hz = OBXA_CUTOFF_MIN_HZ;
        if (hz > OBXA_CUTOFF_MAX_HZ) hz = OBXA_CUTOFF_MAX_HZ;
        return cutoff_hz_to_cc(hz); // inverse of your shared curve (with clamp)
    }

    // Resonance for OBXa is 0..1. To avoid "exactly 1.0" edge cases, clamp slightly below 1.
    static constexpr float OBXA_RES_MAX = 0.91f; // tweak 0.99..0.999 as needed

    inline float cc_to_obxa_res01(uint8_t cc)
    {
        float r = cc_to_norm(cc);       // 0..1
        if (r > OBXA_RES_MAX) r = OBXA_RES_MAX;
        if (r < 0.0f) r = 0.0f;
        return r;
    }

    inline uint8_t obxa_res01_to_cc(float r)
    {
        if (r > OBXA_RES_MAX) r = OBXA_RES_MAX;
        if (r < 0.0f) r = 0.0f;
        // keep UI stable: map OBXA_RES_MAX back to 127 as well
        float n = (OBXA_RES_MAX > 0.0f) ? (r / OBXA_RES_MAX) : 0.0f;
        return norm_to_cc(n);
    }

    // OBXa multimode is 0..1 (selects pole outputs / blends)
    inline float cc_to_obxa_multimode(uint8_t cc) { return cc_to_norm(cc); }
    inline uint8_t obxa_multimode_to_cc(float m)  { return norm_to_cc(m); }

    // OBXa Xpander mode: 0..14
    inline uint8_t cc_to_obxa_xpander_mode(uint8_t cc)
    {
        // spread 0..127 across 0..14
        const uint16_t mode = (uint16_t)cc * (uint16_t)(OBXA_NUM_XPANDER_MODES) / 128u;
        return (mode >= OBXA_NUM_XPANDER_MODES) ? (OBXA_NUM_XPANDER_MODES - 1) : (uint8_t)mode;
    }

    inline uint8_t obxa_xpander_mode_to_cc(uint8_t mode)
    {
        if (mode >= OBXA_NUM_XPANDER_MODES) mode = OBXA_NUM_XPANDER_MODES - 1;
        // return midpoint CC for that bucket (stable round-trip)
        const uint16_t start = (uint16_t)mode * 128u / (uint16_t)OBXA_NUM_XPANDER_MODES;
        const uint16_t end   = (uint16_t)(mode + 1) * 128u / (uint16_t)OBXA_NUM_XPANDER_MODES;
        return (uint8_t)((start + end) / 2u);
    }

    // 2-pole feature toggles if you expose them via CC:
    // (use Bool_0_127 convention: 0=off, 127=on)
    inline bool cc_to_bool(uint8_t cc) { return cc >= 64; }
    inline uint8_t bool_to_cc(bool b)  { return b ? 127 : 0; }

    inline bool cc_to_obxa_two_pole(uint8_t cc)     { return cc_to_bool(cc); }
    inline bool cc_to_obxa_bpblend_2pole(uint8_t cc){ return cc_to_bool(cc); }
    inline bool cc_to_obxa_push_2pole(uint8_t cc)   { return cc_to_bool(cc); }

inline uint8_t time_ms_to_cc(float ms) {
    if (ms <= msMin) return 0;
    if (ms >= msMax) return 127;
    const float cc = 127.0f * logf(ms / msMin) / logf(msMax / msMin);
    return (uint8_t)constrain(lroundf(cc), 0, 127);
}


    inline float cc_to_lfo_hz(uint8_t cc) { return 0.03f * powf(1300.0f, cc_to_norm(cc)); }
    inline uint8_t lfo_hz_to_cc(float hz) {
        if (hz <= 0.03f) return 0;
        if (hz >= 0.03f*1300.0f) return 127;
        float n = logf(hz/0.03f)/logf(1300.0f);
        return norm_to_cc(n);
    }

    inline uint8_t ccFromLfoDest(int dest) {
        if (dest < 0) dest = 0;
        if (dest >= NUM_LFO_DESTS) dest = NUM_LFO_DESTS - 1;
        const uint16_t start = (dest    * 128u) / NUM_LFO_DESTS;
        const uint16_t end   = ((dest+1)* 128u) / NUM_LFO_DESTS;
        return (uint8_t)((start+end)/2);
    }
    inline int lfoDestFromCC(uint8_t cc) {
        uint8_t idx = (uint16_t(cc) * NUM_LFO_DESTS) / 128;
        if (idx >= NUM_LFO_DESTS) idx = NUM_LFO_DESTS - 1;
        return (int)idx;
    }

    inline uint8_t resonance_to_cc(float r) { return norm_to_cc(r); }
    inline float  cc_to_resonance(uint8_t cc){ return cc_to_norm(cc); }

// Generic linear 0..1 helpers, for mix, depths, etc.
    static inline uint8_t norm_to_cc_lin(float x) { return norm_to_cc(x); }
    static inline float  cc_to_norm_lin(uint8_t cc) { return cc_to_norm(cc); }

    // =================== MoogLinear Resonance (k) ===================
    // Extended 3-zone mapping for AudioFilterMoogLinear 'k' parameter.
    // Goal: precise control in 0..1.5, reach 1.5..4 “interesting” region,
    // and a short tail to extreme 4..20.
    //
    // Use these in your filter block instead of the linear 0..1 helpers above:
    //   float k = JT8000Map::cc_to_res_k(cc);
    //   uint8_t cc = JT8000Map::res_k_to_cc(k);

    // Zone bounds (in "k")
    static constexpr float RES_MIN_K  = 0.0f;
    static constexpr float RES_Z1_MAX = 1.5f;   // normal operating sweet spot
    static constexpr float RES_Z2_MAX = 4.0f;   // extended “interesting” region
    static constexpr float RES_MAX_K  = 20.0f;  // extreme tail

    // Zone weights over CC travel (must sum to 1.0)
    // Tune these to taste without refactoring engine/UI.
    static constexpr float RES_W1 = 0.75f;   // 75% of CC span in 0..1.5
    static constexpr float RES_W2 = 0.20f;   // 20% in 1.5..4
    static constexpr float RES_W3 = 0.05f;   // 5%  in 4..20

    // Curves within each zone (>1.0 biases more resolution to lower end of the zone)
    static constexpr float RES_CURVE_Z1 = 1.60f;  // more detail near 0..1.5
    static constexpr float RES_CURVE_Z2 = 1.20f;  // gentle skew 1.5..4
    static constexpr float RES_CURVE_Z3 = 2.20f;  // strong skew 4..20

    // Clamp for safety
    static inline float clamp_res_k(float k) {
        if (k < RES_MIN_K) return RES_MIN_K;
        if (k > RES_MAX_K) return RES_MAX_K;
        return k;
    }

    // Internal easing helpers (kept inline for speed)
    namespace _res_internal {
        inline float zone_map(float t, float a, float b, float curve) {
            if (t <= 0.0f) return a;
            if (t >= 1.0f) return b;
            float u = powf(t, curve);
            return a + (b - a) * u;
        }
        inline float zone_map_inv(float v, float a, float b, float curve) {
            if (v <= a) return 0.0f;
            if (v >= b) return 1.0f;
            float u = (v - a) / (b - a);
            if (curve <= 0.0f) return u; // defensive; we only use >1.0
            return powf(u, 1.0f / curve);
        }
    } // namespace _res_internal

    // CC (0..127) → k (0..20) with 3-zone response
    static inline float cc_to_res_k(uint8_t cc) {
        const float n = clamp01(cc / 127.0f);
        const float W1 = RES_W1;
        const float W2 = RES_W2;
        const float W3 = RES_W3; // = 1 - W1 - W2

        if (n <= W1) {
            // Zone 1: 0 .. 1.5
            const float t = (W1 > 0.0f) ? (n / W1) : 0.0f;
            return clamp_res_k(_res_internal::zone_map(t, RES_MIN_K, RES_Z1_MAX, RES_CURVE_Z1));
        } else if (n <= (W1 + W2)) {
            // Zone 2: 1.5 .. 4
            const float t = (W2 > 0.0f) ? ((n - W1) / W2) : 0.0f;
            return clamp_res_k(_res_internal::zone_map(t, RES_Z1_MAX, RES_Z2_MAX, RES_CURVE_Z2));
        } else {
            // Zone 3: 4 .. 20
            const float t = (W3 > 0.0f) ? ((n - W1 - W2) / W3) : 0.0f;
            return clamp_res_k(_res_internal::zone_map(t, RES_Z2_MAX, RES_MAX_K, RES_CURVE_Z3));
        }
    }

    // k (0..20) → CC (0..127), inverse of above for UI round-trip stability
    static inline uint8_t res_k_to_cc(float k) {
        k = clamp_res_k(k);

        const float W1 = RES_W1;
        const float W2 = RES_W2;
        const float W3 = RES_W3;

        float n = 0.0f; // normalized [0..1]

        if (k <= RES_Z1_MAX) {
            const float t = _res_internal::zone_map_inv(k, RES_MIN_K, RES_Z1_MAX, RES_CURVE_Z1);
            n = t * W1;
        } else if (k <= RES_Z2_MAX) {
            const float t = _res_internal::zone_map_inv(k, RES_Z1_MAX, RES_Z2_MAX, RES_CURVE_Z2);
            n = W1 + t * W2;
        } else {
            const float t = _res_internal::zone_map_inv(k, RES_Z2_MAX, RES_MAX_K, RES_CURVE_Z3);
            n = W1 + W2 + t * W3;
        }

        return (uint8_t)constrain(lroundf(n * 127.0f), 0, 127);
    }

} // namespace JT8000Map
