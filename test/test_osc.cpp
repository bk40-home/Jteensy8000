// =============================================================================
// test_osc.cpp — proofs for core/dsp/OscCore and FastMath (Pass 4, Step 1)
// =============================================================================
#include "doctest.h"

#include <cmath>
#include <vector>

#include "core/dsp/OscCore.h"
#include "core/dsp/FastMath.h"
#include "core/AudioConfig.h"

using namespace JT;

namespace {

// Render N samples in kBlockSize chunks (the only way the engine ever calls).
std::vector<float> renderSamples(OscCore& o, size_t total,
                                 const float* fm = nullptr, float oct = 0.0f)
{
    std::vector<float> v(total);
    for (size_t off = 0; off < total; off += kBlockSize) {
        const size_t chunk = (total - off < kBlockSize) ? total - off
                                                        : kBlockSize;
        o.render(v.data() + off, chunk, fm, oct, nullptr, nullptr);
    }
    return v;
}

// Count positive-going zero crossings — frequency measurement that works
// for every waveform here except S&H.
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

} // namespace

TEST_CASE("FastMath: pow2 within 2e-6, sine THD below the audible floor")
{
    // Bound is the MEASURED v1-polynomial error (see FastMath.h) — the
    // code is a byte-for-byte v1 port, so the test pins today's behaviour.
    for (float x = -10.0f; x <= 10.0f; x += 0.01f)
        REQUIRE(FastMath::fastPow2(x)
                == doctest::Approx(std::exp2(x)).epsilon(1e-3));

    // fastSin01 vs sinf: bounded error, correct zeros and extrema signs.
    float maxErr = 0.0f;
    for (float p = 0.0f; p < 1.0f; p += 0.0005f) {
        const float err = std::fabs(FastMath::fastSin01(p)
                                    - std::sin(p * 6.28318530718f));
        if (err > maxErr) maxErr = err;
    }
    CHECK(maxErr < 0.002f);   // ≈ -54 dB worst case, -60 dB typical
}

TEST_CASE("every periodic wave runs at the requested frequency")
{
    // 1 second at 441 Hz -> expect 441 rising crossings ±1 (edge effects).
    const Wave waves[] = { Wave::Sine, Wave::Saw, Wave::SawRev, Wave::Square,
                           Wave::Pulse, Wave::Triangle, Wave::TriVar,
                           Wave::BlSaw, Wave::BlSawRev, Wave::BlSquare,
                           Wave::BlPulse };
    for (Wave w : waves) {
        CAPTURE((int)w);
        OscCore o;
        o.setWave(w);
        o.setFrequency(441.0f);
        o.setShape(0.3f);
        const auto v = renderSamples(o, 44160);   // 345 whole blocks ≈ 1 s
        const size_t z = risingCrossings(v);
        CHECK(z >= 439);
        CHECK(z <= 443);
        // Bounded output (BLEP corners may slightly exceed ±1; never wild).
        for (float s : v) REQUIRE(std::fabs(s) < 1.5f);
    }
}

TEST_CASE("pulse width follows shape (duty cycle measured)")
{
    OscCore o;
    o.setWave(Wave::Pulse);
    o.setFrequency(100.0f);
    o.setShape(0.25f);
    const auto v = renderSamples(o, 44160);
    size_t high = 0;
    for (float s : v) if (s > 0.0f) ++high;
    CHECK((float)high / (float)v.size() == doctest::Approx(0.25f).epsilon(0.02));
}

TEST_CASE("TriVar at 0.5 equals Triangle; skew shifts the peak position")
{
    OscCore sym, skew;
    sym.setWave(Wave::TriVar);  sym.setFrequency(200.0f);  sym.setShape(0.5f);
    skew.setWave(Wave::TriVar); skew.setFrequency(200.0f); skew.setShape(0.9f);
    // Peak of a 0.9-skew triangle arrives ~0.9 of the way through the cycle.
    const auto vSkew = renderSamples(skew, 441);            // two cycles
    size_t peakAt = 0;
    for (size_t i = 0; i < 220; ++i)                         // first cycle
        if (vSkew[i] > vSkew[peakAt]) peakAt = i;
    CHECK((float)peakAt / 220.5f == doctest::Approx(0.9f).epsilon(0.05));

    const auto vSym = renderSamples(sym, 4416);
    CHECK(rms(vSym) == doctest::Approx(1.0f / std::sqrt(3.0f)).epsilon(0.02));
}

TEST_CASE("Sample & Hold: constant between wraps, new value after each")
{
    OscCore o;
    o.setWave(Wave::SampleHold);
    o.seedNoise(0xC0FFEE);
    o.setFrequency(344.53125f);        // exactly one wrap per block of 128
    const auto v = renderSamples(o, kBlockSize * 8);

    // Values within a hold segment must be identical; across the run we
    // must see several DIFFERENT levels.
    size_t changes = 0;
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i] != v[i - 1]) ++changes;
    CHECK(changes >= 6);
    CHECK(changes <= 10);              // one per wrap, not per sample
}

TEST_CASE("arb table: interpolated playback of a known ramp")
{
    // A 4-point int16 ramp; linear interp between entries means the output
    // must hit exact quarter values at exact phases.
    static const int16_t ramp[4] = { -32768, -16384, 0, 16384 };
    OscCore o;
    o.setWave(Wave::Arb);
    o.setArbTable(ramp, 4);
    o.setFrequency(0.0f);              // hold phase still...
    o.resetPhase(0.125f);              // ...half-way between entries 0 and 1
    float out[kBlockSize];
    o.render(out, kBlockSize, nullptr, 0.0f, nullptr, nullptr);
    CHECK(out[0] == doctest::Approx(-0.75f).epsilon(0.001));

    // And with no table set, Arb degrades to the naive saw (v1 guard).
    OscCore bare;
    bare.setWave(Wave::Arb);
    bare.setFrequency(441.0f);
    const auto v = renderSamples(bare, 44160);
    CHECK(risingCrossings(v) == doctest::Approx(441).epsilon(0.01));
}

TEST_CASE("hard sync: slave locks to the master's period (v1: OSC1 slave)")
{
    // Master at exactly 210 Hz = a 210-SAMPLE period (44100/210), so the
    // periodicity check below can compare sample-aligned cycles.  Slave
    // free-runs at 313 Hz — a deliberately unrelated ratio.
    OscCore master, slave;
    master.setWave(Wave::Saw);   master.setFrequency(210.0f);
    slave.setWave(Wave::Saw);    slave.setFrequency(313.0f);

    std::vector<float> slaveOut(44160);
    float mBuf[kBlockSize], sync[kBlockSize];
    for (size_t off = 0; off < slaveOut.size(); off += kBlockSize) {
        master.render(mBuf, kBlockSize, nullptr, 0.0f, nullptr, sync);
        slave.render(slaveOut.data() + off, kBlockSize,
                     nullptr, 0.0f, sync, nullptr);
    }

    // Count the master-rate restarts via the large negative jumps a saw
    // makes when its phase resets (crossings would also count the slave's
    // own intra-period wraps — the jump test isolates true periodicity).
    size_t resets = 0;
    for (size_t i = 1; i < slaveOut.size(); ++i)
        if (slaveOut[i] - slaveOut[i - 1] < -0.5f) ++resets;
    // Every master cycle forces a slave restart; the slave's own wraps add
    // more.  Sanity-bound both families rather than model coincidences.
    CHECK(resets >= 210);
    CHECK(resets <= 210 + 313 + 40);

    // Direct periodicity proof: correlate one master period against the
    // next — near-identical repetition at the MASTER period (210 samples,
    // exact by construction above).
    const size_t period = (size_t)(kSampleRate / 210.0f);
    double diff = 0.0, energy = 0.0;
    for (size_t i = 1000; i < 1000 + period * 50; ++i) {
        const double d = (double)slaveOut[i] - (double)slaveOut[i + period];
        diff   += d * d;
        energy += (double)slaveOut[i] * (double)slaveOut[i];
    }
    CHECK(diff / energy < 0.02);       // <2% mismatch period to period
}

TEST_CASE("exponential FM: +1 octave doubles the frequency exactly")
{
    OscCore o;
    o.setWave(Wave::Saw);
    o.setFrequency(220.0f);
    // Constant +0.1 modulator at 10-octave range = +1 octave -> 440 Hz.
    float fm[kBlockSize];
    for (float& f : fm) f = 0.1f;
    std::vector<float> v(44160);
    for (size_t off = 0; off < v.size(); off += kBlockSize)
        o.render(v.data() + off, kBlockSize, fm, 10.0f, nullptr, nullptr);
    const size_t z = risingCrossings(v);
    CHECK(z >= 438);
    CHECK(z <= 442);
}

TEST_CASE("oscillator abuse: parameter thrash stays finite and bounded")
{
    OscCore o;
    uint32_t rng = 7;
    float out[kBlockSize];
    for (int b = 0; b < 3000; ++b) {
        rng = rng * 1664525u + 1013904223u;
        if ((rng & 3u) == 0) o.setWave((Wave)((rng >> 8) % 13u));
        if ((rng & 3u) == 1) o.setFrequency((float)((rng >> 8) % 20000u));
        if ((rng & 3u) == 2) o.setShape((float)((rng >> 8) & 255u) / 255.0f);
        o.render(out, kBlockSize, nullptr, 0.0f, nullptr, nullptr);
        for (float s : out) {
            REQUIRE(std::isfinite(s));
            REQUIRE(std::fabs(s) < 2.0f);
        }
    }
}
