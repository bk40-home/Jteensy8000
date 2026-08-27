// =============================================================================
// test_moog_ladder.cpp — MoogLinear4 after the ZDF rewrite
// =============================================================================
#include "doctest.h"
#include <cmath>
#include <initializer_list>
#include "core/dsp/VAFilterCore.h"
#include "core/AudioConfig.h"
using namespace JT;

namespace {
double peakAtFc(float fc, float k, float amp)
{
    const float g = va_compute_g(fc, kSampleRate);
    MoogLinear4 f; double a = 0.0; int c = 0;
    for (int n = 0; n < 20000; ++n) {
        const float x = amp * sinf(2.0f*(float)M_PI*fc*(float)n/kSampleRate);
        f.process(x, g, k);
        if (!std::isfinite(f.y4)) return -1.0;
        if (n > 12000) { a += (double)f.y4*(double)f.y4; ++c; }
    }
    return std::sqrt(a/c) * 1.41421356 / (double)amp;
}
double oscTail(float fc, float k, float drive, double* peak)
{
    const float g = va_compute_g(fc, kSampleRate);
    MoogLinear4 f; const int N = (int)(kSampleRate * 3.0f);
    double a = 0.0, p = 0.0; int c = 0;
    for (int n = 0; n < N; ++n) {
        const float x = (n < 512)
            ? drive * sinf(2.0f*(float)M_PI*220.0f*(float)n/kSampleRate) : 0.0f;
        f.process(x, g, k);
        if (!std::isfinite(f.y4)) { if (peak) *peak = 1e30; return 1e30; }
        if (n > N - 16384) { a += (double)f.y4*(double)f.y4; ++c;
                             if (std::fabs((double)f.y4) > p) p = std::fabs((double)f.y4); }
    }
    if (peak) *peak = p;
    return std::sqrt(a/c);
}
} // namespace

TEST_CASE("Moog: resonant Q is cutoff-INDEPENDENT")
{
    // The old unit-delayed feedback made the SAME k mean wildly different
    // things across the sweep — measured 0.996 at 200 Hz but 32.6 at 10 kHz for
    // k = 3.0. That is the fault that used to make this filter run away.
    for (float fc : {200.0f, 1000.0f, 4000.0f, 10000.0f}) {
        CAPTURE(fc);
        CHECK(peakAtFc(fc, 2.0f, 0.02f) == doctest::Approx(0.5).epsilon(0.03));
        CHECK(peakAtFc(fc, 3.0f, 0.02f) == doctest::Approx(1.0).epsilon(0.03));
    }
}

TEST_CASE("Moog: small-signal peak follows 1/(4-k)")
{
    for (float k : {1.0f, 2.0f, 3.0f}) {
        CAPTURE(k);
        CHECK(peakAtFc(2000.0f, k, 0.02f)
              == doctest::Approx(1.0/(4.0-(double)k)).epsilon(0.03));
    }
}

TEST_CASE("Moog: SINGS, and across the whole cutoff range")
{
    // The map now reaches k = 4.10 at full knob. The AC-coupled feedback used to
    // kill this below ~400 Hz; with it off the amplitude is uniform.
    double pk = 0.0;
    CHECK(oscTail(2000.0f, 3.90f, 1.0f, &pk) == doctest::Approx(0.0).epsilon(1e-6));
    for (float fc : {100.0f, 1000.0f, 6000.0f, 12000.0f}) {
        CAPTURE(fc);
        CHECK(oscTail(fc, 4.10f, 1.0f, &pk) > 0.25);
    }
}

TEST_CASE("Moog: the ring is a LIMIT CYCLE, not a decaying tail")
{
    double p1 = 0.0, p4 = 0.0;
    const double r1 = oscTail(6000.0f, 4.10f, 1.0f, &p1);
    const double r4 = oscTail(6000.0f, 4.10f, 4.0f, &p4);
    CHECK(r4 == doctest::Approx(r1).epsilon(0.02));
    CHECK(p4 == doctest::Approx(p1).epsilon(0.02));
}

TEST_CASE("Moog: bounded at full resonance under hard drive")
{
    for (float fc : {100.0f, 2000.0f, 12000.0f}) {
        double pk = 0.0;
        const double r = oscTail(fc, 4.10f, 4.0f, &pk);
        CAPTURE(fc);
        CHECK(std::isfinite(r));
        CHECK(pk < 1.0);
    }
}

TEST_CASE("Moog: DC gain is 1/(1+k), the real ladder's behaviour")
{
    // With the feedback DC-coupled (JT_OPT_MOOG_FB_AC_COUPLE = 0), resonance
    // thins the low end as it does on the hardware. The old AC-coupled path
    // held DC gain at unity for every k.
    for (float k : {0.0f, 1.0f, 2.0f}) {
        const float g = va_compute_g(1000.0f, kSampleRate);
        MoogLinear4 f;
        for (int n = 0; n < 80000; ++n) f.process(0.1f, g, k);
        CAPTURE(k);
        CHECK((double)f.y4 / 0.1 == doctest::Approx(1.0/(1.0+(double)k)).epsilon(0.02));
    }
}

TEST_CASE("Moog: resonance no longer collapses as input level rises")
{
    // The safe-k limiter throttled feedback on an envelope follower, so louder
    // input meant less resonance — which is why any drive ahead of the filter
    // read as a resonance control. Some level-dependent compression remains
    // (that is the bounded feedback saturator doing its job), but the small
    // signal case must now reach the ideal.
    CHECK(peakAtFc(2000.0f, 3.9f, 0.02f) > 8.0);      // ideal is 10
}
