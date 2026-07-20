// =============================================================================
// OscSection.cpp — implementation (fidelity notes and contracts in the .h)
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "core/OscSection.h"

#include <cmath>     // exp2f — control rate only
#include <string.h>  // memset

#include "core/dsp/FastMath.h"

namespace JT {

namespace {
// v1 SubOscillatorBlock applied a fixed 0.9 headroom inside setAmplitude —
// part of the level balance every patch was dialled against.
constexpr float kSubHeadroom = 0.9f;
// v1 FM full scale: ±1.0 modulator = ±10 octaves (FM_OCTAVE_RANGE).
constexpr float kXmodOctaveRange = 10.0f;
// osc pitch-offset option set {-24,-12,0,+12,+24}, indexed by the select.
constexpr float kPitchOffsetSemis[5] = { -24.0f, -12.0f, 0.0f, 12.0f, 24.0f };
} // namespace

// -----------------------------------------------------------------------------
// Unit pitch — ONE exp2f per real change, never per block ("do not calculate
// if not required": glide and pitch-env will mark dirty when they land).
// -----------------------------------------------------------------------------
void OscSection::Unit::applyPitchIfDirty()
{
    if (!pitchDirty) return;
    pitchDirty = false;

    const float totalSemis = coarseSemis
                           + fineCents * 0.01f
                           + detuneSemis
                           + freqDcSemis;
    const float hz = noteHz * exp2f(totalSemis * (1.0f / 12.0f));

    // Both cores track the same pitch so switching waves mid-note (or the
    // supersaw's centre) never jumps frequency.
    core.setFrequency(hz);     // dirty-checked internally
    ss.setFrequency(hz);       // ditto (also retunes its pitch-tracked HPF)
}

// -----------------------------------------------------------------------------
// Control plane
// -----------------------------------------------------------------------------
void OscSection::noteOn(float noteHz, uint32_t seedBase)
{
    for (int u = 0; u < 2; ++u) {
        _u[u].noteHz     = noteHz;
        _u[u].pitchDirty = true;

        // Distinct, deterministic randomness per unit and purpose.
        _u[u].core.seedNoise(seedBase * 2654435761u + (uint32_t)u);
        _u[u].ss.seedNoise(seedBase * 40503u + (uint32_t)u + 7u);

        // Random start phases — the OscCore phase mirrors v1's supersaw
        // habit of randomising on trigger (kills 8-voice lockstep), and
        // the supersaw REQUIRES it (Szabó §3.4).
        _u[u].ss.noteOn();
    }
    _u[0].core.resetPhase((float)((seedBase >> 4) & 1023u) * (1.0f / 1024.0f));
    _u[1].core.resetPhase((float)((seedBase >> 14) & 1023u) * (1.0f / 1024.0f));

    // Sub: one octave below the NOTE — v1 fed the base note frequency in,
    // untouched by either unit's pitch offsets.
    _subInc   = (noteHz * 0.5f) / kSampleRate;
    _subPhase = 0.0f;

    // Fresh pitch-mod ramp for the new note: a stolen voice must not smear the
    // previous note's pitch sweep into this one.  The voice pushes this block's
    // target before render(); starting prev at 0 makes the first block ramp up
    // from the base note (and a normal pitch env starts near 0 anyway).
    _pitchModOct  = 0.0f;
    _pitchOctPrev = 0.0f;
}

void OscSection::setWave(int unit, int waveOption)
{
    _u[unit].waveOption = waveOption;
    _u[unit].core.setWave((Wave)waveOption);
}

void OscSection::setPitchOffset(int unit, int option)
{
    if (option < 0) option = 0;
    if (option > 4) option = 4;
    _u[unit].coarseSemis = kPitchOffsetSemis[option];
    _u[unit].pitchDirty  = true;
}

void OscSection::setFineTuneCents(int unit, float cents)
{
    _u[unit].fineCents  = cents;
    _u[unit].pitchDirty = true;
}

void OscSection::setDetuneSemis(int unit, float semis)
{
    _u[unit].detuneSemis = semis;
    _u[unit].pitchDirty  = true;
}

void OscSection::setFreqDcSemis(int unit, float semis)
{
    _u[unit].freqDcSemis = semis;
    _u[unit].pitchDirty  = true;
}

void OscSection::setShapeDc(int unit, float dc)
{
    // v1 fed shape DC into the pulse-width mod input: 0 DC = 50% width.
    // The v2 table's bipolar -1..1 covers the full 0..1 width (superset of
    // v1's unipolar CC — see header).  OscCore clamps to 5..95%.
    // Phase 3: store the base so the PWM LFO (render()) can offset it later
    // without losing this knob's own position; apply the base-only width
    // here so a patch with no PWM LFO wired needs no extra per-block work.
    _u[unit].shapeDcBase = dc;
    _u[unit].core.setShape(0.5f + 0.5f * dc);
}

void OscSection::setSupersawDetune(int unit, float v01) { _u[unit].ss.setDetune(v01); }
void OscSection::setSupersawMix(int unit, float v01)    { _u[unit].ss.setMix(v01); }
void OscSection::attachCombStorage(int unit, float* line) { _u[unit].comb.attachStorage(line); }
void OscSection::setFeedbackAmount(int unit, float v01) { _u[unit].comb.setAmount(v01); }
void OscSection::setFeedbackMix(int unit, float v01)    { _u[unit].comb.setMix(v01); }
void OscSection::setRingMix(int unit, float v01)        { _u[unit].ringGain = v01; }

void OscSection::setArbTable(int unit, const int16_t* data, uint16_t len)
{
    _u[unit].core.setArbTable(data, len);
}

float OscSection::nextNoise()
{
    uint32_t x = _rng;                 // xorshift32, same family as OscCore
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    _rng = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

// -----------------------------------------------------------------------------
// Audio plane — the section block.
// -----------------------------------------------------------------------------
void OscSection::render(float* out, size_t n)
{
    _u[0].applyPitchIfDirty();
    _u[1].applyPitchIfDirty();

    // Phase 3 PWM LFO (spec §4/§5 decision #5): recompute BOTH units' width
    // from their OWN base DC + the shared LFO offset, only when that offset
    // is non-zero.  v1 fed the same LFO into both oscillators' shape mixers,
    // hence both units move together.  The common case (no LFO wired to
    // PWM) skips this entirely — setShapeDc already applied the base-only
    // width at knob-set time, so the default patch is byte-identical.
    if (_lfoPwm != 0.0f) {
        _u[0].core.setShape(0.5f + 0.5f * (_u[0].shapeDcBase + _lfoPwm));
        _u[1].core.setShape(0.5f + 0.5f * (_u[1].shapeDcBase + _lfoPwm));
    }

    // Balance law (the flagged v1 collision fix — see header): linear
    // attenuation of the far side, unity in the centre.
    const float balG1 = (_balance > 0.0f) ? (1.0f - _balance) : 1.0f;
    const float balG2 = (_balance < 0.0f) ? (1.0f + _balance) : 1.0f;
    const float g1    = _mix1 * balG1;
    const float g2    = _mix2 * balG2;
    const float ringG = _u[0].ringGain + _u[1].ringGain;   // v1-equivalent sum

    // What actually needs to run this block?
    const bool xmod     = (_xmodDepth > 0.0f);
    // Hard sync needs a real phase-wrapping master — supersaw can't be one.
    const bool sync     = _sync && !_u[1].isSupersaw();
    const bool need2    = (g2 > 0.0f) || (ringG > 0.0f) || xmod || sync;
    const bool need1    = (g1 > 0.0f) || (ringG > 0.0f);
    const bool needSub  = (_subLevel > 0.0f);
    const bool needNoise= (_noiseLevel > 0.0f);

    // Pitch envelope: build a per-sample linear ramp of the pitch-mod offset
    // (octaves) from the value applied last block to this block's target, and
    // deliver it through each unit's exponential-FM input (fmOctaves = 1.0, so
    // the increment scales by 2^ramp).  Ramping across the 128 samples is what
    // keeps a fast pitch env from STEPPING the frequency at block boundaries.
    // Skipped entirely when no pitch mod is active (target and prev both 0), so
    // the default patch is byte-identical to before this pass.
    const bool  pitchActive = (_pitchModOct != 0.0f) || (_pitchOctPrev != 0.0f);

    float b1[kBlockSize];
    float b2[kBlockSize];
    float syncBuf[kBlockSize];
    float pitchRamp[kBlockSize];   // per-sample pitch offset, octaves
    float fm1[kBlockSize];         // OSC1's combined FM feed (pitch [+ x-mod])

    if (pitchActive) {
        const float stepOct = (_pitchModOct - _pitchOctPrev) / (float)n;
        float o = _pitchOctPrev;
        for (size_t i = 0; i < n; ++i) { o += stepOct; pitchRamp[i] = o; }
    }
    _pitchOctPrev = _pitchModOct;   // ramp end == target; carry to next block

    // --- OSC2 first: it may be OSC1's sync master and/or FM modulator ---
    if (need2) {
        // Pitch env modulates BOTH units in v1 (both FM slots fed).  When OSC2
        // is the sync master the slave then locks to the pitch-modulated master
        // — also v1-faithful (the master's FM moved its wrap points).
        const float* fm2 = pitchActive ? pitchRamp : nullptr;
        if (_u[1].isSupersaw()) {
            _u[1].ss.render(b2, n, fm2, 1.0f, nullptr);
        } else {
            _u[1].core.render(b2, n, fm2, 1.0f,
                              nullptr, sync ? syncBuf : nullptr);
        }
        // Comb colours the unit IN PLACE so mix, ring AND the cross-mod
        // feed all see the coloured signal — v1's post-comb tap points.
        if (_u[1].comb.isActive()) _u[1].comb.process(b2, n);
    }

    // --- OSC1: slave/carrier ---
    if (need1) {
        const bool   haveXmod = (xmod && need2);
        const float* fmBuf;
        float        fmOct;
        if (pitchActive) {
            // Sum both exponential-FM sources in octave space — v1 summed the
            // pitch env and cross-mod in the SAME FM mixer, so they add before
            // the 2^ conversion.  Pass with fmOctaves = 1.
            if (haveXmod) {
                const float xg = _xmodDepth * kXmodOctaveRange;
                for (size_t i = 0; i < n; ++i) fm1[i] = b2[i] * xg + pitchRamp[i];
            } else {
                for (size_t i = 0; i < n; ++i) fm1[i] = pitchRamp[i];
            }
            fmBuf = fm1;
            fmOct = 1.0f;
        } else {
            fmBuf = haveXmod ? b2 : nullptr;
            fmOct = _xmodDepth * kXmodOctaveRange;
        }
        if (_u[0].isSupersaw()) {
            // Supersaw takes the FM feed (v1 wired the same mixer into it)
            // but ignores sync — combination absent in v1, see header.
            _u[0].ss.render(b1, n, fmBuf, fmOct, nullptr);
        } else {
            _u[0].core.render(b1, n, fmBuf, fmOct,
                              (sync && need2) ? syncBuf : nullptr, nullptr);
        }
        if (_u[0].comb.isActive()) _u[0].comb.process(b1, n);
    }

    // --- mix: only the terms that exist this block ---
    memset(out, 0, n * sizeof(float));

    if (need1 && g1 > 0.0f)
        for (size_t i = 0; i < n; ++i) out[i] += b1[i] * g1;

    if (need2 && g2 > 0.0f)
        for (size_t i = 0; i < n; ++i) out[i] += b2[i] * g2;

    if (ringG > 0.0f && need1 && need2)
        for (size_t i = 0; i < n; ++i) out[i] += b1[i] * b2[i] * ringG;

    if (needSub) {
        const float gSub = _subLevel * kSubHeadroom;
        float ph = _subPhase;
        for (size_t i = 0; i < n; ++i) {
            out[i] += FastMath::fastSin01(ph) * gSub;
            ph += _subInc;
            if (ph >= 1.0f) ph -= 1.0f;
        }
        _subPhase = ph;
    }

    if (needNoise)
        for (size_t i = 0; i < n; ++i) out[i] += nextNoise() * _noiseLevel;
}

} // namespace JT
