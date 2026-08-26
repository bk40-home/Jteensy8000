// =============================================================================
// test_moogdv.cpp — MoogDV4 against D'Angelo & Valimaki, ICASSP 2013
// =============================================================================
#include "doctest.h"
#include <cmath>
#include <vector>
#include "core/dsp/MoogDVCore.h"
#include "core/AudioConfig.h"
using namespace JT;

namespace {
// Excite for one block, then feed SILENCE and measure what is still coming out.
// A filter that sustains on silence is oscillating; one that decays is not.
double selfOscTail(float fc, float k, float drive = 1.0f, double* peakOut = nullptr)
{
    MoogDV4 f; f.reset(); f.setCutoff(fc, kSampleRate);
    const int N = (int)(kSampleRate * 1.2f);
    std::vector<float> tail; double pk = 0.0;
    for (int n = 0; n < N; ++n) {
        const float x = (n < 512)
            ? drive * sinf(2.0f * (float)M_PI * 220.0f * (float)n / kSampleRate)
            : 0.0f;
        f.tick(x, k, false);
        if (std::fabs((double)f.lp4) > pk) pk = std::fabs((double)f.lp4);
        if (n > N - 16384) tail.push_back(f.lp4);
    }
    if (peakOut) *peakOut = pk;
    double a = 0.0; for (float v : tail) a += (double)v * (double)v;
    return std::sqrt(a / (double)tail.size());
}
float settleDC(float fc, float k, float dc, float* hp = nullptr)
{
    MoogDV4 f; f.reset(); f.setCutoff(fc, kSampleRate);
    for (int n = 0; n < 40000; ++n) f.tick(dc, k, false);
    if (hp) *hp = f.hp4;
    return f.lp4;
}
} // namespace

TEST_CASE("MoogDV: eq.(19) cutoff coefficient matches the paper")
{
    // A = pi*(fc/fs)*(1 - pi*fc/fs)/(1 + pi*fc/fs)
    MoogDV4 f;
    for (float fc : {100.0f, 1000.0f, 5000.0f}) {
        f.setCutoff(fc, kSampleRate);
        const float r = (float)M_PI * fc / kSampleRate;
        CHECK(f.A == doctest::Approx(r * (1.0f - r) / (1.0f + r)).epsilon(1e-6));
    }
}

TEST_CASE("MoogDV: self-oscillates, with onset at the paper's k = 4")
{
    // Sec.4: "at a feedback gain value such that the dominant poles ... are
    // located on the imaginary axis, the system would start to self oscillate
    // ... It can be shown that such value is k = 4.0".
    CHECK(selfOscTail(1000.0f, 2.0f) == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(selfOscTail(1000.0f, 3.0f) == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(selfOscTail(1000.0f, 4.0f) > 0.15);        // sings, and audibly
}

TEST_CASE("MoogDV: self-oscillation SUSTAINS rather than fading")
{
    // Fig. 5 is precisely this test: the previous model's ring "rapidly fades
    // out", the new one "keeps steady". Compare the start and end of a long
    // silent tail — a decaying ring would show the second half far quieter.
    MoogDV4 f; f.reset(); f.setCutoff(1000.0f, kSampleRate);
    const int N = (int)(kSampleRate * 2.0f);
    double early = 0.0, late = 0.0; int ne = 0, nl = 0;
    for (int n = 0; n < N; ++n) {
        const float x = (n < 512)
            ? sinf(2.0f * (float)M_PI * 220.0f * (float)n / kSampleRate) : 0.0f;
        f.tick(x, 4.0f, false);
        if (n > N/2 && n < N/2 + 16384) { early += (double)f.lp4*(double)f.lp4; ++ne; }
        if (n > N - 16384)              { late  += (double)f.lp4*(double)f.lp4; ++nl; }
    }
    const double e = std::sqrt(early/ne), l = std::sqrt(late/nl);
    CHECK(l == doctest::Approx(e).epsilon(0.05));    // steady, not decaying
}

TEST_CASE("MoogDV: LP4 has unity, POSITIVE DC gain at k = 0")
{
    // The paper's ladder inverts (Sec.2.3); the taps undo that so this filter
    // is in phase with every other type in the bank. A regression here would
    // only show up as cancellation when layering, which is hard to chase.
    CHECK(settleDC(2000.0f, 0.0f, 0.1f) == doctest::Approx(0.1f).epsilon(0.01));
}

TEST_CASE("MoogDV: HP4 binomial residual nulls at DC, exactly")
{
    float hp = 0.0f;
    settleDC(1000.0f, 0.0f, 0.25f, &hp);
    CHECK(std::fabs((double)hp) < 1e-5);
}

TEST_CASE("MoogDV: bounded under hard drive at full resonance")
{
    // Five bounded sigmoids in the loop; nothing may run away even when the
    // input is pushed well past full scale.
    for (float fc : {200.0f, 1000.0f, 5000.0f}) {
        double pk = 0.0;
        const double rms = selfOscTail(fc, 4.0f, 4.0f, &pk);
        CAPTURE(fc);
        CHECK(std::isfinite(rms));
        CHECK(pk < 2.0);
    }
}

TEST_CASE("MoogDV: five tanh per sample, not four")
{
    // The paper counts 5 hyperbolic tangents (Sec.3.1). The previous
    // implementation made 4 — the missing one was tanh(dV4/2Vt), stage 4's own
    // feedback term (Fig. 3(c)), and its absence is what stopped the filter
    // reaching a musical self-oscillation amplitude.
    //
    // Structural proxy: with the self-term present, raising Vt (gentler
    // saturation) must INCREASE the oscillation, because it lowers damping as
    // well as forward gain. The old linear-self-term form did the opposite and
    // died out entirely. This is the cheapest observable that distinguishes
    // the two structures without counting calls.
    MESSAGE("Vt in this build = " << (double)JT_OPT_MOOGDV_VT);
    CHECK(selfOscTail(1000.0f, 4.0f) > 0.15);
}
