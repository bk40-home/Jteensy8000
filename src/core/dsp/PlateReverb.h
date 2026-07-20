// =============================================================================
// PlateReverb.h — JT-8000 v2 stereo plate reverb (Dattorro topology)
// =============================================================================
//
// PROVENANCE
//   Ported verbatim (DSP-wise) from v1 `AudioEffectPlateReverbJT.*` + its thin
//   `GlobalFX` wrapper.  Original Dattorro plate + shimmer/pitch: Piotr Zapart
//   (hexefx, MIT).  JT-8000 extensions + tuning: Kris Bishop.  See
//   docs/PHASE5_REVERB_SPEC.md for the file:line diagnosis this port follows.
//
// WHAT CHANGED vs v1 (flagged deviations — CLAUDE.md rule 2)
//   D-4  int16<->float round-trip REMOVED.  v1 was a Teensy `AudioStream`
//        (int16 blocks) so it converted in/out; v2's bus is F32 end-to-end, so
//        we process float directly.  Strictly higher fidelity — NOT bit-exact
//        to v1's integer transport.  `kToFloat/kToInt16` are gone.
//   Mem  The ~155 KB of delay memory is CALLER-OWNED (a float pool passed to
//        begin()), not self-allocated with extmem_malloc.  The engine stays
//        Arduino-free (architecture rule); main.cpp puts the pool in PSRAM
//        (EXTMEM), the host harness on the heap.  Same buffers, same carve-up.
//   Node No separate AudioStream node / no external send+wet mixers.  The tank
//        is driven in place by SynthCore::renderBlock (see processBlock).  v1's
//        GlobalFX clamp/cache/auto-bypass logic lives in SynthCore::applyParam.
//
// WHY THE NESTED DSP STRUCTS ARE HEADER-INLINE (CLAUDE.md rule 4)
//   DelayLine/Allpass/OnePole/PitchShifter are the per-sample inner loop.  They
//   MUST inline into processBlock or every sample pays a call.  Same rationale
//   as VAFilterCore.h.  The big static tables (semitone ratios, fade window)
//   live in the .cpp so only one translation unit carries them.
//
// CPU DISCIPLINE ("do not calculate if not required")
//   - SynthCore skips processBlock entirely when bypassed (manual || mix<=1e-3).
//   - Pre-delay skipped when 0 samples (v2 default: always 0 — no CC for it).
//   - PitchShifter::process returns input with ZERO buffer touches when mix==0
//     or pitch==unity (shimmer + reverb-pitch off by default → no PSRAM work).
//   - Master LP/HP skipped per-block when their coeff is at bypass (<1e-3).
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>
#include <string.h>   // memset
#include <math.h>     // fabsf, sqrtf

#include "core/AudioConfig.h"

namespace JT {

class PlateReverb {
public:
    // -------------------------------------------------------------------------
    // Buffer sizing (samples @ 44.1 kHz).  Values are v1's exact lengths
    // (AudioEffectPlateReverbJT.cpp) — prime-ish, tuned for decorrelation.
    // -------------------------------------------------------------------------
    static constexpr uint32_t kPredelayMax  = 11025;  // 250 ms
    static constexpr uint32_t kIdiffLen0    =   142;
    static constexpr uint32_t kIdiffLen1    =   107;
    static constexpr uint32_t kIdiffLen2    =   379;
    static constexpr uint32_t kIdiffLen3    =   277;
    static constexpr uint32_t kIdiffTotal   = kIdiffLen0 + kIdiffLen1
                                            + kIdiffLen2 + kIdiffLen3; // 905
    static constexpr uint32_t kTankApfLen0  =  1800;
    static constexpr uint32_t kTankApfLen1  =  2656;
    static constexpr uint32_t kTankDlyLen0  =  3720;
    static constexpr uint32_t kTankDlyLen1  =  4217;
    static constexpr uint32_t kPitchBufBits = 12;
    static constexpr uint32_t kPitchBufSize = 1u << kPitchBufBits;      // 4096
    static constexpr uint32_t kPitchBufTotal = 4u * kPitchBufSize;      // 16384

    // Everything EXCEPT the input diffusers lives in the caller pool.  The
    // diffusers (905 floats) are a small DTCM member — fast, no PSRAM hops.
    static constexpr uint32_t kPoolFloats =
        kPredelayMax +
        kTankApfLen0 + kTankApfLen1 +
        kTankDlyLen0 + kTankDlyLen1 +
        kPitchBufTotal;   // 39707

    PlateReverb() = default;

    // Attach the caller-owned pool (kPoolFloats floats, e.g. EXTMEM on Teensy).
    // Zeroes the pool + diffuser buffer, carves the sub-regions, applies the v1
    // GlobalFX ctor one-shot defaults.  Must be called before processBlock.
    // A null pool leaves the reverb inert (processBlock returns immediately) —
    // legal but real builds must provide it.
    void begin(float* pool);

    // -------------------------------------------------------------------------
    // Parameter setters — mirror v1 GlobalFX/tank.  Every setter clamps 0..1
    // and forwards; identical mappings to v1 (docs spec §1.3).  Inputs are the
    // v2 float `norm` (full resolution), NOT a /127 CC byte (spec §1.1a).
    // -------------------------------------------------------------------------
    void setSize(float n);      // room size / decay
    void setHiDamp(float n);    // in-tank HF damping
    void setLoDamp(float n);    // in-tank LF damping
    void setLowpass(float n);   // post-tank master LPF (wet only)
    void setHipass(float n);    // post-tank master HPF (wet only)
    void setShimmer(float n);   // shimmer pitch-shift amount in tank loop
    void setFreeze(bool on);    // infinite hold (saves/restores tail params)

    // -------------------------------------------------------------------------
    // Process one stereo block IN PLACE.  100 % wet tank; the dry/wet blend
    // against the un-reverbed input is done HERE with `mix`:
    //     out = dry + mix * wet          (v1 topology: _wetLevel=1 inside,
    //                                      wet-amp gain = mix outside)
    // Caller guarantees this is only invoked when NOT bypassed (SynthCore holds
    // the manual/auto bypass decision — spec §3).
    // -------------------------------------------------------------------------
    void processBlock(float* left, float* right, size_t n, float mix);

private:
    // ======================= inner-loop DSP structs ==========================
    // (header-inline on purpose — see the file banner.)

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

        static constexpr uint32_t BUF_BITS  = kPitchBufBits;
        static constexpr uint32_t BUF_SIZE  = kPitchBufSize;
        static constexpr uint32_t BUF_MASK  = BUF_SIZE - 1u;
        static constexpr uint32_t FRAC_BITS = 32u - BUF_BITS;
        static constexpr uint32_t FRAC_MASK = (1u << FRAC_BITS) - 1u;
        static constexpr uint32_t DELTA_0   = 1u << FRAC_BITS;    // unity pitch
        static constexpr float    LP_COEFF  = 0.26f;             // ~6 kHz rolloff

        static const float kSemitoneRatios[37];  // -12..+24, in .cpp
        static const float kFadeTable[257];       // raised cosine, in .cpp

        void assign(float* p) { buf = p; clear(); }
        void setPitch(float ratio)     { readAdder = (uint32_t)((float)DELTA_0 * ratio); }
        void setPitchSemitones(int8_t st) {
            if (st < -12) st = -12; else if (st > 24) st = 24;
            setPitch(kSemitoneRatios[st + 12]);
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
            const float    xf0    = kFadeTable[tblIdx];
            const float    xf1    = kFadeTable[tblIdx + 1u];        // [256]=1.0 guard
            float          blend  = xf0 * (1.0f - fadeFrac) + xf1 * fadeFrac;
            if (fadeIdx > 0xFFu) blend = 1.0f - blend;

            float pitched = sMain * blend + sHalf * (1.0f - blend);
            lpState += LP_COEFF * (pitched - lpState);
            pitched = lpState;

            writeAddr = (writeAddr + 1u) & BUF_MASK;
            return pitched * mix + input * (1.0f - mix);
        }
    };

    // ============================ topology ===================================
    DelayLine    _predelay;
    Allpass      _inputDiffuser[4];
    float        _diffuserBuf[kIdiffTotal] = { 0.0f };   // DTCM member

    Allpass      _tankAPF[2];
    DelayLine    _tankDelay[2];
    OnePole_LP   _tankLPF[2];
    OnePole_HP   _tankHPF[2];

    PitchShifter _pitchL,     _pitchR;       // reverb-tail pitch (off by default)
    PitchShifter _pitchShimL, _pitchShimR;   // shimmer (off by default)

    OnePole_LP   _masterLPF[2];
    OnePole_HP   _masterHPF[2];

    // ============================ parameters =================================
    float    _decay        = 0.7f;
    float    _tank0fb      = 0.0f;   // filtered tank outputs carried between
    float    _tank1fb      = 0.0f;   //   samples for cross-feedback
    float    _hiDampCoeff  = 0.3f;
    float    _loDampCoeff  = 0.0f;
    uint32_t _predelaySamples = 0;
    float    _modDepth     = 8.0f;   // samples (fixed v1 default, no CC)
    float    _modRate      = 0.8f;   // Hz     (fixed v1 default, no CC)
    float    _modPhase     = 0.0f;
    float    _modPhaseInc  = 0.0f;
    bool     _frozen       = false;
    float    _diffusionCoeff = 0.65f;
    float    _masterLpCoeff  = 0.0f;
    float    _masterHpCoeff  = 0.0f;
    float    _shimmerMix     = 0.0f;
    float    _freezeBleedGain = 0.0f;

    // Freeze save/restore slots (spec §1.3 freeze).
    float _savedDecay = 0.7f, _savedHiDampCoeff = 0.3f,
          _savedLoDampCoeff = 0.0f, _savedShimmerMix = 0.0f;

    float* _pool = nullptr;

    void assignBuffers();
    void updateModRate() { _modPhaseInc = _modRate / kSampleRate; }

    // Triangle LFO — 4 mul + 1 compare, no sinf (v1).  Bipolar -1..+1.
    inline float triangleLFO() {
        _modPhase += _modPhaseInc;
        if (_modPhase >= 1.0f) _modPhase -= 1.0f;
        float t = _modPhase - 0.5f;
        if (t < 0.0f) t = -t;
        return 4.0f * t - 1.0f;
    }
};

} // namespace JT
