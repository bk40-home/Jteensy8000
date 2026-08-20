// =============================================================================
// FilterSection.cpp — implementation (port provenance in the header)
// =============================================================================
// The shape table, resonance maps and per-type dispatch are ported from v1
// AudioFilterVABank/FilterShape; only the packaging changed (int16 AudioStream
// -> in-place float block).  Where a number now differs from v1 it is because
// the demo-parity pass re-tuned it — every such row carries an inline [3b]/[4c]
// marker and the reasoning lives in the header's CHANGE LOG.
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "core/FilterSection.h"

#include <cmath>   // powf, exp2f, tanf, sqrtf — control rate only

namespace JT {

// -----------------------------------------------------------------------------
// kShape — one row per VA type, in option order.  The values encode each
// topology's audible range and resonance knee.
//
// Provenance of the ORIGINAL numbers (v1 FilterShape.cpp): Moog's k⁴ knee needs
// the strongest γ lift, Korg35's in-loop tanh compresses audible resonance
// early, MoogDV shares the Moog knee but its fcMax is pinned to the
// 1x-oversample ceiling.
//
// Provenance of the CHANGED numbers: the JtFilterTest demo bypassed this table
// entirely (its own copy is dead code) and swept 30 Hz..19.4 kHz with no γ at
// all.  Rows carrying a [3b] or [4c] marker below were re-tuned toward that
// behaviour.  Rows with no marker are v1-verbatim and deliberately so.
// -----------------------------------------------------------------------------
const FilterSection::Shape FilterSection::kShape[17] JT_FLASH_DATA = {
    // ── SVF (Zavalishin §4.1) ────────────────────────────────────────────────
    // R = 1/(2Q) already curves the perceived peak, so γ 0.70 is a mild lift and
    // stays as v1 had it (within a hair of the 0.65 the ladders moved to).
    /* SVF LP2    */ {   40.0f, 14000.0f, 0.70f },
    // [3b] 6000 → 16000.  A 2-pole HP whose corner stops at 6 kHz can never get
    // out of the way of the signal; 16 kHz empties the band as the knob opens,
    // which is what the demo did.  Not taken to 19 kHz: past ~16 kHz an HP
    // corner is inaudible and the last of the knob travel would do nothing.
    /* SVF HP2    */ {   30.0f, 16000.0f, 0.70f },
    /* SVF BP2    */ {   40.0f, 14000.0f, 0.70f },
    // FLAGGED, UNCHANGED: 8 kHz caps this notch well below the demo.  It sat
    // outside the signed-off row list, so it is left alone pending a decision.
    /* SVF NOTCH  */ {  100.0f,  8000.0f, 0.70f },
    // Note: 20 kHz exceeds the 0.45·fs (19,845 Hz) clamp in deriveCutoff, so the
    // top sliver of this row's travel is flat.  Pre-existing; harmless for an
    // all-pass, whose corner only moves phase.  Left as v1 had it.
    /* SVF AP     */ {   30.0f, 20000.0f, 0.70f },

    // ── Moog ladder (Zavalishin §5.1) ────────────────────────────────────────
    // [3b] 12000 → 18000 and [4c] γ 0.30 → 0.65 on all three taps.
    // 18 kHz is the highest musically useful corner below the 0.45·fs clamp;
    // it is what makes a fully-open ladder read as "open" rather than "veiled".
    // γ 0.65 puts half travel at k = 2.53 (was 3.21, demo was 1.98) — the knee
    // is reachable in the middle of the knob again instead of at the bottom.
    /* Moog LP4   */ {   40.0f, 18000.0f, 0.65f },
    /* Moog LP2   */ {   40.0f, 18000.0f, 0.65f },
    // BP2 is the pole-subtraction tap: narrower useful range, so its ceiling is
    // held two kHz lower than the LP taps and its floor stays at 80 Hz.
    /* Moog BP2   */ {   80.0f, 16000.0f, 0.65f },

    // ── Diode ladder (Pirkle AN-6) — OUT OF SCOPE, VERBATIM v1 ──────────────
    // Deliberately untouched by instruction, and independently correct: k spans
    // 0..17 so the linear map already spreads the audible range well (γ 0.45),
    // and 12 kHz is ample for a topology whose character lives in its midrange.
    /* Diode LP4  */ {   40.0f, 12000.0f, 0.45f },

    // ── Korg 35 / TSK (Zavalishin §5.8) ──────────────────────────────────────
    // [3b] LP 12000 → 18000, HP 6000 → 16000; [4c] γ 0.40 → 0.65.
    // The in-loop tanh still compresses audible resonance before the nominal
    // k = 2, which is why γ stays below the SVF rows rather than going to 1.0.
    // Stability re-checked at the new ceiling: at 18 kHz, G2 = g/(1+g)² ≈ 0.177,
    // so the solve's (1 − k·G2) ≈ 0.655 at k = 1.95 — comfortably positive.
    /* Korg35 LP  */ {   40.0f, 18000.0f, 0.65f },
    /* Korg35 HP  */ {   30.0f, 16000.0f, 0.65f },

    // ── TPT 1-pole (Zavalishin §3.1) ─────────────────────────────────────────
    // No resonance, so γ is inert (held at 1.0 for table uniformity).
    // FLAGGED, UNCHANGED: the HP row's 8 kHz ceiling is the same complaint as
    // SVF NOTCH above and is left alone for the same reason.
    /* TPT1 LP    */ {   50.0f, 18000.0f, 1.00f },
    /* TPT1 HP    */ {   30.0f,  8000.0f, 1.00f },

    // ── MoogDV (D'Angelo–Välimäki, ICASSP'13) ────────────────────────────────
    // [4c] γ 0.30 → 0.65 only.  fcMax is NOT a taste choice here: 5.8 kHz is
    // MoogDV4::maxCutoffHz at 1x, the point where the paper's A coefficient
    // peaks before zeroing inside the audio band.  Raising it would darken and
    // then break the filter, never brighten it, so [3b] does not apply.
    // FLAGGED: MoogDV was outside the list of topologies actually under
    // complaint, but it carried the identical γ = 0.30 and therefore the
    // identical fault.  These four rows revert independently if that feel was
    // intentional.
    /* MoogDV LP4 */ {   40.0f,  5800.0f, 0.65f },
    /* MoogDV LP2 */ {   40.0f,  5800.0f, 0.65f },
    /* MoogDV HP4 */ {   40.0f,  5800.0f, 0.65f },
    /* MoogDV BP  */ {   40.0f,  5800.0f, 0.65f },
};

// -----------------------------------------------------------------------------
// v1 mapResonance — normalized (post-γ) resonance to the topology's own k/R.
// UNCHANGED by the demo-parity pass: the demo used exactly these thresholds, so
// the two trees already agreed here.  The knob-feel difference lived entirely
// in the γ applied before this call (see [4c] in the table above).
//
// Thresholds per Zavalishin: SVF self-osc at R=0, Moog k=4, Diode k=17,
// Korg35 k=2; MoogDV's physical model SATURATES past k=4 rather than
// diverging, so it maps to the threshold itself.
// -----------------------------------------------------------------------------
float FilterSection::mapResonance(float r, int type) const
{
    switch (type) {
        case 0: case 1: case 2: case 3: case 4:      // SVF family: R = 1/(2Q)
            return 1.0f - r * 0.99f;                 // r=1 -> R=0.01, near osc
        case 5: case 6: case 7:                      // linear Moog ladder
            return r * 3.95f;                        // just below k=4
        case 8:                                      // diode ladder
            return r * 16.5f;                        // just below k=17
        case 9: case 10:                             // Korg35 (NL-bounded)
            return r * 1.95f;
        case 13: case 14: case 15: case 16:          // MoogDV
            return r * 4.0f;                         // AT the physical onset
        default:                                     // TPT1: no resonance
            return 0.0f;
    }
}

// -----------------------------------------------------------------------------
// Control plane
// -----------------------------------------------------------------------------
void FilterSection::setEngine(int option)
{
    const Engine e = (option == 1) ? Engine::VA : Engine::OBXa;
    if (e == _engine) return;
    _engine = e;
    _dirty  = true;      // the two engines derive different coefficients
    reset();             // v1 FilterBlock reset both engines on switch
}

void FilterSection::setVaType(int option)
{
    if (option < 0)  option = 0;
    if (option > 16) option = 16;
    if (option == _vaType) return;                   // v1: skip when unchanged
    _vaType = option;
    _dirty  = true;                                  // re-shape under new row
    reset();       // v1: glitch-free — the new topology starts from silence
}

void FilterSection::setCutoff(float c01, float hz)
{
    if (c01 < 0.0f) c01 = 0.0f;
    if (c01 > 1.0f) c01 = 1.0f;
    if (c01 == _cutNorm) return;
    _cutNorm = c01;
    _cutHz   = hz;
    _dirty   = true;
}

void FilterSection::setObxaMode(int option)
{
    // v1 SynthEngine::setFilterMode decode, verbatim: clear all flags,
    // set only the active mode's.  Options 4 ("Xpander") and 5
    // ("Xpander M") are aliases in v1 — see the header note.
    _obxa.useTwoPole   = false;
    _obxa.xpander4Pole = false;
    _obxa.bpBlend2Pole = false;
    _obxa.push2Pole    = false;
    switch (option) {
        case 1: _obxa.useTwoPole = true; break;                    // 2-Pole
        case 2: _obxa.useTwoPole = true; _obxa.bpBlend2Pole = true; break;
        case 3: _obxa.useTwoPole = true; _obxa.push2Pole    = true; break;
        case 4: case 5: _obxa.xpander4Pole = true; break;          // Xpander(s)
        default: break;                                            // 4-Pole
    }
}

void FilterSection::setObxaMultimode(float m01)
{
    _obxa.setMultimode(va_clamp(m01, 0.0f, 1.0f));
}

void FilterSection::setObxaXpanderMode(int option)
{
    if (option < 0)  option = 0;
    if (option > 14) option = 14;
    _obxa.xpanderMode = (uint8_t)option;
}

void FilterSection::setResonanceNorm(float r01)
{
    if (r01 < 0.0f) r01 = 0.0f;
    if (r01 > 1.0f) r01 = 1.0f;
    if (r01 == _resNorm) return;
    _resNorm = r01;
    _dirty   = true;
}

// --- Pass 6 modulation depths -------------------------------------------------
// None of these set _dirty: they change the per-block modulation total (modOct),
// which process() recomputes and compares every block, so a change is picked up
// on the next block without a base-shape recompute.  The clamps match the v1
// param ranges (env/keytrack bipolar -1..+1; octaveCtrl a non-negative octave
// count — the table's 0..1 knob is scaled ×10 in SynthCore before it arrives).
void FilterSection::setEnvAmount(float amt)
{
    _envAmount = va_clamp(amt, -1.0f, 1.0f);
}

void FilterSection::setKeyTrackAmount(float amt)
{
    _keyTrackAmt = va_clamp(amt, -1.0f, 1.0f);
}

void FilterSection::setOctaveControl(float octaves)
{
    _octaveCtrl = octaves < 0.0f ? 0.0f : octaves;
}

// Pass 8 velocity terms — per-note DC pushed by Voice at note-on.  No clamp on
// the octave offset (v1 didn't; it is bounded by the sens knob to ±1.5 octaves)
// and none on the env scale (v1's (1−s)+s·vel is inherently in 0..1).  Both
// change only across notes, so the block-rate modOct comparison folds them for
// free — no new dirty flag needed.
void FilterSection::setVelCutoffOffsetOct(float oct)
{
    _velCutoffOct = oct;
}

void FilterSection::setEnvVelScale(float scale)
{
    _velEnvScale = scale;
}

// Phase 3: no clamp — the sum lives in the SAME octave-space bus as key/env
// (computeModOctaves), so it is bounded there, not here, exactly like those
// two terms.
void FilterSection::setLfoCutoff(float x)
{
    _lfoCutoff = x;
}

// --- [2d] drive ---------------------------------------------------------------
// Derives BOTH gains here, at control rate, so the audio plane never touches a
// sqrtf.  Deliberately does NOT set _dirty: drive affects neither the cutoff
// coefficient (tanf) nor the resonance map (powf), and flagging it would force
// a needless recompute of both on every drive step — precisely the "do not
// calculate if not required" rule this file is built around.
//
// The early-out on an unchanged value matters more than it looks: a 14-bit NRPN
// knob sends a dense stream of identical values while it is being held, and the
// sqrtf is the only transcendental in the control path that has no dirty gate
// of its own.
void FilterSection::setDrive(float d)
{
    const float dv = va_clamp(d, kDriveMin, kDriveMax);
    if (dv == _drive) return;
    _drive = dv;

    // Exact float equality against 1.0f is intended: the parameter path either
    // delivers the literal neutral value (a default patch, or the zero position
    // of the recommended 1.0 + 3.0·n mapping) or it does not.  Anything that is
    // merely NEAR unity should still take the driven path, because it is the
    // 1/√drive compensation — not the multiply — that would otherwise go
    // silently missing.
    _driveActive = (dv != 1.0f);
    _driveIn     = dv;

    // 1/√drive: partial compensation.  Full 1/drive compensation was rejected
    // because it cancels the level change that makes drive audible below the
    // saturation knee, leaving a control that appears dead until it is nearly
    // maxed.  See [2d] in the header.
    _driveOut    = _driveActive ? (1.0f / sqrtf(dv)) : 1.0f;
}

void FilterSection::setEnvLevel(float env01)
{
    // EnvGen output is already 0..1; clamp defensively (a stolen-voice retrigger
    // could in principle hand us a transient out-of-range value).
    _envLevel = va_clamp(env01, 0.0f, 1.0f);
}

void FilterSection::noteOn(uint8_t midiNote)
{
    _note = midiNote;      // keytrack pivot is A4 (note 69), computed per block
}

void FilterSection::reset()
{
    _obxa.reset();
    _tpt1.reset();
    _svf.reset();
    _moog.reset();
    _diode.reset();
    _k35lp.reset();
    _k35hp.reset();
    _moogdv.reset();

    // Fresh modulation state for the new note: no smeared env level, and a
    // sentinel that forces the coefficient fold on the next process() block.
    // [6b] depends on this: the sentinel is what guarantees the diode's
    // setCoeffs() is re-run on the first block after any reset.
    _envLevel   = 0.0f;
    _lastModOct = 1.0e30f;

    // Drive is a control-plane setting, not per-note state, so it deliberately
    // survives reset() — a topology switch must not silently drop the user's
    // drive setting back to unity.
}

// -----------------------------------------------------------------------------
// Base coefficients — v1 applyShape, on the dirty flag ("do not calculate if
// not required": zero powf on blocks where no knob moved).  This derives only
// the mod-INDEPENDENT state (shaped base cutoff Hz, resonance).  The final
// cutoff→coefficient step folds in the per-block modulation (deriveCutoff).
// -----------------------------------------------------------------------------
void FilterSection::updateBaseIfDirty()
{
    if (!_dirty) return;
    _dirty = false;

    if (_engine == Engine::OBXa) {
        // Resonance + warp housekeeping (linear 0..1 with the 0.97 ceiling).
        // The cutoff tanf is mod-dependent, so it is derived in deriveCutoff.
        _obxa.setSampleRate(kSampleRate);      // cheap; keeps warp correct
        _obxa.setResonance(va_clamp(_resNorm, 0.0f, OBXaCore::kResMax));
        return;
    }

    const Shape& s = kShape[_vaType];

    // Cutoff: exponential (musical) knob curve across the type's own range.
    // This is the PRE-modulation base; deriveCutoff multiplies 2^modOct on top.
    _fcHz = s.fcMinHz * powf(s.fcMaxHz / s.fcMinHz, _cutNorm);

    // Resonance: per-type γ lift, then the topology map.
    const float rShaped = powf(_resNorm, s.resGamma);
    _k = mapResonance(rShaped, _vaType);
}

// -----------------------------------------------------------------------------
// Per-block modulation total, in octaves (v1 live mod-bus math — see header).
//   keyDC  = clamp( (log2(f/440)/octaveCtrl) · keyTrackAmt, -1, +1 )
//   envDC  = envAmount · envLevel · velEnvScale        (Pass 8: velocity depth)
//   modOct = (keyDC + envDC + lfoCut) · octaveCtrl + velCutoffOct
// log2(f/440) with A4 = MIDI 69 is exactly (note - 69)/12 — no powf needed.
// octaveCtrl == 0 ⇒ no key/env/LFO modulation (and the key term's divide is
// guarded), but the velocity cutoff offset is added OUTSIDE that multiply:
// v1 applied it to base cutoff directly (base·2^offset), independent of
// octaveCtrl, so it must survive octaveCtrl == 0.
// Phase 3: the LFO -> cutoff term (_lfoCutoff) joins the sum INSIDE the
// ×octaveCtrl multiply, mirroring v1's mod-mixer -> cutoffModOctaves path
// (docs/PHASE3_LFO_SPEC.md Decision #4) — same bus as key/env, same silence
// at octaveCtrl == 0.
// -----------------------------------------------------------------------------
float FilterSection::computeModOctaves() const
{
    float keyDC = 0.0f;
    if (_octaveCtrl > 0.0f) {
        const float keyOct = ((float)_note - 69.0f) * (1.0f / 12.0f);
        keyDC = va_clamp((keyOct / _octaveCtrl) * _keyTrackAmt, -1.0f, 1.0f);
    }
    const float envDC = _envAmount * _envLevel * _velEnvScale;
    return (keyDC + envDC + _lfoCutoff) * _octaveCtrl + _velCutoffOct;
}

// -----------------------------------------------------------------------------
// Fold the modulation into the active engine's cutoff coefficient.  One exp2f
// per call: libm, because FastMath::fastPow2 is documented AUDIO-rate only and
// this runs at block rate (v1 used a per-sample fast_pow2; block-rate folding
// makes the exact exp2f both affordable and more accurate — a flagged, benign
// improvement).  Clamp order matches v1: modulate first, clamp after.
// -----------------------------------------------------------------------------
void FilterSection::deriveCutoff(float modOct)
{
    const float modMul = exp2f(modOct);

    if (_engine == Engine::OBXa) {
        const float fc = va_clamp(_cutHz * modMul, 5.0f, 0.24f * kSampleRate);
        _obxaG   = tanf(fc * (1.0f / kSampleRate) * OBXaCore::kPi);
        _obxaLpc = _obxaG / (1.0f + _obxaG);
#ifdef JT_TESTING
        _dbgCutoffHz = fc;
#endif
        return;
    }

    // Integrator gain for the TPT family.
    const float fcClamped = va_clamp(_fcHz * modMul, 5.0f, 0.45f * kSampleRate);
    _g = va_compute_g(fcClamped, kSampleRate);

    if (_vaType >= 13) {
        // MoogDV backstop (v1): clamp the MODULATED Hz to the model's own
        // ceiling — the paper's A coefficient zeros inside the audio band, so
        // exceeding it never brightens, it breaks.
        const float fcDV = va_clamp(_fcHz * modMul, 5.0f,
                                    MoogDV4::maxCutoffHz(kSampleRate));
        _moogdv.setCutoff(fcDV, kSampleRate);
    }
#ifdef JT_TESTING
    _dbgCutoffHz = fcClamped;
#endif
}

// -----------------------------------------------------------------------------
// Audio plane — v1's hoisted-dispatch loop: the type switch is taken once
// per block; each case is a tight loop over one struct so its state lives
// in registers across the block.
//
// [6a] The per-sample tail (saturate → optional drive compensation → clamp →
// store) is factored into the runBlock lambda below, so it happens in the SAME
// pass as the filter evaluation.  The previous shape of this function walked
// the buffer a second time purely to clamp, and the OBXa path walked it up to
// three times.
//
// Why a generic lambda rather than a virtual call or a function pointer: each
// runBlock(...) call site instantiates its own copy with the topology inlined
// straight into the loop body, so the compiler still keeps that topology's
// state in registers across the block — identical codegen to the hand-written
// loops it replaces, without seventeen near-duplicate copies of the tail.
// -----------------------------------------------------------------------------
void FilterSection::process(float* buf, size_t n)
{
    // Base shape (mod-independent) recomputes only when a knob moved.
    const bool baseChanged = _dirty;
    updateBaseIfDirty();

    // Cutoff fold: recompute the engine coefficient only when the base changed
    // OR the modulation total actually moved this block.  A held/idle envelope
    // with static knobs yields a constant modOct → the fold (one exp2f + tanf)
    // is skipped, preserving the "do not calculate if not required" discipline.
    const float modOct     = computeModOctaves();
    const bool  modChanged = (modOct != _lastModOct);
    if (baseChanged || modChanged) {
        deriveCutoff(modOct);
        _lastModOct = modOct;
    }

    // [6b] The one gate the diode's block-rate coefficients need.  setCoeffs()
    // is a pure function of g (K is unused inside it), and g moved iff one of
    // these two did.  A type switch sets _dirty and reset() restores the
    // _lastModOct sentinel, so the first block on this topology always recomputes
    // even if nothing else moved.
    const bool coeffChanged = baseChanged || modChanged;

    if (_engine == Engine::OBXa) {
        // OUT OF SCOPE BY INSTRUCTION — math untouched.  The only change is
        // [6a]: the ±1 clamp is folded into the same pass as the filter call.
        // The 2P/4P branch stays hoisted out of the loop (mode is
        // block-constant) and the core's per-sample work is fully inlined.
        //
        // Note the deliberate ABSENCE of the [1c] saturator here: the VA
        // topologies needed it because they had no coloration of their own,
        // whereas OBXaCore carries its own and must not be double-shaped.
        if (_obxa.useTwoPole) {
            for (size_t i = 0; i < n; ++i)
                buf[i] = va_clamp(_obxa.process2Pole(buf[i], _obxaG), -1.0f, 1.0f);
        } else {
            for (size_t i = 0; i < n; ++i)
                buf[i] = va_clamp(_obxa.process4Pole(buf[i], _obxaG, _obxaLpc),
                                  -1.0f, 1.0f);
        }
        // v1 OBXA_STATE_GUARD: self-heal a poisoned IIR (4 compares).  Still
        // evaluated AFTER the whole block, exactly as before; zeroing after the
        // clamp is bit-identical to clamping after the zero.
        if (_obxa.stateGuard())
            for (size_t i = 0; i < n; ++i) buf[i] = 0.0f;
        return;
    }

    const float g = _g;
    const float k = _k;

    // [2d] Hoisted once per block.  When drive is neutral the else-branch below
    // is byte-for-byte the pre-drive code path.
    const bool  drv  = _driveActive;
    const float dIn  = _driveIn;
    const float dOut = _driveOut;

    // Common per-sample tail for every VA topology.
    //   in  → [×drive] → topology → va_tanh_fast → [×1/√drive] → clamp → out
    //
    // [1c] va_tanh_fast is the Padé [3/3] rational.  It tracks tanh to better
    // than 0.5% across the audible range and reaches EXACTLY 1.0 at x = 3,
    // beyond which it climbs again (asymptote x/9).  The clamp immediately
    // after is therefore load-bearing, not defensive: the pair forms a soft
    // knee up to x = 3 and a hard ceiling past it.  Never remove one without
    // the other.
    auto runBlock = [&](auto evalFilter)
    {
        if (drv) {
            for (size_t i = 0; i < n; ++i) {
                const float y = va_tanh_fast(evalFilter(buf[i] * dIn));
                buf[i] = va_clamp(y * dOut, -1.0f, 1.0f);
            }
        } else {
            for (size_t i = 0; i < n; ++i) {
                const float y = va_tanh_fast(evalFilter(buf[i]));
                buf[i] = va_clamp(y, -1.0f, 1.0f);
            }
        }
    };

    // Same tail WITHOUT the [1c] saturator — used by the Diode ladder only.
    //
    // WHY THE DIODE IS THE EXCEPTION.  The standing instruction is that the
    // diode must not be touched, and it is already the topology signed off as
    // sounding right in this firmware — i.e. right WITHOUT a saturator.  It is
    // also the one filter for which the saturator is not merely inaudible but
    // actively lossy: its derived passband compensation holds peak near 0.8,
    // and va_tanh_fast(0.8) = 0.675, a ~16% level cut applied to a topology
    // whose whole point is that it stays loud into resonance.  Adding it here
    // would quietly undo DIODE_COMP_DC.
    //
    // The demo did saturate this path (its SAT_TANH stage was unconditional),
    // so this is a deliberate, flagged divergence from demo parity in favour of
    // the explicit instruction.  To take the demo's behaviour instead, delete
    // this lambda and point case 8 back at runBlock — one line, no other edits.
    //
    // CONSEQUENCE OF THE EXCLUSION: the diode reaches the hard clamp with no
    // knee, so drive above roughly 1.25 will clip it rather than saturate it.
    // Acceptable while drive is not yet a patch parameter; revisit when the
    // params.yaml work in NEXT DELIVERY lands.
    auto runBlockClean = [&](auto evalFilter)
    {
        if (drv) {
            for (size_t i = 0; i < n; ++i)
                buf[i] = va_clamp(evalFilter(buf[i] * dIn) * dOut, -1.0f, 1.0f);
        } else {
            for (size_t i = 0; i < n; ++i)
                buf[i] = va_clamp(evalFilter(buf[i]), -1.0f, 1.0f);
        }
    };

    switch (_vaType) {
        // ── SVF family (Zavalishin §4.1 p.95) ────────────────────────────────
        case 0:  runBlock([&](float x){ _svf.process(x, g, k); return _svf.lp;    }); break;
        case 1:  runBlock([&](float x){ _svf.process(x, g, k); return _svf.hp;    }); break;
        case 2:  runBlock([&](float x){ _svf.process(x, g, k); return _svf.bp;    }); break;
        case 3:  runBlock([&](float x){ _svf.process(x, g, k); return _svf.notch; }); break;
        // SVF AP is the second [1c] exclusion, on a different ground from the
        // Diode.  An all-pass is DEFINED by |H| = 1 at every frequency — it
        // rearranges phase and changes nothing else, which is exactly why it is
        // useful as a phaser stage or a phase-alignment element.  A saturator
        // is a magnitude-shaping device, so putting one here does not colour an
        // all-pass, it stops it being one.
        // test_filter_section.cpp:125 asserts precisely this property (saw RMS
        // 0.577 survives intact) and caught the mistake; that assertion is
        // correct and is deliberately left unmodified.
        case 4:  runBlockClean([&](float x){ _svf.process(x, g, k); return _svf.allpass(k); }); break;

        // ── Linear Moog ladder (Zavalishin §5.1 p.133) ───────────────────────
        // Left at nl = VA_NL_NONE / drive = 1 (the struct's defaults), matching
        // both v1 and the demo: the ladder's in-loop saturation option costs
        // ~20% CPU for little gain at 8-voice polyphony, and with [1c] the
        // character now comes from the output stage instead.
        case 5:  runBlock([&](float x){ _moog.process(x, g, k); return _moog.y4; }); break;
        case 6:  runBlock([&](float x){ _moog.process(x, g, k); return _moog.y2; }); break;
        // BP from pole subtraction (Zavalishin §5.1 p.135)
        case 7:  runBlock([&](float x){ _moog.process(x, g, k); return _moog.y2 - _moog.y4; }); break;

        // ── Diode ladder — block-rate coefficients + cheap per-sample tick ────
        // [6b]: setCoeffs is now gated.  Core math untouched (out of scope), and
        // runBlockClean (not runBlock) keeps the [1c] saturator off this path —
        // see the lambda's comment for the full reasoning.  At drive == 1 this
        // case is bit-identical to the pre-change firmware.
        case 8:
            if (coeffChanged) _diode.setCoeffs(g, k);
            runBlockClean([&](float x){ return _diode.tick(x, k); });
            break;

        // ── Korg35 / TSK: VA_NL_SAT ALWAYS — linear feedback diverges ────────
        case 9:  runBlock([&](float x){ return _k35lp.process(x, g, k, VA_NL_SAT); }); break;
        case 10: runBlock([&](float x){ return _k35hp.process(x, g, k, VA_NL_SAT); }); break;

        // ── TPT 1-pole (Zavalishin §3.1 p.45) ────────────────────────────────
        // These carry the [1c] saturator too, exactly as the demo did (its
        // SAT_TANH stage was unconditional).  A 1-pole has no resonant peak to
        // tame, so the saturator here is pure output coloration — audible only
        // once the signal approaches full scale, or under drive.
        case 11: runBlock([&](float x){ return _tpt1.processLP(x, g); }); break;
        case 12: runBlock([&](float x){ float lp; return _tpt1.processHP(x, g, lp); }); break;

        // ── MoogDV (D'Angelo–Välimäki) — qcomp true == the v1 bank default ───
        case 13: runBlock([&](float x){ _moogdv.tick(x, k, true); return _moogdv.lp4; }); break;
        case 14: runBlock([&](float x){ _moogdv.tick(x, k, true); return _moogdv.lp2; }); break;
        case 15: runBlock([&](float x){ _moogdv.tick(x, k, true); return _moogdv.hp4; }); break;
        default: runBlock([&](float x){ _moogdv.tick(x, k, true); return _moogdv.bp;  }); break;
    }
}

} // namespace JT
