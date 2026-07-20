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
//   owns the shape rows — ported verbatim, including the WHY comments'
//   conclusions (knee compensation per topology).
//
// V1 SAFETY LESSONS CARRIED OVER (both verified in the v1 dispatch)
//   * Korg35 runs with VA_NL_SAT ALWAYS — the TSK positive-feedback loop
//     diverges with linear feedback at high res/cutoff.
//   * MoogDV cutoff is clamped to MoogDV4::maxCutoffHz(fs) (~5.8 kHz at 1x)
//     AFTER all shaping — the paper's A coefficient zeros inside the audio
//     band, so pushing past the ceiling silences/misbehaves, never brightens.
//
// OMITTED KNOWINGLY (flagged per the no-silent-change rule)
//   * Drive (v1 ×1..4 input gain): CC-only in v1, never a patch parameter,
//     and the v2 table has no filter.drive — ports as neutral 1.0.
//   * v1's SlewedValue smoothing of knob steps: v2's 14-bit NRPN plus
//     block-rate application already steps finely; a global per-param
//     smoother (the table's smooth_ms metadata) is a later engine feature.
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

    void noteOn(uint8_t midiNote);       // captured for key tracking (pivot A4)
    void reset();                        // clear ALL topology states

    // --- audio plane ---
    // setEnvLevel: this block's filter-envelope output (0..1), pushed by the
    // voice BEFORE process().  Sign/depth come from filter.env_amount, so the
    // level itself stays unipolar (matches the v1 EnvelopeBlock output).
    void setEnvLevel(float env01);
    // process: filter one block in place.
    void process(float* buf, size_t n);

#ifdef JT_TESTING
    // Test hooks: the effective (post-modulation, post-clamp) cutoff Hz and the
    // total modulation in octaves, both as of the last process() call.  Let the
    // suite assert the v1 mod math directly instead of inferring it from output
    // spectra.  Compiled out of firmware.
    float debugCutoffHz()   const { return _dbgCutoffHz; }
    float debugModOctaves() const { return _lastModOct; }
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
