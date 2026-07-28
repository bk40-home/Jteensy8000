// =============================================================================
// FxChain.h — JT-8000 v2 per-patch FX chain (JP-8000 "JPFX" engine)
// =============================================================================
//
// PROVENANCE
//   Ported DSP-for-DSP from v1 `AudioEffectJPFX.*` (wrapped by `FXChainBlock.*`).
//   Original JP-8000 FX modelling + tuning: Kris Bishop.  See
//   docs/PHASE6_FXCHAIN_SPEC.md for the file:line diagnosis this port follows.
//
// SIGNAL CHAIN (per sample, identical to v1 AudioEffectJPFX::update)
//   mono in → Saturation → Tone EQ → Modulation → Delay → Limiter → stereo out
//   where "mono in" is the summed voice bus (L+R)*0.5 (v1 JPFX has one mono
//   input), and the stereo out is blended back against the dry bus by the
//   caller-visible dry/jpfx mix (spec §1.4).
//
// WHAT CHANGED vs v1 (flagged deviations — CLAUDE.md rule 2, spec §8)
//   D-1  FX_DRIVE is a 3-way Select {OFF,Soft,Hard} (v2 ParamTable), not v1's
//        continuous 0..1 knob.  Soft/Hard resolve to fixed band-midpoint drives
//        (0.25 / 0.75) so each is representative of its v1 region.
//   D-3  v1's per-layer full-chain bypass has no v2 home (no layers).  Replaced
//        by SynthCore's engaged-gate: the whole processBlock is skipped unless a
//        stage is active.  This class exposes driveActive/modActive/delayActive
//        for that gate.
//   D-7  int16<->float round-trip REMOVED.  v1 was a Teensy AudioStream (int16
//        blocks); v2's bus is F32 end-to-end, so the limiter clamps in float
//        (±0.97) with no *32767 / (int16_t) conversion.  Higher fidelity, NOT
//        bit-exact to v1's integer transport.
//   Mem  Delay + mod circular buffers are CALLER-OWNED (a float pool passed to
//        begin()), not self-allocated with extmem_malloc.  Engine stays
//        Arduino-free; main.cpp puts the pool in PSRAM (EXTMEM), the host on the
//        heap.  Same buffer lengths, same carve-up.
//
// WHY THE INNER-LOOP HELPERS ARE HEADER-INLINE (CLAUDE.md rule 4)
//   applySaturation / applyTone / processModulation / processDelay are the
//   per-sample inner loop; they MUST inline into processBlock or every sample
//   pays a call.  Same rationale as PlateReverb.h / VAFilterCore.h.  The preset
//   tables (mod/delay) are `static const` here, DEFINED once in the .cpp so a
//   single translation unit carries them.
//
// CPU DISCIPLINE ("do not calculate if not required")
//   - SynthCore skips processBlock entirely when the chain is disengaged.
//   - Saturation early-exits when drive == OFF.
//   - Tone EQ early-exits when both gain deltas are ~0 (unity → transparent).
//   - Modulation early-exits when mod effect == OFF.
//   - Delay early-exits when delay effect == OFF.
//   - All ms/Hz/dB→coeff maths is block-rate or on-change, never per-sample.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>
#include <string.h>   // memset
#include <math.h>     // tanhf, fabsf, powf, ceilf

#include "core/AudioConfig.h"

namespace JT {

class FxChain {
public:
    // -------------------------------------------------------------------------
    // Buffer sizing (samples @ 44.1 kHz).  v1 lengths verbatim
    // (AudioEffectJPFX.cpp allocateDelayBuffers): delay sized for 1500 ms max,
    // mod for 50 ms max, each + a 2-sample interpolation guard.
    // -------------------------------------------------------------------------
    static constexpr float    kMaxDelayMs   = 1500.0f;      // v1 JPFX_MAX_DELAY_MS
    static constexpr float    kMaxModMs     = 50.0f;        // v1 JPFX_MAX_MOD_MS
    static constexpr float    kMinDelaySamp = 1.0f;         // v1 JPFX_MIN_DELAY_SAMP
    static constexpr uint8_t  kNumModPresets   = 11;        // v1 JPFX_NUM_MOD_VARIATIONS
    static constexpr uint8_t  kNumDelayPresets = 5;         // v1 JPFX_NUM_DELAY_VARIATIONS

    // ceilf is not constexpr; these are the exact integer results at 44.1 kHz:
    //   ceil(1500e-3 * 44100) + 2 = 66150 + 2 = 66152
    //   ceil(  50e-3 * 44100) + 2 =  2205 + 2 =  2207
    static constexpr uint32_t kDelayLen = 66152;
    static constexpr uint32_t kModLen   = 2207;

    // Caller pool: two delay + two mod buffers (stereo).  ~136718 floats ≈ 534 KB.
    static constexpr uint32_t kPoolFloats = 2u * kDelayLen + 2u * kModLen;

    FxChain() = default;

    // Attach the caller-owned pool (kPoolFloats floats, EXTMEM on Teensy).
    // Zeroes + carves the four circular buffers.  A null pool leaves the chain
    // inert (processBlock bails).  Must be called before processBlock.
    void begin(float* pool);

    // -------------------------------------------------------------------------
    // Parameter setters.  Clamp at the boundary (audio path sees only valid
    // values), set dirty flags for block-rate recompute.  Engineering units
    // (dB / Hz / ms / 0..1) match v1 exactly (spec §1.3); SynthCore does the
    // norm→engineering conversion in applyParam.
    // -------------------------------------------------------------------------

    // Drive: 0 = OFF, 1 = Soft (tanh), 2 = Hard (asymmetric clip).  (D-1)
    void setDriveMode(int mode);

    // Tone EQ: ±12 dB, 0 dB = flat.
    void setBassGain(float dB);
    void setTrebleGain(float dB);

    // Modulation: type -1 = OFF, 0..10 = preset (v1 ModEffectType).
    void setModEffect(int v1Type);
    void setModMix(float mix);          // 0..1 (Level knob)
    void setModRate(float rateHz);      // Hz, 0 = use preset rate
    void setModFeedback(float fb);      // 0..0.99, <0 = use preset feedback

    // Delay: type -1 = OFF, 0..4 = preset (v1 DelayEffectType).
    void setDelayEffect(int v1Type);
    void setDelayMix(float mix);        // 0..1 (no phase-invert on this path, D-6)
    void setDelayFeedback(float fb);    // 0..0.99, <0 = use preset feedback
    void setDelayTime(float ms);        // ms override, 0 = use preset (preserves L/R ratio)

    // ---- Sequencer aux-lane mod inputs (Stage D) -------------------------
    // Block-rate modulation from the step sequencer's aux lane.  Both are
    // no-ops at their default (0), so an un-modulated chain is byte-identical.
    //
    // Drive mod: bipolar −1..+1 aux value, applied as a bass↔treble TILT around
    // the Tone EQ (±kTiltMaxDb at full depth).  Level-neutral colour, click-free
    // (shelf gains are one-pole smoothed).  Works regardless of drive mode — it
    // is a tone colour, not a saturation-amount change.  Name kept for the
    // SynthCore aux-Drive wiring.
    void setDriveMod(float bipolar);
    // Delay-send mod: additive offset onto the FX_DELAY_MIX knob value, the sum
    // clamped 0..1 (Q19).  Only audible when a delay effect is selected.
    void setDelayMixMod(float offset);

    // Output blend against the dry bus (spec §1.4 / Q5).
    void setDryMix(float m);            // 0..1
    void setJpfxMix(float m);           // 0..1

    // Engaged-gate queries for SynthCore (D-3).  "Active" == this stage would
    // do audible work.  If none are active SynthCore skips processBlock.
    bool driveActive() const { return _driveMode != 0; }
    bool modActive()   const { return _modType >= 0; }
    bool delayActive() const { return _delayType >= 0; }

    // Process one stereo block IN PLACE.  Caller guarantees this is only invoked
    // when the chain is engaged (SynthCore holds that decision — spec §3).
    void processBlock(float* left, float* right, size_t n);

#ifdef JT_TESTING
    // Test-only probes for the mapping unit tests (spec §7).  Firmware-free.
    float debugBassDb()   const { return _targetBassDb;   }
    float debugTrebleDb() const { return _targetTrebleDb; }
    int   debugDriveMode()const { return _driveMode;      }
    int   debugModType()  const { return _modType;        }
    int   debugDelayType()const { return _delayType;      }
    float debugModRateHz()const { return _modRateOverride;}
    float debugDelayMs()  const { return _delayTimeOverrideL; }
    float debugModFb()    const { return _modFeedbackOverride; }
    float debugDelayFb()  const { return _delayFeedbackOverride; }
#endif

private:
    // ===================== inner-loop DSP helpers ============================
    // (header-inline on purpose — see the banner.)

    // ---- Saturation (v1 AudioEffectJPFX.cpp:437-457) ----
    // Early-exits when OFF.  Soft = tanh; Hard = asymmetric clip through a
    // DC-block HP.  Gain staging computed block-rate in computeSat().
    inline float applySaturation(float x)
    {
        if (_driveMode == 0) return x;                 // OFF: bypass, zero cost

        const float driven = x * _satInputGain;
        float shaped;
        if (_satIsSoft) {
            shaped = tanhf(driven);                    // FPU-accelerated on M7
        } else {
            // Asymmetric hard clip: +0.7 / -1.0, pre-emphasis HP removes the
            // DC offset the asymmetry introduces (v1 hpAlpha 0.9997).
            const float hpAlpha = 0.9997f;
            const float hpOut   = driven - _hpState;
            _hpState = _hpState * hpAlpha + driven * (1.0f - hpAlpha);
            if      (hpOut >  0.7f) shaped =  0.7f;
            else if (hpOut < -1.0f) shaped = -1.0f;
            else                    shaped =  hpOut;
        }
        return shaped * _satOutputGain;
    }

    // ---- Tone EQ (v1 AudioEffectJPFX.cpp:351-371) ----
    // Two-band crossover: bass = LP(200 Hz), treble = input - LP(3 kHz).
    // out = in + (bassGain-1)*bass + (trebleGain-1)*treble.  Both deltas 0 at
    // unity → exactly transparent, so we early-exit.
    inline void applyTone(float& sl, float& sr)
    {
        if (!_toneActive) return;
        _toneBassLpL += _toneBassAlpha * (sl - _toneBassLpL);
        _toneBassLpR += _toneBassAlpha * (sr - _toneBassLpR);
        _toneTrebLpL += _toneTrebAlpha * (sl - _toneTrebLpL);
        _toneTrebLpR += _toneTrebAlpha * (sr - _toneTrebLpR);
        const float trebL = sl - _toneTrebLpL;
        const float trebR = sr - _toneTrebLpR;
        sl += _toneBassDelta * _toneBassLpL + _toneTrebDelta * trebL;
        sr += _toneBassDelta * _toneBassLpR + _toneTrebDelta * trebR;
    }

    // ---- Modulation (v1 AudioEffectJPFX.cpp:625-763) ----
    inline void processModulation(float inL, float inR, float& outL, float& outR);

    // ---- Delay (v1 AudioEffectJPFX.cpp:766-897) ----
    inline void processDelay(float inL, float inR, float& outL, float& outR);

    // Block-rate recompute helpers (only run when the matching dirty flag set).
    void computeSat();
    void computeTone();
    void updateLfoIncrements();
    void prepareDelay();

    // ========================= parameter state ===============================

    // Attached pool (null until begin()).  Carved into four circular buffers.
    float*   _pool      = nullptr;
    float*   _delayBufL = nullptr;
    float*   _delayBufR = nullptr;
    float*   _modBufL   = nullptr;
    float*   _modBufR   = nullptr;
    uint32_t _delayWriteIdx = 0;
    uint32_t _modWriteIdx   = 0;

    // -- Saturation --
    int   _driveMode    = 0;        // 0=OFF 1=Soft 2=Hard (D-1)
    float _satInputGain = 1.0f;

    // Stage D aux-lane mods (block-rate).  Aux 'Drive' modulates a bass↔treble
    // TILT around the Tone EQ (level-neutral colour), NOT the saturator input
    // gain — modulating the pre-clip gain slammed the tanh/clipper and clicked.
    // _tiltTarget is the raw bipolar aux value (−1..+1); _tiltCur is the
    // block-rate smoothed tilt actually applied.  At full depth → ±kTiltMaxDb
    // (treble +, bass −, and vice-versa).  Default 0 → no effect, tone EQ
    // behaviour byte-identical.
    float _tiltTarget = 0.0f;     // −1..+1 (raw aux value)
    float _tiltCur    = 0.0f;     // smoothed, 0 = flat
    float _delayMixMod  = 0.0f;   // additive offset onto _delayMix (Q19)
    float _satOutputGain= 1.0f;
    bool  _satIsSoft    = true;
    bool  _satDirty     = false;
    float _hpState      = 0.0f;

    // -- Tone EQ -- (coeffs fixed at begin(); depends only on sample rate)
    float _toneBassAlpha = 0.0f;
    float _toneTrebAlpha = 0.0f;
    float _toneBassLpL = 0.0f, _toneBassLpR = 0.0f;
    float _toneTrebLpL = 0.0f, _toneTrebLpR = 0.0f;
    float _toneBassDelta = 0.0f, _toneTrebDelta = 0.0f;
    // Base deltas from the tone knobs (set by computeTone).  The effective
    // deltas above = base + tilt, recomputed every block so the aux tilt never
    // accumulates on itself.
    float _toneBassBase  = 0.0f, _toneTrebBase  = 0.0f;
    float _targetBassDb   = 0.0f;
    float _targetTrebleDb = 0.0f;
    bool  _toneActive = false;
    bool  _toneDirty  = false;

    // -- Modulation --
    int   _modType             = -1;    // -1=OFF, 0..10 (v1 ModEffectType)
    float _modMix              = 0.5f;
    float _modRateOverride     = -1.0f; // -1 = use preset
    float _modFeedbackOverride = -1.0f; // -1 = use preset
    float _lfoPhaseL = 0.0f, _lfoPhaseR = 0.5f;   // R offset for stereo width
    float _lfoIncL   = 0.0f, _lfoIncR   = 0.0f;

    // -- Delay --
    int   _delayType             = -1;   // -1=OFF, 0..4 (v1 DelayEffectType)
    float _delayMix              = 0.5f; // 0..1 (magnitude only, D-6)
    float _delayFeedbackOverride = -1.0f;
    float _delayTimeOverrideL    = -1.0f;
    float _delayTimeOverrideR    = -1.0f;
    uint32_t _delayMuteCounter   = 0;    // click-free preset transition

    // Delay block-constant cache (written by prepareDelay, read per-sample).
    float _delaySampLCached = 0.0f;
    float _delaySampRCached = 0.0f;
    float _delayFbCached    = 0.0f;
    float _delayWetCached   = 0.0f;
    float _delayDryCached   = 1.0f;

    // -- Output limiter (v1 AudioEffectJPFX.cpp:975-999) --
    float _limGain = 1.0f;
    static constexpr float kLimRelease = 0.029f;
    static constexpr float kLimCeiling = 0.97f;

    // -- Output blend (spec §1.4) --
    float _dryMix  = 1.0f;
    float _jpfxMix = 1.0f;

    // ---- Preset tables (defined once in the .cpp) ----
    struct ModParams {
        float baseDelayMsL, baseDelayMsR;
        float depthMsL, depthMsR;
        float rateHz, feedback, mix;
        uint8_t tapCount;
        bool useTriangleLfo;
    };
    struct DelayParams {
        float delayMsL, delayMsR, feedback, mix;
    };
    static const ModParams   kModPresets[kNumModPresets];
    static const DelayParams kDelayPresets[kNumDelayPresets];
};

} // namespace JT
