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

    // Phase 3 PWM LFO (spec §4/§5 decision #5), G2 split: recompute BOTH
    // units' width from their OWN base DC + the LFO lanes, only when a lane
    // is live.  LFO1's lane is scaled per unit (JP-8000 SQR Control 2 gave
    // each osc its own LFO1→width depth); the LFO2/sequencer lane stays
    // common, as on the hardware.  pwmScale defaults to 1.0, so the sum
    // equals the old single-lane value — render baseline byte-identical.
    // The common case (no LFO wired to PWM) still skips this entirely —
    // setShapeDc already applied the base-only width at knob-set time.
    // D-PWM-stick fix (signed off): _pwmWasLive makes the transition to
    // all-zero lanes recompute ONCE MORE, restoring the base widths.
    // Previously a depth cut mid-swing left the last LFO offset applied
    // until shape_dc was next touched.  Steady-state zero still costs only
    // the compares.
    const bool pwmLive = (_lfoPwm1 != 0.0f || _lfoPwmC != 0.0f);
    if (pwmLive || _pwmWasLive) {
        _u[0].core.setShape(0.5f + 0.5f * (_u[0].shapeDcBase
                                           + _lfoPwm1 * _u[0].pwmScale
                                           + _lfoPwmC));
        _u[1].core.setShape(0.5f + 0.5f * (_u[1].shapeDcBase
                                           + _lfoPwm1 * _u[1].pwmScale
                                           + _lfoPwmC));
    }
    _pwmWasLive = pwmLive;

    // Balance law (the flagged v1 collision fix — see header): linear
    // attenuation of the far side, unity in the centre.
    const float balG1 = (_balance > 0.0f) ? (1.0f - _balance) : 1.0f;
    const float balG2 = (_balance < 0.0f) ? (1.0f + _balance) : 1.0f;
    const float g1    = _mix1 * balG1;
    const float g2    = _mix2 * balG2;
    const float ringG = _u[0].ringGain + _u[1].ringGain;   // v1-equivalent sum

    // What actually needs to run this block?
    // G1 X-MOD routing: the effective FM depth is the knob PLUS this block's
    // routed (LFO1 + pitch-env) offset, clamped to the knob's own 0..1 span.
    // With the offset at 0 (destination not X-MOD — the default) xd equals
    // _xmodDepth exactly, so the baseline path and its gating are unchanged.
    float xd = _xmodDepth + _xmodOffset;
    if (xd < 0.0f) xd = 0.0f; else if (xd > 1.0f) xd = 1.0f;
    const bool xmod     = (xd > 0.0f);
    // Hard sync needs a real phase-wrapping master.  Per the JP-8000 panel
    // diagram (manual p.59) OSC1 is the sync MASTER and OSC2 the slave — the
    // reverse of the v1 port we started from (F2).  Supersaw can't be a
    // master (no single wrap point), so the guard is on OSC1 now.
    const bool sync     = _sync && !_u[0].isSupersaw();
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
    // G1 OSC2-only lane: live whenever its target OR its carry-over is
    // non-zero (same rule as the common term, so a just-cleared routing
    // still ramps back to base instead of stepping).  Destination "OSC1+2"
    // — the default — keeps both at 0: two compares, no other cost.
    const bool  extra2Active = (_pitchModOct2 != 0.0f) || (_pitch2OctPrev != 0.0f);

    float b1[kBlockSize];
    float b2[kBlockSize];
    float syncBuf[kBlockSize];
    float pitchRamp[kBlockSize];   // per-sample COMMON pitch offset, octaves
    float pitchRamp2[kBlockSize];  // per-sample OSC2 feed: common + routed lane
    float fm1[kBlockSize];         // OSC1's combined FM feed (pitch [+ x-mod])
    float fm1Delayed[kBlockSize];  // one-sample-delayed X-MOD feed (sync case)

    if (pitchActive) {
        const float stepOct = (_pitchModOct - _pitchOctPrev) / (float)n;
        float o = _pitchOctPrev;
        for (size_t i = 0; i < n; ++i) { o += stepOct; pitchRamp[i] = o; }
    }
    _pitchOctPrev = _pitchModOct;   // ramp end == target; carry to next block

    if (extra2Active) {
        // OSC2's feed = its own ramped lane, summed with the common ramp when
        // that is live too (both are octave-space, they add before the 2^
        // conversion in the FM path — same rule as pitch + x-mod on OSC1).
        const float stepOct2 = (_pitchModOct2 - _pitch2OctPrev) / (float)n;
        float o2 = _pitch2OctPrev;
        if (pitchActive) {
            for (size_t i = 0; i < n; ++i) { o2 += stepOct2; pitchRamp2[i] = pitchRamp[i] + o2; }
        } else {
            for (size_t i = 0; i < n; ++i) { o2 += stepOct2; pitchRamp2[i] = o2; }
        }
    }
    _pitch2OctPrev = _pitchModOct2;

    // =====================================================================
    // OSC1/OSC2 coupled render — JP-8000 signal flow (manual p.59 diagram):
    //   SYNC : OSC1 (master) resets OSC2 (slave)          [swapped from v1]
    //   X-MOD: OSC2 (modulator) FMs OSC1 (carrier)         [unchanged, ✓]
    //   RING : OSC1 × OSC2                                 [order-independent]
    // The two couplings point opposite ways, so with BOTH on there is a
    // genuine circular dependency (OSC1 needs OSC2's output for X-MOD; OSC2
    // needs OSC1's wrap buffer for sync).  The hardware runs them in parallel
    // per sample; a block-serial renderer cannot without interleaving, so we
    // break the loop with a ONE-SAMPLE delay on the X-MOD feed only: OSC1
    // renders first (master), FM-fed by OSC2's output from LAST block plus
    // this block shifted by one (seed = _xmodTail).  22 µs of latency on the
    // FM path, inaudible, and it costs 1 float of state.  Ring/balance are
    // taken AFTER both render, so they are unaffected by the order.
    //
    // Render order by case:
    //   sync (any xmod) : OSC1 first  (master must fill syncBuf before slave)
    //   xmod, no sync   : OSC2 first  (modulator before carrier — v1 path)
    //   neither         : OSC2 first  (harmless; matches the no-sync path)
    // =====================================================================
    const bool haveXmod = xmod && need2;
    const float xg = xd * kXmodOctaveRange;

    // ---- helper lambdas keep the two orderings from duplicating wave logic --
    // OSC2 (the modulator / slave).  syncIn is the master's wrap buffer when
    // OSC2 is the sync slave; fed nullptr when free-running.
    auto renderOsc2 = [&](const float* syncIn)
    {
        const float* fm2 = extra2Active ? pitchRamp2
                         : (pitchActive ? pitchRamp : nullptr);
        if (_u[1].isSupersaw()) {
            _u[1].ss.render(b2, n, fm2, 1.0f, nullptr);   // supersaw ignores sync
        } else {
            _u[1].core.render(b2, n, fm2, 1.0f, syncIn, nullptr);
        }
        if (_u[1].comb.isActive()) _u[1].comb.process(b2, n);
    };

    // OSC1 (the carrier / master).  xmodSrc is the per-sample modulator this
    // block sees (either OSC2's fresh output, or the one-sample-delayed feed
    // when OSC1 has to render first); syncOut is filled when OSC1 is master.
    auto renderOsc1 = [&](const float* xmodSrc, float* syncOut)
    {
        const float* fmBuf;
        float        fmOct;
        if (pitchActive) {
            // Both exponential-FM sources sum in octave space (pitch env +
            // cross-mod add before the 2^ conversion; fmOctaves = 1).
            if (haveXmod) {
                for (size_t i = 0; i < n; ++i) fm1[i] = xmodSrc[i] * xg + pitchRamp[i];
            } else {
                for (size_t i = 0; i < n; ++i) fm1[i] = pitchRamp[i];
            }
            fmBuf = fm1;
            fmOct = 1.0f;
        } else if (haveXmod) {
            // No pitch mod: FM straight from the modulator at full octave scale.
            for (size_t i = 0; i < n; ++i) fm1[i] = xmodSrc[i];
            fmBuf = fm1;
            fmOct = xg;
        } else {
            fmBuf = nullptr;
            fmOct = 0.0f;
        }
        if (_u[0].isSupersaw()) {
            _u[0].ss.render(b1, n, fmBuf, fmOct, nullptr);   // supersaw: no sync-out
        } else {
            _u[0].core.render(b1, n, fmBuf, fmOct, nullptr, syncOut);
        }
        if (_u[0].comb.isActive()) _u[0].comb.process(b1, n);
    };

    if (sync) {
        // --- OSC1 master FIRST -------------------------------------------
        // X-MOD needs OSC2's output, but OSC2 (slave) needs OSC1's syncBuf,
        // which OSC1 hasn't produced yet.  Break the loop with a true
        // one-sample delay that PRESERVES the modulator waveform: this
        // block's FM feed is [last block's final sample, then this-block's
        // OSC2 samples 0..n-2].  We only have last block's OSC2 in _xmodPrev,
        // so we assemble the feed from _xmodPrev shifted by one (sample 0 =
        // _xmodPrev[n-1], sample k = _xmodPrev[k-1]).  That is a whole-block
        // delay in the worst case but keeps the modulator's SHAPE — unlike a
        // DC seed, which would silence the X-MOD timbre during sync.  On
        // steady tones (the X-MOD-under-sync use case) a block of delay is
        // phase only, inaudible; on fast transients it softens by ≤2.9 ms.
        if (need1) {
            if (haveXmod) {
                fm1Delayed[0] = _xmodTail;                 // carry across boundary
                for (size_t i = 1; i < n; ++i) fm1Delayed[i] = _xmodPrev[i - 1];
                renderOsc1(fm1Delayed, syncBuf);
            } else {
                renderOsc1(nullptr, syncBuf);
            }
        } else {
            // OSC1 muted but still the sync master: fill syncBuf via scratch.
            renderOsc1(nullptr, syncBuf);
        }
        // --- OSC2 slave, reset by OSC1's wrap buffer ----------------------
        if (need2) {
            renderOsc2(syncBuf);
            _xmodTail = b2[n - 1];
            memcpy(_xmodPrev, b2, n * sizeof(float));       // seed next block
        }
    } else {
        // --- No sync: OSC2 (modulator) first, then OSC1 (carrier) ---------
        // v1's order; X-MOD reads OSC2's fresh same-block output (no delay).
        if (need2) renderOsc2(nullptr);
        if (need1) renderOsc1(need2 ? b2 : nullptr, nullptr);
        if (need2) {
            _xmodTail = b2[n - 1];
            memcpy(_xmodPrev, b2, n * sizeof(float));
        }
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
