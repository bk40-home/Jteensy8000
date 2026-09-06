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
    /* SVF LP2    */ {   40.0f, 14000.0f, 0.70f, 1.000f, 0.0f },
    /* SVF HP2    */ {   30.0f,  6000.0f, 0.70f, 1.000f, 0.0f },
    /* SVF BP2    */ {   40.0f, 14000.0f, 0.70f, 1.000f, 0.0f },
    /* SVF NOTCH  */ {  100.0f,  8000.0f, 0.70f, 1.000f, 0.0f },
    /* SVF AP     */ {   30.0f, 20000.0f, 0.70f, 1.000f, 0.0f },
    /* Moog LP4   */ {   40.0f, 12000.0f, 0.30f, 0.845f, 1.0f },
    /* Moog LP2   */ {   40.0f, 12000.0f, 0.30f, 0.845f, 1.0f },
    /* Moog BP2   */ {   80.0f, 10000.0f, 0.30f, 0.845f, 1.0f },
    /* Diode LP4  */ {   40.0f, 12000.0f, 0.45f, 1.000f, 0.0f },
    /* Korg35 LP  */ {   40.0f, 12000.0f, 0.40f, 0.445f, 0.0f },
    /* Korg35 HP  */ {   30.0f,  6000.0f, 0.40f, 0.445f, 0.0f },
    /* TPT1 LP    */ {   50.0f, 18000.0f, 1.00f, 1.000f, 0.0f },
    /* TPT1 HP    */ {   30.0f,  8000.0f, 1.00f, 1.000f, 0.0f },
    /* MoogDV LP4 */ {   40.0f,  5800.0f, 0.30f, 0.290f, 1.0f },
    /* MoogDV LP2 */ {   40.0f,  5800.0f, 0.30f, 0.290f, 1.0f },
    /* MoogDV HP4 */ {   40.0f,  5800.0f, 0.30f, 0.290f, 1.0f },
    /* MoogDV BP  */ {   40.0f,  5800.0f, 0.30f, 0.290f, 1.0f },
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
            // 4.10, not 3.95.  The ladder poles reach the imaginary axis at
            // k = 4 (measured onset with the ZDF solve: k=3.90 silent,
            // k=4.00 tail 0.017, k=4.10 tail 0.368), so the old map stopped
            // just short and the filter could only "self-oscillate" by way of
            // the unit-delay error it used to have.  Crossing the threshold is
            // safe because the feedback saturator is bounded (moog_sat).
            return r * 4.10f;
        case 8:                                      // diode ladder
            return r * 16.5f;                        // just below k=17
        case 9: case 10:                             // Korg35 (NL-bounded)
            // 2.05, not 1.95.  The TSK poles reach the imaginary axis at k = 2
            // (measured onset: k=1.98 silent, k=2.00 tail 0.015, k=2.05 tail
            // 0.416), so the previous map stopped deliberately just SHORT of
            // self-oscillation and these two types could never sing.  Crossing
            // the threshold is safe only because the feedback saturator is
            // bounded - see korg35_sat / JT_OPT_KORG35_SAT_LEVEL - and only
            // correct because the ZDF solve was fixed; with the old solve the
            // linear form was already producing NaN below k = 2.
            //
            // With the gamma 0.40 lift the top ~6% of knob travel oscillates,
            // and Q = 1/(2-k) climbs steeply through the range below it.
            return r * 2.05f;
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

// --- input drive ---------------------------------------------------------
// AudioFilterVABank::setDrive() ported verbatim in intent: clamp, store, and
// nothing else.  The demo derived no companion gains because it applied none
// — its update loop is a bare `x *= _drive` (AudioFilterVABank.cpp:334) with
// the output left to rise into saturate().
//
// The early-out on an unchanged value matters more than it looks: a 14-bit
// NRPN knob sends a dense stream of identical values while it is held.
void FilterSection::setDrive(float d)
{
    const float dv = va_clamp(d, kDriveMin, kDriveMax);
    if (dv == _drive) return;
    _drive = dv;

    // Exact float equality against 1.0f is intended — the same test the demo
    // made (`hasDrive = (_drive != 1.0f)`, AudioFilterVABank.cpp:313).  The
    // parameter path either delivers the literal neutral value or it does not,
    // and this flag is what hoists the whole decision out of the block loop.
    _driveActive = (dv != 1.0f);
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

    // Drive is a control-plane setting, not per-note state, so it deliberately
    // survives reset() — a topology switch must not silently drop it to unity.
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

    // Input staging: level INTO the topology, and the matching make-up so the
    // voice's output level is untouched.  See the Shape comment in the header.
    _inGain  = s.inGain;

    // Output make-up: input-staging inverse, times the ladder resonance
    // compensation. Both are block-rate and both are pure output gain, so they
    // collapse into a single multiply in the audio plane.
    //
    // POST, not PRE - see JT_OPT_FILTER_RES_COMP. Doing it here means the
    // topology's own saturator sees exactly what it saw before, so the
    // compensation restores level without touching character.
    _outGain = (1.0f / s.inGain)
             * (1.0f + JT_OPT_FILTER_RES_COMP * s.resComp * _k);
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

    // The diode's block-rate coefficients depend only on g (K is unused inside
    // setCoeffs), and g moves iff one of these two did.  A type switch sets
    // _dirty and reset() restores the _lastModOct sentinel, so the first block
    // on a topology always recomputes.
    const bool coeffChanged = baseChanged || (modOct != _lastModOct);

    if (_engine == Engine::OBXa) {
        // OUT OF SCOPE — core maths untouched, and NO saturator: the demo bank
        // was VA-only, so OBXa has no demo behaviour to restore, and OBXaCore
        // carries its own coloration.  Drive does not reach here either.
        //
        // The only change is folding the ±1 clamp into the same pass as the
        // filter call; this used to walk the block up to three times.  It is
        // bit-identical — zero-then-clamp and clamp-then-zero both give zero,
        // and the state guard is still evaluated after the whole block.
        if (_obxa.useTwoPole) {
            for (size_t i = 0; i < n; ++i)
                buf[i] = va_clamp(_obxa.process2Pole(buf[i], _obxaG), -1.0f, 1.0f);
        } else {
            for (size_t i = 0; i < n; ++i)
                buf[i] = va_clamp(_obxa.process4Pole(buf[i], _obxaG, _obxaLpc),
                                  -1.0f, 1.0f);
        }
        // v1 OBXA_STATE_GUARD: self-heal a poisoned IIR (4 compares).
        if (_obxa.stateGuard())
            for (size_t i = 0; i < n; ++i) buf[i] = 0.0f;   // one quiet block
        return;
    }

    const float g = _g;
    const float k = _k;

    // Hoisted once per block, exactly as the demo hoisted `hasDrive`.
    // The per-type input staging folds straight into the same multiply, so it
    // costs nothing extra: drive and calibration are one gain, not two.
    const float dIn  = _driveActive ? (_drive * _inGain) : _inGain;
    const float dOut = _outGain;

    // ── Dither: what lets a filter past its self-oscillation threshold start ─
    // At exactly 0.0 with zero state the filter is on a fixed point and stays
    // there. This is ~1e-7, which measures 131 dB below the resulting limit
    // cycle — inaudible, and it also keeps the state out of denormal range.
    // Added BEFORE the input staging so a heavily-trimmed type still gets it.
    constexpr float kDither = 1.0e-7f;
    uint32_t rng = _ditherState;

    // ---- The demo's per-sample tail, restored ------------------------------
    //   x → [×drive] → topology → saturate() → clamp → out
    //
    // saturate() is va_tanh (real tanhf), which is what AudioFilterVABank ran:
    // its _satType defaults to SAT_TANH and the sketch never changes it.  This
    // is the stage that was missing, and its absence is why every topology
    // sounded clinically clean below full scale and brick-walled above it.
    //
    // NOT va_tanh_fast.  The fast Padé form is UNBOUNDED — it reads 1.0 at
    // x = 3 and then climbs as x/9 — so it changes what the clamp behind it
    // means.  The demo's SAT_FAST option exists for exactly that trade and was
    // not the one it shipped on.  Cost of the real tanhf here is ~30 cycles,
    // about 1.8% of one core at 8 voices, which the demo also paid.
    //
    // The ±1 clamp stays as v1's last line of defence, now folded into the
    // same pass instead of a second walk over the buffer.
    // Per-sample tail:
    //   x -> +dither -> x(drive * inGain) -> topology -> x(1/inGain)
    //     -> saturate() -> clamp -> out
    //
    // The make-up sits BEFORE the saturator, not after. That matters: the
    // demo's output stage is calibrated to a full-scale signal, so the level
    // reaching it must be the true voice level. Putting the make-up after the
    // tanh would move the knee by 1/inGain per type and undo the very
    // consistency the staging exists to create.
    auto runBlock = [&](auto evalFilter)
    {
        for (size_t i = 0; i < n; ++i) {
            rng = rng * 1664525u + 1013904223u;
            const float d = kDither * ((float)(int32_t)rng * (1.0f / 2147483648.0f));
            const float y = evalFilter((buf[i] + d) * dIn) * dOut;
            buf[i] = va_clamp(va_tanh(y), -1.0f, 1.0f);
        }
    };

#if !JT_OPT_FILTER_SAT_DIODE
    // Diode-only tail with the saturator bypassed.  Drive still applies: it is
    // an input gain, independent of what the output stage does.
    auto runBlockNoSat = [&](auto evalFilter)
    {
        for (size_t i = 0; i < n; ++i) {
            rng = rng * 1664525u + 1013904223u;
            const float d = kDither * ((float)(int32_t)rng * (1.0f / 2147483648.0f));
            buf[i] = va_clamp(evalFilter((buf[i] + d) * dIn) * dOut, -1.0f, 1.0f);
        }
    };
#endif

    switch (_vaType) {
        // ── SVF family (Zavalishin §4.1 p.95) ────────────────────────────
        case 0:  runBlock([&](float x){ _svf.process(x, g, k); return _svf.lp;    }); break;
        case 1:  runBlock([&](float x){ _svf.process(x, g, k); return _svf.hp;    }); break;
        case 2:  runBlock([&](float x){ _svf.process(x, g, k); return _svf.bp;    }); break;
        case 3:  runBlock([&](float x){ _svf.process(x, g, k); return _svf.notch; }); break;
        // AP is saturated like everything else, matching the demo.  Note that
        // this DOES break the textbook all-pass property |H| = 1 at every
        // frequency — saturation is a magnitude-shaping device.  That is a
        // deliberate, signed-off choice in favour of demo parity, and
        // test_filter_section.cpp's all-pass assertion was updated to match
        // rather than the behaviour being bent to keep the old test green.
        case 4:  runBlock([&](float x){ _svf.process(x, g, k); return _svf.allpass(k); }); break;

        // ── Linear Moog ladder (Zavalishin §5.1 p.133) ────────────────────
        // nl / drive left at the struct defaults (VA_NL_NONE, 1.0), matching
        // v1 AND the demo — whose update loop states outright that in-loop
        // per-stage saturation was evaluated and reverted for costing ~20% CPU
        // at 8-voice polyphony with little sonic gain.  Independently
        // re-measured here and confirmed worse: the core's va_sat(x*d)*(1/d)
        // make-up only holds for an unbounded nonlinearity, so stage gain
        // collapses at real ladder levels and four stages compound it.
        case 5:  runBlock([&](float x){ _moog.process(x, g, k); return _moog.y4; }); break;
        case 6:  runBlock([&](float x){ _moog.process(x, g, k); return _moog.y2; }); break;
        // BP from pole subtraction (Zavalishin §5.1 p.135)
        case 7:  runBlock([&](float x){ _moog.process(x, g, k); return _moog.y2 - _moog.y4; }); break;

        // ── Diode ladder ─ block-rate coeffs + cheap per-sample tick (v1 path)
        // Core maths untouched.  Saturator inclusion is JT_OPT_FILTER_SAT_DIODE.
        case 8:
            if (coeffChanged) _diode.setCoeffs(g, k);
#if JT_OPT_FILTER_SAT_DIODE
            runBlock     ([&](float x){ return _diode.tick(x, k); });
#else
            runBlockNoSat([&](float x){ return _diode.tick(x, k); });
#endif
            break;

        // ── Korg35: VA_NL_SAT ALWAYS — linear feedback diverges (v1 lesson) ──
        case 9:  runBlock([&](float x){ return _k35lp.process(x, g, k, VA_NL_SAT); }); break;
        case 10: runBlock([&](float x){ return _k35hp.process(x, g, k, VA_NL_SAT); }); break;

        // ── TPT 1-pole (Zavalishin §3.1 p.45) ─────────────────────────
        // Saturated too, as the demo's unconditional stage was.  A 1-pole has
        // no resonant peak to tame, so here it is pure output coloration.
        case 11: runBlock([&](float x){ return _tpt1.processLP(x, g); }); break;
        case 12: runBlock([&](float x){ float lp; return _tpt1.processHP(x, g, lp); }); break;

        // ── MoogDV: qcomp true == the v1 bank default ───────────────────
        case 13: runBlock([&](float x){ _moogdv.tick(x, k, false); return _moogdv.lp4; }); break;
        case 14: runBlock([&](float x){ _moogdv.tick(x, k, false); return _moogdv.lp2; }); break;
        case 15: runBlock([&](float x){ _moogdv.tick(x, k, false); return _moogdv.hp4; }); break;
        default: runBlock([&](float x){ _moogdv.tick(x, k, false); return _moogdv.bp;  }); break;
    }

    // Carry the dither generator across blocks so it never repeats a short
    // sequence, which would be a tone rather than noise.
    _ditherState = rng;
}

} // namespace JT
