// =============================================================================
// test_filter_env.cpp — proofs for the Pass 6 filter-cutoff modulation
// =============================================================================
// The v1 LIVE mod-bus math (verified in VoiceBlock + FilterBlock +
// AudioFilter*::update), reproduced by FilterSection at block rate:
//     keyDC  = clamp( (log2(f/440)/octaveCtrl) · keyTrackAmt, -1, +1 )
//     envDC  = envAmount · envLevel
//     modOct = (keyDC + envDC) · octaveCtrl
//     cutoff = cutoffBase · 2^modOct
// The JT_TESTING debug accessors expose the effective cutoff and modOct so the
// math is asserted directly, not inferred from output spectra.  One end-to-end
// test drives the whole path through the ParameterStore.
//
// Base engine for the exact-ratio tests is OBXa (the section default): its base
// cutoff IS the Hz passed to setCutoff, so expected ratios are hand-computable
// without re-deriving the VA per-type shaping curve.
// =============================================================================
#include "doctest.h"

#include <cmath>
#include <initializer_list>

#include "core/FilterSection.h"
#include "core/SynthCore.h"
#include "core/dsp/Curves.h"
#include "gen/ParamTable.h"

using namespace JT;
using namespace JT::Params;

namespace {

// Read the effective cutoff after one processed block at a given env level.
// buf is silence — process() only needs to run to refresh the coefficients.
float cutoffAtEnv(FilterSection& f, float env01)
{
    float buf[kBlockSize] = { 0.0f };
    f.setEnvLevel(env01);
    f.process(buf, kBlockSize);
    return f.debugCutoffHz();
}

} // namespace

TEST_CASE("filter env: positive amount opens the cutoff by 2^(amount·octaves)")
{
    FilterSection f;                        // OBXa default: base cutoff == Hz
    f.setCutoff(0.5f, 500.0f);
    f.setKeyTrackAmount(0.0f);
    f.setOctaveControl(1.0f);               // 1 octave of depth
    f.setEnvAmount(1.0f);                    // full positive depth

    const float base = cutoffAtEnv(f, 0.0f);
    CHECK(base == doctest::Approx(500.0f).epsilon(0.02));   // env at 0 ⇒ no shift

    const float open = cutoffAtEnv(f, 1.0f);
    // modOct = (0 + 1·1)·1 = 1 octave ⇒ cutoff doubles.
    CHECK(open == doctest::Approx(base * 2.0f).epsilon(0.01));
    CHECK(f.debugModOctaves() == doctest::Approx(1.0f).epsilon(1e-4));

    // Half env level ⇒ half an octave ⇒ ×√2.
    const float half = cutoffAtEnv(f, 0.5f);
    CHECK(half == doctest::Approx(base * std::sqrt(2.0f)).epsilon(0.01));
}

TEST_CASE("filter env: negative amount inverts (closes the cutoff)")
{
    FilterSection f;
    f.setCutoff(0.5f, 500.0f);
    f.setKeyTrackAmount(0.0f);
    f.setOctaveControl(1.0f);
    f.setEnvAmount(-1.0f);                   // inverted envelope

    const float base = cutoffAtEnv(f, 0.0f);
    const float shut = cutoffAtEnv(f, 1.0f);
    // modOct = (0 + (-1)·1)·1 = -1 octave ⇒ cutoff halves.
    CHECK(shut == doctest::Approx(base * 0.5f).epsilon(0.01));
    CHECK(shut < base);
}

TEST_CASE("filter env: octave-control scales the modulation depth")
{
    // Same env, twice the octave control ⇒ twice the octaves ⇒ ratio squares.
    FilterSection a, b;
    for (FilterSection* f : { &a, &b }) {
        f->setCutoff(0.5f, 500.0f);
        f->setKeyTrackAmount(0.0f);
        f->setEnvAmount(1.0f);
    }
    a.setOctaveControl(2.0f);
    b.setOctaveControl(4.0f);

    const float aTop = cutoffAtEnv(a, 1.0f);   // modOct = 2 ⇒ ×4
    const float bTop = cutoffAtEnv(b, 1.0f);   // modOct = 4 ⇒ ×16
    CHECK(a.debugModOctaves() == doctest::Approx(2.0f).epsilon(1e-4));
    CHECK(b.debugModOctaves() == doctest::Approx(4.0f).epsilon(1e-4));
    CHECK(aTop == doctest::Approx(500.0f * 4.0f).epsilon(0.01));
    CHECK(bTop == doctest::Approx(500.0f * 16.0f).epsilon(0.01));
}

TEST_CASE("filter env: octave-control 0 (and env-amount 0) make the mod inert")
{
    FilterSection f;
    f.setCutoff(0.5f, 500.0f);
    f.setEnvAmount(1.0f);
    f.setKeyTrackAmount(1.0f);
    f.setOctaveControl(0.0f);                // master depth 0 ⇒ nothing moves
    f.noteOn(96);                            // a high note — keytrack still inert
    CHECK(cutoffAtEnv(f, 0.0f) == doctest::Approx(500.0f).epsilon(0.02));
    CHECK(cutoffAtEnv(f, 1.0f) == doctest::Approx(500.0f).epsilon(0.02));
    CHECK(f.debugModOctaves() == doctest::Approx(0.0f));

    // Depth on, but env amount 0 ⇒ env contributes nothing.
    FilterSection g;
    g.setCutoff(0.5f, 500.0f);
    g.setEnvAmount(0.0f);
    g.setKeyTrackAmount(0.0f);
    g.setOctaveControl(4.0f);
    CHECK(cutoffAtEnv(g, 1.0f) == doctest::Approx(500.0f).epsilon(0.02));
}

TEST_CASE("key tracking: pivot is A4 (note 69), full amount ≈ 1 V/oct")
{
    FilterSection f;
    f.setCutoff(0.5f, 500.0f);
    f.setEnvAmount(0.0f);                    // isolate key tracking
    f.setKeyTrackAmount(1.0f);
    f.setOctaveControl(10.0f);               // large ⇒ ±1 oct won't hit the clamp

    f.noteOn(69);                            // A4 — the pivot ⇒ no shift
    CHECK(cutoffAtEnv(f, 0.0f) == doctest::Approx(500.0f).epsilon(0.02));

    f.noteOn(81);                            // A5, +1 octave ⇒ cutoff ×2
    CHECK(cutoffAtEnv(f, 0.0f) == doctest::Approx(1000.0f).epsilon(0.02));

    f.noteOn(57);                            // A3, -1 octave ⇒ cutoff ÷2
    CHECK(cutoffAtEnv(f, 0.0f) == doctest::Approx(250.0f).epsilon(0.02));
}

TEST_CASE("key tracking: contribution is capped at ±octaveControl octaves")
{
    // v1 clamps the (divided) keytrack DC to ±1 BEFORE re-scaling by octaveCtrl,
    // so the keytrack shift can never exceed ±octaveCtrl octaves however far the
    // note is from the pivot.
    FilterSection f;
    f.setCutoff(0.5f, 500.0f);
    f.setEnvAmount(0.0f);
    f.setKeyTrackAmount(1.0f);
    f.setOctaveControl(2.0f);                // cap = ±2 octaves

    f.noteOn(127);                           // way above pivot: would be +4.83 oct
    CHECK(cutoffAtEnv(f, 0.0f) == doctest::Approx(500.0f * 4.0f).epsilon(0.02));
    CHECK(f.debugModOctaves() == doctest::Approx(2.0f).epsilon(1e-4));   // capped

    f.noteOn(0);                             // way below pivot
    CHECK(cutoffAtEnv(f, 0.0f) == doctest::Approx(500.0f / 4.0f).epsilon(0.02));
    CHECK(f.debugModOctaves() == doctest::Approx(-2.0f).epsilon(1e-4));
}

TEST_CASE("key tracking + env sum on the same bus (v1 mod-bus behaviour)")
{
    // Both sources add in octave space before the 2^ conversion.
    FilterSection f;
    f.setCutoff(0.5f, 500.0f);
    f.setOctaveControl(4.0f);
    f.setKeyTrackAmount(1.0f);
    f.setEnvAmount(1.0f);
    f.noteOn(81);                            // +1 oct of pitch

    // keyDC = clamp((1/4)·1,±1) = 0.25 ; envDC = 1·1 = 1 ; modOct = (1.25)·4 = 5
    CHECK(cutoffAtEnv(f, 1.0f) != doctest::Approx(500.0f));
    CHECK(f.debugModOctaves() == doctest::Approx(5.0f).epsilon(1e-3));
}

TEST_CASE("filter env reaches the voices via the store and sweeps the output")
{
    // End-to-end: a low base cutoff with a positive filter env should let more
    // energy through over the note than the same patch with env amount 0 (amp
    // env identical in both, so the difference isolates the cutoff sweep).
    auto energyWithEnvAmount = [](float envAmt) {
        ParameterStore store;
        static float combPool[SynthCore::kCombPoolFloats];
        SynthCore core(store, combPool);
        float L[kBlockSize], R[kBlockSize];

        store.setEngineering(ID::MIX_OSC2, 0.0f, Origin::Ui);
        store.setEngineering(ID::ENV_AMP_SUSTAIN, 1.0f, Origin::Ui);   // hold level
        store.set(ID::FILTER_ENGINE, 1.0f, Origin::MidiUsbDev);             // VA
        // Dark base so the env has clear room to open the filter.
        store.setEngineering(ID::FILTER_CUTOFF, 200.0f, Origin::MidiUsbDev);
        store.setEngineering(ID::FILTER_OCTAVE_CONTROL, 0.4f, Origin::MidiUsbDev);
        store.setEngineering(ID::FILTER_ENV_AMOUNT, envAmt, Origin::MidiUsbDev);
        // A slow-ish filter env so the sweep spans several blocks.
        store.setEngineering(ID::ENV_FILTER_ATTACK, 5.0f, Origin::MidiUsbDev);
        store.setEngineering(ID::ENV_FILTER_DECAY, 200.0f, Origin::MidiUsbDev);
        store.setEngineering(ID::ENV_FILTER_SUSTAIN, 1.0f, Origin::MidiUsbDev);

        core.noteOn(45, 127);
        double acc = 0.0;
        for (int b = 0; b < 60; ++b) {
            core.renderBlock(L, R, kBlockSize);
            for (size_t k = 0; k < kBlockSize; ++k) acc += (double)L[k] * (double)L[k];
        }
        return acc;
    };

    const double flat  = energyWithEnvAmount(0.0f);   // cutoff parked dark
    const double swept = energyWithEnvAmount(1.0f);    // env opens the filter
    CHECK(swept > flat * 1.2);
    CHECK(flat > 0.0);
}
