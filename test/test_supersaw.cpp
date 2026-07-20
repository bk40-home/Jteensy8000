// =============================================================================
// test_supersaw.cpp — proofs for core/dsp/SupersawOsc (Pass 4, Step 2)
// =============================================================================
// Pins the ported v1 behaviour: unison at zero detune, non-periodicity when
// spread, the Szabó gain curves at their measured endpoints, HPF DC removal,
// shared-FM pitch response, and seeded determinism.
// =============================================================================
#include "doctest.h"

#include <cmath>
#include <vector>
#include <initializer_list>

#include "core/dsp/SupersawOsc.h"
#include "core/AudioConfig.h"

using namespace JT;

namespace {

std::vector<float> renderN(SupersawOsc& o, size_t total,
                           const float* fm = nullptr, float oct = 0.0f)
{
    std::vector<float> v(total);
    for (size_t off = 0; off < total; off += kBlockSize) {
        const size_t chunk = (total - off < kBlockSize) ? total - off : kBlockSize;
        o.render(v.data() + off, chunk, fm, oct, nullptr);
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

// Normalised mismatch between the signal and itself one period later —
// ~0 for a periodic signal, order-1 for a detuned swarm.
double periodicity(const std::vector<float>& v, size_t period)
{
    double diff = 0.0, energy = 1e-12;
    for (size_t i = 1000; i + period < v.size(); ++i) {
        const double d = (double)v[i] - (double)v[i + period];
        diff   += d * d;
        energy += (double)v[i] * (double)v[i];
    }
    return diff / energy;
}

} // namespace

TEST_CASE("detune 0 is true unison: periodic at the centre frequency")
{
    SupersawOsc o;
    o.seedNoise(42);
    o.setFrequency(kSampleRate / 100.0f);     // exactly 100-sample period
    o.setDetune(0.0f);
    o.setMix(0.5f);
    o.noteOn();
    const auto v = renderN(o, 44160);
    CHECK(periodicity(v, 100) < 0.001);       // 7 voices collapse to one saw
}

TEST_CASE("detune spreads the swarm: no longer periodic, crossings rise")
{
    SupersawOsc lo, hi;
    lo.seedNoise(42); hi.seedNoise(42);
    lo.setFrequency(kSampleRate / 100.0f);
    hi.setFrequency(kSampleRate / 100.0f);
    lo.setDetune(0.0f);
    hi.setDetune(1.0f);
    hi.setMix(0.9f);                          // sides audible
    lo.noteOn(); hi.noteOn();

    const auto vLo = renderN(lo, 44160);
    const auto vHi = renderN(hi, 44160);
    CHECK(periodicity(vHi, 100) > 0.05);      // beats break the period

    // Detuned sides run up to ±11% of centre — total positive-going
    // crossings must exceed the unison count noticeably.
    CHECK(risingCrossings(vHi) > risingCrossings(vLo) + 20);
}

TEST_CASE("mix knob: level stays compensated-flat; side swarm emerges")
{
    // The Szabó gain curves drop the centre while raising the sides, and
    // random-phase summation adds side POWER, not amplitude — so absolute
    // RMS is roughly flat by design (that is what mix compensation is FOR).
    // Pin that stability band (measured from the port: 0.32..0.41 over the
    // whole grid) rather than a naive coherent-sum prediction.
    auto rmsAt = [](float mix, float det) {
        SupersawOsc o;
        o.seedNoise(7);
        o.setFrequency(441.0f);
        o.setDetune(det);
        o.setMix(mix);
        o.noteOn();
        const auto v = renderN(o, 44160);
        double acc = 0.0;
        for (float s : v) acc += (double)s * (double)s;
        return (float)std::sqrt(acc / (double)v.size());
    };
    for (float mix : { 0.0f, 0.5f, 1.0f })
        for (float det : { 0.0f, 1.0f }) {
            CAPTURE(mix); CAPTURE(det);
            const float r = rmsAt(mix, det);
            CHECK(r > 0.25f);
            CHECK(r < 0.55f);
        }

    // The AUDIBLE meaning of mix, pinned via periodicity at full detune:
    // mix=0 -> sides ~ -43 dB each, output near-periodic at the centre;
    // mix=1 -> the swarm dominates and periodicity collapses.
    auto periodicityAt = [](float mix) {
        SupersawOsc o;
        o.seedNoise(9);
        o.setFrequency(kSampleRate / 100.0f);
        o.setDetune(1.0f);
        o.setMix(mix);
        o.noteOn();
        return periodicity(renderN(o, 44160), 100);
    };
    CHECK(periodicityAt(0.0f) < 0.02);
    CHECK(periodicityAt(1.0f) > 0.20);
}

TEST_CASE("pitch-tracked HPF removes DC from the naive-saw sum")
{
    SupersawOsc o;
    o.seedNoise(3);
    o.setFrequency(110.0f);
    o.setDetune(0.7f);
    o.setMix(0.8f);
    o.noteOn();
    const auto v = renderN(o, 44160);
    double mean = 0.0;
    for (float s : v) mean += (double)s;
    mean /= (double)v.size();
    CHECK(std::fabs(mean) < 0.01);            // sub-fundamental content gone
}

TEST_CASE("shared FM: +1 octave doubles the unison rate (fastPow2 path)")
{
    SupersawOsc o;
    o.seedNoise(5);
    o.setFrequency(220.0f);
    o.setDetune(0.0f);
    o.setMix(0.0f);
    o.noteOn();
    float fm[kBlockSize];
    for (float& f : fm) f = 0.1f;             // +1 octave at 10-oct range
    const auto v = renderN(o, 44160, fm, 10.0f);
    const size_t z = risingCrossings(v);
    CHECK(z >= 437);
    CHECK(z <= 443);
}

TEST_CASE("seeded determinism: same seed identical, different seed different")
{
    auto run = [](uint32_t seed) {
        SupersawOsc o;
        o.seedNoise(seed);
        o.setFrequency(441.0f);
        o.setDetune(0.9f);
        o.setMix(0.9f);
        o.noteOn();
        return renderN(o, 4096);
    };
    const auto a = run(1234), b = run(1234), c = run(5678);
    REQUIRE(a == b);                          // bit-exact reproducibility
    CHECK(a != c);                            // and the phases really vary
}

TEST_CASE("supersaw abuse: parameter thrash stays finite and clipped to ±1")
{
    SupersawOsc o;
    uint32_t rng = 11;
    float out[kBlockSize];
    for (int b = 0; b < 2000; ++b) {
        rng = rng * 1664525u + 1013904223u;
        if ((rng & 3u) == 0) o.setFrequency((float)((rng >> 8) % 18000u));
        if ((rng & 3u) == 1) o.setDetune((float)((rng >> 8) & 255u) / 255.0f);
        if ((rng & 3u) == 2) o.setMix((float)((rng >> 8) & 255u) / 255.0f);
        if ((rng & 7u) == 3) o.noteOn();
        o.render(out, kBlockSize, nullptr, 0.0f, nullptr);
        for (float s : out) {
            REQUIRE(std::isfinite(s));
            REQUIRE(std::fabs(s) <= 1.0f);    // v1's output clip guarantees
        }
    }
}
