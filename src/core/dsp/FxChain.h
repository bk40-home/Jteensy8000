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
//   mono in → Drive → Tone EQ → Modulation → Delay → Limiter → stereo out
//   where "mono in" is the summed voice bus (L+R)*0.5 (v1 JPFX has one mono
//   input), and the stereo out is blended back against the dry bus by the
//   caller-visible dry/jpfx mix (spec §1.4).
//
// WHAT CHANGED vs v1 (flagged deviations — CLAUDE.md rule 2, spec §8)
//   D-1  (superseded 2026-10-24) FX_DRIVE was a 3-way Select {OFF,Soft,Hard}.
//        It is now a continuous Amount 0..1 again, driving a redesigned stage
//        (anti-aliased asymmetric soft clip + voicing filters + auto makeup) —
//        see drive-fx/drive-design-proposal-2026-10-24.md and applyDrive().
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
//   applyDrive / applyTone / processModulation / processDelay are the
//   per-sample inner loop; they MUST inline into processBlock or every sample
//   pays a call.  Same rationale as PlateReverb.h / VAFilterCore.h.  The preset
//   tables (mod/delay) are `static const` here, DEFINED once in the .cpp so a
//   single translation unit carries them.
//
// CPU DISCIPLINE ("do not calculate if not required")
//   - SynthCore skips processBlock entirely when the chain is disengaged.
//   - Drive early-exits when Amount == 0.
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
#include "core/dsp/FastMath.h"   // fastSin01 — D4 wow/flutter, per-sample

namespace JT {

class FxChain {
public:
    // -------------------------------------------------------------------------
    // Buffer sizing (samples @ 44.1 kHz).  v1 lengths verbatim
    // (AudioEffectJPFX.cpp allocateDelayBuffers): delay sized for 1500 ms max,
    // mod for 50 ms max, each + a 2-sample interpolation guard.
    // -------------------------------------------------------------------------
    // kMaxDelayMs was v1's JPFX_MAX_DELAY_MS (1500) until the PSRAM arena
    // landed.  The old ceiling was a MEMORY limit, not a musical one: at 534 KB
    // the four buffers were most of what the pre-arena EXTMEM budget could
    // spare.  With the arena carving from one region, 10 s of stereo delay
    // costs 3.38 MB of an 8 MB chip and buys the long ambient/looping repeats
    // the JP-8000 never had.
    //
    // kMinDelayMs is NEW and is the bottom of the user-facing Log sweep in
    // params.yaml (fx.delay_time).  It is NOT a DSP clamp — kMinDelaySamp
    // still guards the read pointer — it exists so the table has a min > 0,
    // which Curves::toEngineering requires for a Log curve.
    static constexpr float    kMinDelayMs   = 10.0f;
    static constexpr float    kMaxDelayMs   = 10000.0f;
    static constexpr float    kMaxModMs     = 50.0f;        // v1 JPFX_MAX_MOD_MS
    static constexpr float    kMinDelaySamp = 1.0f;         // v1 JPFX_MIN_DELAY_SAMP
    static constexpr uint8_t  kNumModPresets   = 11;        // v1 JPFX_NUM_MOD_VARIATIONS
    // 3a/4a (JP capture gap analysis 2026-10-22): the twelve JP-8000 Multi-FX
    // modulation types, APPENDED after the v1 presets (fx.mod_effect option
    // indices 12..23 -> engine types 11..22).  Option indices are stored in
    // patches, so the v1 presets keep 0..10 untouched.
    static constexpr uint8_t  kNumJpModTypes   = 12;
    static constexpr uint8_t  kNumModTypes     = kNumModPresets + kNumJpModTypes;
    static constexpr uint8_t  kNumDelayPresets = 5;         // v1 JPFX_NUM_DELAY_VARIATIONS

    // Rate-derived (AudioConfig.h msToSamples): the duration is fixed, the
    // sample count follows kSampleRate.  +2 is the interpolation guard.
    // Unchanged at 44.1 kHz (441000+2=441002, 2205+2=2207); at 48 kHz these
    // become 480002 / 2402 so 10 s / 50 ms of buffer are still available.
    static constexpr uint32_t kDelayLen = msToSamples(kMaxDelayMs) + 2u;
    static constexpr uint32_t kModLen   = msToSamples(kMaxModMs)   + 2u;

    // ---- D4 tape engine ----------------------------------------------------
    // Rates are fixed, not exposed: wow and flutter are characteristics of the
    // transport, and two more knobs for them would be knobs nobody moves.  The
    // two rates are deliberately NOT harmonically related, so the combined
    // drift never settles into an audible repeating cycle.
    // ---- X2: delay-time slew -------------------------------------------------
    // The read pointer used to jump to its new distance the instant the block
    // boundary arrived.  A time change therefore stepped the buffer index by
    // thousands of samples between two consecutive output samples — a hard
    // discontinuity, i.e. a click, which with feedback engaged was then written
    // back into the line and recirculated on every repeat.
    //
    // Slewing the tap distance fixes it, and is also what a real transport
    // does: the motor ramps, so turning the time knob PITCH-BENDS the repeats.
    // That is the sound you want from a tape echo, so this is a feature on the
    // Tape engine and a click-fix on the Digital one.
    //
    // ~150 ms one-pole.  Applied to BOTH engines (signed off as option X2):
    // the Digital path is unchanged whenever the time is NOT moving, because
    // of kSlewSnap below.
    static constexpr float kTimeSlewMs   = 150.0f;

    // ...but a one-pole ALONE is not enough.  Its first step is proportional to
    // the distance, so a 900 -> 300 ms jump (26460 samples) starts out moving
    // the read pointer 4 samples per sample — a 400 % rate, which is a violent
    // pitch dive rather than a tape glide.  And its tail is asymptotic, so it
    // would take ~2.5 s to land exactly.
    //
    // Clamping the per-sample STEP fixes both ends: small changes still take
    // the smooth one-pole path, large ones become a constant-rate glide that
    // arrives in bounded time.  0.2 samples per sample is about 315 cents of
    // bend while moving — a strong but musical tape swoop — and crosses that
    // 26460-sample jump in roughly 3 s.
    //
    // THE TRADE IS REAL AND UNAVOIDABLE: a large time change cannot be both
    // fast and smooth, because the pointer has to physically travel the
    // distance.  Tape glides (slowly — a Space Echo does too); digital delays
    // crossfade instead.  This is the glide.  If the Digital engine ever wants
    // instant retiming, it needs a crossfade between two taps, not a faster
    // ramp — see the ledger.
    static constexpr float kTimeSlewMaxStep = 0.2f;   // samples per sample

    // A one-pole never exactly arrives, and on float32 it does not even get
    // close: at a tap distance of 13233 samples one ULP is ~0.001, so once the
    // remaining error drops to ~3 samples the one-pole's step (error * coeff)
    // falls below half an ULP and ROUNDS AWAY ENTIRELY.  The slew then stalls
    // permanently 3 samples off target — which would silently break every
    // downstream bit-identity claim and the render baseline for the rest of the
    // session.  The stall point scales with the tap distance, so it is worse on
    // long delays (~100 samples off at the 10 s end).
    //
    // A minimum step guarantees convergence from any distance at any magnitude,
    // and the "step would reach or pass the target" test below then lands on it
    // EXACTLY.  0.01 samples per sample closes the final few samples in a few
    // milliseconds, far too slow to be audible as anything.
    static constexpr float kSlewMinStep  = 0.01f;    // samples per sample

    // ---- W3: drift specified in CENTS, not milliseconds -----------------------
    // The first version set the modulation depth as a fraction of the delay
    // time and capped it in ms.  That was the wrong unit.  Pitch deviation from
    // a modulated delay is A * 2*pi*f, so a fixed ms depth produced WILDLY
    // different amounts of audible wobble depending on the delay time and the
    // rate: 5 cents at 200 ms but 40 cents at 1500 ms, from one knob position.
    // Measured against a Space Echo (30-60 cents peak) the short-delay end was
    // 3-5x too subtle, which is exactly what it sounded like.
    //
    // Now the knob means what you HEAR: peak pitch deviation in cents.  The
    // required delay amplitude is derived from it and the current rate
    //     A = dev / (2*pi*f),   dev = 2^(cents/1200) - 1
    // so the wobble stays the same whatever the delay time or rate.
    static constexpr float kWowMaxCents     = 60.0f;
    static constexpr float kFlutterMaxCents = 25.0f;

    // Wow rate is now a PARAMETER (fx.delay_wow_rate); this is its default and
    // the range the setter clamps to.  Flutter stays fixed — it is scrape, a
    // property of the transport, and a second rate knob nobody moves.
    static constexpr float kWowHzDefault = 0.7f;
    static constexpr float kWowHzMin     = 0.2f;
    static constexpr float kWowHzMax     = 3.0f;
    static constexpr float kFlutterHz    = 7.3f;

    // Real wow is not a sine.  A second component at an irrational-ish multiple
    // of the rate keeps the drift from ever repeating audibly; 0.73 was chosen
    // because it shares no small factor with 1.
    static constexpr float kWowRate2Ratio = 0.73f;
    static constexpr float kWowMix1       = 0.65f;   // primary component
    static constexpr float kWowMix2       = 0.35f;   // secondary

    // The right channel's drift runs a fraction of a cycle behind the left, so
    // the wobble MOVES across the image instead of pumping both sides together.
    // The first version applied one identical drift to both taps, which is why
    // it read as flat.
    static constexpr float kDriftPhaseR   = 0.27f;

    // Safety clamp: the drift may never exceed this fraction of the delay
    // itself, or a slow deep wow on a 50 ms slapback would modulate straight
    // through zero and fold the read pointer.
    static constexpr float kDriftMaxFrac  = 0.25f;

    // Caller pool: two delay + two mod buffers (stereo).  886418 floats ≈ 3.38 MB.
    // Carved from the PsramArena on Teensy, a heap vector on the host.
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

    // Drive Amount 0..1 (clamped).  0 = stage bypassed at zero cost; above 0
    // the stage runs and the Amount sets gain, asymmetry and voicing together.
    void setDriveAmount(float amount);

    // Tone EQ in JP panel steps: -64..+63, 0 = flat (out-of-range is clamped).
    // The step -> shelf law is the measured JP one (docs: tone-ctrl/
    // jp-tone-capture-analysis-2026-10-23.md); see computeTone().
    void setBassStep(int step);
    void setTrebleStep(int step);

    // Modulation: type -1 = OFF, 0..10 = v1 preset (v1 ModEffectType),
    // 11..22 = JP-8000 Multi-FX type (kJpModTypes order).  Out of range = OFF.
    void setModEffect(int v1Type);
    void setModMix(float mix);          // 0..1 (Level knob, square-law send — 2a)
    void setModRate(float rateHz);      // Hz, 0 = use preset rate
    void setModFeedback(float fb);      // 0..0.99, <0 = use preset feedback

    // Delay: type -1 = OFF, 0..4 = preset (v1 DelayEffectType).
    void setDelayEffect(int v1Type);
    void setDelayMix(float mix);        // 0..1 (no phase-invert on this path, D-6)
    void setDelayFeedback(float fb);    // 0..0.99, <0 = use preset feedback
    // Delay time in MILLISECONDS, always an explicit override (preserves the
    // active preset's L/R ratio so panning presets stay stereo).
    //
    // The old "0 = revert to the preset's time" sentinel was REMOVED when
    // fx.delay_time became a Log-curve parameter: a Log curve cannot emit 0
    // (Curves.cpp requires min > 0), so the sentinel had become unreachable
    // from the knob and would have silently mapped the bottom of the sweep to
    // kMinDelayMs anyway.  Presets still supply the L/R ratio and the feedback
    // default — only their delay TIME is now always overridden.
    void setDelayTime(float ms);

    // ---- D4 tape-engine setters ---------------------------------------------
    // All four are inert while the engine is Digital; they are still STORED so
    // that flipping to Tape and back does not lose the settings (the same
    // contract visible_when has on the panels — hidden, not reset).
    void setDelayEngine(int engine);    // 0 = Digital (default), 1 = Tape
    void setDelayTone(float hz);        // feedback-loop one-pole LP cutoff
    void setDelaySat(float norm);       // 0..1 drive into the feedback soft-clip
    void setDelayWow(float norm);       // 0..1 -> 0..kWowMaxCents of drift
    void setDelayWowRate(float hz);     // capstan rate, kWowHzMin..kWowHzMax
    void setDelayFlutter(float norm);   // 0..1 -> 0..kFlutterMaxCents

    // ---- Sequencer aux-lane mod inputs (Stage D) -------------------------
    // Block-rate modulation from the step sequencer's aux lane.  Both are
    // no-ops at their default (0), so an un-modulated chain is byte-identical.
    //
    // Drive mod: bipolar −1..+1 aux value, applied as a bass↔treble TILT around
    // the Tone EQ (±kTiltMaxDb at full depth).  Level-neutral colour, click-free
    // (shelf gains are one-pole smoothed).  Works regardless of drive mode — it
    // is a tone colour, not a saturation-amount change.  Name kept for the
    // SynthCore aux-Drive wiring.
    void setToneTiltMod(float bipolar);
    // Drive-amount mod: scales the saturator's INPUT gain by 2^(mod * 1.5),
    // i.e. about +/-9 dB into the shaper at full depth.  Inaudible while
    // fx.drive is 0, because applyDrive bypasses then — the aux lane cannot
    // conjure a drive stage that is not running.
    void setDriveAmountMod(float bipolar);
    // Delay-send mod: additive offset onto the FX_DELAY_MIX knob value, the sum
    // clamped 0..1 (Q19).  Only audible when a delay effect is selected.
    void setDelayMixMod(float offset);

    // Output blend against the dry bus (spec §1.4 / Q5).
    void setDryMix(float m);            // 0..1
    void setJpfxMix(float m);           // 0..1

    // Engaged-gate queries for SynthCore (D-3).  "Active" == this stage would
    // do audible work.  If none are active SynthCore skips processBlock.
    bool driveActive() const { return _driveAmount > 0.0f; }
    // The tone TILT colours the signal whatever the drive mode is, so it must
    // be able to engage the chain on its own — otherwise selecting the aux
    // Tone destination on an all-OFF chain does nothing at all.  Includes
    // _tiltCur so the chain stays engaged while the tilt smooths back to flat.
    bool toneTiltActive() const { return _tiltTarget != 0.0f || _tiltCur != 0.0f; }
    // Tone knobs off-centre must engage the chain on their own (2026-10-24
    // sign-off a): on the JP, Bass/Treble work whatever the FX are doing.
    // Uses the knob STEPS, not _toneActive — that flag is only refreshed by
    // computeTone() inside processBlock, which never runs while disengaged.
    bool toneActive() const { return _bassStep != 0 || _trebleStep != 0; }
    bool modActive()   const { return _modType >= 0; }
    bool delayActive() const { return _delayType >= 0; }

    // Process one stereo block IN PLACE.  Caller guarantees this is only invoked
    // when the chain is engaged (SynthCore holds that decision — spec §3).
    void processBlock(float* left, float* right, size_t n);

#ifdef JT_TESTING
    // Test-only probes for the mapping unit tests (spec §7).  Firmware-free.
    int   debugBassStep()   const { return _bassStep;   }
    int   debugTrebleStep() const { return _trebleStep; }
    // Effective plateau gains / one-pole coefficients after the last
    // recompute (base x tilt).  Used by the tone-law tests.
    float debugBassGain()   const { return _toneBassDelta + 1.0f; }
    float debugTrebleGain() const { return _toneTrebDelta + 1.0f; }
    float debugBassAlpha()  const { return _toneBassAlpha; }
    float debugTrebAlpha()  const { return _toneTrebAlpha; }
    bool  debugToneActive() const { return _toneActive; }
    float debugDriveAmount() const { return _driveAmount; }
    float debugDriveMakeup() const { return _driveMakeup; }
    int   debugModType()  const { return _modType;        }
    int   debugDelayType()const { return _delayType;      }
    float debugModRateHz()const { return _modRateOverride;}
    float debugDelayMs()  const { return _delayTimeOverrideL; }
    float debugModFb()    const { return _modFeedbackOverride; }
    float debugDelayFb()  const { return _delayFeedbackOverride; }
    // Samples of wet-mute still owed after a preset change.  Exposed so the
    // mute-clamp regression (10 s buffer -> 10 s dropout) has a test that
    // fails loudly rather than only being audible on the bench.
    uint32_t debugDelayMuteCounter() const { return _delayMuteCounter; }
    // D4: the single gate the per-sample tape work sits behind.  Exposed so a
    // test can prove the Digital path never enters it, and that a Tape patch
    // with every control at zero does not either.
    bool  debugTapeActive() const { return _tapeActive; }
    // W3: total wow drift amplitude in SAMPLES after the cents conversion and
    // the kDriftMaxFrac clamp.  The cents-based depth is only meaningful if
    // this is delay-time independent and scales as 1/rate, which is what the
    // W3 tests assert.
    float debugWowAmpSamples() const { return _wowSampAmp1 + _wowSampAmp2; }
    // X2: the tap distance actually in use, and the target it is slewing to.
    // Equality of these two is what makes a settled delay bit-identical.
    float debugTapSamples()    const { return _dSampLCur; }
    float debugTapTarget()     const { return _delaySampLCached; }
    float debugFbLpG()      const { return _fbLpG;      }
#endif

private:
    // ===================== inner-loop DSP helpers ============================
    // (header-inline on purpose — see the banner.)

    // ---- X2: one slew step for a delay tap ----
    // One-pole toward the target, rate-clamped at both ends: kTimeSlewMaxStep
    // stops a large jump from becoming a violent swoop, kSlewMinStep stops the
    // float32 stall described at its definition.  Landing is EXACT — once a
    // step would reach or pass the target, assign the target itself, which is
    // what keeps a settled delay bit-identical to the pre-slew engine.
    static inline float slewTap(float cur, float target, float coeff)
    {
        const float delta = target - cur;
        if (delta == 0.0f) return target;

        float step = delta * coeff;
        if (step >  kTimeSlewMaxStep) step =  kTimeSlewMaxStep;
        if (step < -kTimeSlewMaxStep) step = -kTimeSlewMaxStep;
        if (step >= 0.0f) { if (step <  kSlewMinStep) step =  kSlewMinStep; }
        else              { if (step > -kSlewMinStep) step = -kSlewMinStep; }

        // Would this step reach or overshoot?  Then we have arrived.
        if ((step >= 0.0f && step >= delta) || (step < 0.0f && step <= delta))
            return target;
        return cur + step;
    }

    // ---- D4: feedback-path soft clip ----
    // Deliberately NOT applyDrive() below: that one belongs to the JPFX
    // drive STAGE and carries its own gain staging, filters and DC blocker.  The
    // tape loop wants something cheaper and gentler that runs on every repeat.
    //
    // Cubic soft clip, x - x^3/3, saturating to +/-2/3 outside |x| <= 1.  No
    // transcendentals, no table, three multiplies.  Gain-compensated by
    // _satComp so the knob changes timbre and not level — otherwise every
    // adjustment would read as "louder = better".
    static inline float tapeSoftClip(float x, float drive, float comp)
    {
        const float y = x * drive;
        const float c = (y >  1.0f) ?  0.6666667f
                      : (y < -1.0f) ? -0.6666667f
                                    : y * (1.0f - y * y * (1.0f / 3.0f));
        return c * comp;
    }

    // ---- Drive (redesigned 2026-10-24, sign-off 1a-5a) ----
    // Continuous Amount 0..1, 0 = bypass at zero cost.  Mono, per sample:
    //   1. pre low-shelf cut   xp = x - depth*LP140(x)   (stops chords mudding)
    //   2. shaper input        u  = gain*xp + bias       (bias -> even harmonics)
    //   3. ADAA1 of f(u) = u/sqrt(1+u^2), whose antiderivative is
    //      F(u) = sqrt(1+u^2).  (F(u)-F(u1))/(u-u1) simplifies EXACTLY to
    //          y = (u + u1) / (F(u) + F(u1))
    //      so there is no divide by a tiny difference and no special case.
    //   4. minus f(bias)       removes the static offset of the biased curve
    //   5. post top-cut blend  y += postMix*(LP7k(y) - y)  (tames fizz)
    //   6. DC blocker AFTER the shaper (the old Hard mode had it before, so
    //      the clip's own DC was never removed)
    //   7. automatic makeup gain (loudness held roughly constant)
    // Gains, bias, depths and makeup are all block-rate (computeDrive() and
    // processBlock); per sample this is ~1 sqrt + 1 divide + a few MACs.
    inline float applyDrive(float x)
    {
        if (_driveAmount <= 0.0f) return x;            // OFF: bypass, zero cost

        // 1. Pre-shaper low shelf (TPT one-pole LP at a fixed corner).
        const float vp = (x - _drvPreS) * _drvPreG;
        const float lp = _drvPreS + vp;   _drvPreS = lp + vp;
        const float xp = x - _drivePreDepth * lp;

        // 2-4. Biased algebraic sigmoid, first-order ADAA (closed form above).
        const float u  = xp * _driveGainEff + _driveBias;
        const float Fu = sqrtf(1.0f + u * u);           // VSQRT on the M7 FPU
        float y = (u + _drvU1) / (Fu + _drvF1) - _driveBiasOut;
        _drvU1 = u;   _drvF1 = Fu;

        // 5. Post top-cut, blended in with Amount (transparent as Amount -> 0).
        const float vq = (y - _drvPostS) * _drvPostG;
        const float lq = _drvPostS + vq;  _drvPostS = lq + vq;
        y += _drivePostMix * (lq - y);

        // 6. DC blocker: out = y - y[n-1] + R*out[n-1].
        const float dc = y - _drvDcX1 + _drvDcR * _drvDcY1;
        _drvDcX1 = y;   _drvDcY1 = dc;

        return dc * _driveMakeup;                        // 7.
    }

    // ---- Tone EQ (JP-measured 1st-order shelves) ----
    // out = in + (Gb-1)*LP_b(in) + (Gt-1)*(in - LP_t(in))
    // 4 one-poles per sample as v1 (TPT form: +1 add each).  JP shape comes from
    // the block-rate coefficient rule in setToneCoeffs(): the LP corner moves
    // with the gain so the shelf always STARTS at 445 Hz (bass) / 951 Hz
    // (treble) and boost/cut are mirror images.  Both deltas 0 at flat →
    // exactly transparent, so we early-exit.
    inline void applyTone(float& sl, float& sr)
    {
        if (!_toneActive) return;
        // TPT (trapezoidal / Zavalishin) one-pole LPs: v = (x - s)*G,
        // lp = s + v, s = lp + v.  The zero at Nyquist makes HP = x - LP
        // reach its full plateau, which the treble shelf needs (a plain
        // one-pole leaves the treble ~2 dB short at 10 kHz).  The _tone*Lp*
        // members hold the integrator states.
        const float vbL = (sl - _toneBassLpL) * _toneBassAlpha;
        const float vbR = (sr - _toneBassLpR) * _toneBassAlpha;
        const float vtL = (sl - _toneTrebLpL) * _toneTrebAlpha;
        const float vtR = (sr - _toneTrebLpR) * _toneTrebAlpha;
        const float bassL = _toneBassLpL + vbL;   _toneBassLpL = bassL + vbL;
        const float bassR = _toneBassLpR + vbR;   _toneBassLpR = bassR + vbR;
        const float lpTL  = _toneTrebLpL + vtL;   _toneTrebLpL = lpTL  + vtL;
        const float lpTR  = _toneTrebLpR + vtR;   _toneTrebLpR = lpTR  + vtR;
        sl += _toneBassDelta * bassL + _toneTrebDelta * (sl - lpTL);
        sr += _toneBassDelta * bassR + _toneTrebDelta * (sr - lpTR);
    }

    // ---- Modulation (v1 AudioEffectJPFX.cpp:625-763) ----
    inline void processModulation(float inL, float inR, float& outL, float& outR);

    // ---- JP-8000 Multi-FX modulation (measured pair engine, 2026-10-23) ----
    // Structure measured from the JE-8086 noise capture
    // (chorus-fx/jp-noise-capture-analysis-2026-10-23.md), identical for all
    // twelve types:
    //   W = g * [ x(t - c - d(t)) - x(t - c + d(t)) ]  +  g3 * x(t - 2c)
    //   L = dry + m^2 * W        R = dry - m^2 * W     (MONO wet, R inverted)
    // d(t) = D * LFO (static / tri / sine / unipolar tri), then a per-type
    // wet high-pass.  Deep Phasing adds a feedback term (its resonance).  One mono wet path: 2-3 reads
    // per sample, about half the work of the per-side voices it replaces.
    inline void processModulationJp(float inL, float inR, float& outL, float& outR);
    void prepareJpMod();     // control-plane: type change only

    // ---- Delay (v1 AudioEffectJPFX.cpp:766-897) ----
    inline void processDelay(float inL, float inR, float& outL, float& outR);

    // D4: block-rate cache for the tape engine.  Called from prepareDelay with
    // the resolved tap distances, because the drift amplitude scales with them.
    void prepareTape(float dSampL, float dSampR);

    // Block-rate recompute helpers (only run when the matching dirty flag set).
    void computeDrive();                       // Amount -> gain/bias/voicing
    float computeDriveMakeup(float gain) const; // loudness match at `gain`
    void resetDriveState();                    // clear filter/ADAA state
    void computeTone();
    // Tone coefficients from EFFECTIVE plateau gains (base x tilt).  Block rate
    // only: called from computeTone() (dirty) and the tilt step (while active).
    void setToneCoeffs(float gBass, float gTreb);
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

    // -- Drive -- (Amount -> coefficients at block rate, see computeDrive())
    float _driveAmount   = 0.0f;   // 0..1, 0 = bypass
    float _driveGainBase = 1.0f;   // kDriveGain0 * 2^(Amount * kDriveOct)
    float _driveBias     = 0.0f;   // shaper-input offset (even harmonics)
    float _driveBiasOut  = 0.0f;   // f(bias), removed after the shaper
    float _drivePreDepth = 0.0f;   // pre low-shelf depth, 0..kDrivePreDepth
    float _drivePostMix  = 0.0f;   // post top-cut blend, 0..1
    float _driveMakeup   = 1.0f;   // output makeup gain
    float _driveMakeupAt = -1.0f;  // _driveGainEff the makeup was computed for
    bool  _driveDirty    = false;
    // Fixed coefficients (set in begin(); depend only on the sample rate).
    float _drvPreG = 0.0f, _drvPostG = 0.0f, _drvDcR = 0.0f;
    // Per-sample state.
    float _drvPreS = 0.0f, _drvPostS = 0.0f;   // TPT integrator states
    float _drvU1   = 0.0f, _drvF1    = 1.0f;   // ADAA previous u and F(u)
    float _drvDcX1 = 0.0f, _drvDcY1  = 0.0f;   // DC blocker history

    // Stage D aux-lane mods (block-rate).  Aux 'Drive' modulates a bass↔treble
    // TILT around the Tone EQ (level-neutral colour), NOT the saturator input
    // gain — modulating the pre-clip gain slammed the tanh/clipper and clicked.
    // _tiltTarget is the raw bipolar aux value (−1..+1); _tiltCur is the
    // block-rate smoothed tilt actually applied.  At full depth → ±kTiltMaxDb
    // (treble +, bass −, and vice-versa).  Default 0 → no effect, tone EQ
    // behaviour byte-identical.
    float _tiltTarget = 0.0f;     // -1..+1 (raw aux value)
    float _tiltCur    = 0.0f;     // smoothed, 0 = flat

    // Drive-amount mod, same smoothing idiom as the tilt.  _driveGainEff is
    // _driveGainBase folded with the smoothed mod ONCE PER BLOCK, so the
    // per-sample drive keeps a single gain multiply.
    float _driveModTarget = 0.0f; // -1..+1 (raw aux value)
    float _driveModCur    = 0.0f; // smoothed, 0 = no change
    float _driveGainEff   = 1.0f; // _driveGainBase * 2^(_driveModCur * 1.5)
    float _delayMixMod  = 0.0f;   // additive offset onto _delayMix (Q19)

    // -- Tone EQ -- (coeffs recomputed at block rate when gain/tilt changes)
    float _toneBassAlpha = 0.0f;   // TPT one-pole G, bass LP
    float _toneTrebAlpha = 0.0f;   // TPT one-pole G, treble LP (HP = in - LP)
    float _toneBassLpL = 0.0f, _toneBassLpR = 0.0f;
    float _toneTrebLpL = 0.0f, _toneTrebLpR = 0.0f;
    float _toneBassDelta = 0.0f, _toneTrebDelta = 0.0f;   // effective G - 1
    // Base plateau gains from the tone knobs (set by computeTone).  The tilt
    // step multiplies these each block, so the aux tilt never accumulates.
    float _toneBassGain = 1.0f, _toneTrebGain = 1.0f;
    int   _bassStep   = 0;         // JP panel step -64..+63
    int   _trebleStep = 0;
    bool  _toneActive = false;
    bool  _toneDirty  = false;

    // -- Modulation --
    int   _modType             = -1;    // -1=OFF, 0..10 v1 preset, 11..22 JP type
    float _modMix              = 0.5f;
    float _modWetGain          = 0.25f;   // 2a: _modMix^2, cached by setModMix
    float _modRateOverride     = -1.0f; // -1 = use preset
    float _modFeedbackOverride = -1.0f; // -1 = use preset
    float _lfoPhaseL = 0.0f, _lfoPhaseR = 0.5f;   // R offset for stereo width
    float _lfoIncL   = 0.0f, _lfoIncR   = 0.0f;

    // -- JP Multi-FX pair-engine cache (filled by prepareJpMod) --
    // Pre-scaled to samples / cycles so the inner loop is add, wrap, shape,
    // two multiply-adds, two or three interpolated reads.
    float   _jpPhase    = 0.0f;   // LFO phase, cycles 0..1
    float   _jpInc      = 0.0f;   // cycles per sample (0 when static)
    float   _jpCentre   = 0.0f;   // c, samples
    float   _jpDepth    = 0.0f;   // D, samples
    float   _jpStaticD  = 0.0f;   // d for static types, samples
    float   _jpG        = 0.0f;   // pair gain g
    float   _jpG3       = 0.0f;   // fixed-tap gain at 2c (0 = no third read)
    float   _jpFeedback = 0.0f;   // W fed back into the line (Deep Phasing)
    float   _jpLastW    = 0.0f;   // previous W, for the feedback write
    uint8_t _jpShape    = 0;      // JpLfoShape
    // Wet high-pass (the JE high-passes W per type: 70 Hz .. 1.5 kHz).
    // TPT one-pole: G computed once per type change, 2 MACs per stage per sample.
    float   _jpHpG      = 0.0f;
    float   _jpHpS[2]   = {};     // integrator state per stage
    uint8_t _jpHpPoles  = 0;

    // -- Delay --
    int   _delayType             = -1;   // -1=OFF, 0..4 (v1 DelayEffectType)
    float _delayMix              = 0.5f; // 0..1 (magnitude only, D-6)
    float _delayFeedbackOverride = -1.0f;
    float _delayTimeOverrideL    = -1.0f;
    float _delayTimeOverrideR    = -1.0f;
    uint32_t _delayMuteCounter   = 0;    // click-free preset transition

    // ---- D4 tape engine ------------------------------------------------------
    // CONTROL-PLANE values (written by the setters, read at block rate).
    int   _delayEngine    = 0;           // 0 Digital, 1 Tape
    float _delayToneHz    = 12000.0f;
    float _delaySatNorm   = 0.0f;
    float _delayWowNorm   = 0.0f;
    float _delayWowHz     = kWowHzDefault;
    float _delayFlutNorm  = 0.0f;

    // BLOCK-RATE CACHE (prepareDelay).  The per-sample loop reads only these,
    // so no transcendental or divide ever runs inside processDelay.
    bool  _tapeActive     = false;       // engine == Tape AND something to do
    float _fbLpG          = 1.0f;        // one-pole coefficient, 1 == bypass
    float _satDrive       = 1.0f;        // 1..4
    float _satComp        = 1.0f;        // 1/_satDrive, restores unity level
    // X2: the tap distance the read pointer is ACTUALLY using, slewed toward
    // _delaySampLCached / _delaySampRCached.  Drift is added on top of these.
    float _dSampLCur      = -1.0f;       // -1 == "not primed yet", snap on first use
    float _dSampRCur      = -1.0f;
    float _timeSlewCoeff  = 0.0f;        // block-rate cache

    float _wowSampAmp1    = 0.0f;        // drift amplitude in SAMPLES
    float _wowSampAmp2    = 0.0f;
    float _flutSampAmp    = 0.0f;
    float _wowInc1        = 0.0f;        // phase increment per sample (0..1)
    float _wowInc2        = 0.0f;
    float _flutInc        = 0.0f;

    // PER-SAMPLE STATE.
    float _fbLpStateL     = 0.0f;
    float _fbLpStateR     = 0.0f;
    float _wowPhase1      = 0.0f;        // 0..1
    float _wowPhase2      = 0.41f;       // offset so the two do not start together
    float _flutPhase      = 0.25f;

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
    // JP Multi-FX type description (measured pair engine, 2026-10-23).
    // kJpLfoUni = unipolar triangle, d = D * (1 + tri) / 2: the taps meet
    // (W = 0) once per cycle but never cross (Deep Phasing).
    enum JpLfoShape : uint8_t { kJpLfoStatic = 0, kJpLfoTri = 1, kJpLfoSine = 2, kJpLfoUni = 3 };
    struct JpModParams {
        float   centreMs;   // c: centre of the tap pair
        float   depthMs;    // D: LFO amplitude of d (static types: the fixed d)
        uint8_t shape;      // JpLfoShape
        float   rateHz;     // LFO rate (ignored when static)
        float   g;          // pair gain
        float   g3;         // gain of the fixed tap at 2c (0 = none)
        float   feedback;   // W fed back into the delay line (0 = none)
        float   hpHz;       // wet high-pass corner (measured per type)
        uint8_t hpPoles;    // 0..2 cascaded one-pole TPT stages
    };
    static const ModParams   kModPresets[kNumModPresets];
    static const JpModParams kJpModTypes[kNumJpModTypes];
    static const DelayParams kDelayPresets[kNumDelayPresets];
};

} // namespace JT
