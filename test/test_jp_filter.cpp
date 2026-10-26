// =============================================================================
// test_jp_filter.cpp — proofs for core/dsp/JpFilter and its FilterSection
// wiring (filter.va_type 17..22, JP-8000 modes fitted to the JE-8086)
// =============================================================================
// The chain of proof, in order:
//   1. the table and the section agree on 23 VA types, and 17..22 are JP;
//   2. the laws reproduce the fitted constants exactly (cutoff, R knots, trim);
//   3. the REAL filter, driven through FilterSection with sines, matches the
//      analytic digital model it was fitted as (so 4 applies to it);
//   4. that model lands on the JE-8086 measurements (scene 14) — -3 dB points
//      for all six modes, the -24 resonance trim, and the top-end behaviour;
//   5. it stays finite at the extremes.
// Reference data: syx-import/scene14-noise-filter-analysis-2026-10-26.md.
// =============================================================================
#include "doctest.h"

#include <cmath>
#include <complex>
#include <initializer_list>
#include <string>

#include "core/FilterSection.h"
#include "core/ParameterStore.h"
#include "core/dsp/JpFilter.h"
#include "gen/ParamTable.h"

using namespace JT;
using namespace JT::Params;

namespace {

using Mode = JpFilter::Mode;
constexpr double kPi = 3.14159265358979323846;   // M_PI is not ISO C++
constexpr Mode kModes[6] = { Mode::LP24, Mode::LP12, Mode::HP24,
                             Mode::HP12, Mode::BP24, Mode::BP12 };

// --- analytic digital model: exactly what fitjp_final.py fitted -------------
// g = tan(pi fc/fs), s = j tan(w/2)/g, D = s^2 + 2Rs + 1; LP 1/D, HP s^2/D,
// BP s/D; -24 squares the section; times outputTrim.  Double on purpose: it
// is the reference, not the thing under test.
double modelDb(Mode m, float code, float reso, double fHz)
{
    const double fs = (double)kSampleRate;
    double fc = (double)JpFilter::cutoffHz(code);
    const double hi = (double)JpFilter::ceilingHz(m, kSampleRate);
    if (fc > hi) fc = hi;
    if (fc < 5.0) fc = 5.0;
    const double R = (double)JpFilter::damping(reso);
    const double g = std::tan(kPi * fc / fs);
    const std::complex<double> s(0.0, std::tan(kPi * fHz / fs) / g);
    const std::complex<double> D = s * s + 2.0 * R * s + 1.0;
    std::complex<double> sec;
    switch (m) {
        case Mode::LP24: case Mode::LP12: sec = 1.0 / D;   break;
        case Mode::HP24: case Mode::HP12: sec = s * s / D; break;
        default:                          sec = s / D;     break;
    }
    if (JpFilter::isCascade(m)) sec = sec * sec;
    const double mag = std::abs(sec) * (double)JpFilter::outputTrim(m, (float)R);
    return 20.0 * std::log10(mag + 1e-12);
}

// --- the real filter: a sine through FilterSection, steady-state gain -------
// Amplitude 1e-3 keeps the output saturator (tanh) linear even on a +30 dB
// resonant peak, and sits 80 dB above the 1e-7 dither.
double measureDb(Mode m, float code, float reso, float fHz)
{
    FilterSection f;
    f.setEngine(1);
    f.setVaType(FilterSection::kJpFirstVaType + (int)m);
    f.setCutoff(code / 127.0f, 0.0f);
    f.setResonanceNorm(reso / 127.0f);

    constexpr size_t kN = 24000;                      // 0.5 s
    constexpr float  kA = 1.0e-3f;
    static float buf[kN];
    const float inc = fHz / kSampleRate;
    float ph = 0.0f;
    for (size_t i = 0; i < kN; ++i) {
        buf[i] = kA * sinf(6.28318530718f * ph);
        ph += inc; if (ph >= 1.0f) ph -= 1.0f;
    }
    for (size_t i = 0; i < kN; i += 128) f.process(buf + i, 128);
    double acc = 0.0;                                 // RMS of the last half
    for (size_t i = kN / 2; i < kN; ++i) acc += (double)buf[i] * (double)buf[i];
    const double rms = std::sqrt(acc / (double)(kN / 2));
    return 20.0 * std::log10(rms / ((double)kA / std::sqrt(2.0)) + 1e-12);
}

// -3 dB point of the model, read from the STOPBAND side (as the JE analysis
// did): LP = highest f still >= -3 dB; HP = lowest; BP = the peak.
double modelEdgeHz(Mode m, float code)
{
    double best = 0.0, bestDb = -1e9, lo = 0.0, hi = 0.0;
    for (int i = 0; i <= 48 * 10; ++i) {                  // 20 Hz..20 kHz, 1/48 oct
        const double f = 20.0 * std::pow(2.0, i / 48.0);
        if (f > 20000.0) break;
        const double db = modelDb(m, code, 0.0f, f);
        if (db >= -3.0) { if (lo == 0.0) lo = f; hi = f; }
        if (db > bestDb) { bestDb = db; best = f; }
    }
    switch (m) {
        case Mode::LP24: case Mode::LP12: return hi;
        case Mode::HP24: case Mode::HP12: return lo;
        default:                          return best;
    }
}

} // namespace

// 1 ---------------------------------------------------------------------------
TEST_CASE("jp filter: the table carries 23 VA types and 17..22 are the JP modes")
{
    const ParamDesc& d = kParams[ParameterStore::indexOf(ID::FILTER_VA_TYPE)];
    CHECK(d.optionCount == FilterSection::kNumVaTypes);
    const char* jp[6] = { "JP LPF-24", "JP LPF-12", "JP HPF-24",
                          "JP HPF-12", "JP BPF-24", "JP BPF-12" };
    for (int i = 0; i < 6; ++i)
        CHECK(std::string(d.options[FilterSection::kJpFirstVaType + i]) == jp[i]);
}

// 2 ---------------------------------------------------------------------------
TEST_CASE("jp filter: laws reproduce the fitted constants")
{
    CHECK(JpFilter::cutoffHz(64.0f) == doctest::Approx(565.3909f).epsilon(1e-5));
    CHECK(JpFilter::cutoffHz(64.0f + 11.74617f) / JpFilter::cutoffHz(64.0f)
          == doctest::Approx(2.0f).epsilon(1e-5));
    const float knots[5][2] = { {0, 0.5f}, {32, 0.341832f}, {64, 0.157627f},
                                {96, 0.040869f}, {127, 0.003517f} };
    for (auto& k : knots)
        CHECK(JpFilter::damping(k[0]) == doctest::Approx(k[1]).epsilon(1e-4));
    for (int c = 1; c <= 127; ++c)                       // Q only ever rises
        CHECK(JpFilter::damping((float)c) < JpFilter::damping((float)(c - 1)));
    for (Mode m : kModes)
        CHECK(JpFilter::outputTrim(m, 0.5f) == doctest::Approx(1.0f));
    CHECK(JpFilter::outputTrim(Mode::LP24, 0.05f) == doctest::Approx(0.1f));
    CHECK(JpFilter::outputTrim(Mode::LP12, 0.05f) == doctest::Approx(powf(0.1f, 0.1f)));
}

// 3 ---------------------------------------------------------------------------
TEST_CASE("jp filter: the real filter matches the fitted model (sine sweep)")
{
    const float codes[3] = { 48.0f, 80.0f, 112.0f };
    const float resos[2] = { 0.0f, 64.0f };
    const float freqs[8] = { 60, 150, 400, 1000, 2500, 6000, 12000, 18000 };
    double worst = 0.0;
    for (Mode m : kModes)
        for (float c : codes)
            for (float r : resos)
                for (float f : freqs) {
                    const double want = modelDb(m, c, r, f);
                    if (want < -40.0) continue;          // below the 1e-3 tone's floor
                    const double got = measureDb(m, c, r, f);
                    const double e = std::fabs(got - want);
                    if (e > worst) worst = e;
                    CHECK(e < 0.25);
                }
    MESSAGE("worst |real - model| = " << worst << " dB");
}

// 4 ---------------------------------------------------------------------------
TEST_CASE("jp filter: -3 dB points land on the JE-8086 (scene 14, reso 0)")
{
    // JE: LP = -3 dB point, HP = -3 dB point, BP = peak; codes 48/64/80/96/112.
    const double je[6][5] = {
        {  259,  649, 1570, 4138, 12106 },   // LPF-24
        {  278,  708, 1866, 4753, 13665 },   // LPF-12
        {  193,  484, 1211, 3083,  7990 },   // HPF-24
        {  149,  444, 1018, 2828,  7201 },   // HPF-12
        {  246,  555, 1297, 3541, 11493 },   // BPF-24 (peak)
        {  214,  616, 1253, 4211, 12532 },   // BPF-12 (peak)
    };
    const float codes[5] = { 48, 64, 80, 96, 112 };
    double worst = 0.0;
    for (int mi = 0; mi < 6; ++mi)
        for (int ci = 0; ci < 5; ++ci) {
            const double oct = std::log2(modelEdgeHz(kModes[mi], codes[ci]) / je[mi][ci]);
            if (std::fabs(oct) > worst) worst = std::fabs(oct);
            // The JE values come from 1/6-octave-smoothed noise; their own
            // scatter about the fitted law is up to ~0.35 oct for BPF-12.
            CHECK(std::fabs(oct) < 0.40);
        }
    MESSAGE("worst |model - JE| = " << worst << " oct");
}

TEST_CASE("jp filter: -24 resonance trims the passband like the JE; -12 barely")
{
    // LPF cut 64, passband at fc/8 (~70 Hz), reso 96 relative to reso 0.
    // JE: -24 -21.7 dB, -12 -2.1..-3.0 dB.
    const float f = JpFilter::cutoffHz(64.0f) / 8.0f;
    const double d24 = measureDb(Mode::LP24, 64, 96, f) - measureDb(Mode::LP24, 64, 0, f);
    const double d12 = measureDb(Mode::LP12, 64, 96, f) - measureDb(Mode::LP12, 64, 0, f);
    CHECK(d24 == doctest::Approx(20.0 * std::log10(2.0 * 0.040869)).epsilon(0.02));
    CHECK(std::fabs(d24 - (-21.7)) < 1.0);
    CHECK(d12 < -1.5); CHECK(d12 > -3.5);
    // HPF -24 loses the same in ITS passband (8x fc) — the ladder could not.
    const float fh = JpFilter::cutoffHz(64.0f) * 8.0f;
    const double h24 = measureDb(Mode::HP24, 64, 96, fh) - measureDb(Mode::HP24, 64, 0, fh);
    CHECK(std::fabs(h24 - (-21.2)) < 1.0);
}

TEST_CASE("jp filter: top end — LP open at 127, HP saturates near 13 kHz")
{
    CHECK(measureDb(Mode::LP24, 127, 0, 18000.0f) > -3.0);   // JE: open to 20 kHz
    CHECK(measureDb(Mode::LP12, 127, 0, 18000.0f) > -3.0);
    const double hp = modelEdgeHz(Mode::HP24, 127.0f);       // JE: 13.2..13.4 kHz
    CHECK(std::fabs(std::log2(hp / 13300.0)) < 0.25);
}

TEST_CASE("jp filter: linear at full-scale voice level (H2/H2b: linear to 4, knee 8)")
{
    // The voice reaches the filter at full scale (noise is uniform +-1).  The
    // same noise run at full scale and at 1e-3 must agree once rescaled, i.e.
    // the output stage must not distort a normal signal.  With the shared
    // tanh(y) tail this measured ~-15 dB of distortion on HPF-24 and filled
    // the stopband 18 dB down (scene 14 recapture); H2's 8*tanh(y/8) left a
    // cubic term ~-50 dB; H2b is EXACTLY linear below 4, so a +-1 voice is
    // undistorted and only the 1e-7 dither differs (1e-4 relative in the
    // rescaled small run, i.e. ~-80 dB).  Limit -70 dB.
    for (Mode m : kModes) {
        float big[9600], small[9600];
        uint32_t rng = 99u;
        for (size_t i = 0; i < 9600; ++i) {
            rng = rng * 1664525u + 1013904223u;
            big[i]   = (float)(int32_t)rng * (1.0f / 2147483648.0f);
            small[i] = big[i] * 1.0e-3f;
        }
        for (float* buf : { big, small }) {
            FilterSection f; f.setEngine(1);
            f.setVaType(FilterSection::kJpFirstVaType + (int)m);
            f.setCutoff(112.0f / 127.0f, 0.0f);
            for (size_t i = 0; i < 9600; i += 128) f.process(buf + i, 128);
        }
        double sig = 0.0, err = 0.0;
        for (size_t i = 4800; i < 9600; ++i) {
            const double e = (double)big[i] - 1.0e3 * (double)small[i];
            sig += (double)big[i] * (double)big[i]; err += e * e;
        }
        const double distDb = 10.0 * std::log10(err / sig + 1e-30);
        CHECK(distDb < -70.0);
        MESSAGE("mode " << (int)m << " distortion " << distDb << " dB");
    }
}

// 5 ---------------------------------------------------------------------------
TEST_CASE("jp filter: H2b output curve - linear to 4, smooth join, bounded by 8")
{
    // Drive the JP tail directly with a DC ramp through an open LP: the
    // filter passes DC at unity (reso 0 trim 1), so output == the tail curve.
    auto tail = [](float x) {
        FilterSection f; f.setEngine(1);
        f.setVaType(FilterSection::kJpFirstVaType + (int)Mode::LP12);
        f.setCutoff(1.0f, 0.0f);
        float buf[128];
        for (int b = 0; b < 40; ++b) { for (float& v : buf) v = x; f.process(buf, 128); }
        return buf[127];
    };
    CHECK(tail(1.0f)  == doctest::Approx(1.0f).epsilon(1e-4));      // linear
    CHECK(tail(3.9f)  == doctest::Approx(3.9f).epsilon(1e-4));      // still linear
    CHECK(tail(-3.9f) == doctest::Approx(-3.9f).epsilon(1e-4));
    const float a = tail(4.2f), b = tail(6.0f), c = tail(50.0f);
    CHECK(a > 4.0f); CHECK(a < 4.2f);                               // shoulder starts
    CHECK(b > a);    CHECK(b < 6.0f);                               // monotonic, compressing
    CHECK(c <= FilterSection::kJpSatKnee); CHECK(c > 7.9f);        // bounded by K
    CHECK(tail(-50.0f) == doctest::Approx(-c));                     // odd-symmetric
}

TEST_CASE("jp filter: finite at full resonance across a cutoff sweep, then quiet")
{
    for (Mode m : kModes) {
        FilterSection f; f.setEngine(1);
        f.setVaType(FilterSection::kJpFirstVaType + (int)m);
        f.setResonanceNorm(1.0f);
        uint32_t rng = 12345u;
        float buf[128];
        bool finite = true;
        for (int b = 0; b < 750; ++b) {                       // 2 s, full-scale noise
            f.setCutoff((float)(b % 128) / 127.0f, 0.0f);
            for (float& x : buf) { rng = rng * 1664525u + 1013904223u;
                                   x = (float)(int32_t)rng * (1.0f / 2147483648.0f); }
            f.process(buf, 128);
            // Bounded by the JP tail's knee/clamp (H2), not by 1.
            for (float x : buf) finite = finite && std::isfinite(x)
                                     && std::fabs(x) <= FilterSection::kJpSatKnee;
        }
        CHECK(finite);
        f.setResonanceNorm(0.0f);                             // then silence decays
        for (int b = 0; b < 400; ++b) { for (float& x : buf) x = 0.0f; f.process(buf, 128); }
        CHECK(std::fabs(buf[127]) < 1e-3f);
    }
}
