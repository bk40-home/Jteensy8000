// =============================================================================
// RoomReverb.cpp — see RoomReverb.h for the topology and the cost accounting.
// =============================================================================

#include "core/dsp/RoomReverb.h"

#include <string.h>   // memset

namespace JT {

namespace {
inline float clamp01(float x) { return (x < 0.0f) ? 0.0f : (x > 1.0f) ? 1.0f : x; }

// Diffusion coefficient for the allpasses.  Fixed rather than exposed: the
// plate's diffusion has no CC either, and a room's diffusion is a property of
// the walls, not a performance control.
constexpr float kApGain = 0.62f;
}

JT_COLD void RoomReverb::begin(float* pool)
{
    _pool = pool;
    if (!_pool) return;                 // inert; processBlock bails on null

    memset(_pool, 0, kPoolFloats * sizeof(float));
    memset(_apBuf, 0, sizeof(_apBuf));

    // Carve the pool: ER line then tail loop, contiguous.
    float* p = _pool;
    _er.buf   = p;  _er.len   = kErLen;   _er.writeIdx = 0;  p += kErLen;
    _tail.buf = p;  _tail.len = kTailLen; _tail.writeIdx = 0;

    // Allpasses into the DTCM member, contiguous.
    const uint32_t apLens[4] = { kApLen0, kApLen1, kApLen2, kApLen3 };
    float* d = _apBuf;
    for (uint8_t i = 0; i < 4; ++i) {
        _ap[i].dl.buf      = d;
        _ap[i].dl.len      = apLens[i];
        _ap[i].dl.writeIdx = 0;
        _ap[i].gain        = kApGain;
        d += apLens[i];
    }

    _tailLP.clear();  _tailLP.coeff = _hiDampCoeff;
    _tailHP.clear();  _tailHP.coeff = _loDampCoeff;
    for (uint8_t i = 0; i < 2; ++i) {
        _masterLP[i].clear(); _masterLP[i].coeff = 0.0f;
        _masterHP[i].clear(); _masterHP[i].coeff = 0.0f;
    }
    _tailFb = 0.0f;
}

void RoomReverb::clearTail()
{
    // Same contract as the plate's: drop the RECIRCULATING state without
    // memsetting the pool, which would be far too slow for the audio path.
    _tailFb = 0.0f;
    _tailLP.clear();
    _tailHP.clear();
    for (uint8_t i = 0; i < 4; ++i) _ap[i].clear();
    for (uint8_t i = 0; i < 2; ++i) { _masterLP[i].clear(); _masterHP[i].clear(); }
}

// ---------------------------------------------------------------------------
// size — drives BOTH the tail feedback and the early-reflection spread.
//
// One control moving two things is deliberate: in a real room the reflections
// arrive later AND the tail rings longer as the room grows, and splitting them
// into two knobs would let you dial physically impossible combinations that all
// sound wrong.  Feedback tops out at 0.85 rather than 1.0 — a room that rings
// forever is a plate, and freeze() is where infinite hold belongs.
// ---------------------------------------------------------------------------
void RoomReverb::setSize(float n)
{
    if (_frozen) return;                     // freeze owns the feedback
    n = clamp01(n);
    _feedback = 0.20f + 0.65f * n;           // 0.20 .. 0.85
    _erSpread = 0.35f + 0.65f * n;           // taps pull in for a small room
}

void RoomReverb::setHiDamp(float n)
{
    // Matches the plate's mapping so the control behaves the same on both.
    _hiDampCoeff  = clamp01(n) * 0.85f;
    _tailLP.coeff = _hiDampCoeff;
}

void RoomReverb::setLoDamp(float n)
{
    _loDampCoeff  = clamp01(n) * 0.85f;
    _tailHP.coeff = _loDampCoeff;
}

void RoomReverb::setLowpass(float n)
{
    _masterLpCoeff = clamp01(n) * 0.95f;
    _masterLP[0].coeff = _masterLpCoeff;
    _masterLP[1].coeff = _masterLpCoeff;
}

void RoomReverb::setHipass(float n)
{
    _masterHpCoeff = clamp01(n) * 0.95f;
    _masterHP[0].coeff = _masterHpCoeff;
    _masterHP[1].coeff = _masterHpCoeff;
}

void RoomReverb::setFreeze(bool on)
{
    if (_frozen == on) return;
    _frozen = on;
    if (on) { _savedFeedback = _feedback; _feedback = 1.0f; }
    else    { _feedback = _savedFeedback; }
}

// ---------------------------------------------------------------------------
// processBlock — the per-sample loop.
//
// PSRAM traffic is 9 accesses per sample: 1 ER write, 6 ER taps, 1 tail write,
// 1 tail read.  Everything else (four allpasses, four one-poles) is DTCM or
// register state.  Compare the plate's 14, all of which also carry the
// fractional interpolation of two modulated allpasses.
// ---------------------------------------------------------------------------
void RoomReverb::processBlock(float* left, float* right, size_t n,
                              float mixStart, float mixEnd)
{
    if (!_pool) return;

    // Block-constant caches — member loads hoisted out of the loop, matching
    // the plate's discipline.
    const float feedback   = _feedback;
    const float spread     = _erSpread;
    const float inputGain  = _frozen ? 0.02f : 1.0f;   // freeze bleed
    const bool  doMasterLP = (_masterLpCoeff > 0.001f);
    const bool  doMasterHP = (_masterHpCoeff > 0.001f);

    // Tap distances scale with size.  Resolved per BLOCK, not per sample, and
    // clamped to at least 1 so a tiny room cannot read the sample being
    // written this instant.
    uint32_t tapL[3], tapR[3];
    for (uint8_t t = 0; t < 3; ++t) {
        uint32_t dl = (uint32_t)((float)kTapL[t] * spread);
        uint32_t dr = (uint32_t)((float)kTapR[t] * spread);
        tapL[t] = (dl < 1u) ? 1u : dl;
        tapR[t] = (dr < 1u) ? 1u : dr;
    }

    // Mix ramp for ReverbRack's crossfade.  At steady state the increment is
    // exactly 0.0f — see IReverb.h.
    float       mix    = mixStart;
    const float mixInc = (n > 0) ? ((mixEnd - mixStart) / (float)n) : 0.0f;

    for (size_t i = 0; i < n; ++i) {
        const float inL = left[i];
        const float inR = right[i];
        const float monoIn = (inL + inR) * 0.5f * inputGain;

        _er.write(monoIn);

        // Early reflections — six integer reads, alternating polarity so the
        // cluster does not sum into a comb (see kTapGain).
        const float erL = _er.read(tapL[0]) * kTapGain[0]
                        + _er.read(tapL[1]) * kTapGain[1]
                        + _er.read(tapL[2]) * kTapGain[2];
        const float erR = _er.read(tapR[0]) * kTapGain[0]
                        + _er.read(tapR[1]) * kTapGain[1]
                        + _er.read(tapR[2]) * kTapGain[2];

        // Late tail — ONE mono loop.  Fed by the direct input plus the ER
        // cluster, so the tail inherits the room's reflection pattern instead
        // of being an unrelated wash bolted on.
        float loop = monoIn + (erL + erR) * 0.5f + _tailFb * feedback;
        loop = _ap[0].process(loop);
        loop = _ap[1].process(loop);
        _tail.write(loop);

        // Damping sits INSIDE the loop, so the tail darkens progressively
        // rather than being filtered once on the way out.
        _tailFb = _tailHP.process(_tailLP.process(_tail.read(_tail.len - 1)));

        // Re-open the mono tail to stereo through two different allpasses.
        // Cheaper than a stereo tail and, for a diffuse field, no less correct.
        float wetL = (erL + _ap[2].process(_tailFb)) * kWetScale;
        float wetR = (erR + _ap[3].process(_tailFb)) * kWetScale;

        if (doMasterLP) { wetL = _masterLP[0].process(wetL); wetR = _masterLP[1].process(wetR); }
        if (doMasterHP) { wetL = _masterHP[0].process(wetL); wetR = _masterHP[1].process(wetR); }

        float outL = inL + mix * wetL;
        float outR = inR + mix * wetR;

        // Soft clip — free unless we actually overshoot (same form as the plate).
        if (outL > 1.0f || outL < -1.0f) outL = outL / (1.0f + fabsf(outL));
        if (outR > 1.0f || outR < -1.0f) outR = outR / (1.0f + fabsf(outR));

        left[i]  = outL;
        right[i] = outR;
        mix += mixInc;
    }
}

} // namespace JT
