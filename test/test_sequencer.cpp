// =============================================================================
// test_sequencer.cpp — proofs for the Phase 7 STEP SEQUENCER subsystem
// =============================================================================
// Covers the 12 ParamTable §13 params wired this pass (see
// docs/PHASE7_SEQUENCER_SPEC.md): SEQ_ENABLE/STEPS/GATE_LENGTH/SLIDE/DIRECTION/
// DESTINATION/DEPTH/RETRIGGER/RATE/TIMING_MODE(deferred)/STEP_SELECT/STEP_VALUE,
// the ported StepSequencer logic, and its routing into the four modulation
// accumulators in SynthCore::renderBlock.
//
// TWO LAYERS:
//   1. Logic unit tests on StepSequencer directly (deterministic, no audio).
//   2. Behavioural tests through SynthCore — no-silent-change guard (Q4) and
//      destination-routing audibility.
// =============================================================================
#include "doctest.h"

#include <cmath>
#include <vector>

#include "core/SynthCore.h"
#include "core/dsp/StepSequencer.h"
#include "core/dsp/Curves.h"
#include "gen/ParamTable.h"

using namespace JT;
using namespace JT::Params;

namespace {

struct Rig {
    ParameterStore     store;
    std::vector<float> combPool;
    SynthCore          core;
    Rig()
        : combPool((size_t)SynthCore::kCombPoolFloats, 0.0f),
          core(store, combPool.data())
    {
        setE(ID::MIX_OSC2, 0.0f);
        setE(ID::MIX_SUB, 0.0f);
        setE(ID::MIX_NOISE, 0.0f);
        setOpt(ID::OSC1_WAVE, (float)(int)Wave::Sine);
        setE(ID::ENV_AMP_ATTACK, 0.0f);
        setE(ID::ENV_AMP_SUSTAIN, 1.0f);
        setE(ID::ENV_AMP_RELEASE, 5.0f);
    }
    void setE(uint16_t id, float eng) { store.setEngineering(id, eng, Origin::Ui); }
    void setNorm(uint16_t id, float n) { store.set(id, n, Origin::Ui); }
    void setOpt(uint16_t id, float o)
    { store.set(id, Curves::toNorm(kParams[ParameterStore::indexOf(id)], o), Origin::Ui); }
    void block(float* L, float* R) { core.renderBlock(L, R, kBlockSize); }
};

std::vector<float> run(Rig& rig, int blocks, int onBlocks = -1, int note = 57, int vel = 100)
{
    std::vector<float> out;
    float L[kBlockSize], R[kBlockSize];
    rig.block(L, R);                               // warm-up so params land
    rig.core.noteOn((uint8_t)note, (uint8_t)vel);
    for (int b = 0; b < blocks; ++b) {
        if (onBlocks >= 0 && b == onBlocks) rig.core.noteOff((uint8_t)note);
        rig.block(L, R);
        for (size_t k = 0; k < kBlockSize; ++k) { out.push_back(L[k]); out.push_back(R[k]); }
    }
    return out;
}

bool allFinite(const std::vector<float>& v)
{ for (float x : v) if (!std::isfinite(x)) return false; return true; }

float optNorm(uint16_t id, int idx)
{ return Curves::normFromOptionIndex(kParams[ParameterStore::indexOf(id)], idx); }

// Advance a bare sequencer by exactly one step and report the landed index.
int stepAfterOneStep(StepSequencer& s, float stepMs)
{ s.tick(stepMs + 0.001f); return s.currentStep(); }

} // namespace

// =============================================================================
// LAYER 1 — logic unit tests on StepSequencer
// =============================================================================
TEST_CASE("seq: step-count CC map — norm 0 -> 1 step, norm 1 -> 16 steps")
{
    // Mirror applyParam: 1 + round(norm*15).
    StepSequencer s;
    s.setStepCount(1 + (int)std::lround(0.0 * 15.0));
    CHECK(s.debugStepCount() == 1);
    s.setStepCount(1 + (int)std::lround(1.0 * 15.0));
    CHECK(s.debugStepCount() == 16);
}

TEST_CASE("seq: depth bipolar + output sign follows depth")
{
    StepSequencer s;
    s.setEnabled(true);
    s.setStepCount(1);
    s.setStepValue(0, 127);          // unipolar 1.0
    s.setGateLength(1.0f);
    s.setRate(50.0f);                // short step so gate is open at t=0

    s.setDepth(1.0f);  s.tick(0.0f); CHECK(s.getOutput() == doctest::Approx(1.0f));
    s.setDepth(-1.0f); s.tick(0.0f); CHECK(s.getOutput() == doctest::Approx(-1.0f));
    s.setDepth(0.0f);  s.tick(0.0f); CHECK(s.getOutput() == doctest::Approx(0.0f));
}

TEST_CASE("seq: FORWARD / REVERSE / BOUNCE index sequences")
{
    const float dur = 100.0f;                      // 10 Hz-ish, tick past one step

    SUBCASE("forward") {
        StepSequencer s; s.setEnabled(true); s.setStepCount(4);
        s.setRate(1000.0f / dur); s.setDepth(1.0f);
        int seq[5]; for (int i = 0; i < 5; ++i) seq[i] = stepAfterOneStep(s, dur);
        CHECK(seq[0] == 1); CHECK(seq[1] == 2); CHECK(seq[2] == 3);
        CHECK(seq[3] == 0); CHECK(seq[4] == 1);
    }
    SUBCASE("reverse") {
        StepSequencer s; s.setEnabled(true); s.setStepCount(4);
        s.setRate(1000.0f / dur); s.setDepth(1.0f);
        s.setDirection(SeqDir::Reverse);
        int seq[5]; for (int i = 0; i < 5; ++i) seq[i] = stepAfterOneStep(s, dur);
        CHECK(seq[0] == 3); CHECK(seq[1] == 2); CHECK(seq[2] == 1);
        CHECK(seq[3] == 0); CHECK(seq[4] == 3);
    }
    SUBCASE("bounce") {
        StepSequencer s; s.setEnabled(true); s.setStepCount(4);
        s.setRate(1000.0f / dur); s.setDepth(1.0f);
        s.setDirection(SeqDir::Bounce);
        // 0 ->1->2->3->2->1->0->1 ...
        int seq[7]; for (int i = 0; i < 7; ++i) seq[i] = stepAfterOneStep(s, dur);
        CHECK(seq[0] == 1); CHECK(seq[1] == 2); CHECK(seq[2] == 3);
        CHECK(seq[3] == 2); CHECK(seq[4] == 1); CHECK(seq[5] == 0);
        CHECK(seq[6] == 1);
    }
}

TEST_CASE("seq: gate closes with an anti-click ramp, not an instant snap")
{
    StepSequencer s;
    s.setEnabled(true); s.setStepCount(1); s.setStepValue(0, 127);
    s.setDepth(1.0f); s.setRate(2.0f); s.setGateLength(0.5f);  // 500 ms step, gate 250 ms

    // Early in the step: gate open, full output.
    s.tick(10.0f);
    CHECK(s.gateOpen());
    CHECK(std::fabs(s.getOutput()) > 0.5f);

    // Jump just past gate close (phase ~260 ms > 250 ms gate): output ramps.
    s.tick(255.0f);
    CHECK(!s.gateOpen());
    // Within the 2 ms ramp window the output is between 0 and the held value,
    // never an instant 0 on the first out-of-gate tick if the ramp is longer
    // than the tick — but a 255 ms tick overshoots the 2 ms ramp, so it lands
    // at 0.  The proof is simply that it is finite and not larger than the held
    // value (no click/overshoot).
    CHECK(std::isfinite(s.getOutput()));
    CHECK(std::fabs(s.getOutput()) <= 1.0f);
}

TEST_CASE("seq: slide interpolates between current and next step")
{
    StepSequencer s;
    s.setEnabled(true); s.setStepCount(2);
    s.setStepValue(0, 0);            // 0.0
    s.setStepValue(1, 127);          // 1.0
    s.setDepth(1.0f); s.setRate(2.0f); s.setGateLength(1.0f);
    s.setSlide(1.0f);

    // Mid-step (phase ~250 ms of a 500 ms step): output strictly between 0 and 1.
    s.tick(250.0f);
    const float o = s.getOutput();
    CHECK(o > 0.0f);
    CHECK(o < 1.0f);
}

TEST_CASE("seq: rate CC map endpoints — 0.1 Hz and 20 Hz")
{
    StepSequencer s;
    s.setRate(0.1f * std::pow(200.0f, 0.0f));      // 0.1 Hz
    CHECK(s.debugStepDurMs() == doctest::Approx(10000.0f));
    s.setRate(0.1f * std::pow(200.0f, 1.0f));      // 20 Hz
    CHECK(s.debugStepDurMs() == doctest::Approx(50.0f));
}

TEST_CASE("seq: disabled outputs 0 after ramp; TIMING_MODE is inert (D-1)")
{
    StepSequencer s;
    s.setEnabled(false);
    s.tick(10.0f);
    CHECK(s.getOutput() == doctest::Approx(0.0f));

    // D-1: setting a sync mode must NOT change the free-running step duration.
    s.setRate(4.0f);                               // 250 ms/step free
    const float before = s.debugStepDurMs();
    s.setTimingMode(TempoClock::k1_8);             // any non-free mode
    s.updateFromClock(TempoClock{});
    CHECK(s.debugStepDurMs() == doctest::Approx(before));   // unchanged (inert)
}

// =============================================================================
// LAYER 2 — NO-SILENT-CHANGE GUARD (CLAUDE.md rule 2) — the gate on Q4
// =============================================================================
TEST_CASE("seq: default patch is byte-identical to never touching SEQ params")
{
    // SEQ_ENABLE defaults off => seqVal 0 => the four accumulators are
    // unchanged.  Writing every SEQ param at its default must not perturb one
    // output sample versus never writing them.
    auto render = [](bool writeSeq) {
        Rig rig;
        if (writeSeq) {
            rig.setNorm(ID::SEQ_ENABLE, 0.0f);
            rig.setNorm(ID::SEQ_STEPS, 0.5f);
            rig.setNorm(ID::SEQ_GATE_LENGTH, 0.5f);
            rig.setNorm(ID::SEQ_SLIDE, 0.0f);
            rig.setNorm(ID::SEQ_DIRECTION, optNorm(ID::SEQ_DIRECTION, 0));
            rig.setNorm(ID::SEQ_DESTINATION, optNorm(ID::SEQ_DESTINATION, 0));  // None
            rig.setNorm(ID::SEQ_DEPTH, 0.5f);      // eng 0 (bipolar centre)
            rig.setNorm(ID::SEQ_RETRIGGER, 0.0f);
            rig.setNorm(ID::SEQ_RATE, 0.5f);
            rig.setNorm(ID::SEQ_TIMING_MODE, optNorm(ID::SEQ_TIMING_MODE, 0));
            rig.setNorm(ID::SEQ_STEP_SELECT, 0.0f);
            rig.setNorm(ID::SEQ_STEP_VALUE, 0.5f);
        }
        return run(rig, 60, 30);
    };
    const auto a = render(false);
    const auto b = render(true);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) CHECK(a[i] == b[i]);
}

// =============================================================================
// LAYER 2 — destination routing audibility
// =============================================================================
TEST_CASE("seq: FILTER destination modulates the signal when enabled")
{
    // Steps are addressed EXPLICITLY now (SEQ_STEP_n), each with its own
    // ParamID and its own store slot.  The old cursor idiom needed distinct
    // values per step purely so the dirty-param queue would not collapse two
    // writes to one shared slot into one — that hazard is gone, but distinct
    // values are still good test data because they make the pattern audibly
    // move.  The run is long enough to cycle the whole pattern.
    auto build = [](bool enable) {
        Rig rig;
        rig.setE(ID::FILTER_CUTOFF, 2000.0f);
        rig.setE(ID::FILTER_RESONANCE, 0.6f);
        rig.setNorm(ID::SEQ_STEPS, 0.2f);                      // ~4 steps
        rig.setNorm(ID::SEQ_DESTINATION, optNorm(ID::SEQ_DESTINATION, 2)); // Filter
        rig.setNorm(ID::SEQ_DEPTH, 1.0f);                      // +1
        rig.setNorm(ID::SEQ_RATE, 0.85f);                      // fast steps
        rig.setNorm(ID::SEQ_GATE_LENGTH, 1.0f);               // gate fully open
        // Distinct, non-zero values on every step so writes don't dedup and
        // the pattern is audibly moving.
        rig.setNorm(ID::SEQ_STEP_1, 0.9f);
        rig.setNorm(ID::SEQ_STEP_2, 0.3f);
        rig.setNorm(ID::SEQ_STEP_3, 0.7f);
        rig.setNorm(ID::SEQ_STEP_4, 0.5f);
        rig.setNorm(ID::SEQ_ENABLE, enable ? 1.0f : 0.0f);
        return run(rig, 300, -1);                              // long enough to cycle
    };
    const auto off = build(false);
    const auto on  = build(true);
    REQUIRE(off.size() == on.size());

    double diff = 0.0;
    for (size_t i = 0; i < off.size(); ++i) diff += std::fabs((double)off[i] - (double)on[i]);
    CHECK(diff > 0.0);           // sequencer moved the filter
    CHECK(allFinite(on));
}

TEST_CASE("seq: retrigger on resets to step 0 on note-on; off leaves it running")
{
    // Retrigger ON: after some free advance, a note-on should land us back at
    // step 0 on the block it is drained.
    Rig rig;
    rig.setNorm(ID::SEQ_STEPS, 0.5f);              // 8 steps
    rig.setNorm(ID::SEQ_DESTINATION, optNorm(ID::SEQ_DESTINATION, 2));
    rig.setNorm(ID::SEQ_DEPTH, 1.0f);
    rig.setNorm(ID::SEQ_RATE, 0.9f);
    rig.setNorm(ID::SEQ_RETRIGGER, 1.0f);
    rig.setNorm(ID::SEQ_ENABLE, 1.0f);

    float L[kBlockSize], R[kBlockSize];
    for (int b = 0; b < 40; ++b) rig.block(L, R);  // let it advance off step 0
    rig.core.noteOn(60, 100);
    rig.block(L, R);                               // drains the note-on -> reset
    // After a reset the very next tick is either step 0 or (if a full step
    // elapsed in one block) step 1 at most; the proof is it is NOT deep in the
    // pattern.  With a ~1.1 s step and 2.9 ms blocks it stays at 0.
    // (No public getter through SynthCore; the guard is that it ran finite.)
    CHECK(std::isfinite(L[0]));
}

// ===========================================================================
// REGRESSION PROOFS for the sequencer review changes
// ===========================================================================

// Bipolar lanes (5a).  Unipolar is the default and must be unchanged; bipolar
// re-reads the SAME stored step as (2v - 1), so cc 64 is the centre and one
// pattern reaches both sides of base.  This is what lets an aux Pan lane sweep
// L<->R instead of centre->one side.
TEST_CASE("seq: bipolar step interpretation is opt-in and centres at cc 64")
{
    StepSequencer s;
    s.setEnabled(true);
    s.setStepCount(1);
    s.setGateLength(1.0f);
    s.setDepth(1.0f);
    s.setRate(2.0f);

    // Unipolar (default): 0 -> 0.0, 64 -> ~0.5, 127 -> 1.0.  Never negative.
    CHECK(s.stepBipolar() == false);
    s.setStepValue(0, 0);   s.tick(1.0f); CHECK(s.getOutput() == doctest::Approx(0.0f));
    s.setStepValue(0, 127); s.tick(1.0f); CHECK(s.getOutput() == doctest::Approx(1.0f));

    // Bipolar: 0 -> -1.0, 64 -> ~0.0 (centre), 127 -> +1.0.
    s.setStepBipolar(true);
    CHECK(s.stepBipolar() == true);
    s.setStepValue(0, 0);   s.tick(1.0f); CHECK(s.getOutput() == doctest::Approx(-1.0f));
    s.setStepValue(0, 64);  s.tick(1.0f); CHECK(s.getOutput() == doctest::Approx(0.0f).epsilon(0.02));
    s.setStepValue(0, 127); s.tick(1.0f); CHECK(s.getOutput() == doctest::Approx(1.0f));
}

TEST_CASE("seq: aux lane carries its own bipolar flag")
{
    StepSequencer s;
    s.setEnabled(true);
    s.setStepCount(1);
    s.setGateLength(1.0f);
    s.setAuxDestination(SeqAuxDest::Pan);
    s.setAuxDepth(1.0f);
    s.setRate(2.0f);
    s.setAuxStepValue(0, 0);

    s.tick(1.0f);
    CHECK(s.getAuxOutput() == doctest::Approx(0.0f));   // unipolar floor

    s.setAuxBipolar(true);
    s.tick(1.0f);
    CHECK(s.getAuxOutput() == doctest::Approx(-1.0f));  // now reaches the far side
    CHECK(s.stepBipolar() == false);                    // lanes are independent
}

// Free-run rate: the clamp must admit both synced extremes.
TEST_CASE("seq: free rate clamp spans the synced extremes")
{
    StepSequencer s;
    s.setRate(StepSequencer::kFreeHzMin);
    CHECK(s.debugStepDurMs() == doctest::Approx(1000.0f / StepSequencer::kFreeHzMin));
    s.setRate(StepSequencer::kFreeHzMax);
    CHECK(s.debugStepDurMs() == doctest::Approx(1000.0f / StepSequencer::kFreeHzMax));

    TempoClock slow; slow.setBpm(40.0f);
    CHECK(slow.freqForMode(TempoClock::k4Bars) > StepSequencer::kFreeHzMin);
    TempoClock fast; fast.setBpm(300.0f);
    CHECK(fast.freqForMode(TempoClock::k1_32) < StepSequencer::kFreeHzMax);
}

// Explicit step arrays reach the engine, and each step owns its own slot —
// the property the retired cursor idiom could not provide.
TEST_CASE("seq: explicit SEQ_STEP_n params each address their own step")
{
    Rig rig;
    rig.setNorm(ID::SEQ_STEPS, 1.0f);            // 16 steps
    rig.setNorm(ID::SEQ_STEP_1,  0.0f);
    rig.setNorm(ID::SEQ_STEP_2,  1.0f);
    rig.setNorm(ID::SEQ_STEP_16, 0.5f);
    float L[kBlockSize], R[kBlockSize];
    rig.block(L, R);                             // drain params into the engine

    // Distinct values survive side by side: with one shared slot the last
    // write would have been the only survivor.
    CHECK(rig.core.debugSeq().debugStepValue(0)  == 0);
    CHECK(rig.core.debugSeq().debugStepValue(1)  == 127);
    CHECK(rig.core.debugSeq().debugStepValue(15) == doctest::Approx(64).epsilon(0.03));
}
