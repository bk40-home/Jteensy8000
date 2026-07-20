// =============================================================================
// test_parameter_store.cpp — proofs for core/ParameterStore
// =============================================================================
// CONCURRENCY-TEST PHILOSOPHY
//   The target is a single-core Cortex-M7 where the audio ISR preempts the
//   control plane but never the reverse.  Host threads model SMP — a
//   DIFFERENT (stronger, but differently-shaped) machine — so instead of
//   racing std::threads we interleave DETERMINISTICALLY: a fake "audio ISR"
//   is injected at the exact points inside publish() where a real ISR could
//   land (the JT_TESTING hook).  Every interesting interleaving is then a
//   reproducible test case, not a flaky maybe.
// =============================================================================
#include "doctest.h"

#include <cmath>

#include "core/ParameterStore.h"
#include "core/dsp/Curves.h"
#include "gen/ParamTable.h"

using namespace JT;
using namespace JT::Params;

// Drain every dirty flag, returning how many parameters were flagged.
// Mirrors the engine's per-block applyDirtyParams() loop.
static size_t drainDirty(ParameterStore& s)
{
    size_t n = 0, i;
    while ((i = s.takeNextDirty()) != ParameterStore::kInvalidIndex) {
        ++n;
        REQUIRE(i < ParameterStore::kCount);
    }
    return n;
}

TEST_CASE("boot state: defaults published, everything dirty exactly once")
{
    ParameterStore s;

    // Every parameter flagged once — the engine's first block applies the
    // complete state with no special init path (see header rationale).
    CHECK(drainDirty(s) == ParameterStore::kCount);
    CHECK(drainDirty(s) == 0);                 // ...and only once

    // The published snapshot equals the table defaults (through Curves).
    const float* snap = s.acquireSnapshot();
    for (size_t i = 0; i < kParamCount; ++i) {
        const ParamDesc& d = kParams[i];
        CAPTURE(d.key);
        CHECK(snap[i] == doctest::Approx(Curves::toNorm(d, d.def)));
        CHECK(s.origin(d.id) == Origin::Init);
    }
}

TEST_CASE("single set: visible to both planes, exactly one dirty flag")
{
    ParameterStore s;
    drainDirty(s);                             // consume the boot flags

    REQUIRE(s.set(ID::FILTER_CUTOFF, 0.25f, Origin::MidiUsbDev));

    // Control plane sees it immediately...
    CHECK(s.get(ID::FILTER_CUTOFF) == doctest::Approx(0.25f));
    // ...the audio plane sees the value AND exactly one dirty index...
    const size_t idx = ParameterStore::indexOf(ID::FILTER_CUTOFF);
    CHECK(s.acquireSnapshot()[idx] == doctest::Approx(0.25f));
    CHECK(s.takeNextDirty() == idx);
    CHECK(drainDirty(s) == 0);
    // ...and the origin is queryable for echo suppression.
    CHECK(s.origin(ID::FILTER_CUTOFF) == Origin::MidiUsbDev);
}

TEST_CASE("engineering-unit set round-trips through the curve")
{
    ParameterStore s;
    REQUIRE(s.setEngineering(ID::FILTER_CUTOFF, 1000.0f, Origin::SysEx));
    CHECK(s.getEngineering(ID::FILTER_CUTOFF) == doctest::Approx(1000.0f).epsilon(0.001));
}

TEST_CASE("unknown ParamID is rejected and changes nothing")
{
    ParameterStore s;
    drainDirty(s);
    CHECK_FALSE(s.set(0x3FFF, 0.5f, Origin::MidiUsbDev));       // valid-range, unused
    CHECK_FALSE(s.setEngineering(0x3FFF, 1.0f, Origin::MidiUsbDev));
    CHECK(s.get(0x3FFF) == 0.0f);
    CHECK(ParameterStore::indexOf(0x3FFF) == ParameterStore::kInvalidIndex);
    CHECK(drainDirty(s) == 0);                            // no stray flags
}

TEST_CASE("input hygiene: NaN and out-of-range normalized values are clamped")
{
    ParameterStore s;
    s.set(ID::FILTER_RESONANCE, std::nanf(""), Origin::MidiUsbDev);
    CHECK(s.get(ID::FILTER_RESONANCE) == 0.0f);
    s.set(ID::FILTER_RESONANCE, 7.0f, Origin::MidiUsbDev);
    CHECK(s.get(ID::FILTER_RESONANCE) == 1.0f);
}

TEST_CASE("bulk load: invisible mid-bulk, atomic at endBulk, nesting works")
{
    ParameterStore s;
    drainDirty(s);
    const size_t cutIdx = ParameterStore::indexOf(ID::FILTER_CUTOFF);
    const float  before = s.acquireSnapshot()[cutIdx];

    s.beginBulk();
    s.beginBulk();                                        // nested (patch-in-perf)
    s.set(ID::FILTER_CUTOFF,    0.9f, Origin::PatchLoad);
    s.set(ID::FILTER_RESONANCE, 0.9f, Origin::PatchLoad);

    // Mid-bulk: audio still sees the OLD world, zero dirty flags —
    // this is the half-loaded-patch bug of v1 made impossible.
    CHECK(s.acquireSnapshot()[cutIdx] == doctest::Approx(before));
    CHECK(drainDirty(s) == 0);
    // Control plane, by contrast, reads its own fresh writes.
    CHECK(s.get(ID::FILTER_CUTOFF) == doctest::Approx(0.9f));

    s.endBulk();                                          // inner: still held
    CHECK(drainDirty(s) == 0);
    s.endBulk();                                          // outer: publish

    CHECK(s.acquireSnapshot()[cutIdx] == doctest::Approx(0.9f));
    CHECK(drainDirty(s) == 2);                            // exactly the 2 sets
}

TEST_CASE("resetToDefaults restores boot state through one publish")
{
    ParameterStore s;
    drainDirty(s);
    s.set(ID::FILTER_CUTOFF, 0.1f, Origin::Ui);
    drainDirty(s);

    s.resetToDefaults(Origin::PatchLoad);
    CHECK(drainDirty(s) == ParameterStore::kCount);       // full re-apply
    const ParamDesc& d = *find(ID::FILTER_CUTOFF);
    CHECK(s.get(ID::FILTER_CUTOFF) == doctest::Approx(Curves::toNorm(d, d.def)));
}

TEST_CASE("back-to-back sets of one param: last write wins, one recompute")
{
    // A knob sweep delivers many sets per block; the engine must apply only
    // the latest value and only once per drain.
    ParameterStore s;
    drainDirty(s);
    for (int i = 0; i <= 100; ++i)
        s.set(ID::FILTER_CUTOFF, (float)i / 100.0f, Origin::Ui);

    const size_t idx = ParameterStore::indexOf(ID::FILTER_CUTOFF);
    CHECK(s.takeNextDirty() == idx);                      // one flag...
    CHECK(drainDirty(s) == 0);
    CHECK(s.acquireSnapshot()[idx] == doctest::Approx(1.0f));   // ...latest value
}

// =============================================================================
// Modelled ISR preemption inside publish()
// =============================================================================
namespace {

// The fake audio ISR: acquires a snapshot and checks that a MARKER GROUP of
// parameters is internally consistent — all still at generation A values or
// all at generation B values.  Seeing a mixture would mean a real ISR could
// hear half a patch.
struct IsrProbe {
    ParameterStore* store = nullptr;
    size_t idxA = 0, idxB = 0, idxC = 0;
    float genOld = 0.0f, genNew = 0.0f;
    int   calls = 0;
    bool  sawMixed = false;
    bool  sawNew = false;

    static void hook(void* ctx) { static_cast<IsrProbe*>(ctx)->run(); }

    void run()
    {
        ++calls;
        const float* v = store->acquireSnapshot();
        const bool aNew = v[idxA] == genNew;
        const bool bNew = v[idxB] == genNew;
        const bool cNew = v[idxC] == genNew;
        if (aNew != bNew || bNew != cNew) sawMixed = true;   // FAILURE
        if (aNew && bNew && cNew)         sawNew = true;
        // Consuming dirty flags mid-publish is also legal for a real ISR —
        // exercise it so the fetch_or/fetch_and interplay is covered too.
        size_t i;
        while ((i = store->takeNextDirty()) != ParameterStore::kInvalidIndex)
            (void)i;
    }
};

} // namespace

TEST_CASE("bulk publish is atomic at every modelled preemption point")
{
    ParameterStore s;
    drainDirty(s);

    IsrProbe probe;
    probe.store = &s;
    probe.idxA = ParameterStore::indexOf(ID::FILTER_CUTOFF);
    probe.idxB = ParameterStore::indexOf(ID::ENV_AMP_ATTACK);
    probe.idxC = ParameterStore::indexOf(ID::MASTER_VOLUME);
    s.testPreemptHook = &IsrProbe::hook;
    s.testPreemptCtx  = &probe;

    // 50 generations of "patch load": three params move together each time.
    // The hook fires at BOTH injection points of every publish, so this
    // exercises 100 distinct mid-publish preemptions.
    for (int g = 1; g <= 50; ++g) {
        probe.genOld = probe.genNew;
        probe.genNew = (float)g / 64.0f;
        s.beginBulk();
        s.set(ID::FILTER_CUTOFF,   probe.genNew, Origin::PatchLoad);
        s.set(ID::ENV_AMP_ATTACK,  probe.genNew, Origin::PatchLoad);
        s.set(ID::MASTER_VOLUME,   probe.genNew, Origin::PatchLoad);
        s.endBulk();
        // After publish the control plane must still converge even though
        // the "ISR" stole dirty flags mid-publish: values are correct...
        CHECK(s.acquireSnapshot()[probe.idxA] == doctest::Approx(probe.genNew));
        CHECK(s.acquireSnapshot()[probe.idxB] == doctest::Approx(probe.genNew));
        drainDirty(s);                              // ...flags fully drainable
    }

    CHECK(probe.calls == 100);                      // both points, every publish
    CHECK_FALSE(probe.sawMixed);                    // NEVER a torn generation
    CHECK(probe.sawNew);                            // hook really saw new data
}

TEST_CASE("preempting ISR between flip and dirty-raise only delays, never loses")
{
    // The documented worst case: an ISR lands after the flip but before the
    // dirty bits rise.  It must find zero flags THEN, and exactly the right
    // flags on its next "block".
    ParameterStore s;
    drainDirty(s);

    struct Counter {
        ParameterStore* store; size_t taken = 0;
        static void hook(void* ctx)
        {
            auto* c = static_cast<Counter*>(ctx);
            size_t i;
            while ((i = c->store->takeNextDirty()) != ParameterStore::kInvalidIndex)
                ++c->taken;
        }
    } counter{&s};
    s.testPreemptHook = &Counter::hook;
    s.testPreemptCtx  = &counter;

    s.set(ID::FILTER_CUTOFF, 0.42f, Origin::MidiUsbDev);

    // Hook ran twice inside publish(); flags rise only after point 2, so the
    // fake ISR must have taken nothing...
    CHECK(counter.taken == 0);
    // ...and the change is waiting for the next block: delayed, not lost.
    CHECK(s.takeNextDirty() == ParameterStore::indexOf(ID::FILTER_CUTOFF));
    CHECK(drainDirty(s) == 0);
}
