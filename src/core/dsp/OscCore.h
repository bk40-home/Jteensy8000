// =============================================================================
// OscCore.h — multi-waveform oscillator core for JT-8000 v2 (Pass 4, Step 1)
// =============================================================================
//
// ROLE
//   One phase-accumulator oscillator implementing every wave in the table's
//   'osc_wave' option set EXCEPT supersaw (its own class, own state).  Two
//   of these plus a SupersawOsc form an OscUnit inside OscSection; the enum
//   order below matches the generated option set EXACTLY — patches store
//   the option index, so this ordering is frozen alongside the table:
//     ["SIN","SAW","SQR","TRI","ARB","PLS","rSAW","S&H","vTRI",
//      "BLS","rBLS","BLSQ","BLP","SSAW","vSAW","BLvSAW","vTRI","BLvTRI",
//      "JvSAW","JvTRI","JTRIM","JFBK"]
//
// NAIVE vs BAND-LIMITED — BOTH ON PURPOSE
//   v1 offers aliased naive waves alongside polyBLEP versions; v1 patches
//   rely on the naive ones, so do not "fix" the naive modes.
//   CORRECTION (2026-10-22, JE-8086 measurement): the JP-8000's OSC1 SAW,
//   SQR and TRI are BAND-LIMITED (−42..−52 dB alias energy at C7, cleaner
//   than our polyBLEP's −34..−38 dB) — only its SUPER SAW aliases like
//   naive saws.  So the band-limited waves (BLS/BLSQ/BLP, JvSAW, JTRIM) are
//   the JP-faithful targets; the JP importer maps to them.
//
// SYNC AND FM HOOKS (v1 CrossModSync semantics, confirmed from source)
//   * Hard sync: the MASTER fills a per-sample wrap buffer (fraction of the
//     sample at which its phase wrapped, or <0 for none); the SLAVE resets
//     its phase at those positions.  In the v2 voice OSC2 is master, OSC1
//     slave — exactly v1's assignment.
//   * FM (cross-mod): an optional per-sample buffer of raw modulator output
//     scales the slave's phase increment by 2^(mod × octaves) via
//     FastMath::fastPow2 — the same exponential-FM path v1 routed through
//     the FM mixer at ±10 octaves full scale.
//   Both hooks are nullptr when unused and cost ONE branch per block.
//
// CPU DISCIPLINE
//   Waveform dispatch happens once per block (switch outside the loop).
//   setFrequency() dirty-checks.  Phase stepping (with its optional FM and
//   sync conditionals) is a forced-inline helper shared by every wave loop,
//   so the generated code stays tight without 14 copy-pasted loops.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "core/AudioConfig.h"
#include "core/dsp/WaveTableSet.h"

namespace JT {

// -----------------------------------------------------------------------------
// F3 — hard-sync anti-aliasing (compile-time option, DEFAULT OFF).
//   The JP-8000 is a digital synth and its hard sync ALIASES: the forced
//   phase reset injects a step discontinuity with unbounded harmonics.  We
//   keep that as the default because it is part of the instrument's actual
//   character (leave-aliased, per sign-off).  Define JT_SYNC_ANTIALIAS=1 at
//   build time (e.g. -DJT_SYNC_ANTIALIAS=1 in platformio.ini build_flags) to
//   apply a 2-sample polyBLEP correction at each reset instead — cleaner, at
//   the cost of a little maths in the phase-step hot path.
//   The flag is read in step()'s sync-reset branch; when 0 the correction
//   compiles to nothing.
// -----------------------------------------------------------------------------
#ifndef JT_SYNC_ANTIALIAS
#define JT_SYNC_ANTIALIAS 0
#endif

// -----------------------------------------------------------------------------
// vSHAPE normalisation (compile-time option, DEFAULT ON).
//   The JP-8000 SHAPE morphs (vSAW / vTRI, indices 14-17 below) change the
//   SPECTRUM, not the peak level: vSAW is a fixed +-1 pk-pk at every shape
//   (verified), and the vTRI reflect-fold is bounded to +-1 by construction.
//   So with JT_VSHAPE_NORMALISE=1 the two morphs already sit at the engine's
//   +-1 nominal like every other wave, and this flag currently costs nothing
//   at run time either way.  It exists as the A/B switch you asked for: define
//   -DJT_VSHAPE_NORMALISE=0 to DISABLE the (reserved) level-compensation path,
//   leaving the raw generator output so you can hear the authentic amplitude
//   behaviour if a future shape law introduces a level dip/swell.  The flag is
//   read in the vSAW/vTRI loops; when the maths is a no-op it compiles away.
// -----------------------------------------------------------------------------
#ifndef JT_VSHAPE_NORMALISE
#define JT_VSHAPE_NORMALISE 1
#endif

// Option indices of the generated 'osc_wave' set — see ordering note above.
enum class Wave : uint8_t {
    Sine        = 0,
    Saw         = 1,    // naive (aliased — intentional, see header note)
    Square      = 2,    // naive
    Triangle    = 3,
    Arb         = 4,    // AKWF wavetable (int16, linear interpolation)
    Pulse       = 5,    // naive, width from shape
    SawRev      = 6,    // naive reverse saw
    SampleHold  = 7,    // stepped noise at the oscillator rate
    TriVar      = 8,    // variable-skew triangle, skew from shape
    BlSaw       = 9,    // polyBLEP saw
    BlSawRev    = 10,   // polyBLEP reverse saw
    BlSquare    = 11,   // polyBLEP square
    BlPulse     = 12,   // polyBLEP pulse, width from shape
    Supersaw    = 13,   // handled by SupersawOsc — never reaches this class
    // --- JP-8000 OSC1 "SHAPE" morphs (append-only; indices frozen) ----------
    // Both mechanisms are inferred from the JP-8000 manual's SHAPE diagrams and
    // confirmed by spectral analysis (see OscCore.cpp for the derivation):
    //   vSAW  = saw morph: blend of saw + its octave, fundamental cancels to a
    //           thin/HPF-like tone at shape centre, strong fundamental at the
    //           extremes — exactly the manual's "either end = thick bass,
    //           centre = as though an HPF were applied".  Reuses _shape.
    //   vTRI  = triangle morph: amplitude gain (1..4) then reflect-fold, so the
    //           plain triangle grows extra lobes as shape rises — the manual's
    //           "more overtones, similar to a square wave with an LPF".
    VarSaw      = 14,   // naive saw morph (aliases — intentional pairing)
    BlVarSaw    = 15,   // polyBLEP saw morph
    VarTri      = 16,   // naive triangle fold
    BlVarTri    = 17,   // 2x-oversampled, base-triangle polyBLEP fold
    // --- MEASURED JP-8000 OSC1 SHAPE waves (append-only; indices frozen) ---
    // All three measured from calibrated JE-8086 (JP-8000 DSP emulation)
    // Ctrl1 ramps, 2026-10-22 (project doc osc-shapes/jp-osc1-capture-
    // analysis).  SHAPE: shape_dc -1 -> JP Ctrl1 min, +1 -> Ctrl1 max.  The
    // formula morphs above (vSAW/vTRI) stay untouched for existing patches.
    //   JvSAW  — CLOSED FORM since 2026-10-22 (sign-off item 2: replaces the
    //            hand-swept 17-frame table in place): saw + β·B3, see .cpp.
    //   JvTRI  — measured table, 33 frames × 7 levels, re-generated from the
    //            calibrated ramp (item 3; the 2026-10 table was not the JP
    //            triangle).  data/akwf/JpMorph/JpMorph_VTri.h.
    //   JTRIM  — JP "TRIANGLE MOD", closed form wrap(g·tri) (item 4).
    JpVarSaw    = 18,   // "JvSAW"
    JpVarTri    = 19,   // "JvTRI"
    JpTriMod    = 20,   // "JTRIM"
    // --- JP-8000 FEEDBACK OSC (append-only; 2026-10-23, option 1) ----------
    // Handled by JpFeedbackOsc (own state, like Supersaw) — never reaches this
    // class's loops; OscCore renders silence if it ever does.
    JpFeedback  = 21,   // "JFBK"
};

class OscCore {
public:
    // --- control plane (block boundaries) ---
    void setWave(Wave w)              { _wave = w; updatePulseDc(); }
    void setFrequency(float hz);      // dirty-checked
    // Shape 0..1: pulse width for Pulse/BlPulse, rise-fraction for TriVar.
    // Clamped to 0.05..0.95 so pulse never degenerates to DC.  The table
    // morph position keeps the FULL 0..1 range (clamped only to 0..1) so
    // the measured JP waves reach their end frames.
    void setShape(float s);
    // PWM law (mix.pwm_law, sign-off 5b 2026-10-22).  false = JtPwm, the v1
    // 5..95 % pulse clamp (default, byte-identical).  true = JpPwm, the
    // JP-8000 law measured on a JE-8086: the pulse-width floor is 50 %, so
    // width + LFO never passes through a square — the JP's PW knob narrows
    // one way only and its PWM flattens at 50 % instead of folding back.
    // Pulse/BlPulse only; the SHAPE morph position (_morph) is unaffected.
    // Re-applies the last shape so a law switch takes effect immediately.
    void setJpPwmClamp(bool on);
    // Arbitrary wavetable: any length ≥ 2 (AKWF uses 600), int16 samples.
    // Passing nullptr falls back to naive saw (matches v1's arbdata guard).
    // Wrapped internally as a 1-frame, 1-level WaveTableSet so ARB and the
    // JP morphs share ONE reader (byte-identical ARB output — see .cpp).
    void setArbTable(const int16_t* data, uint16_t length);
    // Morph set for the JpVarSaw/JpVarTri waves (static flash data from
    // WavetableLib).  nullptr = those waves fall back to naive saw.
    void setMorphSet(const WaveTableSet* set) { _morphSet = (set && set->valid()) ? set : nullptr; }

    void resetPhase(float phase01);   // caller-supplied randomisation
    // Read-only views for OscSection's JpXmod sync clock (2026-10-25): the
    // clock mirrors this core's unmodulated phase and increment exactly, so
    // with X-MOD at 0 the slave resets land where this core's wraps did.
    float increment() const { return _inc; }    // per-sample, pre-FM
    // DC the rendered output still carries: a Pulse/BlPulse at width w sits
    // at mean 2w-1; under JpPwm render() removes it (JP pulses are AC-coupled,
    // 2026-10-25), so this returns 0 there.  0 for every other wave.
    // Lets JpXmod strip the modulator DC with no per-sample work.
    float outputDc()  const { return _pulseDcRaw - _dcOut; }
    // Width the pulse is actually playing (after the law's clamp).
    float pulseWidth() const { return _pw; }
    float phase()     const { return _phase; }  // 0..1
    // Sample & Hold needs a noise source; seeding keeps voices decorrelated
    // yet the whole engine deterministic for tests and renders.
    void seedNoise(uint32_t seed)     { _rng = seed | 1u; }

    // --- audio plane ---
    // WRITES one block (replaces, no accumulate — OscSection owns mixing).
    //   fmBuf   : optional per-sample FM input.  nullptr = no FM.
    //             fmIsRatio == false (default): ±1-ish modulator samples;
    //               pitch scales by 2^(fmBuf[i] × fmOctaves)  (exponential).
    //             fmIsRatio == true: fmBuf[i] is the increment MULTIPLIER
    //               itself, any sign (fmOctaves ignored) — through-zero
    //               linear FM for JpXmod; a negative value runs the phase
    //               backwards.
    //   syncIn  : optional master wrap buffer (see fillsSyncOut); slave
    //             resets phase where syncIn[i] >= 0.  nullptr = free-run.
    //   syncOut : optional wrap buffer THIS osc fills as master.
    //             Values: fraction 0..1 of the sample where the wrap
    //             happened, or -1 when none.  nullptr = don't record.
    void render(float* out, size_t n,
                const float* fmBuf, float fmOctaves,
                const float* syncIn, float* syncOut,
                bool fmIsRatio = false);

private:
    // FM modes for the step()/render templates (see render()).
    static constexpr int kFmNone  = 0;   // no FM input
    static constexpr int kFmExp2  = 1;   // 2^(fm × octaves): JtXmod, pitch env
    static constexpr int kFmRatio = 2;   // fm is the signed increment ratio: JpXmod

    // Picks the sync-in/sync-out instantiation for one FM mode.
    template <int FmMode>
    void renderSyncVariant(float* out, size_t n, const float* fmBuf, float fmOctaves,
                           const float* syncIn, float* syncOut);

    // Shared per-sample phase step with the two optional behaviours.
    // Templated on the presence flags so each wave loop compiles to the
    // minimal variant actually needed (the switch below picks it) — zero
    // per-sample cost for features not in use.
    template <int FmMode, bool HasSyncIn, bool HasSyncOut>
    inline float step(size_t i, const float* fmBuf, float fmOctaves,
                      const float* syncIn, float* syncOut);

    // One rendering loop per waveform family; templated the same way and
    // instantiated from render()'s dispatch.
    template <int FmMode, bool HasSyncIn, bool HasSyncOut>
    void renderImpl(float* out, size_t n,
                    const float* fmBuf, float fmOctaves,
                    const float* syncIn, float* syncOut);

    // THE wavetable reader (ARB + JP morphs).  Picks the band-limited level
    // and the two morph frames ONCE per block, then runs a tight per-sample
    // loop.  Morph=false (1-frame sets, i.e. ARB) reads one frame only and is
    // arithmetically identical to the pre-merge ARB loop.
    template <int FmMode, bool HasSyncIn, bool HasSyncOut, bool Morph>
    void renderTable(float* out, size_t n,
                     const float* fmBuf, float fmOctaves,
                     const float* syncIn, float* syncOut,
                     const WaveTableSet& ts);

    float nextNoise();                // xorshift32, ±1.0

    Wave     _wave   = Wave::Saw;
    float    _phase  = 0.0f;          // 0..1
    float    _inc    = 0.0f;          // per-sample increment
    float    _freq   = -1.0f;         // dirty check
    float    _shape  = 0.5f;          // TriVar skew (always 5..95 %)
    float    _pw     = 0.5f;          // Pulse/BlPulse width (floor per law)
    float    _shapeIn = 0.5f;         // last raw setShape() value (law re-apply)
    float    _pwFloor = 0.05f;        // 0.05 JtPwm (v1) / 0.5 JpPwm
    // Pulse DC (2026-10-25, sign-off 2a).  _pulseDcRaw = 2·_pw - 1 for the
    // pulse waves, else 0.  _dcOut is the part render() subtracts: equal to
    // _pulseDcRaw under JpPwm (the JP pulse is DC-free), 0 under JtPwm — so
    // JtPwm output is byte-identical (the subtract loop never runs).
    float    _pulseDcRaw = 0.0f;
    float    _dcOut      = 0.0f;
    void     updatePulseDc();
    float    _shValue = 0.0f;         // current Sample & Hold level
#if JT_SYNC_ANTIALIAS
    // F3: sub-sample position of a pending sync-reset step (<0 = none).
    // Set in step(), consumed by the wave loop's polyBLEP correction.
    float    _blepFrac = -1.0f;
    // Residual BLEP energy carried into the NEXT sample (see syncStepBlep).
    float    _blepCarry = 0.0f;
    // Output value the slave held on the sample BEFORE a reset, so the loop
    // can measure the step height (post − pre) at the reset instant.
    float    _preResetOut = 0.0f;
#endif
    uint32_t _rng    = 0x9E3779B9u;

    // --- table reader state ---
    // ARB keeps just pointer + length (as v1); render() wraps them in a
    // 1-frame, 1-level WaveTableSet ON THE STACK each block, so an OscCore
    // copy can never hold a descriptor pointing into another object.
    const int16_t* _arbData = nullptr;
    uint16_t       _arbLen  = 0;
    // JP morph set (flash, owned by WavetableLib) and its morph position.
    const WaveTableSet* _morphSet = nullptr;
    float          _morph = 0.5f;           // 0..1, NOT pulse-clamped
};

} // namespace JT
