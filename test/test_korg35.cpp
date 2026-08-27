// =============================================================================
// test_korg35.cpp — Korg 35 / TSK ZDF solve (Zavalishin, The Art of VA Filter
// Design, Sallen-Key chapter)
// =============================================================================
#include "doctest.h"
#include <cmath>
#include <initializer_list>
#include "core/dsp/VAFilterCore.h"
#include "core/AudioConfig.h"
using namespace JT;

namespace {
// A ZDF solve is only correct if it PREDICTS the value the forward pass then
// actually produces. This is the test that caught the original fault, and it is
// the one worth keeping: it needs no reference implementation and no ear.
double lpSolveError(float fc, float k)
{
    const float g = va_compute_g(fc, kSampleRate);
    TPT1 p1, p2;
    double sig = 0.0, err = 0.0;
    for (int n = 0; n < 4000; ++n) {
        const float x = sinf(2.0f * (float)M_PI * 220.0f * (float)n / kSampleRate);
        const float g1 = g / (1.0f + g), G2 = g1 / (1.0f + g);
        const float inv = 1.0f / (1.0f + g);
        const float pred = (G2*x + p1.s*inv*inv - p2.s*inv) / (1.0f - k*G2);
        const float lp1 = p1.processLP(x + k*pred, g);
        const float lp2 = p2.processLP(lp1, g);
        const float act = lp1 - lp2;
        if (n > 2000) { sig += (double)act*(double)act; err += ((double)pred-(double)act)*((double)pred-(double)act); }
    }
    return std::sqrt(err / sig);
}
// Excite briefly, then run on silence and measure the SETTLED tail only, so a
// loud excitation transient cannot be mistaken for instability.
double selfOscTail(float fc, float k, bool hp, double* peak, float drive = 1.0f)
{
    const float g = va_compute_g(fc, kSampleRate);
    Korg35LP L; Korg35HP H;
    const int N = (int)(kSampleRate * 3.0f);
    double a = 0.0, p = 0.0; int c = 0;
    for (int n = 0; n < N; ++n) {
        const float x = (n < 512)
            ? drive * sinf(2.0f*(float)M_PI*220.0f*(float)n/kSampleRate) : 0.0f;
        const float y = hp ? H.process(x, g, k, VA_NL_SAT) : L.process(x, g, k, VA_NL_SAT);
        if (!std::isfinite(y)) { if (peak) *peak = 1e30; return 1e30; }
        if (n > N - 16384) {
            a += (double)y*(double)y; ++c;
            if (std::fabs((double)y) > p) p = std::fabs((double)y);
        }
    }
    if (peak) *peak = p;
    return std::sqrt(a / c);
}
// amp defaults to full scale; pass a small value to measure the SMALL-SIGNAL
// response, which is the only regime where a level-dependent limiter is
// expected to be transparent.
double peakAtFc(float fc, float k, bool hp, VANonlin nl, float amp = 1.0f)
{
    const float g = va_compute_g(fc, kSampleRate);
    Korg35LP L; Korg35HP H;
    double a = 0.0; int c = 0;
    for (int n = 0; n < 12000; ++n) {
        const float x = amp * sinf(2.0f * (float)M_PI * fc * (float)n / kSampleRate);
        const float y = hp ? H.process(x, g, k, nl) : L.process(x, g, k, nl);
        if (!std::isfinite(y)) return -1.0;
        if (n > 6000) { a += (double)y*(double)y; ++c; }
    }
    return std::sqrt(a / c) * 1.41421356 / (double)amp;
}
} // namespace

TEST_CASE("Korg35: ZDF solve is self-consistent at every cutoff")
{
    // The original solve normalised the DENOMINATOR by (1+g) but not the state
    // terms in the numerator, leaving both (1+g) times too large. Harmless while
    // g << 1, fatal as g grows: 0.7% error at 100 Hz, 18% at 2 kHz, 286% at
    // 6 kHz, NaN by 12 kHz — and 12 kHz is the top of this type's kShape range.
    for (float fc : {100.0f, 500.0f, 2000.0f, 6000.0f, 12000.0f}) {
        CAPTURE(fc);
        CHECK(lpSolveError(fc, 1.5f) < 1e-4);
    }
}

TEST_CASE("Korg35: resonant Q is cutoff-INDEPENDENT")
{
    // The signature of a correct ZDF. The TSK peak is 1/(2-k) regardless of
    // where the cutoff sits; the old solve's peak drifted with g, which is what
    // made the filter feel different at each end of the knob.
    const double expect = 1.0 / (2.0 - 1.95);          // = 20
    for (float fc : {500.0f, 2000.0f, 6000.0f, 12000.0f}) {
        CAPTURE(fc);
        CHECK(peakAtFc(fc, 1.95f, false, VA_NL_NONE) == doctest::Approx(expect).epsilon(0.02));
        CHECK(peakAtFc(fc, 1.95f, true,  VA_NL_NONE) == doctest::Approx(expect).epsilon(0.02));
    }
}

TEST_CASE("Korg35: peak follows 1/(2-k)")
{
    for (float k : {0.5f, 1.0f, 1.5f, 1.9f}) {
        CAPTURE(k);
        CHECK(peakAtFc(12000.0f, k, false, VA_NL_NONE)
              == doctest::Approx(1.0 / (2.0 - (double)k)).epsilon(0.02));
    }
}

TEST_CASE("Korg35: linear feedback is STABLE across the whole range")
{
    // VA_NL_SAT was documented as mandatory because the linear form diverged.
    // It diverged because the solve was wrong, not because the topology is
    // unstable. mapResonance caps k at 1.95, strictly below the k = 2 blow-up,
    // so with the algebra right the linear form has headroom by construction.
    for (float fc : {100.0f, 2000.0f, 6000.0f, 12000.0f}) {
        for (float k : {1.5f, 1.9f, 1.95f}) {
            CAPTURE(fc); CAPTURE(k);
            CHECK(peakAtFc(fc, k, false, VA_NL_NONE) > 0.0);   // -1 would be NaN
            CHECK(peakAtFc(fc, k, true,  VA_NL_NONE) > 0.0);
        }
    }
}

TEST_CASE("Korg35: the scaled saturator is transparent below oscillation")
{
    // The old bare va_sat(x) had its knee fixed at +/-1, so at high resonance it
    // compressed the feedback ~13x and flattened the peak to ~1.4 whatever the
    // knob did. korg35_sat divides in and multiplies back out, giving unit slope
    // at the origin, so the peak must now track the ideal 1/(2-k).
    for (float k : {0.5f, 1.0f, 1.5f}) {
        CAPTURE(k);
        // Small-signal: the limiter must be out of the way here. At FULL scale
        // it legitimately engages (k=1.5 measures 1.43 against the ideal 2.0),
        // which is the level-dependent behaviour we want, not a defect.
        CHECK(peakAtFc(2000.0f, k, false, VA_NL_SAT, 0.05f)
              == doctest::Approx(1.0 / (2.0 - (double)k)).epsilon(0.05));
    }
}

TEST_CASE("Korg35: SINGS, with onset at the theoretical k = 2")
{
    // The TSK poles reach the imaginary axis at k = 2. mapResonance now reaches
    // 2.05 at full knob so the threshold is actually crossed; below it the
    // filter must stay silent, or the resonance knob would be unusable.
    double pk = 0.0;
    CHECK(selfOscTail(2000.0f, 1.90f, false, &pk) == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(selfOscTail(2000.0f, 1.98f, false, &pk) == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(selfOscTail(2000.0f, 2.05f, false, &pk) > 0.3);      // sings, audibly
    CHECK(selfOscTail(2000.0f, 2.05f, true,  &pk) > 0.3);      // HP too
}

TEST_CASE("Korg35: the ring is a LIMIT CYCLE, not a decaying tail")
{
    // A true limit cycle settles to the same amplitude regardless of how hard it
    // was excited. If this ever became drive-dependent it would mean the filter
    // is ringing down from the excitation rather than sustaining on its own.
    double p1 = 0.0, p4 = 0.0;
    const double p1r = selfOscTail(6000.0f, 2.05f, false, &p1, 1.0f);
    const double p4r = selfOscTail(6000.0f, 2.05f, false, &p4, 4.0f);
    CHECK(p4r == doctest::Approx(p1r).epsilon(0.01));
    CHECK(p4  == doctest::Approx(p1).epsilon(0.01));
}

TEST_CASE("Korg35: oscillation stays bounded across the whole cutoff range")
{
    for (float fc : {100.0f, 500.0f, 2000.0f, 6000.0f, 12000.0f}) {
        double pk = 0.0;
        const double r = selfOscTail(fc, 2.05f, false, &pk, 4.0f);
        CAPTURE(fc);
        CHECK(std::isfinite(r));
        CHECK(pk < 1.5);
    }
}
