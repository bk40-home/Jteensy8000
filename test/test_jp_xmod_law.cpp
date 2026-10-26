// =============================================================================
// test_jp_xmod_law.cpp — JpXmod depth by OSC2 wave, pulse DC (2026-10-25)
// =============================================================================
// Sign-off 1a / 2a after the X-MOD grid (xmod/xmod-grid-analysis-2026-10-25):
//   * JpXmodLaw::depthScale — triangle 1.0, saw 0.50, pulse 0.296 → 0.277 at
//     79 %; sync ×0.623 for non-pulse waves only.
//   * OscCore pulse DC — removed under JpPwm, untouched (byte-identical)
//     under JtPwm; outputDc() reports what is left.
//   * OscSection JpXmod — a pulse modulator's DC never moves OSC1's mean pitch.
// =============================================================================
#include "doctest.h"

#include <cmath>
#include <vector>

#include "core/OscSection.h"
#include "core/dsp/JpXmodLaw.h"
#include "core/dsp/OscCore.h"

using namespace JT;

TEST_CASE("JpXmodLaw: depth scale by OSC2 wave and sync")
{
    using JpXmodLaw::depthScale;
    CHECK(depthScale(Wave::JpVarTri, 0.5f, false) == 1.0f);
    CHECK(depthScale(Wave::JpVarTri, 0.5f, true)  == doctest::Approx(0.623f));
    CHECK(depthScale(Wave::Triangle, 0.5f, true)  == doctest::Approx(0.623f));
    CHECK(depthScale(Wave::JpVarSaw, 0.5f, false) == doctest::Approx(0.50f));
    CHECK(depthScale(Wave::BlSaw,    0.5f, true)  == doctest::Approx(0.50f * 0.623f));
    CHECK(depthScale(Wave::BlPulse,  0.5f,  false) == doctest::Approx(0.296f));
    CHECK(depthScale(Wave::BlPulse,  0.79f, false) == doctest::Approx(0.277f).epsilon(0.002));
    CHECK(depthScale(Wave::BlPulse,  0.79f, true)  == depthScale(Wave::BlPulse, 0.79f, false));
    CHECK(depthScale(Wave::Square,   0.5f,  true)  == doctest::Approx(0.296f));
    CHECK(depthScale(Wave::Sine,     0.5f,  false) == 1.0f);       // unmeasured: calibrated law
}

namespace {
float meanOf(OscCore& c, size_t blocks)
{
    double s = 0.0; size_t n = 0;
    float buf[kBlockSize];
    for (size_t b = 0; b < blocks; ++b) {
        c.render(buf, kBlockSize, nullptr, 0.0f, nullptr, nullptr);
        for (float v : buf) { s += (double)v; ++n; }
    }
    return (float)(s / (double)n);
}
} // namespace

TEST_CASE("OscCore: JP pulse is DC-free under JpPwm; JtPwm keeps its DC (byte-identical)")
{
    // 200 Hz at 48 kHz = 240 samples/cycle: 1280 blocks hold a whole number of cycles.
    OscCore jt; jt.setFrequency(200.0f); jt.setWave(Wave::BlPulse); jt.setShape(0.79f);
    OscCore jp; jp.setFrequency(200.0f); jp.setWave(Wave::BlPulse); jp.setJpPwmClamp(true); jp.setShape(0.79f);
    CHECK(jt.outputDc() == doctest::Approx(0.58f));
    CHECK(jp.outputDc() == 0.0f);
    CHECK(meanOf(jt, 1280) == doctest::Approx(0.58f).epsilon(0.01));
    CHECK(std::fabs(meanOf(jp, 1280)) < 0.005f);

    // A square / non-pulse wave reports no DC under either law.
    OscCore sq; sq.setWave(Wave::BlSquare); sq.setJpPwmClamp(true); sq.setShape(0.79f);
    CHECK(sq.outputDc() == 0.0f);
}

TEST_CASE("JpXmod: a pulse modulator's DC does not shift OSC1's mean pitch")
{
    // OSC1 naive saw at 220 Hz; OSC2 a 79 % pulse under JtPwm (DC 0.58 in its
    // output).  Without the AC-coupling the mean rate would rise by
    // Δf·0.58 ≈ +420 Hz at this depth.  Net wraps (forward minus backward)
    // over the render give the mean frequency.
    OscSection s;
    s.noteOn(220.0f, 11);
    s.setWave(0, (int)Wave::Saw);
    s.setWave(1, (int)Wave::BlPulse);
    s.setShapeDc(1, 2.0f * 0.79f - 1.0f);     // width 79 %
    s.setBalance(-1.0f);                       // OSC1 only
    s.setXmodType(1);
    s.setCrossMod(0.3f);
    std::vector<float> v;
    float buf[kBlockSize];
    const size_t blocks = 375;                 // 1.0 s at 48 kHz
    for (size_t b = 0; b < blocks; ++b) { s.render(buf, kBlockSize); v.insert(v.end(), buf, buf + kBlockSize); }
    float amp = 0.0f;
    for (float x : v) amp = std::fmax(amp, std::fabs(x));
    int net = 0;
    for (size_t i = 1; i < v.size(); ++i) {
        const float d = v[i] - v[i - 1];
        if (d < -amp) ++net;                   // forward wrap (+1 -> -1)
        else if (d > amp) --net;               // backward wrap
    }
    const float meanHz = (float)net / ((float)v.size() / kSampleRate);
    INFO("mean OSC1 rate ", meanHz, " Hz");
    CHECK(meanHz == doctest::Approx(220.0f).epsilon(0.02));
}
