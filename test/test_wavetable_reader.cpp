// =============================================================================
// test_wavetable_reader.cpp — proofs for OscCore's ONE table reader
// =============================================================================
// ARB (AKWF, 1 frame × 1 level) and the measured JP-8000 SHAPE morphs
// (17 frames × 7 band-limited levels) share renderTable().  These tests pin:
//   * SHAPE reaches EVERY frame — min, centre and max read frames 0, 8 and 16
//     sample-exact (no 5..95 % pulse clamp on the morph);
//   * a full SHAPE sweep is click-free (block-rate morph, no steps);
//   * the level selector keeps a high note alias-free;
//   * OscSection attaches the flash set when the wave is selected.
// ARB byte-identity across the merge is proved separately by a cmp of rendered
// output before/after (all FM/sync feature combinations) — see delivery note.
// =============================================================================
#include "doctest.h"

#include <cmath>
#include <vector>

#include "core/dsp/OscCore.h"
#include "core/OscSection.h"
#include "core/WavetableLib.h"
#include "core/AudioConfig.h"

using namespace JT;

namespace {

std::vector<float> renderN(OscCore& o, size_t total)
{
    std::vector<float> v(total);
    for (size_t off = 0; off < total; off += kBlockSize) {
        const size_t chunk = (total - off < kBlockSize) ? total - off : kBlockSize;
        o.render(v.data() + off, chunk, nullptr, 0.0f, nullptr, nullptr);
    }
    return v;
}

// Level-0 sample k of frame f, exactly as stored (int16 -> float, /32768).
float tableSample(const WaveTableSet& s, uint32_t f, uint32_t k)
{
    return (float)s.data[f * s.samplesPerFrame + s.levelOffset[0] + k] * (1.0f / 32768.0f);
}

} // namespace

TEST_CASE("JP morph sets: geometry is what the reader assumes")
{
    for (Wave w : { Wave::JpVarSaw, Wave::JpVarTri }) {
        const WaveTableSet* s = WavetableLib::jpMorphSet(w);
        REQUIRE(s != nullptr);
        REQUIRE(s->valid());
        CHECK(s->frames == 17);
        CHECK(s->levelLen[0] == 2048);
        uint32_t sum = 0;
        for (uint16_t l = 0; l < s->levels; ++l) {
            CHECK(s->levelOffset[l] == sum);              // contiguous levels
            if (l) CHECK(s->levelHarm[l] < s->levelHarm[l - 1]);   // most-harmonics-first
            sum += s->levelLen[l];
        }
        CHECK(sum == s->samplesPerFrame);
    }
    CHECK(WavetableLib::jpMorphSet(Wave::Saw) == nullptr);
}

TEST_CASE("JP morph: SHAPE min / centre / max read frames 0 / 8 / 16 exactly")
{
    // inc = 1/2048 exactly (dyadic): phase lands ON table points, frac = 0,
    // level 0 is selected (320 harmonics × 1/2048 < 0.5).  step() returns the
    // phase AFTER advancing, so output sample k reads table index k+1.
    const float hz = kSampleRate / 2048.0f;
    for (Wave w : { Wave::JpVarSaw, Wave::JpVarTri }) {
        const WaveTableSet* s = WavetableLib::jpMorphSet(w);
        struct { float shape; uint32_t frame; } cases[] = { {0.0f, 0}, {0.5f, 8}, {1.0f, 16} };
        for (auto c : cases) {
            OscCore o; o.setWave(w); o.setMorphSet(s);
            o.setFrequency(hz); o.resetPhase(0.0f);
            o.setShape(c.shape);
            const auto v = renderN(o, 2048);
            float worst = 0.0f;
            for (uint32_t k = 0; k < 2048; ++k)
                worst = std::fmax(worst, std::fabs(v[k] - tableSample(*s, c.frame, (k + 1u) % 2048u)));
            INFO("wave ", (int)w, " shape ", c.shape, " frame ", c.frame);
            CHECK(worst < 1e-6f);
        }
    }
}

TEST_CASE("JP morph: the 5..95 % pulse clamp does NOT limit the morph")
{
    // Shape 0.02 must be closer to frame 0 than shape 0.05 is — i.e. the
    // morph honours values the pulse clamp would have pinned.
    const float hz = kSampleRate / 2048.0f;
    const WaveTableSet* s = WavetableLib::jpMorphSet(Wave::JpVarSaw);
    auto errToFrame0 = [&](float shape) {
        OscCore o; o.setWave(Wave::JpVarSaw); o.setMorphSet(s);
        o.setFrequency(hz); o.resetPhase(0.0f); o.setShape(shape);
        const auto v = renderN(o, 2048);
        double e = 0.0;
        for (uint32_t k = 0; k < 2048; ++k) {
            const double d = v[k] - tableSample(*s, 0, (k + 1u) % 2048u);
            e += d * d;
        }
        return e;
    };
    CHECK(errToFrame0(0.02f) < errToFrame0(0.05f));
}

TEST_CASE("JP morph: a full SHAPE sweep is click-free")
{
    // Sweep 0 -> 1 over 2 s at 220 Hz.  The largest sample-to-sample step
    // while sweeping must stay within the largest step of the static end
    // frames (+10 %): no block-boundary jumps from the morph.
    for (Wave w : { Wave::JpVarSaw, Wave::JpVarTri }) {
        const WaveTableSet* s = WavetableLib::jpMorphSet(w);
        auto maxStep = [](const std::vector<float>& v) {
            float m = 0.0f;
            for (size_t i = 1; i < v.size(); ++i) m = std::fmax(m, std::fabs(v[i] - v[i - 1]));
            return m;
        };
        float staticMax = 0.0f;
        for (float sh = 0.0f; sh <= 1.0f; sh += 1.0f / 16.0f) {   // every frame
            OscCore o; o.setWave(w); o.setMorphSet(s); o.setFrequency(220.0f); o.setShape(sh);
            staticMax = std::fmax(staticMax, maxStep(renderN(o, 4800)));
        }
        OscCore o; o.setWave(w); o.setMorphSet(s); o.setFrequency(220.0f);
        const size_t total = (size_t)(2.0f * kSampleRate);
        std::vector<float> v(total);
        for (size_t off = 0; off < total; off += kBlockSize) {
            o.setShape((float)off / (float)total);
            o.render(v.data() + off, kBlockSize, nullptr, 0.0f, nullptr, nullptr);
        }
        for (float x : v) REQUIRE(std::isfinite(x));
        for (float x : v) REQUIRE(std::fabs(x) <= 1.0f);
        INFO("wave ", (int)w, " sweep max step ", maxStep(v), " static max ", staticMax);
        CHECK(maxStep(v) <= staticMax * 1.10f);
    }
}

TEST_CASE("JP morph: level selection keeps a high note alias-free")
{
    // 2345.6 Hz (non-integer period).  Hann-windowed DFT over 0.1 s: energy
    // more than 40 Hz away from any harmonic must be < 1 % of the total.
    // (A full-band 2048-point table here would fold ~300 harmonics.)
    const float hz = 2345.6f;
    const size_t N = (size_t)(0.1f * kSampleRate);
    for (Wave w : { Wave::JpVarSaw, Wave::JpVarTri }) {
        OscCore o; o.setWave(w); o.setMorphSet(WavetableLib::jpMorphSet(w));
        o.setFrequency(hz); o.setShape(0.5f);
        const auto v = renderN(o, N);
        const double pi = 3.14159265358979323846;
        double onH = 0.0, total = 0.0;
        for (size_t b = 1; b < N / 2; ++b) {
            const double f = (double)b * (double)kSampleRate / (double)N;
            double re = 0.0, im = 0.0;
            for (size_t i = 0; i < N; ++i) {
                const double win = 0.5 - 0.5 * std::cos(2.0 * pi * (double)i / (double)(N - 1));
                const double ph  = 2.0 * pi * (double)b * (double)i / (double)N;
                re += win * (double)v[i] * std::cos(ph);
                im -= win * (double)v[i] * std::sin(ph);
            }
            const double e = re * re + im * im;
            total += e;
            const double h = f / (double)hz;
            if (std::fabs(h - std::round(h)) * (double)hz < 40.0) onH += e;
        }
        INFO("wave ", (int)w, " off-harmonic fraction ", 1.0 - onH / total);
        CHECK(1.0 - onH / total < 0.01);
    }
}

TEST_CASE("OscSection: selecting JvSAW / JvTRI attaches the measured set")
{
    // Without a set the core would fall back to a naive saw.  The section
    // must attach it on setWave, so the output differs from that fallback.
    for (int opt : { (int)Wave::JpVarSaw, (int)Wave::JpVarTri }) {
        OscSection jp;  jp.noteOn(kSampleRate / 100.0f, 3);
        OscSection saw; saw.noteOn(kSampleRate / 100.0f, 3);
        jp.setMixOsc2(0.0f);  saw.setMixOsc2(0.0f);
        jp.setWave(0, opt);   saw.setWave(0, (int)Wave::Saw);
        std::vector<float> a(38 * kBlockSize), b(38 * kBlockSize);   // whole blocks
        for (size_t off = 0; off < a.size(); off += kBlockSize) {
            jp.render(a.data() + off, kBlockSize);
            saw.render(b.data() + off, kBlockSize);
        }
        double diff = 0.0;
        for (size_t i = 0; i < a.size(); ++i) diff += std::fabs((double)a[i] - (double)b[i]);
        CHECK(diff / (double)a.size() > 0.05);
        for (float x : a) REQUIRE(std::isfinite(x));
    }
}
