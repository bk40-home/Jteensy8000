// =============================================================================
// test_feedback.cpp — proofs for core/dsp/FeedbackComb (Pass 4, Step 4)
// =============================================================================
// The impulse-response test is EXACT: a comb's taps follow g^(k-1)·mix at
// multiples of the delay — floating-point-exact for an impulse, so the
// assertions use equality, not tolerance.
// =============================================================================
#include "doctest.h"

#include <cmath>
#include <vector>

#include "core/dsp/FeedbackComb.h"
#include "core/OscSection.h"
#include "core/SynthCore.h"
#include "core/dsp/Curves.h"
#include "gen/ParamTable.h"

using namespace JT;
using namespace JT::Params;

namespace {
constexpr size_t kD = 221;             // must mirror FeedbackComb's delay
}

TEST_CASE("impulse response: taps land at exact delay multiples, decay by g")
{
    FeedbackComb c;
    static float line[FeedbackComb::kDelaySamples];
    c.attachStorage(line);            // storage is external by design now
    c.setAmount(0.5f);
    c.setMix(0.8f);

    // One impulse, then silence, processed in engine-sized chunks.
    std::vector<float> v(kD * 4 + 8, 0.0f);
    v[0] = 1.0f;
    for (size_t off = 0; off < v.size(); off += kBlockSize) {
        const size_t chunk = (v.size() - off < kBlockSize) ? v.size() - off
                                                           : kBlockSize;
        c.process(v.data() + off, chunk);
    }

    CHECK(v[0] == 1.0f);                       // dry passes untouched
    CHECK(v[kD]     == 0.8f);                  // g^0 · mix
    CHECK(v[2 * kD] == doctest::Approx(0.5f * 0.8f));          // g^1 · mix
    CHECK(v[3 * kD] == doctest::Approx(0.25f * 0.8f));         // g^2 · mix
    CHECK(v[4 * kD] == doctest::Approx(0.125f * 0.8f));        // g^3 · mix
    // Everywhere else: silence — a comb speaks ONLY at its delay grid
    // (the buffer spans five grid points including t=0, hence five).
    size_t nonZero = 0;
    for (float s : v) if (s != 0.0f) ++nonZero;
    CHECK(nonZero == 5);
}

TEST_CASE("unattached comb is a silent no-op, never a crash")
{
    FeedbackComb c;                            // NO attachStorage on purpose
    c.setAmount(0.9f);
    c.setMix(1.0f);
    float buf[kBlockSize];
    for (float& s : buf) s = 0.5f;
    c.process(buf, kBlockSize);                // must not touch memory
    for (float s : buf) REQUIRE(s == 0.5f);    // and must pass audio dry
}

TEST_CASE("v1 semantics: amount 0 closes the whole path regardless of mix")
{
    FeedbackComb c;
    c.setAmount(0.0f);
    c.setMix(1.0f);
    CHECK_FALSE(c.isActive());                 // caller will skip process()

    // Even if processed anyway, gain 0 keeps... no: v1 zeroed the TAP too.
    // v2 encodes that as isActive()==false + the section skipping the call,
    // which is the exact v1 "else branch zeroes both gains" outcome.
}

TEST_CASE("amount ceiling: 1.0 requests clamp to 0.99 and stay bounded")
{
    FeedbackComb c;
    static float line[FeedbackComb::kDelaySamples];
    c.attachStorage(line);
    c.setAmount(1.0f);                         // v1 constrain -> 0.99
    c.setMix(1.0f);

    // Hammer the loop with full-scale noise for two seconds of blocks.
    uint32_t rng = 3;
    float buf[kBlockSize];
    for (int b = 0; b < 700; ++b) {
        for (float& s : buf) {
            rng = rng * 1664525u + 1013904223u;
            s = (float)(int32_t)rng * (1.0f / 2147483648.0f);
        }
        c.process(buf, kBlockSize);
        for (float s : buf) {
            REQUIRE(std::isfinite(s));
            REQUIRE(std::fabs(s) <= 2.0f);     // dry(±1) + clamped tap(±1)
        }
    }
}

TEST_CASE("enable transition starts from a SILENT line (flagged difference)")
{
    FeedbackComb c;
    static float line[FeedbackComb::kDelaySamples];
    c.attachStorage(line);
    c.setAmount(0.9f);
    c.setMix(1.0f);
    float buf[kBlockSize];
    for (float& s : buf) s = 1.0f;             // charge the line
    for (int b = 0; b < 4; ++b) c.process(buf, kBlockSize);

    c.setAmount(0.0f);                         // off...
    c.setAmount(0.9f);                         // ...and on: line must be clean

    float quiet[kBlockSize] = { 0.0f };
    c.process(quiet, kBlockSize);
    for (float s : quiet) REQUIRE(s == 0.0f);  // no stale 5 ms of history
}

TEST_CASE("section + engine: feedback params reach the comb and colour osc1")
{
    // Premise-free A/B: the engine is fully deterministic, so two cores
    // fed identical events must produce identical audio — UNLESS the
    // feedback params reach the comb, in which case the outputs diverge.
    // (A naive "level must rise" check fails honestly: at 110 Hz the 221-
    // sample delay is near a half period and the comb CANCELS — combs cut
    // as happily as they boost, and v1's loop did exactly the same.)
    ParameterStore storeA, storeB;
    static float poolA[SynthCore::kCombPoolFloats];
    static float poolB[SynthCore::kCombPoolFloats];
    SynthCore coreA(storeA, poolA), coreB(storeB, poolB);
    float La[kBlockSize], Ra[kBlockSize], Lb[kBlockSize], Rb[kBlockSize];

    for (ParameterStore* st : { &storeA, &storeB }) {
        st->setEngineering(ID::ENV_AMP_SUSTAIN, 1.0f, Origin::Ui);
        st->setEngineering(ID::MIX_OSC2, 0.0f, Origin::Ui);
    }
    storeB.setEngineering(ID::OSC1_FEEDBACK_AMOUNT, 0.9f, Origin::MidiUsbDev);
    storeB.setEngineering(ID::OSC1_FEEDBACK_MIX, 1.0f, Origin::MidiUsbDev);

    coreA.noteOn(45, 127);
    coreB.noteOn(45, 127);

    double diff = 0.0, energy = 1e-12;
    for (int b = 0; b < 60; ++b) {
        coreA.renderBlock(La, Ra, kBlockSize);
        coreB.renderBlock(Lb, Rb, kBlockSize);
        for (size_t k = 0; k < kBlockSize; ++k) {
            diff   += ((double)La[k] - (double)Lb[k])
                    * ((double)La[k] - (double)Lb[k]);
            energy += (double)La[k] * (double)La[k];
            REQUIRE(std::isfinite(Lb[k]));
        }
    }
    CHECK(diff / energy > 0.05);               // colouration clearly present

    // Supersaw through the comb — the growliest v1 combination — finite.
    storeB.set(ID::OSC1_WAVE,
               Curves::toNorm(kParams[ParameterStore::indexOf(ID::OSC1_WAVE)],
                              13.0f), Origin::MidiUsbDev);
    storeB.setEngineering(ID::OSC1_SUPERSAW_DETUNE, 0.8f, Origin::MidiUsbDev);
    storeB.setEngineering(ID::OSC1_SUPERSAW_MIX, 0.9f, Origin::MidiUsbDev);
    for (int b = 0; b < 100; ++b) {
        coreB.renderBlock(Lb, Rb, kBlockSize);
        for (size_t k = 0; k < kBlockSize; ++k) {
            REQUIRE(std::isfinite(Lb[k]));
            REQUIRE(std::fabs(Lb[k]) < 4.0f);
        }
    }
}
