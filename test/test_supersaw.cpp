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

    // LESSON (2026-10-22, JP mix law 1b): this used to assert that rising
    // zero-crossings exceed the unison count by 20.  That premise held only
    // while v1 split the side gain by 6 (quiet sides = small wobbles adding
    // crossings to a dominant centre saw).  With the JP per-voice law the
    // swarm's mean frequency is still f0, so crossings do NOT rise — the DSP
    // is right, the proxy was wrong.  Pin the physics directly instead: the
    // lowest voice (−11.0 %) must be present at harmonic 4 in the detuned
    // render and absent in the unison one.
    auto lowVoiceAmp = [](const std::vector<float>& v) {
        const double w = 2.0 * 3.14159265358979 * 4.0 * ((double)kSampleRate / 100.0)
                       * (1.0 - 0.11002313) / (double)kSampleRate;
        double re = 0.0, im = 0.0;
        for (size_t i = 0; i < 44100; ++i) {
            const double t = (double)i / 44099.0;
            const double win = 0.42 - 0.5 * std::cos(6.28318530718 * t) + 0.08 * std::cos(12.5663706144 * t);
            re += (double)v[i] * win * std::cos(w * (double)i);
            im -= (double)v[i] * win * std::sin(w * (double)i);
        }
        return std::sqrt(re * re + im * im);
    };
    CHECK(lowVoiceAmp(vHi) > 20.0 * lowVoiceAmp(vLo));
}

TEST_CASE("mix knob: JP-8000 law - side/centre amplitude follows the JE-8086")
{
    // 2026-10-22 sign-off 1b: each side voice gets the FULL Szabó side gain
    // (v1 divided it by 6).  Reference = side/centre amplitude measured from
    // a JE-8086 capture at full detune, mix 0..1 (osc-shapes doc, §5).
    // Measured the same way here: single-bin DFTs at harmonic 4 of each of
    // the seven voice frequencies over 1 s.  Tolerance 0.08 covers the
    // capture's knob-calibration offset (JP sits ~0.06 under Szabó mid-range).
    static const float kJpRatio[11] = { 0.070f, 0.120f, 0.238f, 0.374f, 0.515f, 0.632f,
                                        0.757f, 0.898f, 1.025f, 1.154f, 1.281f };
    static const double kOff[7] = { -0.11002313, -0.06288439, -0.01952356, 0.0,
                                     0.01991221,  0.06216538,  0.10745242 };
    const float f0 = 130.8128f;
    for (int m = 0; m <= 10; m += 2) {
        SupersawOsc o;
        o.seedNoise(5);
        o.setFrequency(f0);
        o.setDetune(1.0f);
        o.setMix((float)m / 10.0f);
        o.noteOn();
        const auto v = renderN(o, (size_t)kSampleRate + 4800);
        double amp[7];
        for (int k = 0; k < 7; ++k) {
            const double w = 2.0 * 3.14159265358979 * 4.0 * (double)f0 * (1.0 + kOff[k]) / (double)kSampleRate;
            double re = 0.0, im = 0.0;
            const size_t L = (size_t)kSampleRate;
            for (size_t i = 0; i < L; ++i) {
                // Blackman window: keeps neighbouring voices' leakage out.
                const double t = (double)i / (double)(L - 1);
                const double win = 0.42 - 0.5 * std::cos(6.28318530718 * t) + 0.08 * std::cos(12.5663706144 * t);
                const double x = (double)v[i + 4800] * win;
                re += x * std::cos(w * (double)i);
                im -= x * std::sin(w * (double)i);
            }
            amp[k] = std::sqrt(re * re + im * im);
        }
        const double side = (amp[0] + amp[1] + amp[2] + amp[4] + amp[5] + amp[6]) / 6.0;
        CAPTURE(m);
        const double ratio = side / amp[3];
        CAPTURE(ratio);
        CHECK(std::fabs(ratio - (double)kJpRatio[m]) < 0.08);
    }
}

TEST_CASE("mix knob: JP level - no compensation, RMS tracks the JE-8086")
{
    // JE-8086 supersaw RMS relative to its own plain saw, full detune:
    // mix 0 -> 0.581, 0.5 -> 0.831, 1.0 -> 0.882 (osc-shapes doc, §5).
    // A plain ±1 saw has RMS 1/sqrt(3).  ±0.10 covers random-phase spread.
    auto rmsAt = [](float mix) {
        SupersawOsc o;
        o.seedNoise(7);
        o.setFrequency(441.0f);
        o.setDetune(1.0f);
        o.setMix(mix);
        o.noteOn();
        const auto v = renderN(o, 48000);
        double acc = 0.0;
        for (float s : v) acc += (double)s * (double)s;
        return std::sqrt(acc / (double)v.size()) * std::sqrt(3.0);
    };
    CHECK(std::fabs(rmsAt(0.0f) - 0.581) < 0.10);
    CHECK(std::fabs(rmsAt(0.5f) - 0.831) < 0.10);
    CHECK(std::fabs(rmsAt(1.0f) - 0.882) < 0.10);
}

TEST_CASE("mix knob: side swarm emerges (periodicity)")
{
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

TEST_CASE("supersaw abuse: parameter thrash stays finite and inside the ±3.0 safety bound")
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
            REQUIRE(std::fabs(s) <= 3.0f);    // kClip safety bound (1b, header deviation 5)
        }
    }
}
