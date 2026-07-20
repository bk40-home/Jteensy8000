// =============================================================================
// test_wavetables.cpp — proofs for WavetableLib + the Arb playback path
// =============================================================================
#include "doctest.h"

#include <cmath>

#include "core/WavetableLib.h"
#include "core/SynthCore.h"
#include "core/dsp/Curves.h"
#include "gen/ParamTable.h"

using namespace JT;
using namespace JT::Params;

TEST_CASE("library: every bank populated, tables 600 samples, clamps hold")
{
    for (int b = 0; b < WavetableLib::kNumBanks; ++b) {
        CAPTURE(b);
        CHECK(WavetableLib::bankCount(b) > 0);
        uint16_t len = 0;
        const int16_t* t = WavetableLib::akwfTable(b, 0, len);
        REQUIRE(t != nullptr);
        CHECK(len == 600);

        // Out-of-range index clamps to the bank's LAST wave, never nullptr.
        uint16_t len2 = 0;
        const int16_t* last = WavetableLib::akwfTable(b, 9999, len2);
        CHECK(last != nullptr);
        CHECK(len2 == 600);
    }
    // Out-of-range banks are inert, not lethal.
    uint16_t len = 1;
    CHECK(WavetableLib::akwfTable(-1, 0, len) == nullptr);
    CHECK(len == 0);
    CHECK(WavetableLib::akwfTable(99, 0, len) == nullptr);

    // v1 bucketing laws at the edges.
    CHECK(WavetableLib::bankFromNorm(0.0f) == 0);
    CHECK(WavetableLib::bankFromNorm(1.0f) == 9);
    CHECK(WavetableLib::bankFromNorm(0.65f) == 6);          // BwSin's bucket
    CHECK(WavetableLib::indexFromNorm(1.0f, 2)
          == (int)WavetableLib::bankCount(2) - 1);
}

TEST_CASE("engine: Arb wave plays an AKWF sine at the right pitch")
{
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);
    float L[kBlockSize], R[kBlockSize];

    store.setEngineering(ID::ENV_AMP_SUSTAIN, 1.0f, Origin::Ui);
    store.setEngineering(ID::MIX_OSC2, 0.0f, Origin::Ui);
    // OSC1 -> Arb (option 4), bank -> BwSin (bucket 6), index -> first wave.
    const size_t idxW = ParameterStore::indexOf(ID::OSC1_WAVE);
    store.set(ID::OSC1_WAVE, Curves::toNorm(kParams[idxW], 4.0f), Origin::MidiUsbDev);
    store.set(ID::OSC1_ARB_BANK, 0.65f, Origin::MidiUsbDev);
    store.set(ID::OSC1_ARB_INDEX, 0.0f, Origin::MidiUsbDev);

    core.noteOn(69, 127);                        // A4 = 440 Hz
    // Settle one block (note event + params), then measure a whole second.
    core.renderBlock(L, R, kBlockSize);
    size_t crossings = 0;
    float prev = 0.0f;
    double acc = 0.0;
    const int blocks = 345;                      // ≈ 1.0 s
    for (int b = 0; b < blocks; ++b) {
        core.renderBlock(L, R, kBlockSize);
        for (size_t i = 0; i < kBlockSize; ++i) {
            if (prev <= 0.0f && L[i] > 0.0f) ++crossings;
            prev = L[i];
            acc += (double)L[i] * (double)L[i];
            REQUIRE(std::isfinite(L[i]));
        }
    }
    // A near-sine single-cycle wave crosses once per period: ~440/s.
    CHECK(crossings >= 435);
    CHECK(crossings <= 445);
    CHECK(std::sqrt(acc / (double)(blocks * (int)kBlockSize)) > 0.01);
}

TEST_CASE("engine: bank/index thrash mid-note is glitch-safe and finite")
{
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);
    float L[kBlockSize], R[kBlockSize];
    store.setEngineering(ID::ENV_AMP_SUSTAIN, 1.0f, Origin::Ui);
    const size_t idxW = ParameterStore::indexOf(ID::OSC1_WAVE);
    store.set(ID::OSC1_WAVE, Curves::toNorm(kParams[idxW], 4.0f), Origin::MidiUsbDev);
    core.noteOn(50, 120);

    uint32_t rng = 31;
    for (int b = 0; b < 500; ++b) {
        rng = rng * 1664525u + 1013904223u;
        if ((b & 3) == 0)
            store.set(ID::OSC1_ARB_BANK,
                      (float)((rng >> 8) & 255u) / 255.0f, Origin::MidiUsbDev);
        if ((b & 3) == 2)
            store.set(ID::OSC1_ARB_INDEX,
                      (float)((rng >> 9) & 255u) / 255.0f, Origin::MidiUsbDev);
        core.renderBlock(L, R, kBlockSize);
        for (size_t i = 0; i < kBlockSize; ++i) {
            REQUIRE(std::isfinite(L[i]));
            REQUIRE(std::fabs(L[i]) < 2.0f);
        }
    }
}
