// =============================================================================
// test_osc_section.cpp — proofs for core/OscSection + SynthCore wiring
// =============================================================================
// Two layers: the section driven directly (source mixing, ring, sync, sub,
// x-mod, balance), then the same behaviours driven THROUGH the ParameterStore
// to prove the applyParam switch and option-index conversions.
// =============================================================================
#include "doctest.h"

#include <cmath>
#include <vector>

#include "core/OscSection.h"
#include "core/SynthCore.h"
#include "core/dsp/Curves.h"
#include "gen/ParamTable.h"

using namespace JT;
using namespace JT::Params;

namespace {

std::vector<float> renderSec(OscSection& s, size_t total)
{
    std::vector<float> v(total);
    for (size_t off = 0; off < total; off += kBlockSize) {
        const size_t chunk = (total - off < kBlockSize) ? total - off : kBlockSize;
        s.render(v.data() + off, chunk);
    }
    return v;
}

size_t risingCrossings(const std::vector<float>& v)
{
    size_t c = 0;
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i - 1] <= 0.0f && v[i] > 0.0f) ++c;
    return c;
}

float rms(const std::vector<float>& v)
{
    double acc = 0.0;
    for (float s : v) acc += (double)s * (double)s;
    return (float)std::sqrt(acc / (double)v.size());
}

double periodicity(const std::vector<float>& v, size_t period)
{
    double diff = 0.0, energy = 1e-12;
    for (size_t i = 500; i + period < v.size(); ++i) {
        const double d = (double)v[i] - (double)v[i + period];
        diff   += d * d;
        energy += (double)v[i] * (double)v[i];
    }
    return diff / energy;
}

} // namespace

// =============================================================================
// Section level
// =============================================================================

TEST_CASE("section defaults match the v1 boot patch: BOTH oscillators on")
{
    // The table (and v1) boot with osc1 AND osc2 at 0.787 in unison —
    // random phases mean the pair's RMS is statistical, so pin frequency
    // exactly and level loosely, then pin the true-solo level.
    OscSection s;
    s.noteOn(441.0f, 99);
    const auto v = renderSec(s, 44160);
    CHECK(risingCrossings(v) == doctest::Approx(441).epsilon(0.01));
    CHECK(rms(v) > 0.2f);
    CHECK(rms(v) < 1.0f);

    s.setMixOsc2(0.0f);                         // now a genuine solo saw
    const auto solo = renderSec(s, 44160);
    CHECK(rms(solo) == doctest::Approx(0.577f * 0.787402f).epsilon(0.02));
}

TEST_CASE("dual osc: detuned second oscillator beats against the first")
{
    OscSection s;
    s.noteOn(kSampleRate / 100.0f, 5);          // both units on by default
    const auto clean = renderSec(s, 44160);
    CHECK(periodicity(clean, 100) < 0.01);      // in tune: periodic

    s.setDetuneSemis(1, 0.3f);                  // 30 cents up on OSC2
    const auto beat = renderSec(s, 44160);
    CHECK(periodicity(beat, 100) > 0.05);       // beating breaks the period
}

TEST_CASE("pitch offsets: -12 halves OSC1's rate; fine tune nudges it")
{
    OscSection s;
    s.noteOn(441.0f, 5);
    s.setMixOsc2(0.0f);                         // isolate OSC1
    s.setPitchOffset(0, 1);                     // option 1 = "-12"
    const auto v = renderSec(s, 44160);
    CHECK(risingCrossings(v) == doctest::Approx(220).epsilon(0.02));

    s.setPitchOffset(0, 2);                     // back to "0"
    s.setFineTuneCents(0, 100.0f);              // +100 cents = +1 semitone
    const auto w = renderSec(s, 44160);
    CHECK(risingCrossings(w) == doctest::Approx(467).epsilon(0.02));  // 441×2^(1/12)
}

TEST_CASE("sub: sine one octave below the note, with the v1 0.9 headroom")
{
    OscSection s;
    s.noteOn(441.0f, 5);
    s.setMixOsc1(0.0f);                         // sub alone
    s.setMixOsc2(0.0f);
    s.setMixSub(1.0f);
    const auto v = renderSec(s, 44160);
    CHECK(risingCrossings(v) == doctest::Approx(220).epsilon(0.02));
    // Sine RMS 0.707 × level 1.0 × headroom 0.9 ≈ 0.636.
    CHECK(rms(v) == doctest::Approx(0.7071f * 0.9f).epsilon(0.03));
}

TEST_CASE("noise: flat-ish, bounded, level-scaled; silent at zero")
{
    OscSection s;
    s.noteOn(441.0f, 5);
    s.setMixOsc1(0.0f);
    s.setMixOsc2(0.0f);
    s.setMixNoise(0.5f);
    const auto v = renderSec(s, 44160);
    CHECK(rms(v) == doctest::Approx(0.5f / std::sqrt(3.0f)).epsilon(0.05));
    for (float x : v) REQUIRE(std::fabs(x) <= 0.5f);
}

TEST_CASE("ring: two sines make sum and difference tones, not the inputs")
{
    // 300 Hz × 441 Hz ring product = 141 Hz + 741 Hz components ONLY.
    // With input mixes at zero the fundamental count reflects the product:
    // zero crossings of cos(2π·141t)−cos(2π·741t) style signals land well
    // above either input alone — pin the energy and the v1 gain-sum rule.
    OscSection a;
    a.noteOn(441.0f, 5);
    a.setWave(0, (int)Wave::Sine);
    a.setWave(1, (int)Wave::Sine);
    a.setDetuneSemis(1, -6.6603f);              // 441 → ~300 Hz
    a.setMixOsc1(0.0f);
    a.setMixOsc2(0.0f);
    a.setRingMix(0, 0.5f);
    const auto v = renderSec(a, 44160);
    // Product of ±1 sines: RMS = 0.5 × ringGain.
    CHECK(rms(v) == doctest::Approx(0.5f * 0.5f).epsilon(0.05));

    // v1 rule: the two ring params SUM (identical products in v1's mixer).
    a.setRingMix(1, 0.5f);                      // now 0.5 + 0.5
    const auto w = renderSec(a, 44160);
    CHECK(rms(w) == doctest::Approx(0.5f * 1.0f).epsilon(0.05));
}

TEST_CASE("hard sync: OSC1 locks to OSC2's period; supersaw master disables")
{
    OscSection s;
    s.noteOn(kSampleRate / 100.0f, 5);          // OSC2 master: 100 samples
    s.setDetuneSemis(0, 7.0f);                  // OSC1 free rate ≠ master
    s.setSyncEnabled(true);
    s.setMixOsc2(0.0f);                         // listen to the slave only
                                                // (master keeps rendering:
                                                // sync demands it)
    const auto v = renderSec(s, 44160);
    CHECK(periodicity(v, 100) < 0.02);          // locked to the master

    s.setWave(1, (int)Wave::Supersaw);          // master can't sync now
    const auto w = renderSec(s, 44160);
    CHECK(periodicity(w, 100) > 0.05);          // slave free-runs again
}

TEST_CASE("cross-mod: depth widens the spectrum (periodicity collapses)")
{
    OscSection s;
    s.noteOn(kSampleRate / 100.0f, 5);
    s.setDetuneSemis(1, 3.37f);                 // inharmonic modulator
    s.setMixOsc2(0.0f);                         // modulator inaudible itself
    const auto dry = renderSec(s, 44160);
    CHECK(periodicity(dry, 100) < 0.01);

    s.setCrossMod(0.3f);                        // ±3 octaves of exp FM
    const auto fm = renderSec(s, 44160);
    CHECK(periodicity(fm, 100) > 0.10);
    for (float x : fm) REQUIRE(std::isfinite(x));
}

TEST_CASE("balance: v1 crossfade law composed with (not overwriting) mixes")
{
    OscSection s;
    s.noteOn(441.0f, 5);
    s.setDetuneSemis(1, 0.2f);                  // both on (table default)
    const float both = rms(renderSec(s, 44160));

    s.setBalance(-1.0f);                        // fully OSC1
    const float osc1Only = rms(renderSec(s, 44160));
    s.setBalance(1.0f);                         // fully OSC2
    const float osc2Only = rms(renderSec(s, 44160));

    CHECK(osc1Only == doctest::Approx(0.577f * 0.787402f).epsilon(0.03));
    CHECK(osc2Only == doctest::Approx(0.577f * 0.787402f).epsilon(0.03));
    CHECK(both > osc1Only * 1.15f);             // centre carries both
}

TEST_CASE("supersaw on BOTH units renders and stays bounded")
{
    OscSection s;
    s.noteOn(110.0f, 77);
    s.setWave(0, (int)Wave::Supersaw);
    s.setWave(1, (int)Wave::Supersaw);
    s.setMixOsc2(0.7f);
    s.setSupersawDetune(0, 0.8f); s.setSupersawMix(0, 0.9f);
    s.setSupersawDetune(1, 0.6f); s.setSupersawMix(1, 0.7f);
    s.setDetuneSemis(1, -12.0f);                // octave-down second swarm
    const auto v = renderSec(s, 44160);
    CHECK(rms(v) > 0.1f);
    for (float x : v) {
        REQUIRE(std::isfinite(x));
        REQUIRE(std::fabs(x) < 2.0f);
    }
}

// =============================================================================
// Engine level: the same behaviours THROUGH the store — proves the
// applyParam switch, engineering conversions and option-index mapping.
// =============================================================================

TEST_CASE("engine wiring: waves, mixes, sync and supersaw via store writes")
{
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);
    float L[kBlockSize], R[kBlockSize];

    // RMS over the WHOLE window, not the last block: detuned sources BEAT,
    // and a single-block sample can land in a destructive trough (found the
    // hard way — the first version of this test measured a beating pair at
    // exactly the wrong instant and read near-silence).
    auto run = [&](int blocks) {
        double acc = 0.0;
        for (int i = 0; i < blocks; ++i) {
            core.renderBlock(L, R, kBlockSize);
            for (size_t k = 0; k < kBlockSize; ++k)
                acc += (double)L[k] * (double)L[k];
        }
        return (float)std::sqrt(acc / (double)(blocks * (int)kBlockSize));
    };

    // Hold the amp envelope fully open: the default patch decays toward a
    // low sustain, which would otherwise dominate successive RMS windows.
    store.setEngineering(ID::ENV_AMP_SUSTAIN, 1.0f, Origin::Ui);
    store.setEngineering(ID::ENV_AMP_DECAY, 5000.0f, Origin::Ui);

    // TABLE DEFAULT has osc2 already on (v1 boot patch) — zero it first
    // so the baseline really is one oscillator.
    store.setEngineering(ID::MIX_OSC2, 0.0f, Origin::MidiUsbDev);
    core.noteOn(57, 127);                                   // A3
    const float saw = run(30);
    CHECK(saw > 0.01f);

    // Enable OSC2 with detune (both via engineering-unit setters).
    store.setEngineering(ID::MIX_OSC2, 0.787402f, Origin::MidiUsbDev);
    // 3.6 st detune -> ~50 Hz beat at A3: several full beat cycles per
    // 30-block window, so the averaged RMS is stable.
    store.setEngineering(ID::OSC2_DETUNE, 0.3f, Origin::MidiUsbDev);
    const float dual = run(30);
    CHECK(dual > saw * 1.1f);                               // second source

    // Switch OSC1 to supersaw by OPTION INDEX through the select curve.
    store.set(ID::OSC1_WAVE,
              Curves::toNorm(kParams[ParameterStore::indexOf(ID::OSC1_WAVE)],
                             (float)Wave::Supersaw),
              Origin::MidiUsbDev);
    store.setEngineering(ID::OSC1_SUPERSAW_DETUNE, 0.8f, Origin::MidiUsbDev);
    store.setEngineering(ID::OSC1_SUPERSAW_MIX, 0.9f, Origin::MidiUsbDev);
    const float swarm = run(60);
    CHECK(swarm > 0.05f);

    // Sync toggle exercises the bool path end to end.
    store.set(ID::MIX_OSC_SYNC, 1.0f, Origin::MidiUsbDev);
    const float synced = run(30);
    CHECK(synced > 0.0f);

    // And the panic path still silences the grown voice.
    core.allSoundOff();
    core.renderBlock(L, R, kBlockSize);
    core.renderBlock(L, R, kBlockSize);
    for (size_t k = 0; k < kBlockSize; ++k) REQUIRE(L[k] == 0.0f);
}
