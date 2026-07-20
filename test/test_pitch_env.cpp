// =============================================================================
// test_pitch_env.cpp — proofs for the Pass 7 pitch-envelope modulation
// =============================================================================
// The pitch env is a third per-voice EnvGen scaled by a ±24 st depth, summed
// into the oscillators' exponential-FM input exactly as v1 summed it into the
// FM mixer.  Net effect on a unit: freq → base · 2^(envLevel·depthSemis/12).
//
// SMOOTHING (the "non-zipped" requirement): OscSection applies the block's
// pitch target as a PER-SAMPLE linear ramp from the value applied last block,
// so the frequency never STEPS at a block boundary.  These tests assert the
// frequency SHIFT numerically (rising-crossing counts over ~1 s ≈ Hz, the same
// technique as test_osc_section.cpp) and the block-level ramp scaffolding via
// the JT_TESTING debug accessors.  The perceptual zipper check is the render
// scene 11_pitch_env.wav — a 0 ms-attack pitch blip you listen to.
//
// Sine carriers throughout: a clean zero-crossing count is an unambiguous
// frequency read-out.
// =============================================================================
#include "doctest.h"

#include <cmath>
#include <vector>

#include "core/OscSection.h"
#include "core/SynthCore.h"
#include "core/dsp/OscCore.h"
#include "core/dsp/Curves.h"
#include "gen/ParamTable.h"

using namespace JT;
using namespace JT::Params;

namespace {

// One second (a touch over) of section output — enough that rising-crossing
// count reads directly as Hz.
constexpr size_t kOneSec = 44160;

std::vector<float> renderSec(OscSection& s, size_t total)
{
    std::vector<float> v(total);
    for (size_t off = 0; off < total; off += kBlockSize) {
        const size_t chunk = (total - off < kBlockSize) ? total - off : kBlockSize;
        s.render(v.data() + off, chunk);
    }
    return v;
}

// Rising zero-crossings over [begin, end) → cycles; ÷ duration = Hz.
size_t risingCrossings(const std::vector<float>& v, size_t begin, size_t end)
{
    size_t c = 0;
    for (size_t i = (begin == 0 ? 1 : begin); i < end; ++i)
        if (v[i - 1] <= 0.0f && v[i] > 0.0f) ++c;
    return c;
}
size_t risingCrossings(const std::vector<float>& v)
{
    return risingCrossings(v, 0, v.size());
}

float freqHz(const std::vector<float>& v, size_t begin, size_t end)
{
    const size_t cycles = risingCrossings(v, begin, end);
    const double secs   = (double)(end - begin) / (double)kSampleRate;
    return (float)((double)cycles / secs);
}

} // namespace

// =============================================================================
// Section level — the frequency shift and its fidelity boundaries
// =============================================================================

TEST_CASE("pitch mod: a constant offset shifts the oscillator by 2^(semis/12)")
{
    OscSection s;
    s.setWave(0, (int)Wave::Sine);              // clean crossings
    s.setMixOsc2(0.0f);                          // isolate OSC1
    s.noteOn(440.0f, 7);

    const auto base = renderSec(s, kOneSec);
    CHECK(risingCrossings(base) == doctest::Approx(440).epsilon(0.02));

    s.setPitchModSemis(12.0f);                   // +1 octave
    const auto up = renderSec(s, kOneSec);
    CHECK(risingCrossings(up) == doctest::Approx(880).epsilon(0.02));

    s.setPitchModSemis(-12.0f);                  // -1 octave
    const auto down = renderSec(s, kOneSec);
    CHECK(risingCrossings(down) == doctest::Approx(220).epsilon(0.02));

    s.setPitchModSemis(0.0f);                    // back to the note
    const auto home = renderSec(s, kOneSec);
    CHECK(risingCrossings(home) == doctest::Approx(440).epsilon(0.02));
}

TEST_CASE("pitch mod: reaches OSC2 as well as OSC1 (v1 fed both FM slots)")
{
    OscSection s;
    s.setWave(1, (int)Wave::Sine);
    s.setMixOsc1(0.0f);                          // isolate OSC2
    s.noteOn(440.0f, 7);

    s.setPitchModSemis(12.0f);
    const auto up = renderSec(s, kOneSec);
    CHECK(risingCrossings(up) == doctest::Approx(880).epsilon(0.02));
}

TEST_CASE("pitch mod: the SUB oscillator is NOT modulated (v1 fidelity)")
{
    // v1 fed the sub raw note Hz — untouched by either unit's pitch offsets AND
    // by the pitch env.  Sub sits an octave below the note: 440 → 220 Hz, and a
    // +12 st osc offset must leave it at 220.
    OscSection s;
    s.setMixOsc1(0.0f);
    s.setMixOsc2(0.0f);
    s.setMixSub(1.0f);
    s.noteOn(440.0f, 7);

    s.setPitchModSemis(12.0f);                   // would double an OSC's pitch
    const auto v = renderSec(s, kOneSec);
    CHECK(risingCrossings(v) == doctest::Approx(220).epsilon(0.02));
}

TEST_CASE("pitch mod: base pitch is untouched — mod rides the FM path, not _freq")
{
    // The target is set by setPitchModSemis but only APPLIED (with the ramp) on
    // render().  Before the first render the applied offset still reads 0 — proof
    // the mod does not instantly retune the base oscillator (applyPitchIfDirty);
    // after one block the ramp has carried it to the target.  A stepped design
    // would show the target applied immediately with no ramp state between them.
    OscSection s;
    s.setWave(0, (int)Wave::Sine);
    s.setMixOsc2(0.0f);
    s.noteOn(440.0f, 7);

    float buf[kBlockSize];
    s.setPitchModSemis(24.0f);                   // +2 octaves target
    CHECK(s.debugPitchTargetSemis() == doctest::Approx(24.0f));
    CHECK(s.debugPitchAppliedOct()  == doctest::Approx(0.0f));   // not yet applied

    s.render(buf, kBlockSize);
    CHECK(s.debugPitchAppliedOct()  == doctest::Approx(2.0f));   // ramp ended AT 24 st

    // Dropping the target back ramps down again (prev != target during the move).
    s.setPitchModSemis(0.0f);
    CHECK(s.debugPitchAppliedOct()  == doctest::Approx(2.0f));   // last block's value
    s.render(buf, kBlockSize);
    CHECK(s.debugPitchAppliedOct()  == doctest::Approx(0.0f));
}

TEST_CASE("pitch mod: idle (depth path off) leaves the section byte-unchanged")
{
    // Two sections, one that never sees a pitch call and one explicitly pushed 0:
    // both must render the identical default saw (the no-silent-change guarantee).
    OscSection a, b;
    a.noteOn(220.0f, 3);
    b.noteOn(220.0f, 3);
    b.setPitchModSemis(0.0f);                    // 0 target, 0 prev ⇒ inert path

    const auto va = renderSec(a, kBlockSize * 8);
    const auto vb = renderSec(b, kBlockSize * 8);
    for (size_t i = 0; i < va.size(); ++i)
        CHECK(va[i] == doctest::Approx(vb[i]));
}

// =============================================================================
// End-to-end — the ENV_PITCH_* params through the store drive a real pitch blip
// =============================================================================

TEST_CASE("pitch env reaches the voices via the store and blips the pitch down")
{
    // A classic "zap": full positive depth, instant attack, fast decay to sustain
    // 0.  The note starts two octaves sharp and falls to pitch — so an early
    // window must read much higher in frequency than a settled late window.
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);
    float L[kBlockSize], R[kBlockSize];

    store.setEngineering(ID::MIX_OSC2, 0.0f, Origin::Ui);          // solo OSC1
    const size_t idxW = ParameterStore::indexOf(ID::OSC1_WAVE);
    store.set(ID::OSC1_WAVE, Curves::toNorm(kParams[idxW], (float)(int)Wave::Sine),
              Origin::Ui);
    store.setEngineering(ID::ENV_AMP_ATTACK, 0.0f, Origin::Ui);
    store.setEngineering(ID::ENV_AMP_SUSTAIN, 1.0f, Origin::Ui);   // flat gain

    store.setEngineering(ID::ENV_PITCH_DEPTH, 1.0f, Origin::MidiUsbDev); // +24 st
    store.setEngineering(ID::ENV_PITCH_ATTACK, 0.0f, Origin::MidiUsbDev);
    store.setEngineering(ID::ENV_PITCH_DECAY, 40.0f, Origin::MidiUsbDev);
    store.setEngineering(ID::ENV_PITCH_SUSTAIN, 0.0f, Origin::MidiUsbDev);

    core.noteOn(69, 100);                                          // A4 = 440 Hz
    std::vector<float> mono;
    mono.reserve(kOneSec);
    while (mono.size() < kOneSec) {
        core.renderBlock(L, R, kBlockSize);
        for (size_t k = 0; k < kBlockSize; ++k) mono.push_back(L[k]);
    }

    // Early: first ~90 ms, still sharp from the decaying blip.  Late: settled.
    const float early = freqHz(mono, 0, 4000);
    const float late  = freqHz(mono, 20000, 44000);
    CHECK(late  == doctest::Approx(440.0f).epsilon(0.05));         // back to pitch
    CHECK(early > 600.0f);                                         // clearly sharp
    CHECK(early > late * 1.3f);
}
