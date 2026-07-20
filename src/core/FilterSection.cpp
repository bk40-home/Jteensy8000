// =============================================================================
// FilterSection.cpp — implementation (port provenance in the header)
// =============================================================================
// The shape table, resonance maps and per-type dispatch are ported from v1
// AudioFilterVABank/FilterShape VERBATIM in their numbers and structure;
// only the packaging changed (int16 AudioStream -> in-place float block).
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "core/FilterSection.h"

#include <cmath>   // powf — control rate only

namespace JT {

// -----------------------------------------------------------------------------
// v1 kFilterShape — one row per VA type, in option order.  The values encode
// each topology's audible range and resonance knee (v1 FilterShape.cpp, with
// its measurement provenance): Moog's k⁴ knee needs the strongest γ lift,
// Korg35's in-loop tanh compresses audible resonance early, MoogDV shares
// the Moog knee but its fcMax is pinned to the 1x-oversample ceiling.
// -----------------------------------------------------------------------------
const FilterSection::Shape FilterSection::kShape[17] JT_FLASH_DATA = {
    /* SVF LP2    */ {   40.0f, 14000.0f, 0.70f },
    /* SVF HP2    */ {   30.0f,  6000.0f, 0.70f },
    /* SVF BP2    */ {   40.0f, 14000.0f, 0.70f },
    /* SVF NOTCH  */ {  100.0f,  8000.0f, 0.70f },
    /* SVF AP     */ {   30.0f, 20000.0f, 0.70f },
    /* Moog LP4   */ {   40.0f, 12000.0f, 0.30f },
    /* Moog LP2   */ {   40.0f, 12000.0f, 0.30f },
    /* Moog BP2   */ {   80.0f, 10000.0f, 0.30f },
    /* Diode LP4  */ {   40.0f, 12000.0f, 0.45f },
    /* Korg35 LP  */ {   40.0f, 12000.0f, 0.40f },
    /* Korg35 HP  */ {   30.0f,  6000.0f, 0.40f },
    /* TPT1 LP    */ {   50.0f, 18000.0f, 1.00f },
    /* TPT1 HP    */ {   30.0f,  8000.0f, 1.00f },
    /* MoogDV LP4 */ {   40.0f,  5800.0f, 0.30f },
    /* MoogDV LP2 */ {   40.0f,  5800.0f, 0.30f },
    /* MoogDV HP4 */ {   40.0f,  5800.0f, 0.30f },
    /* MoogDV BP  */ {   40.0f,  5800.0f, 0.30f },
};

// -----------------------------------------------------------------------------
// v1 mapResonance — normalized (post-γ) resonance to the topology's own k/R.
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
    _envLevel   = 0.0f;
    _lastModOct = 1.0e30f;
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
    const float modOct = computeModOctaves();
    if (baseChanged || modOct != _lastModOct) {
        deriveCutoff(modOct);
        _lastModOct = modOct;
    }

    if (_engine == Engine::OBXa) {
        // The 2P/4P branch is hoisted out of the loop (mode is block-
        // constant); the core's per-sample work is fully inlined.
        if (_obxa.useTwoPole) {
            for (size_t i = 0; i < n; ++i)
                buf[i] = _obxa.process2Pole(buf[i], _obxaG);
        } else {
            for (size_t i = 0; i < n; ++i)
                buf[i] = _obxa.process4Pole(buf[i], _obxaG, _obxaLpc);
        }
        // v1 OBXA_STATE_GUARD: self-heal a poisoned IIR (4 compares).
        if (_obxa.stateGuard())
            for (size_t i = 0; i < n; ++i) buf[i] = 0.0f;   // one quiet block
        for (size_t i = 0; i < n; ++i) buf[i] = va_clamp(buf[i], -1.0f, 1.0f);
        return;
    }

    const float g = _g;
    const float k = _k;

    switch (_vaType) {
        case 0:  for (size_t i = 0; i < n; ++i) { _svf.process(buf[i], g, k); buf[i] = _svf.lp; } break;
        case 1:  for (size_t i = 0; i < n; ++i) { _svf.process(buf[i], g, k); buf[i] = _svf.hp; } break;
        case 2:  for (size_t i = 0; i < n; ++i) { _svf.process(buf[i], g, k); buf[i] = _svf.bp; } break;
        case 3:  for (size_t i = 0; i < n; ++i) { _svf.process(buf[i], g, k); buf[i] = _svf.notch; } break;
        case 4:  for (size_t i = 0; i < n; ++i) { _svf.process(buf[i], g, k); buf[i] = _svf.allpass(k); } break;

        case 5:  for (size_t i = 0; i < n; ++i) { _moog.process(buf[i], g, k); buf[i] = _moog.y4; } break;
        case 6:  for (size_t i = 0; i < n; ++i) { _moog.process(buf[i], g, k); buf[i] = _moog.y2; } break;
        case 7:  // BP from pole subtraction (Zavalishin §5.1 p.135)
                 for (size_t i = 0; i < n; ++i) { _moog.process(buf[i], g, k); buf[i] = _moog.y2 - _moog.y4; } break;

        case 8:  // diode: block-rate coeffs + cheap per-sample tick (v1 path)
                 _diode.setCoeffs(g, k);
                 for (size_t i = 0; i < n; ++i) buf[i] = _diode.tick(buf[i], k);
                 break;

        // Korg35: VA_NL_SAT ALWAYS — linear feedback diverges (v1 lesson).
        case 9:  for (size_t i = 0; i < n; ++i) buf[i] = _k35lp.process(buf[i], g, k, VA_NL_SAT); break;
        case 10: for (size_t i = 0; i < n; ++i) buf[i] = _k35hp.process(buf[i], g, k, VA_NL_SAT); break;

        case 11: for (size_t i = 0; i < n; ++i) buf[i] = _tpt1.processLP(buf[i], g); break;
        case 12: for (size_t i = 0; i < n; ++i) { float lp; buf[i] = _tpt1.processHP(buf[i], g, lp); } break;

        // MoogDV: qcomp true == the v1 bank default.
        case 13: for (size_t i = 0; i < n; ++i) { _moogdv.tick(buf[i], k, true); buf[i] = _moogdv.lp4; } break;
        case 14: for (size_t i = 0; i < n; ++i) { _moogdv.tick(buf[i], k, true); buf[i] = _moogdv.lp2; } break;
        case 15: for (size_t i = 0; i < n; ++i) { _moogdv.tick(buf[i], k, true); buf[i] = _moogdv.hp4; } break;
        default: for (size_t i = 0; i < n; ++i) { _moogdv.tick(buf[i], k, true); buf[i] = _moogdv.bp;  } break;
    }

    // v1's output clamp: the last line of defence stays (±1 on the wire).
    for (size_t i = 0; i < n; ++i) buf[i] = va_clamp(buf[i], -1.0f, 1.0f);
}

} // namespace JT
