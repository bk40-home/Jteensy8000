// =============================================================================
// FilterSection.h — the per-voice filter of JT-8000 v2 (Pass 5)
// =============================================================================
//
// ROLE
//   One filter section per voice, dispatching over the table's filter
//   engines and types.  This step ports the complete VA ENGINE — all 17
//   'v_a_filter' types over the verbatim v1 cores (VAFilterCore.h /
//   MoogDVCore.h).  The OBXa engine (filter.mode / Xpander) is the next
//   step (5.2 delivered it): filter.mode decodes to the 2P/4P/Xpander flag
//   set exactly as v1's setFilterMode; the Xpander sub-mode comes from
//   obxa_xpander_mode.  NOTE (verified): v1's "Xpander" and "Xpander M"
//   options decode IDENTICALLY and both consume the sub-mode param — they
//   are aliases in v1's code, preserved as aliases here.
//
// PER-TYPE SHAPING = V1 KNOB FEEL (verified: v1 applyShape + kFilterShape)
//   v1 mapped the NORMALIZED cutoff/resonance knobs under a per-type row:
//   cutoff on an exponential curve between that type's [fcMin, fcMax], and
//   resonance through a per-type γ lift before the topology's k/R map.
//   FilterSection therefore consumes NORM values (the store's engineering
//   Hz remains canonical in patches, exactly as v1's stored CC was) and
//   owns the shape rows — see the CHANGE LOG below for the two rows of the
//   table that have since been re-tuned against the JtFilterTest demo.
//
// V1 SAFETY LESSONS CARRIED OVER (both verified in the v1 dispatch)
//   * Korg35 runs with VA_NL_SAT ALWAYS — the TSK positive-feedback loop
//     diverges with linear feedback at high res/cutoff.
//   * MoogDV cutoff is clamped to MoogDV4::maxCutoffHz(fs) (~5.8 kHz at 1x)
//     AFTER all shaping — the paper's A coefficient zeros inside the audio
//     band, so pushing past the ceiling silences/misbehaves, never brightens.
//
// PASS 6 — CUTOFF MODULATION (env + key tracking), now implemented here
//   Ported from v1's LIVE mod path (VoiceBlock + FilterBlock mod bus +
//   AudioFilter{VABank,OBXa}::update), NOT the filters' internal env/keytrack
//   fields — those are dead in v1 (VoiceBlock never drives setEnvValue/
//   setKeyTrack per block).  The live math, verified in source:
//       keyDC  = clamp( (log2(f/440) / octaveCtrl) · keyTrackAmt, -1, +1 )
//       envDC  = envAmount · envShape01            (env shape 0..1, sign in amt)
//       modOct = (keyDC + envDC) · octaveCtrl
//       cutoff = cutoffBase · 2^modOct             (then per-engine clamp)
//   octaveCtrl cancels in the key term (≈1 V/oct at amt=1, capped at
//   ±octaveCtrl octaves by the clamp) but directly scales env depth.  Applied
//   at BLOCK rate for both engines, matching v1's JT_OPT_*_BLOCKRATE_MOD path.
//
// PASS 8 — VELOCITY SENSITIVITY (now wired here for the two filter-path terms)
//   v1 computed both at NOTE-ON (VoiceBlock::noteOn) as static per-note DC:
//     * VELOCITY_FILTER_SENS: cutoffOct = sens·(vel−0.5)·3, cutoff = base·2^oct
//       — a bipolar ±1.5-octave cutoff shift, applied to base cutoff and hence
//       INDEPENDENT of octaveCtrl.  Ported as _velCutoffOct, added to modOct
//       OUTSIDE the octaveCtrl multiply so it acts even at octaveCtrl == 0.
//     * VELOCITY_ENV_SENS: envScale = (1−sens)+sens·vel, envAmount ×= envScale
//       — velocity scales filter-env DEPTH.  Ported as _velEnvScale, a factor on
//       envDC (default 1.0 = no-op, so a default patch is byte-unchanged).
//   The Voice owns the sens knobs and pushes these two derived scalars at
//   note-on (mirroring v1's VoiceBlock→_filter.setCutoff/setEnvModAmount).  Both
//   ride the existing "modOct != _lastModOct" gate: a new note whose velocity
//   differs shifts modOct → one recompute, then static — no per-block cost.
//
// =============================================================================
// CHANGE LOG — DEMO PARITY PASS (signed off 1c / 2d / 3b / 4c / 6a / 6b / 6c)
// =============================================================================
// Provenance: a side-by-side of this file against the JtFilterTest demo bank
// (AudioFilterVABank.{h,cpp}) found five behavioural gaps.  Four are closed
// here; the fifth (the JP / NLLadderNB modes, absent from this tree's
// VAFilterCore.h) was explicitly deferred — see DEFERRED, below.
//
// [1c] OUTPUT SATURATION — RESTORED.  The demo ran EVERY filter output through
//      saturate() → tanhf(), because AudioFilterVABank's _satType defaults to
//      SAT_TANH.  This tree only hard-clamped to ±1, so the VA topologies were
//      clinically clean below full scale and brick-walled above it — the single
//      biggest reason "the others" sounded thin next to the demo.  The Diode
//      alone was unaffected, and that is exactly why it already sounded right:
//      its derived passband compensation holds peak below 0.8 at K=16, so it
//      never reached the clamp and never needed the tanh knee.
//      We restore the coloration with va_tanh_fast (Padé [3/3], ~8 cycles)
//      rather than tanhf (~30): the curve tracks tanh to better than 0.5% for
//      |x| ≤ 2.5, which covers the whole audible range, at roughly a quarter of
//      the cost (≈0.5% of one core at 8 voices instead of ≈1.8%).
//      MANDATORY PAIRING: va_tanh_fast is UNBOUNDED — it reads exactly 1.0 at
//      x = 3 and then grows as x/9.  The ±1 clamp that already followed the
//      filter is therefore no longer merely defensive; it IS the top of the
//      curve.  The two together form a soft knee up to x = 3 and a hard ceiling
//      beyond, so the clamp must never be removed from the saturated path.
//      SCOPE — TWO DELIBERATE EXCLUSIONS, both flagged for review:
//        * OBXa: untouched.  Out of scope by instruction, and its core already
//          carries its own coloration — saturating again would double-shape it.
//        * SVF AP: untouched, on different grounds.  An all-pass is DEFINED by
//          |H| = 1 at every frequency; a saturator is a magnitude-shaping
//          device, so applying one does not colour an all-pass, it stops it
//          being one.  test_filter_section.cpp:125 asserts exactly this and
//          caught it — that assertion is correct and stays unmodified.
//        * Diode LP4: untouched.  Out of scope by the same instruction, and it
//          is the one topology already signed off as correct in THIS firmware,
//          i.e. correct WITHOUT a saturator.  It is also the case where the
//          saturator does measurable harm rather than nothing: peak sits near
//          0.8 by design, and va_tanh_fast(0.8) = 0.675, so the stage would cut
//          ~16% off the exact level DIODE_COMP_DC exists to hold up.
//          The demo DID saturate the diode (its SAT_TANH stage was
//          unconditional), so this is a knowing divergence from demo parity in
//          favour of the standing instruction.  Reverting to demo behaviour is
//          a one-line change at case 8 in the .cpp — no other edits.
//
// [2d] INPUT DRIVE — ADDED (setDrive), with output compensation.  The demo's
//      audible "drive" was NOT AudioFilterVABank::setDrive() — that method
//      exists but the sketch never calls it.  CC25 drove an AudioAmplifier
//      placed AHEAD of the filter (JtfilterTest.ino:121/136, gain 0.1..4.0).
//      Algebraically identical to scaling x on the way in, but it means drive
//      always fed the tanh output stage above; drive without [1c] would be a
//      clipping control, not a saturation control.
//      DIFFERENCE FROM THE DEMO (deliberate, signed off as option 2d): the demo
//      left the level to ride up with drive and expected you to pull the
//      monitor gain back by hand.  A voice inside a polyphonic mix cannot do
//      that, so the output is compensated by 1/√drive — enough to keep
//      perceived level roughly constant while still letting drive read as an
//      increase in weight.  Full compensation (1/drive) was rejected: it makes
//      drive sound like it does nothing until the knee is reached.
//      COST DISCIPLINE: drive == 1.0 (the default, and the value every existing
//      patch will load) sets _driveActive false and the two multiplies are not
//      merely skipped per sample — the whole branch is hoisted out of the block
//      loop, so an undriven voice is bit-identical to the pre-change code.
//      NOT YET A PATCH PARAMETER.  filter.drive does not exist in params.yaml,
//      so nothing calls setDrive() yet; this delivery only opens the seam.  See
//      NEXT DELIVERY at the bottom of this block.
//
// [3b] CUTOFF CEILINGS — RAISED on the rows that were audibly capped.  The demo
//      swept 30 Hz .. 19,462 Hz for EVERY type (its own FilterShape table is
//      dead code — grep finds no reference to kFilterShape outside its own two
//      files), so a fully-open knob there was genuinely open.  Here the same
//      knob topped out at 6 kHz on the high-passes and 10–12 kHz on the
//      ladders, which is most of the remaining "lacking" once [1c] is in.
//      Rows changed: SVF HP2, Moog LP4/LP2/BP2, Korg35 LP/HP.  Rows left alone:
//      Diode (out of scope by instruction, and 12 kHz is ample for a diode
//      ladder) and all four MoogDV rows (5.8 kHz is the model's own physical
//      ceiling, not a taste choice — see MoogDV4::maxCutoffHz).
//      STILL CAPPED, FLAGGED FOR A LATER PASS: SVF NOTCH (8 kHz) and TPT1 HP
//      (8 kHz).  Both were outside the signed-off list, so both are untouched.
//
// [4c] RESONANCE γ — SOFTENED to 0.65 on the ladder rows.  The demo applied NO
//      γ at all, so its knob fed mapResonance() directly.  Here γ = 0.30 turned
//      a half-travel knob into r = 0.81, i.e. Moog k = 3.21 against the demo's
//      1.98: resonance arrived far too early and the entire top half of the
//      knob was self-oscillation.  γ = 0.65 puts half travel at r = 0.64 (Moog
//      k = 2.53), keeping useful lift at the bottom without collapsing the top.
//      Rows changed: Moog ×3, Korg35 ×2, and — FLAGGED, as it sits outside the
//      list of topologies actually under complaint — MoogDV ×4, which shared
//      the identical γ = 0.30 and therefore the identical fault.  Veto that one
//      row set if MoogDV's current feel was intentional and it reverts alone.
//      Rows left alone: Diode (γ 0.45, out of scope) and SVF (γ 0.70, already
//      within a hair of 0.65 and not under complaint).
//
// [6a] The per-block output clamp used to be a SECOND full pass over the buffer
//      after the dispatch switch, and the OBXa path walked the block up to
//      THREE times (process, guard-zero, clamp).  Both are folded into a single
//      pass.  The OBXa fold is bit-identical: zero-then-clamp and
//      clamp-then-zero both yield zero, and the state guard is still evaluated
//      after the whole block, exactly as before.
//
// [6b] _diode.setCoeffs() ran unconditionally every block.  It depends only on
//      g (K is unused inside it), and g only moves when the base shape or the
//      modulation total moved — both of which process() already computes.  It
//      is now gated on that same condition.  Correctness of the gate: a type
//      switch sets _dirty, and reset() restores the _lastModOct sentinel, so
//      the first block after either always recomputes.
//
// [6c] Spelling standardised to en-GB ("analogue").  Audit note: the two files
//      in scope were already compliant; the only "Analog" left in the DSP tree
//      is MoogDVCore.h:27, which is the title of the D'Angelo–Välimäki paper
//      and is correct as a citation.  No change was needed.
//
// DEFERRED (option 5a — no action this delivery)
//   The demo carries four further types, FILTER_JP_LP24/LP12/HP24/BP at indices
//   13..16, backed by an NLLadderNB struct (Newton-Bisection implicit ladder)
//   that does not exist in this tree's VAFilterCore.h at all.  Note the index
//   collision: 13..16 mean MoogDV here and JP there.  If they are ever ported
//   they must be APPENDED at 17..20 — inserting at 13 would silently remap
//   every stored patch.  Logged in DEFERRALS_LEDGER.md.
//
// NEXT DELIVERY (cross-repo, not started here)
//   filter.drive → params.yaml, then simultaneous regen of ParamTable.h /
//   JtBridgeTable.h / ParamMap.md across all three repos, then the SynthCore
//   push that calls setDrive().  Recommended mapping: norm 0..1 → 1.0 + 3.0·n,
//   so the knob's zero position IS unity and every existing patch loads
//   byte-unchanged.  (The raw 0.1..4.0 span the demo's amplifier used would put
//   unity at an awkward norm of 0.231 and let the knob attenuate, which is a
//   headroom control, not a drive control.)
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "core/AudioConfig.h"
#include "core/dsp/VAFilterCore.h"
#include "core/dsp/MoogDVCore.h"
#include "core/dsp/OBXaCore.h"

namespace JT {

class FilterSection {
public:
    // Option indices of the generated sets — frozen with the table.
    enum class Engine : uint8_t { OBXa = 0, VA = 1 };

    // --- control plane (block boundaries) ---
    void setEngine(int option);          // REAL switch now: reset on change
    void setVaType(int option);          // 17 types; state reset on change (v1)
    // Cutoff carries both views: VA shapes the NORM under its per-type row
    // (v1 knob feel); OBXa consumes the Hz directly on the global curve
    // (v1 cc_to_obxa_cutoff_hz), clamped to its own 0.24·fs ceiling.
    void setCutoff(float norm01, float hz);
    void setResonanceNorm(float r01);    // OBXa additionally caps at 0.97

    // --- OBXa engine (option order == the generated 'filter_mode' set) ---
    void setObxaMode(int option);        // 4P / 2P / 2P BP / 2P Push / Xp / XpM
    void setObxaMultimode(float m01);    // pole morph (4P) / BP blend (2P BP)
    void setObxaXpanderMode(int option); // 0..14 into the pole-mix matrix

    // --- Pass 6: cutoff modulation (control plane, block boundaries) ---
    // Depths only; the per-block combination lives in the audio plane below.
    void setEnvAmount(float amt);        // FILTER_ENV_AMOUNT, bipolar -1..+1
    void setKeyTrackAmount(float amt);   // FILTER_KEY_TRACK, bipolar -1..+1
    void setOctaveControl(float octaves);// FILTER_OCTAVE_CONTROL, 0..10 octaves

    // --- Pass 8: velocity → filter (per-note DC, pushed by Voice at note-on) ---
    // Both derive from velocity in Voice (which owns the sens knobs); the filter
    // just consumes the results, exactly as v1's VoiceBlock drove the filter.
    void setVelCutoffOffsetOct(float oct);  // VELOCITY_FILTER_SENS result, octaves
    void setEnvVelScale(float scale);       // VELOCITY_ENV_SENS result, ×envDepth

    // --- Phase 3: LFO -> cutoff (block rate, pushed by SynthCore each block
    // before process()) --- Net LFO contribution, ALREADY summed across both
    // global LFOs and their FILTER depths (spec §4).  Added inside the
    // ×octaveCtrl term in computeModOctaves(), mirroring v1's mod-mixer path
    // — silent at octaveCtrl == 0 (v1-faithful, same as key/env).
    void setLfoCutoff(float x);

    // --- [2d] input drive (VA engine only) ---
    // Scales the signal INTO the topology, so it meets the saturator harder;
    // the output is compensated by 1/√drive so the voice does not simply get
    // louder as it gets dirtier.  1.0 is neutral AND free: the whole drive
    // branch is hoisted out of the block loop when inactive, so an undriven
    // voice costs exactly what it did before drive existed.
    //
    // Clamped to [kDriveMin, kDriveMax].  No _dirty flag: drive touches neither
    // the cutoff coefficient nor the resonance map, so setting it must NOT
    // trigger a tanf/powf recompute.
    void setDrive(float d);

    void noteOn(uint8_t midiNote);       // captured for key tracking (pivot A4)
    void reset();                        // clear ALL topology states

    // --- audio plane ---
    // setEnvLevel: this block's filter-envelope output (0..1), pushed by the
    // voice BEFORE process().  Sign/depth come from filter.env_amount, so the
    // level itself stays unipolar (matches the v1 EnvelopeBlock output).
    void setEnvLevel(float env01);
    // process: filter one block in place.
    void process(float* buf, size_t n);

    // Drive limits.  The lower bound is inherited from the demo's input
    // amplifier span (it can attenuate); the parameter mapping recommended in
    // the header's NEXT DELIVERY block never goes below 1.0, so the sub-unity
    // region exists only for bench work and future sound-design headroom.
    static constexpr float kDriveMin = 0.1f;
    static constexpr float kDriveMax = 4.0f;

    // --- bring-up introspection (hardware-visible, NOT test-gated) ---
    // Deliberately outside JT_TESTING, following debugClockBpm()'s precedent:
    // these answer "did the knob actually reach the DSP?" on a real board over
    // serial, which is the one question a host test cannot settle.  All four
    // are trivial accessors and cost nothing when nothing calls them.
    float debugDrive()       const { return _drive; }        // clamped user value
    bool  debugDriveActive() const { return _driveActive; }  // false ⇒ neutral path
    int   debugVaType()      const { return _vaType; }
    bool  debugEngineIsVa()  const { return _engine == Engine::VA; }

#ifdef JT_TESTING
    // Test hooks: the effective (post-modulation, post-clamp) cutoff Hz and the
    // total modulation in octaves, both as of the last process() call.  Let the
    // suite assert the v1 mod math directly instead of inferring it from output
    // spectra.  Compiled out of firmware.
    float debugCutoffHz()   const { return _dbgCutoffHz; }
    float debugModOctaves() const { return _lastModOct; }
    // [2d] lets the suite assert the compensation law without reaching into
    // private state or having to measure it from a rendered block.
    float debugDriveIn()    const { return _driveIn; }
    float debugDriveOut()   const { return _driveOut; }
#endif

private:
    // v1 kFilterShape row: per-type knob→Hz range and resonance γ.
    struct Shape { float fcMinHz, fcMaxHz, resGamma; };
    static const Shape kShape[17];

    float mapResonance(float rShaped, int type) const;
    void  updateBaseIfDirty();           // shape → base cutoff/res, on _dirty
    float computeModOctaves() const;     // (keyTrack + env) × octaveControl
    void  deriveCutoff(float modOct);    // fold mod into the engine coeffs

    Engine _engine   = Engine::OBXa;     // table default — the v1 boot engine
    int    _vaType   = 0;                // SVF LP2
    float  _cutNorm  = 1.0f;
    float  _cutHz    = 20000.0f;         // OBXa view of the same knob
    float  _resNorm  = 0.0f;
    bool   _dirty    = true;

    // Block-rate derived state (one tanf/powf set per actual change).
    float  _g        = 0.0f;             // TPT integrator gain
    float  _k        = 0.0f;             // topology-mapped resonance
    float  _fcHz     = 20000.0f;         // shaped cutoff (pre-DV clamp)

    uint8_t _note    = 60;               // keytrack input (pivot A4 = note 69)

    // --- Pass 6 cutoff modulation state ---
    float _envAmount   = 0.0f;           // FILTER_ENV_AMOUNT, -1..+1 (signed depth)
    float _keyTrackAmt = 0.0f;           // FILTER_KEY_TRACK,  -1..+1
    float _octaveCtrl  = 0.0f;           // FILTER_OCTAVE_CONTROL, octaves (norm×10)
    float _envLevel    = 0.0f;           // this block's filter-env output, 0..1
    // --- Pass 8 velocity → filter (per-note DC set at note-on) ---
    float _velCutoffOct = 0.0f;          // VELOCITY_FILTER_SENS: added to modOct
    float _velEnvScale  = 1.0f;          // VELOCITY_ENV_SENS: factor on envDC (1=off)
    // --- Phase 3 LFO -> cutoff (block rate, pushed each block by SynthCore) ---
    float _lfoCutoff   = 0.0f;           // net LFO term, inside the ×octaveCtrl sum

    // --- [2d] drive state, all derived once in setDrive() ---
    // _driveIn / _driveOut are kept pre-computed so the audio plane never runs
    // a sqrtf; _driveActive exists so the audio plane never even compares
    // floats per sample (the branch is hoisted to once per block).
    float _drive       = 1.0f;           // user value, clamped to [min,max]
    float _driveIn     = 1.0f;           // pre-filter gain  = _drive
    float _driveOut    = 1.0f;           // post-sat  gain   = 1/√_drive
    bool  _driveActive = false;          // false ⇒ neutral, zero per-sample cost

    // Cached total modulation (octaves) from the last derive.  A sentinel that
    // no real modOct can equal forces the first fold; thereafter the fold is
    // skipped whenever the modulation is static (env idle/held, knobs still).
    float _lastModOct  = 1.0e30f;
#ifdef JT_TESTING
    float _dbgCutoffHz = 0.0f;           // effective cutoff, last process()
#endif

    // One instance of every topology; only the ACTIVE one is processed, the
    // rest are cold state (~40 floats total — cheaper than any union dance).
    TPT1         _tpt1;
    SVF2         _svf;
    MoogLinear4  _moog;
    DiodeLadder4 _diode;
    Korg35LP     _k35lp;
    Korg35HP     _k35hp;
    MoogDV4      _moogdv;

    // --- OBXa engine state ---
    OBXaCore _obxa;
    float    _obxaG   = 0.0f;            // block-rate tanf (see OBXaCore hdr)
    float    _obxaLpc = 0.0f;
};

} // namespace JT
