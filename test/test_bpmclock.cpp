// =============================================================================
// test_bpmclock.cpp — proofs for the Phase 3 subsystem 2 BPM clock + LFO sync
// =============================================================================
// Two layers, per docs/PHASE3_BPMCLOCK_SPEC.md §7:
//   UNIT   — the TempoClock class in isolation: the CORRECTED division->Hz
//            math (divide, not v1's multiply — Decision #3), the 40..300 clamp,
//            and BPM-linearity.
//   INTEG  — through ParameterStore -> SynthCore: CLOCK_TEMPO + LFO*_SYNC
//            resolve to the exact osc rate; Free restores the FREQ knob; the
//            Ext-MIDI source selection is inert (Decision #2); and the
//            byte-identical default-patch guarantee (no LFO synced).
// Sync rates are read via SynthCore::debugLfoRateHz (a JT_TESTING accessor) so
// the assertions are EXACT rather than statistical — the wiring is what this
// pass adds; that a synced LFO then audibly modulates is already covered by
// test_lfo.cpp (same Lfo, same destinations).
// =============================================================================
#include "doctest.h"

#include <vector>

#include "core/dsp/TempoClock.h"
#include "core/SynthCore.h"
#include "core/dsp/Curves.h"
#include "gen/ParamTable.h"

using namespace JT;
using namespace JT::Params;

namespace {

// One renderBlock drains the store's dirty params through applyParam (the LFO
// rate is resolved there, never per block — Decision #5), so tests read the
// effective rate only AFTER a drain.
void drain(SynthCore& core)
{
    float L[kBlockSize], R[kBlockSize];
    core.renderBlock(L, R, kBlockSize);
}

// Normalised store value for a Select option index (SYNC / SOURCE params).
float optNorm(uint16_t id, float option)
{
    return Curves::toNorm(kParams[ParameterStore::indexOf(id)], option);
}

} // namespace

// =============================================================================
// UNIT — TempoClock in isolation
// =============================================================================

TEST_CASE("TempoClock: freqForMode at 120 BPM matches the musical divisions")
{
    TempoClock c;
    c.setBpm(120.0f);
    // Corrected (divide) mapping: a quarter note at 120 BPM is 0.5 s -> 2 Hz,
    // and sub-quarter divisions get FASTER (unlike v1's buggy multiply).
    CHECK(c.freqForMode(TempoClock::k1_4)   == doctest::Approx(2.0f));
    CHECK(c.freqForMode(TempoClock::k1_8)   == doctest::Approx(4.0f));
    CHECK(c.freqForMode(TempoClock::k1_16)  == doctest::Approx(8.0f));
    CHECK(c.freqForMode(TempoClock::k1_2)   == doctest::Approx(1.0f));
    CHECK(c.freqForMode(TempoClock::k1Bar)  == doctest::Approx(0.5f));
    CHECK(c.freqForMode(TempoClock::k4Bars) == doctest::Approx(0.125f));
    // Triplet: 2 / 0.3333 = 6.0006 Hz — the rounding tolerance covers v1's
    // rounded multiplier decimals (Decision #4).
    CHECK(c.freqForMode(TempoClock::k1_8T)  == doctest::Approx(6.0f).epsilon(0.01));
    // Free is "not synced": a non-positive sentinel the caller falls back on.
    CHECK(c.freqForMode(TempoClock::kFree) <= 0.0f);
}

TEST_CASE("TempoClock: setBpm clamps to v1's 40..300 range")
{
    TempoClock c;
    c.setBpm(10.0f);   CHECK(c.bpm() == doctest::Approx(40.0f));
    c.setBpm(999.0f);  CHECK(c.bpm() == doctest::Approx(300.0f));
    c.setBpm(128.0f);  CHECK(c.bpm() == doctest::Approx(128.0f));
}

TEST_CASE("TempoClock: sync frequency scales linearly with BPM")
{
    TempoClock a, b;
    a.setBpm(120.0f);
    b.setBpm(240.0f);
    for (int m = TempoClock::k4Bars; m < TempoClock::kNumModes; ++m)
        CHECK(b.freqForMode(m) == doctest::Approx(2.0f * a.freqForMode(m)));
}

// =============================================================================
// INTEGRATION — through ParameterStore -> SynthCore
// =============================================================================

TEST_CASE("BPM sync: LFO1_SYNC=1/4 locks LFO1 to 2 Hz at 120 BPM, 4 Hz at 240")
{
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);

    store.setEngineering(ID::CLOCK_TEMPO, 120.0f, Origin::Ui);
    store.set(ID::LFO1_SYNC, optNorm(ID::LFO1_SYNC, (float)TempoClock::k1_4), Origin::Ui);
    drain(core);
    CHECK(core.debugLfoRateHz(0) == doctest::Approx(2.0f));

    // Tempo edit re-resolves the synced rate on the spot (refreshSyncedLfos).
    store.setEngineering(ID::CLOCK_TEMPO, 240.0f, Origin::Ui);
    drain(core);
    CHECK(core.debugLfoRateHz(0) == doctest::Approx(4.0f));
}

TEST_CASE("BPM sync: Free restores the LFO1_FREQ knob Hz after a synced mode")
{
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);

    store.setEngineering(ID::CLOCK_TEMPO, 120.0f, Origin::Ui);
    store.setEngineering(ID::LFO1_FREQ, 5.0f, Origin::Ui);         // free knob = 5 Hz
    drain(core);
    CHECK(core.debugLfoRateHz(0) == doctest::Approx(5.0f));        // Free by default

    store.set(ID::LFO1_SYNC, optNorm(ID::LFO1_SYNC, (float)TempoClock::k1_4), Origin::Ui);
    drain(core);
    CHECK(core.debugLfoRateHz(0) == doctest::Approx(2.0f));        // locked to clock

    store.set(ID::LFO1_SYNC, optNorm(ID::LFO1_SYNC, (float)TempoClock::kFree), Origin::Ui);
    drain(core);
    CHECK(core.debugLfoRateHz(0) == doctest::Approx(5.0f));        // knob restored
}

TEST_CASE("BPM sync: LFO2 syncs independently of LFO1")
{
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);

    store.setEngineering(ID::CLOCK_TEMPO, 120.0f, Origin::Ui);
    store.setEngineering(ID::LFO1_FREQ, 3.0f, Origin::Ui);             // LFO1 stays Free
    store.set(ID::LFO2_SYNC, optNorm(ID::LFO2_SYNC, (float)TempoClock::k1_8), Origin::Ui);
    drain(core);
    CHECK(core.debugLfoRateHz(0) == doctest::Approx(3.0f));            // Free knob
    CHECK(core.debugLfoRateHz(1) == doctest::Approx(4.0f));            // 1/8 @120 = 4 Hz
}

TEST_CASE("BPM sync: selecting Ext MIDI clock source is inert this pass (Decision #2)")
{
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);

    store.setEngineering(ID::CLOCK_TEMPO, 120.0f, Origin::Ui);
    store.set(ID::LFO1_SYNC, optNorm(ID::LFO1_SYNC, (float)TempoClock::k1_4), Origin::Ui);
    drain(core);
    const float internalRate = core.debugLfoRateHz(0);
    CHECK(internalRate == doctest::Approx(2.0f));

    // No external-clock BPM source exists yet: the selection is stored but the
    // internal BPM keeps driving, so the resolved rate does not move.
    store.set(ID::CLOCK_CLOCK_SOURCE, optNorm(ID::CLOCK_CLOCK_SOURCE, (float)TempoClock::kExtMidi), Origin::Ui);
    drain(core);
    CHECK(core.debugLfoRateHz(0) == doctest::Approx(internalRate));
}

TEST_CASE("BPM clock: with no LFO synced, writing clock params is byte-identical")
{
    // The default patch leaves both LFOs at SYNC=Free with all depths 0, so
    // they are disengaged (never ticked): the clock has nothing to drive, and
    // touching CLOCK_TEMPO/SOURCE must not perturb a single output sample
    // (the no-silent-change guarantee, CLAUDE.md rule 2).
    auto render = [](bool writeClock) {
        ParameterStore store;
        static float combPool[SynthCore::kCombPoolFloats];
        SynthCore core(store, combPool);
        if (writeClock) {
            store.setEngineering(ID::CLOCK_TEMPO, 90.0f, Origin::Ui);
            store.set(ID::CLOCK_CLOCK_SOURCE, optNorm(ID::CLOCK_CLOCK_SOURCE, (float)TempoClock::kExtMidi), Origin::Ui);
        }
        core.noteOn(57, 100);
        std::vector<float> out;
        float L[kBlockSize], R[kBlockSize];
        for (int b = 0; b < 100; ++b) {
            core.renderBlock(L, R, kBlockSize);
            for (size_t k = 0; k < kBlockSize; ++k) { out.push_back(L[k]); out.push_back(R[k]); }
        }
        return out;
    };

    const auto a = render(false);
    const auto b = render(true);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) CHECK(a[i] == b[i]);
}
