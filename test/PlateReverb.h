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
#include "core/dsp/IReverb.h"
#include "core/dsp/ReverbPrimitives.h"

namespace JT {

// -----------------------------------------------------------------------------
// PlateTank<kShimmer> — the Dattorro tank, with the octave-up shimmer stage
// compiled IN or OUT.
//
// WHY A TEMPLATE AND NOT A RUNTIME FLAG.  The shimmer stage sits INSIDE the
// per-sample tank loop, so a runtime bool would cost a branch per sample per
// channel, and holding the shifters behind a pointer would cost an indirect
// call where PitchShifter::process currently inlines.  `if constexpr` folds the
// stage away entirely: the Plate variant's machine code contains no trace of
// it.  The loop still lives in the .cpp — the two variants are explicitly
// instantiated at the bottom of that file — so the project's .h/.cpp split
// holds.  Cost is one extra copy of the tank loop in flash.
//
// WHY THE REVERB-TAIL PITCH SHIFTERS ARE GONE.  `_pitchL` / `_pitchR` were set
// to unity with mix 0 in begin() and never touched again: no setter, no
// parameter, no NRPN reached them.  They were 8192 floats (32 KB) of
// permanently unreachable PSRAM.  Removing them is bit-exact — PitchShifter::
// process returns its input untouched at mix 0.
// -----------------------------------------------------------------------------
template <bool kShimmer>
class PlateTank : public IReverb {
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
    // Forwarded from the primitive so the pool arithmetic below has one source.
    static constexpr uint32_t kPitchBufSize = Rv::PitchShifter::BUF_SIZE;  // 4096
    // Two shifters (L+R) on the Shimmer variant, none on the Plate.  Was FOUR
    // before the split: the other pair drove the unreachable reverb-tail pitch.
    static constexpr uint32_t kPitchBufTotal = kShimmer ? (2u * kPitchBufSize) : 0u;

    // Everything EXCEPT the input diffusers lives in the caller pool.  The
    // diffusers (905 floats) are a small DTCM member — fast, no PSRAM hops.
    // Plate   23418 floats =  91.5 KB  (was 39707 / 155.1 KB before the split)
    // Shimmer 31610 floats = 123.5 KB
    static constexpr uint32_t kPoolFloats =
        kPredelayMax +
        kTankApfLen0 + kTankApfLen1 +
        kTankDlyLen0 + kTankDlyLen1 +
        kPitchBufTotal;

    PlateTank() = default;

    // ---- IReverb ------------------------------------------------------------
    uint32_t    poolFloats() const override { return kPoolFloats; }
    const char* name()       const override { return kShimmer ? "shimmer" : "plate"; }
    void        clearTail() override;

    // Attach the caller-owned pool (kPoolFloats floats, e.g. EXTMEM on Teensy).
    // Zeroes the pool + diffuser buffer, carves the sub-regions, applies the v1
    // GlobalFX ctor one-shot defaults.  Must be called before processBlock.
    // A null pool leaves the reverb inert (processBlock returns immediately) —
    // legal but real builds must provide it.
    void begin(float* pool) override;

    // -------------------------------------------------------------------------
    // Parameter setters — mirror v1 GlobalFX/tank.  Every setter clamps 0..1
    // and forwards; identical mappings to v1 (docs spec §1.3).  Inputs are the
    // v2 float `norm` (full resolution), NOT a /127 CC byte (spec §1.1a).
    // -------------------------------------------------------------------------
    void setSize(float n)    override;   // room size / decay
    void setHiDamp(float n)  override;   // in-tank HF damping
    void setLoDamp(float n)  override;   // in-tank LF damping
    void setLowpass(float n) override;   // post-tank master LPF (wet only)
    void setHipass(float n)  override;   // post-tank master HPF (wet only)
    void setShimmer(float n) override;   // no-op on the Plate variant
    void setFreeze(bool on)  override;   // infinite hold

    // -------------------------------------------------------------------------
    // Process one stereo block IN PLACE.  100 % wet tank; the dry/wet blend
    // against the un-reverbed input is done HERE with `mix`:
    //     out = dry + mix * wet          (v1 topology: _wetLevel=1 inside,
    //                                      wet-amp gain = mix outside)
    // Caller guarantees this is only invoked when NOT bypassed (SynthCore holds
    // the manual/auto bypass decision — spec §3).
    // -------------------------------------------------------------------------
    void processBlock(float* left, float* right, size_t n,
                      float mixStart, float mixEnd) override;

private:
    // ---- inner-loop primitives -------------------------------------------
    // Moved to ReverbPrimitives.h so Room/Hall/Gated share one copy.  Aliased
    // here so the tank code below reads exactly as it did before the move.
    using DelayLine    = Rv::DelayLine;
    using Allpass      = Rv::Allpass;
    using OnePole_LP   = Rv::OnePole_LP;
    using OnePole_HP   = Rv::OnePole_HP;
    using PitchShifter = Rv::PitchShifter;


    // ============================ topology ===================================
    DelayLine    _predelay;
    Allpass      _inputDiffuser[4];
    float        _diffuserBuf[kIdiffTotal] = { 0.0f };   // DTCM member

    Allpass      _tankAPF[2];
    DelayLine    _tankDelay[2];
    OnePole_LP   _tankLPF[2];
    OnePole_HP   _tankHPF[2];

    // Shimmer shifters exist only on the Shimmer variant; the Plate variant
    // declares a zero-length array so no storage and no code is emitted.
    PitchShifter _pitchShim[kShimmer ? 2 : 0];

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

// Two concrete algorithms, both explicitly instantiated in PlateReverb.cpp.
// PlateReverb keeps its original name so every existing reference still reads
// naturally; ShimmerReverb is the split-out octave-up variant.
using PlateReverb   = PlateTank<false>;
using ShimmerReverb = PlateTank<true>;

} // namespace JT
