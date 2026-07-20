// =============================================================================
// test_velocity.cpp — proofs for the Pass 8 velocity-sensitivity params
// =============================================================================
// Three 0..1 knobs, all computed at NOTE-ON as static per-note DC (verified in
// v1 VoiceBlock::noteOn, lines 73-123):
//
//   VELOCITY_AMP_SENS    velAmpScale = (1−s) + s·velNorm
//                        osc gain    = velNorm · velAmpScale
//                        (s=0 ⇒ linear velNorm; s=1 ⇒ velNorm²)
//   VELOCITY_FILTER_SENS cutoffOct   = s·(velNorm−0.5)·3       (±1.5 oct bipolar)
//                        cutoff      = base · 2^cutoffOct        (indep. of octCtrl)
//   VELOCITY_ENV_SENS    envScale    = (1−s) + s·velNorm
//                        filterEnv depth ×= envScale
//
// The filter terms are asserted two ways: the FilterSection FOLD in isolation
// (exact, via the JT_TESTING debug cutoff), and the v1 FORMULA end-to-end through
// a Voice (velocity → the derived DC).  The amp term is asserted end-to-end
// through SynthCore by the output-amplitude ratio between two velocities — the
// only observable of _velGain, which is private by design.
//
// DEFAULT-CHANGE GUARD: the Pass-8 sign-off changed VELOCITY_AMP_SENS's table
// default 0.5→0.0 (v1 power-on = 0.0 = linear).  The amp ratio at the DEFAULT
// patch must therefore read linear (≈ velNorm ratio), not squared — that check
// doubles as the regression guard on the hand-edited ParamTable default.
// =============================================================================
#include "doctest.h"

#include <cmath>

#include "core/FilterSection.h"
#include "core/Voice.h"
#include "core/SynthCore.h"
#include "core/dsp/Curves.h"
#include "gen/ParamTable.h"

using namespace JT;
using namespace JT::Params;

namespace {

// Effective cutoff after one processed block at a given env level (silence in —
// process() only needs to run to refresh the coefficients).  Same technique as
// test_filter_env.cpp.
float cutoffAtEnv(FilterSection& f, float env01)
{
    float buf[kBlockSize] = { 0.0f };
    f.setEnvLevel(env01);
    f.process(buf, kBlockSize);
    return f.debugCutoffHz();
}

// Note-on a Voice at a velocity, render one block, read the effective cutoff.
// The velocity sens knobs must already be set (they are consumed inside noteOn).
float voiceCutoffAtVel(Voice& v, uint8_t vel)
{
    float L[kBlockSize] = { 0.0f }, R[kBlockSize] = { 0.0f };
    v.noteOn(69, vel, 0.5f);              // A4; pivot note, key term is neutral
    v.render(L, R, kBlockSize);
    return v.filter().debugCutoffHz();
}

constexpr float kVelNorm = 1.0f / 127.0f;

} // namespace

// =============================================================================
// VELOCITY_FILTER_SENS — bipolar cutoff shift, neutral at mid velocity
// =============================================================================

TEST_CASE("velocity→cutoff fold: the offset adds even at octaveControl 0")
{
    // v1 applied the velocity cutoff shift to BASE cutoff (base·2^offset), so it
    // is independent of octaveControl — it must survive octaveControl == 0, which
    // zeroes the key/env path.  OBXa default: base cutoff == the Hz passed in.
    FilterSection f;
    f.setCutoff(0.5f, 1000.0f);
    f.setOctaveControl(0.0f);                 // kills key+env modulation
    f.setEnvAmount(1.0f);

    f.setVelCutoffOffsetOct(1.5f);
    const float up = cutoffAtEnv(f, 1.0f);    // env ignored (octCtrl 0); offset stands
    CHECK(f.debugModOctaves() == doctest::Approx(1.5f).epsilon(1e-4));
    CHECK(up == doctest::Approx(1000.0f * std::exp2(1.5f)).epsilon(0.02));

    f.setVelCutoffOffsetOct(-1.5f);
    const float dn = cutoffAtEnv(f, 1.0f);
    CHECK(dn == doctest::Approx(1000.0f * std::exp2(-1.5f)).epsilon(0.02));

    f.setVelCutoffOffsetOct(0.0f);
    CHECK(cutoffAtEnv(f, 1.0f) == doctest::Approx(1000.0f).epsilon(0.02));
}

TEST_CASE("velocity→cutoff formula: sens·(vel−0.5)·3, neutral at velNorm 0.5")
{
    // End-to-end through a Voice: setVelFilterSens → noteOn derives the octave DC.
    Voice v;
    v.filter().setCutoff(0.5f, 1000.0f);
    v.filter().setOctaveControl(0.0f);        // isolate the velocity term
    v.setVelFilterSens(1.0f);                 // full range: ±1.5 oct

    const float hi = voiceCutoffAtVel(v, 127); // velNorm≈1   ⇒ +1.5 oct
    const float lo = voiceCutoffAtVel(v, 0);   // velNorm 0   ⇒ −1.5 oct
    const float mid = voiceCutoffAtVel(v, 64);  // velNorm≈0.5 ⇒ ~neutral

    CHECK(hi  == doctest::Approx(1000.0f * std::exp2(1.0f  * (127.0f * kVelNorm - 0.5f) * 3.0f)).epsilon(0.02));
    CHECK(lo  == doctest::Approx(1000.0f * std::exp2(1.0f  * (0.0f            - 0.5f) * 3.0f)).epsilon(0.02));
    CHECK(mid == doctest::Approx(1000.0f).epsilon(0.03));   // 64/127 ≈ 0.504 ⇒ ~1.0×
    CHECK(hi > mid);
    CHECK(mid > lo);
}

TEST_CASE("velocity→cutoff: sens 0 (default) is inert")
{
    Voice v;
    v.filter().setCutoff(0.5f, 1000.0f);
    v.filter().setOctaveControl(0.0f);
    // _velFilterSens defaults to 0 — no setter call.
    CHECK(voiceCutoffAtVel(v, 127) == doctest::Approx(1000.0f).epsilon(0.02));
    CHECK(voiceCutoffAtVel(v, 1)   == doctest::Approx(1000.0f).epsilon(0.02));
}

// =============================================================================
// VELOCITY_ENV_SENS — velocity scales filter-env depth
// =============================================================================

TEST_CASE("velocity→env-depth fold: envVelScale multiplies the env contribution")
{
    // modOct = envAmount·envLevel·envVelScale·octaveControl (key term off).
    FilterSection f;
    f.setCutoff(0.5f, 500.0f);
    f.setKeyTrackAmount(0.0f);
    f.setOctaveControl(1.0f);
    f.setEnvAmount(1.0f);

    f.setEnvVelScale(1.0f);
    const float full = cutoffAtEnv(f, 1.0f);   // modOct = 1 ⇒ ×2
    CHECK(full == doctest::Approx(500.0f * 2.0f).epsilon(0.01));

    f.setEnvVelScale(0.5f);
    const float half = cutoffAtEnv(f, 1.0f);   // modOct = 0.5 ⇒ ×√2
    CHECK(half == doctest::Approx(500.0f * std::sqrt(2.0f)).epsilon(0.01));

    f.setEnvVelScale(0.0f);
    const float none = cutoffAtEnv(f, 1.0f);   // depth fully removed ⇒ base
    CHECK(none == doctest::Approx(500.0f).epsilon(0.01));
}

TEST_CASE("velocity→env-depth formula: (1−s)+s·velNorm scales depth")
{
    // At full sens, a soft note gets almost no env depth, a hard note gets full.
    // Held env at level 1 via fast attack + sustain 1; read cutoff after settling.
    Voice v;
    v.filter().setCutoff(0.5f, 500.0f);
    v.filter().setKeyTrackAmount(0.0f);
    v.filter().setOctaveControl(1.0f);
    v.filter().setEnvAmount(1.0f);
    v.filterEnv().setAttackMs(0.0f);
    v.filterEnv().setDecayMs(1.0f);
    v.filterEnv().setSustain(1.0f);            // env parks at 1.0
    v.setVelEnvSens(1.0f);

    auto settledCutoff = [](Voice& vc, uint8_t vel) {
        float L[kBlockSize] = { 0.0f }, R[kBlockSize] = { 0.0f };
        vc.noteOn(69, vel, 0.5f);
        for (int b = 0; b < 8; ++b) vc.render(L, R, kBlockSize);  // reach sustain
        return vc.filter().debugCutoffHz();
    };

    const float hard = settledCutoff(v, 127);  // envScale≈1 ⇒ full octave ⇒ ×2
    const float soft = settledCutoff(v, 1);    // envScale≈0 ⇒ ~no depth ⇒ ~base

    CHECK(hard == doctest::Approx(500.0f * 2.0f).epsilon(0.03));
    CHECK(soft == doctest::Approx(500.0f).epsilon(0.05));
}

// =============================================================================
// VELOCITY_AMP_SENS — velocity → gain curve, end-to-end through SynthCore
// =============================================================================

namespace {

// RMS of a settled window of a single note at 'vel', with VELOCITY_AMP_SENS set
// to 'ampSensNorm'.  Fresh engine per call so nothing leaks between velocities.
float noteRms(float ampSensNorm, uint8_t vel)
{
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);
    float L[kBlockSize], R[kBlockSize];

    store.setEngineering(ID::VELOCITY_AMP_SENS, ampSensNorm, Origin::Ui);
    store.setEngineering(ID::ENV_AMP_ATTACK, 0.0f, Origin::Ui);   // instant on
    store.setEngineering(ID::ENV_AMP_DECAY, 1.0f, Origin::Ui);
    store.setEngineering(ID::ENV_AMP_SUSTAIN, 1.0f, Origin::Ui);  // flat gain

    // renderBlock drains NOTE events before DIRTY params, so a note fired in the
    // same batch would read the OLD sens.  One warm-up block flushes the params
    // first — mirroring real use, where the knob is set long before the note.
    core.renderBlock(L, R, kBlockSize);

    core.noteOn(69, vel);

    double sumsq = 0.0;
    size_t count = 0;
    const size_t settle = 4000, window = 20000;
    size_t produced = 0;
    while (produced < settle + window) {
        core.renderBlock(L, R, kBlockSize);
        for (size_t k = 0; k < kBlockSize; ++k, ++produced) {
            if (produced >= settle) { sumsq += (double)L[k] * (double)L[k]; ++count; }
        }
    }
    return (float)std::sqrt(sumsq / (double)count);
}

} // namespace

TEST_CASE("velocity→amp: default (sens 0) is LINEAR in velocity")
{
    // Guards the hand-edited ParamTable default (0.5→0.0): the ratio of a hard
    // to a mid note must equal the velNorm ratio, NOT its square.
    const float rms127 = noteRms(0.0f, 127);
    const float rms64  = noteRms(0.0f, 64);
    const float ratio  = rms127 / rms64;
    const float linear = (127.0f * kVelNorm) / (64.0f * kVelNorm);   // ≈1.984
    CHECK(ratio == doctest::Approx(linear).epsilon(0.03));
}

TEST_CASE("velocity→amp: sens 1 SQUARES the velocity response")
{
    const float rms127 = noteRms(1.0f, 127);
    const float rms64  = noteRms(1.0f, 64);
    const float ratio  = rms127 / rms64;
    const float vn127 = 127.0f * kVelNorm, vn64 = 64.0f * kVelNorm;
    const float squared = (vn127 * vn127) / (vn64 * vn64);           // ≈3.938
    CHECK(ratio == doctest::Approx(squared).epsilon(0.03));
}
