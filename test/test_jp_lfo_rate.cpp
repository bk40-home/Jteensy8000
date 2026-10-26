// =============================================================================
// test_jp_lfo_rate.cpp — the JP-8000 LFO RATE law (lfo1/lfo2.rate_law = JpRate)
// =============================================================================
// UNIT  — JpLfoRate::hzFromNorm lands on every measured JE-8086 point
//         (lfo/lfo1-rate-capture-analysis-2026-10-25.md), is monotonic, clamps.
// INTEG — through ParameterStore -> SynthCore: JpRate reinterprets the SAME
//         LFO1_FREQ knob position, in either param order; switching back to
//         JtRate restores the JT-law Hz; sync divisions ignore the law; the
//         two LFOs switch independently; JtRate (default) is byte-identical.
// =============================================================================
#include "doctest.h"

#include <cmath>
#include <vector>

#include "core/dsp/JpLfoRate.h"
#include "core/dsp/TempoClock.h"
#include "core/SynthCore.h"
#include "core/dsp/Curves.h"
#include "gen/ParamTable.h"

using namespace JT;
using namespace JT::Params;

namespace {

void drain(SynthCore& core)
{
    float L[kBlockSize], R[kBlockSize];
    core.renderBlock(L, R, kBlockSize);
}

float optNorm(uint16_t id, float option)
{
    return Curves::toNorm(kParams[ParameterStore::indexOf(id)], option);
}

// JT law for a knob byte: 0.03 * (39 / 0.03)^(b / 127)  (params.yaml lfo1.freq).
float jtHz(float b) { return Curves::toEngineering(kParams[ParameterStore::indexOf(ID::LFO1_FREQ)], b / 127.0f); }

} // namespace

TEST_CASE("JpLfoRate: every measured JE-8086 rate is hit")
{
    struct { float rate, hz; } pts[] = {
        {0, 0.0488f}, {16, 0.1373f}, {32, 0.3889f}, {48, 1.1040f}, {64, 3.2258f},
        {72, 4.1822f}, {96, 9.0845f}, {112, 15.3631f}, {120, 19.9723f} };
    for (auto p : pts) {
        INFO("RATE ", p.rate);
        CHECK(JpLfoRate::hzFromNorm(p.rate / 127.0f) == doctest::Approx(p.hz).epsilon(0.001));
    }
    CHECK(JpLfoRate::hzFromNorm(1.0f) == doctest::Approx(25.15f).epsilon(0.001));   // extrapolated end
}

TEST_CASE("JpLfoRate: monotonic, interpolates log-linearly, clamps")
{
    float prev = 0.0f;
    for (int i = 0; i <= 1270; ++i) {
        const float hz = JpLfoRate::hzFromNorm((float)i / 1270.0f);
        CHECK(hz > prev);
        prev = hz;
    }
    // Midway between RATE 64 and 72 is the geometric mean of their rates.
    CHECK(JpLfoRate::hzFromNorm(68.0f / 127.0f) == doctest::Approx(std::sqrt(3.2258f * 4.1822f)).epsilon(0.001));
    CHECK(JpLfoRate::hzFromNorm(-1.0f) == JpLfoRate::hzFromNorm(0.0f));
    CHECK(JpLfoRate::hzFromNorm(2.0f)  == JpLfoRate::hzFromNorm(1.0f));
}

TEST_CASE("lfo1.rate_law: JpRate reads the same knob through the JP curve, either order")
{
    for (int lawFirst = 0; lawFirst < 2; ++lawFirst) {
        INFO("law written first: ", lawFirst);
        ParameterStore store;
        static float combPool[SynthCore::kCombPoolFloats];
        SynthCore core(store, combPool);
        if (lawFirst) { store.set(ID::LFO1_RATE_LAW, optNorm(ID::LFO1_RATE_LAW, 1.0f), Origin::Ui); drain(core); }
        store.set(ID::LFO1_FREQ, 64.0f / 127.0f, Origin::Ui);
        if (!lawFirst) { drain(core); store.set(ID::LFO1_RATE_LAW, optNorm(ID::LFO1_RATE_LAW, 1.0f), Origin::Ui); }
        drain(core);
        CHECK(core.debugLfoRateHz(0) == doctest::Approx(3.2258f).epsilon(0.001));

        // Back to JtRate: the JT law at the same knob (0.03 * 1300^(64/127) = 1.11 Hz).
        store.set(ID::LFO1_RATE_LAW, optNorm(ID::LFO1_RATE_LAW, 0.0f), Origin::Ui);
        drain(core);
        CHECK(core.debugLfoRateHz(0) == doctest::Approx(jtHz(64.0f)));
        CHECK(core.debugLfoRateHz(0) == doctest::Approx(1.1126f).epsilon(0.001));
    }
}

TEST_CASE("lfo1.rate_law: sync divisions ignore the law; each LFO has its own switch")
{
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);
    store.setEngineering(ID::CLOCK_TEMPO, 120.0f, Origin::Ui);
    store.set(ID::LFO1_RATE_LAW, optNorm(ID::LFO1_RATE_LAW, 1.0f), Origin::Ui);
    store.set(ID::LFO1_FREQ, 96.0f / 127.0f, Origin::Ui);
    store.set(ID::LFO2_FREQ, 96.0f / 127.0f, Origin::Ui);
    store.set(ID::LFO1_SYNC, optNorm(ID::LFO1_SYNC, (float)TempoClock::k1_4), Origin::Ui);
    drain(core);
    CHECK(core.debugLfoRateHz(0) == doctest::Approx(2.0f));                  // 1/4 @ 120
    CHECK(core.debugLfoRateHz(1) == doctest::Approx(jtHz(96.0f)));           // LFO2 still JtRate

    store.set(ID::LFO1_SYNC, optNorm(ID::LFO1_SYNC, (float)TempoClock::kFree), Origin::Ui);
    drain(core);
    CHECK(core.debugLfoRateHz(0) == doctest::Approx(9.0845f).epsilon(0.001)); // free: JP law
}

TEST_CASE("lfo2.rate_law: JpRate on LFO2, either order, sync unaffected")
{
    for (int lawFirst = 0; lawFirst < 2; ++lawFirst) {
        INFO("law written first: ", lawFirst);
        ParameterStore store;
        static float combPool[SynthCore::kCombPoolFloats];
        SynthCore core(store, combPool);
        store.setEngineering(ID::CLOCK_TEMPO, 120.0f, Origin::Ui);
        if (lawFirst) { store.set(ID::LFO2_RATE_LAW, optNorm(ID::LFO2_RATE_LAW, 1.0f), Origin::Ui); drain(core); }
        store.set(ID::LFO2_FREQ, 64.0f / 127.0f, Origin::Ui);
        store.set(ID::LFO1_FREQ, 64.0f / 127.0f, Origin::Ui);
        if (!lawFirst) { drain(core); store.set(ID::LFO2_RATE_LAW, optNorm(ID::LFO2_RATE_LAW, 1.0f), Origin::Ui); }
        drain(core);
        CHECK(core.debugLfoRateHz(1) == doctest::Approx(3.2258f).epsilon(0.001)); // LFO2: JP law
        CHECK(core.debugLfoRateHz(0) == doctest::Approx(jtHz(64.0f)));            // LFO1 untouched

        store.set(ID::LFO2_SYNC, optNorm(ID::LFO2_SYNC, (float)TempoClock::k1_8), Origin::Ui);
        drain(core);
        CHECK(core.debugLfoRateHz(1) == doctest::Approx(4.0f));                   // 1/8 @ 120

        store.set(ID::LFO2_SYNC, optNorm(ID::LFO2_SYNC, (float)TempoClock::kFree), Origin::Ui);
        store.set(ID::LFO2_RATE_LAW, optNorm(ID::LFO2_RATE_LAW, 0.0f), Origin::Ui);
        drain(core);
        CHECK(core.debugLfoRateHz(1) == doctest::Approx(jtHz(64.0f)));            // back to JT
    }
}

TEST_CASE("lfo1/lfo2.rate_law: JtRate (default) renders byte-identically")
{
    // LFO1 engaged on pitch so a rate change would be audible; writing the
    // default law explicitly must not move a single sample.
    auto render = [](bool writeLaw) {
        ParameterStore store;
        static float combPool[SynthCore::kCombPoolFloats];
        SynthCore core(store, combPool);
        store.set(ID::LFO1_FREQ, 70.0f / 127.0f, Origin::Ui);
        store.set(ID::LFO2_FREQ, 50.0f / 127.0f, Origin::Ui);
        store.setEngineering(ID::LFO1_PITCH_DEPTH, 0.5f, Origin::Ui);
        store.setEngineering(ID::LFO2_FILTER_DEPTH, 0.5f, Origin::Ui);
        store.setEngineering(ID::LFO2_MOD_WHEEL, 0.0f, Origin::Ui);   // LFO2 live without the wheel
        if (writeLaw) {
            store.set(ID::LFO1_RATE_LAW, optNorm(ID::LFO1_RATE_LAW, 0.0f), Origin::Ui);
            store.set(ID::LFO2_RATE_LAW, optNorm(ID::LFO2_RATE_LAW, 0.0f), Origin::Ui);
        }
        core.noteOn(57, 100);
        std::vector<float> out;
        float L[kBlockSize], R[kBlockSize];
        for (int b = 0; b < 200; ++b) {
            core.renderBlock(L, R, kBlockSize);
            for (size_t k = 0; k < kBlockSize; ++k) { out.push_back(L[k]); out.push_back(R[k]); }
        }
        return out;
    };
    const auto a = render(false);
    const auto b = render(true);
    REQUIRE(a.size() == b.size());
    size_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff += (a[i] != b[i]);
    CHECK(diff == 0);
}
