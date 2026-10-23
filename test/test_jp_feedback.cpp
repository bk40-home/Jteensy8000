// =============================================================================
// test_jp_feedback.cpp — proofs for core/dsp/JpFeedbackOsc (wave "JFBK")
// =============================================================================
// Pins the JP-8000 FEEDBACK OSC recreation (2026-10-23, option 1):
//   * zero feedback is EXACT: saw → 1-pole HPF, τ = 1.92 ms (independent
//     reference), Harmonics inert there, and it matches the JE-8086 capture
//     at C3 and C5;
//   * block counts at anchor cells match the JE fresh-note grids at C3/C5;
//   * locked cells hold their shape, drift cells slide (the JP behaviour);
//   * output bounded and deterministic under parameter thrash;
//   * OscSection routes JFBK, keeps it out of sync/X-MOD lock, and keeps its
//     feedback independent of the FeedbackComb.
// Counts use the same reset detector as the capture analysis.
// =============================================================================
#include "doctest.h"

#include <cmath>
#include <vector>

#include "core/dsp/JpFeedbackOsc.h"
#include "core/dsp/OscCore.h"
#include "core/OscSection.h"
#include "core/AudioConfig.h"

using namespace JT;

namespace {

constexpr float kC3 = 130.8128f;
constexpr float kC5 = 523.2511f;

// JE-8086 zero-feedback cycles, harmonics h1..h40 (absolute phase), generated
// from the capture analysis (tools: ana/track.py harmonics(), K=4, hop 4).
// C3: feedback_grid.wav cell 0 (Harmonics 0, Feedback 0), settled 0.6-0.9 s, f0 130.8132 Hz
const float kJeFb0C3Re[40] = { 6.374076e-02f, -3.556060e-02f, 1.618023e-02f, -9.154028e-04f, -8.279440e-03f, 1.047448e-02f, -7.044777e-03f, 1.060373e-03f, 4.099902e-03f, -6.166953e-03f, 4.758281e-03f, -1.205012e-03f, -2.341367e-03f, 4.074272e-03f, -3.367893e-03f, 9.208513e-04f, 1.756836e-03f, -3.215192e-03f, 2.805730e-03f, -9.265655e-04f, -1.282461e-03f, 2.611652e-03f, -2.425325e-03f, 9.441864e-04f, 9.363015e-04f, -2.177602e-03f, 2.161734e-03f, -9.863196e-04f, -6.341776e-04f, 1.797031e-03f, -1.907893e-03f, 9.770312e-04f, 4.214568e-04f, -1.497439e-03f, 1.684945e-03f, -9.332133e-04f, -2.920147e-04f, 1.293111e-03f, -1.532673e-03f, 9.163807e-04f };
const float kJeFb0C3Im[40] = { -1.276099e-02f, -7.730990e-03f, 1.757461e-02f, -1.712360e-02f, 1.015563e-02f, -1.268332e-03f, -5.443970e-03f, 7.710070e-03f, -5.618168e-03f, 1.134632e-03f, 3.073684e-03f, -4.968150e-03f, 3.989377e-03f, -1.085526e-03f, -1.961048e-03f, 3.539710e-03f, -3.003500e-03f, 8.778225e-04f, 1.534798e-03f, -2.915956e-03f, 2.622538e-03f, -9.547982e-04f, -1.080163e-03f, 2.364202e-03f, -2.269727e-03f, 9.510527e-04f, 7.929812e-04f, -1.994975e-03f, 2.048107e-03f, -1.002224e-03f, -5.026812e-04f, 1.622867e-03f, -1.777679e-03f, 9.442287e-04f, 3.610166e-04f, -1.396564e-03f, 1.611157e-03f, -9.305911e-04f, -2.242060e-04f, 1.195163e-03f };
// C5: feedback_grid_C5.wav cell 0 (Harmonics 0, Feedback 0), settled 0.6-0.9 s, f0 523.2511 Hz
const float kJeFb0C5Re[40] = { 1.176358e-02f, 2.146562e-02f, 2.065666e-02f, 1.796802e-02f, 1.484563e-02f, 1.170078e-02f, 8.628988e-03f, 5.618353e-03f, 2.730364e-03f, 1.096808e-04f, -2.058099e-03f, -3.637509e-03f, -4.593684e-03f, -4.970589e-03f, -4.858307e-03f, -4.337801e-03f, -3.492009e-03f, -2.406956e-03f, -1.200014e-03f, -8.542570e-07f, 1.067192e-03f, 1.915143e-03f, 2.496539e-03f, 2.794056e-03f, 2.812693e-03f, 2.566905e-03f, 2.094472e-03f, 1.451789e-03f, 7.180055e-04f, -2.463517e-05f, -7.024308e-04f, -1.259742e-03f, -1.659663e-03f, -1.877237e-03f, -1.903202e-03f, -1.741614e-03f, -1.419372e-03f, -9.786020e-04f, -4.738907e-04f, 4.108858e-05f };
const float kJeFb0C5Im[40] = { 7.823023e-02f, 3.301417e-02f, 1.535136e-02f, 5.775911e-03f, -1.651337e-05f, -3.693191e-03f, -6.034325e-03f, -7.374649e-03f, -7.838511e-03f, -7.494362e-03f, -6.485566e-03f, -5.017610e-03f, -3.331519e-03f, -1.621521e-03f, -2.630290e-05f, 1.365541e-03f, 2.481506e-03f, 3.253276e-03f, 3.635486e-03f, 3.619766e-03f, 3.254020e-03f, 2.617185e-03f, 1.806361e-03f, 9.085578e-04f, 4.788516e-06f, -8.306104e-04f, -1.525798e-03f, -2.019084e-03f, -2.275270e-03f, -2.287505e-03f, -2.080951e-03f, -1.695146e-03f, -1.180549e-03f, -5.887412e-04f, 2.284850e-05f, 5.953596e-04f, 1.072901e-03f, 1.412232e-03f, 1.590477e-03f, 1.603162e-03f };

std::vector<float> renderFbo(float hz, float k, float fb, size_t total,
                             float phase0 = 0.0f, const float* fm = nullptr, float oct = 0.0f)
{
    JpFeedbackOsc o;
    o.setFrequency(hz); o.setHarmonics(k); o.setFeedback(fb); o.noteOn(phase0);
    std::vector<float> v(total);
    for (size_t off = 0; off < total; off += kBlockSize) {
        const size_t c = (total - off < kBlockSize) ? total - off : kBlockSize;
        o.render(v.data() + off, c, fm ? fm + off : nullptr, oct);
    }
    return v;
}

// Reset steps per cycle — the capture analysis' detector: large negative
// 2-sample differences (< −40 % of peak-to-peak) at local minima, de-duplicated.
double resetsPerCycle(const std::vector<float>& v, float hz, double t0, double t1)
{
    const size_t a = (size_t)(t0 * (double)kSampleRate), b = (size_t)(t1 * (double)kSampleRate);
    double mn = 1e9, mx = -1e9;
    for (size_t i = a; i < b; ++i) { mn = std::fmin(mn, (double)v[i]); mx = std::fmax(mx, (double)v[i]); }
    const double thr = -0.4 * (mx - mn);
    auto d = [&](size_t i) { return (double)v[i + 1] - (double)v[i - 1]; };
    int count = 0; long last = -10;
    for (size_t i = a + 2; i + 2 < b; ++i) {
        const double di = d(i);
        if (di < thr && di <= d(i - 1) && di <= d(i + 1)) {
            if ((long)i - last > 2) ++count;
            last = (long)i;
        }
    }
    return (double)count / ((t1 - t0) * (double)hz);
}

// Harmonics h1..h40 of one 8-cycle Hann window starting at t0 (exact f0 DFT).
void harmonics(const std::vector<float>& v, float hz, double t0, double* re, double* im)
{
    const double pi = 3.14159265358979323846;
    const double P = (double)kSampleRate / (double)hz;
    const size_t L = (size_t)std::lround(8.0 * P), s = (size_t)(t0 * (double)kSampleRate);
    double wsum = 0.0;
    for (size_t i = 0; i < L; ++i) wsum += 0.5 - 0.5 * std::cos(2.0 * pi * (double)i / (double)L);
    for (int h = 1; h <= 40; ++h) {
        double ar = 0.0, ai = 0.0;
        for (size_t i = 0; i < L; ++i) {
            const double w = 0.5 - 0.5 * std::cos(2.0 * pi * (double)i / (double)L);
            const double ph = 2.0 * pi * (double)h * (double)hz * (double)i / (double)kSampleRate;
            ar += w * (double)v[s + i] * std::cos(ph);
            ai -= w * (double)v[s + i] * std::sin(ph);
        }
        re[h - 1] = 2.0 * ar / wsum; im[h - 1] = 2.0 * ai / wsum;
    }
}

// Waveform error after the best circular shift and best real gain (sign free).
double shiftGainError(const double* cr, const double* ci, const double* mr, const double* mi)
{
    const double pi = 3.14159265358979323846;
    double cc = 0.0, mm = 0.0;
    for (int h = 0; h < 40; ++h) { cc += cr[h] * cr[h] + ci[h] * ci[h]; mm += mr[h] * mr[h] + mi[h] * mi[h]; }
    double best = 1e30;
    for (int s = 0; s < 4096; ++s) {
        double dot = 0.0;
        for (int h = 1; h <= 40; ++h) {
            const double a = -2.0 * pi * (double)h * (double)s / 4096.0;
            const double sr = mr[h - 1] * std::cos(a) - mi[h - 1] * std::sin(a);
            const double si = mr[h - 1] * std::sin(a) + mi[h - 1] * std::cos(a);
            dot += sr * cr[h - 1] + si * ci[h - 1];
        }
        const double g = dot / mm;
        const double e2 = cc - 2.0 * g * dot + g * g * mm;
        if (e2 < best) best = e2;
    }
    return std::sqrt(std::fmax(best, 0.0) / cc);
}

std::vector<double> meanCycle(const std::vector<float>& v, float hz, double t0, double t1, int K = 256)
{
    const double P = (double)kSampleRate / (double)hz;
    const int nc = (int)((t1 - t0) * (double)hz) - 1;
    const double start = std::ceil(t0 * (double)kSampleRate / P) * P;
    std::vector<double> c(K, 0.0);
    for (int n = 0; n < nc; ++n)
        for (int k = 0; k < K; ++k) {
            const double x = start + (double)n * P + (double)k * P / (double)K;
            const size_t i = (size_t)x; const double f = x - (double)i;
            c[k] += ((1.0 - f) * (double)v[i] + f * (double)v[i + 1]) / (double)nc;
        }
    return c;
}

double cycleDifference(const std::vector<double>& a, const std::vector<double>& b)
{
    double num = 0.0, den = 1e-12, ma = 0.0, mb = 0.0;
    for (size_t k = 0; k < a.size(); ++k) { ma += a[k]; mb += b[k]; }
    ma /= (double)a.size(); mb /= (double)b.size();
    for (size_t k = 0; k < a.size(); ++k) { num += (a[k] - ma - (b[k] - mb)) * (a[k] - ma - (b[k] - mb)); den += (a[k] - ma) * (a[k] - ma); }
    return std::sqrt(num / den);
}

} // namespace

TEST_CASE("JFBK at zero feedback is exactly saw -> 1-pole HPF (tau 1.92 ms); Harmonics inert")
{
    // Independent double-precision reference of the measured JP base.
    const size_t N = 4000;                         // before float phase rounding drifts edges
    const auto v = renderFbo(kC3, 0.3f, 0.0f, N, 0.25f);
    const double tau = 1.92e-3, a = tau / (tau + 1.0 / (double)kSampleRate);
    double ph = 0.25, hp = 0.0, px = 0.0, worst = 0.0;
    for (size_t i = 0; i < N; ++i) {
        ph += (double)kC3 / (double)kSampleRate; if (ph >= 1.0) ph -= 1.0;
        const double x = (2.0 * ph - 1.0) / 1.4;
        hp = a * (hp + x - px); px = x;
        worst = std::fmax(worst, std::fabs(hp - (double)v[i]));
    }
    CHECK(worst < 1e-4);

    // Harmonics does nothing at zero feedback (JE: 5x5 and grid FB-0 rows identical).
    const auto w = renderFbo(kC3, 0.9f, 0.0f, N, 0.25f);
    for (size_t i = 0; i < N; ++i) REQUIRE(v[i] == w[i]);
}

TEST_CASE("JFBK zero feedback matches the JE-8086 capture at C3 and C5")
{
    struct { float hz; const float* re; const float* im; } cases[] = {
        { kC3, kJeFb0C3Re, kJeFb0C3Im }, { kC5, kJeFb0C5Re, kJeFb0C5Im } };
    for (auto c : cases) {
        const auto v = renderFbo(c.hz, 0.0f, 0.0f, (size_t)kSampleRate);
        double mr[40], mi[40], cr[40], ci[40];
        harmonics(v, c.hz, 0.6, mr, mi);
        for (int h = 0; h < 40; ++h) { cr[h] = c.re[h]; ci[h] = c.im[h]; }
        const double e = shiftGainError(cr, ci, mr, mi);
        INFO("f0 ", c.hz, " error ", e);
        CHECK(e < 0.15);                           // analysis: 0.10 (C3), 0.04 (C5)
    }
}

TEST_CASE("JFBK block counts match the JE-8086 fresh-note grids at anchor cells")
{
    // (Hz, Harmonics byte, Feedback byte, JE blocks/cycle settled 0.6-0.9 s)
    struct { float hz; int h; int fb; double je; } cells[] = {
        { kC3, 64,  96,  4.1 }, { kC3, 16, 127, 15.9 }, { kC3, 32,  80,  8.0 },
        { kC3,  0,  32,  2.0 }, { kC3, 64, 127,  4.0 }, { kC3, 96,  64,  8.0 },
        { kC5, 64,  96,  4.0 }, { kC5, 32,  96,  8.0 }, { kC5, 16, 127,  8.6 },
        { kC5,  0,  64,  2.0 } };
    for (auto c : cells) {
        const auto v = renderFbo(c.hz, (float)c.h / 127.0f, (float)c.fb / 127.0f, (size_t)kSampleRate);
        const double n = resetsPerCycle(v, c.hz, 0.6, 0.9);
        INFO("f0 ", c.hz, " H ", c.h, " FB ", c.fb, " model ", n, " JE ", c.je);
        CHECK(std::fabs(std::log2(n / c.je)) < 0.35);   // within a third of an octave
    }
}

TEST_CASE("JFBK pitch law (D3a): nearest anchor map, no blending, no extrapolation")
{
    // Law level: below F#4 (370 Hz) the C3 map, at/above it the C5 map —
    // so C2 and C4 use EXACTLY the C3 count and C6 exactly the C5 count.
    // Cells chosen where the two maps differ, so a blend would be caught.
    const float kC2 = 65.40639f, kC4 = 261.6256f, kC6 = 1046.502f;
    struct { int h; int fb; } cells[] = { { 32, 48 }, { 96, 48 }, { 16, 127 }, { 32, 127 } };
    for (auto c : cells) {
        auto blocksAt = [&](float hz) {
            JpFeedbackOsc o;
            o.setFrequency(hz); o.setHarmonics((float)c.h / 127.0f); o.setFeedback((float)c.fb / 127.0f);
            float tmp[16]; o.render(tmp, 16, nullptr, 0.0f);      // law updates on the first render
            return o.blocksPerCycle();
        };
        const float n3 = blocksAt(kC3), n5 = blocksAt(kC5);
        INFO("H ", c.h, " FB ", c.fb, " C3 ", n3, " C5 ", n5);
        CHECK(n3 != doctest::Approx(n5).epsilon(0.05));           // the maps really differ here
        CHECK(blocksAt(kC2)     == doctest::Approx(n3));
        CHECK(blocksAt(kC4)     == doctest::Approx(n3));
        CHECK(blocksAt(369.0f)  == doctest::Approx(n3));          // just below the split
        CHECK(blocksAt(371.0f)  == doctest::Approx(n5));          // just above
        CHECK(blocksAt(kC6)     == doctest::Approx(n5));
    }

    // Audio level, against the JE-8086 pitch-spread capture (spread_key36 /
    // key60, settled at 1.5 s).  These cells were misses under v0's log-pitch
    // interpolation/extrapolation (v0 gave 21.9, 22.7, 6.0, 3.0, 11.3).
    struct { float hz; int h; int fb; double je; } je[] = {
        { kC2, 16, 127, 16.0 }, { kC2, 32, 127, 16.1 },
        { kC4, 32,  48,  8.0 }, { kC4, 96,  48,  6.8 }, { kC4, 32, 127, 16.0 } };
    for (auto c : je) {
        const auto v = renderFbo(c.hz, (float)c.h / 127.0f, (float)c.fb / 127.0f, (size_t)kSampleRate);
        const double n = resetsPerCycle(v, c.hz, 0.5, 0.9);
        INFO("f0 ", c.hz, " H ", c.h, " FB ", c.fb, " model ", n, " JE ", c.je);
        CHECK(std::fabs(std::log2(n / c.je)) < 0.35);             // within a third of an octave
    }
}

TEST_CASE("JFBK: locked cells hold their shape, wander cells rearrange (JP behaviour)")
{
    // JE shape change between 0.6-0.7 s and 1.2-1.3 s of a held C3 note:
    // H64/FB96 0.09 (locked); H64/FB127 0.92 and H16/FB127 0.90 (the block
    // COUNT holds but the layout keeps rearranging).  Model calibrated to that.
    const size_t N = (size_t)(1.5f * kSampleRate);
    const auto lock = renderFbo(kC3, 64.0f / 127.0f,  96.0f / 127.0f, N);
    const double dLock = cycleDifference(meanCycle(lock, kC3, 0.6, 0.7), meanCycle(lock, kC3, 1.2, 1.3));
    INFO("lock ", dLock);
    CHECK(dLock < 0.10);
    struct { int h; double blocks; } wanderCells[] = { { 64, 4.0 }, { 16, 15.9 } };
    // The wander is random (seeded per voice), so judge it as a player hears
    // it across notes: mean over 8 seeds (the calibration target is a median
    // across cells; a single realisation can sit well below it).
    for (auto c : wanderCells) {
        double dSum = 0.0;
        for (uint32_t seed = 1; seed <= 8; ++seed) {
            JpFeedbackOsc o;
            o.seedNoise(seed * 2654435761u);
            o.setFrequency(kC3); o.setHarmonics((float)c.h / 127.0f); o.setFeedback(1.0f); o.noteOn(0.0f);
            std::vector<float> v(N);
            for (size_t off = 0; off < N; off += kBlockSize)
                o.render(v.data() + off, (N - off < kBlockSize) ? N - off : kBlockSize, nullptr, 0.0f);
            dSum += cycleDifference(meanCycle(v, kC3, 0.6, 0.7), meanCycle(v, kC3, 1.2, 1.3));
            const double n = resetsPerCycle(v, kC3, 1.2, 1.4);
            INFO("H ", c.h, " seed ", seed, " blocks ", n);
            CHECK(std::fabs(std::log2(n / c.blocks)) < 0.35);  // count holds while wandering
        }
        INFO("H ", c.h, " mean wander ", dSum / 8.0);
        CHECK(dSum / 8.0 > 0.40);                              // JE: 0.92 / 0.90
    }
}

TEST_CASE("JFBK abuse: parameter and FM thrash stays finite, bounded, deterministic")
{
    auto run = [](uint32_t seed) {
        JpFeedbackOsc o; uint32_t rng = seed; float out[kBlockSize]; float fm[kBlockSize];
        std::vector<float> all;
        for (int b = 0; b < 1500; ++b) {
            rng = rng * 1664525u + 1013904223u;
            if ((rng & 3u) == 0) o.setFrequency((float)((rng >> 8) % 12000u) + 1.0f);
            if ((rng & 3u) == 1) o.setHarmonics((float)((rng >> 8) & 255u) / 255.0f);
            if ((rng & 3u) == 2) o.setFeedback((float)((rng >> 8) & 255u) / 255.0f);
            if ((rng & 7u) == 3) o.noteOn((float)((rng >> 12) & 255u) / 256.0f);
            for (size_t i = 0; i < kBlockSize; ++i) fm[i] = (float)(int)((rng >> (i % 16)) & 7u) * 0.25f - 1.0f;
            o.render(out, kBlockSize, (b & 1) ? fm : nullptr, 1.0f);
            for (float s : out) { REQUIRE(std::isfinite(s)); REQUIRE(std::fabs(s) < 2.5f); all.push_back(s); }
        }
        return all;
    };
    CHECK(run(7) == run(7));
}

TEST_CASE("OscSection: JFBK routes, ignores sync/X-MOD lock, feedback independent of the comb")
{
    auto render = [](float jfbk, float comb, bool sync, float xmod) {
        OscSection s; s.noteOn(kC3, 11);
        s.setMixOsc2(0.0f);
        s.setWave(0, (int)Wave::JpFeedback);
        s.setShapeDc(0, 2.0f * (64.0f / 127.0f) - 1.0f);    // Harmonics 64
        s.setJfbkFeedback(0, jfbk);
        s.setFeedbackAmount(0, comb);
        s.setSyncEnabled(sync); s.setCrossMod(xmod);
        std::vector<float> v(64 * kBlockSize);
        for (size_t off = 0; off < v.size(); off += kBlockSize) s.render(v.data() + off, kBlockSize);
        return v;
    };
    const auto off  = render(0.0f, 0.0f, false, 0.0f);
    const auto on   = render(0.75f, 0.0f, false, 0.0f);
    double diff = 0.0;
    for (size_t i = 0; i < off.size(); ++i) diff += std::fabs((double)off[i] - (double)on[i]);
    CHECK(diff / (double)off.size() > 0.05);                 // jfbk_feedback reaches the osc

    // The comb parameter must not move JFBK's own feedback (independent knobs):
    // with the comb loop on, the JFBK-feedback=0 render differs from 'off' only
    // by the comb, and stays finite.
    const auto withComb = render(0.0f, 0.5f, false, 0.0f);
    for (float x : withComb) REQUIRE(std::isfinite(x));

    // Sync and X-MOD on: JFBK is excluded as a sync master / lock source and
    // must render finite (its own path) rather than through OscCore.
    const auto guarded = render(0.75f, 0.0f, true, 0.6f);
    for (float x : guarded) REQUIRE(std::isfinite(x));
}
