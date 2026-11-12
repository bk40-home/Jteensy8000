// =============================================================================
// test_bend_wheel.cpp — pitch bend up/down ranges, held-bend re-scaling, and
// the mod wheel's LFO2 gate (2026-10-31, scene 19 follow-up).
// =============================================================================
// Measured on the JE-8086 (fm1-port/scene19-wheel-bend-knees-analysis):
//   * bend up and down have INDEPENDENT ranges (voice.bend_range = up,
//     voice.bend_range_down = down);
//   * a range change acts on a bend that is already held — it must not wait
//     for the next bend message (the old engine did, so every capture cell
//     showed the previous cell's range);
//   * LFO2 is silent with the wheel down and modulates with it up.
// Pitch is read by zero crossings of a sine oscillator, as in the other
// voice-level tests.
// =============================================================================
#include "doctest.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/SynthCore.h"
#include "core/dsp/Curves.h"
#include "gen/ParamTable.h"

using namespace JT;
using namespace JT::Params;

namespace {

struct Rig {
    ParameterStore     store;
    std::vector<float> pool;
    SynthCore          core;
    Rig()
        : pool((size_t)SynthCore::kCombPoolFloats, 0.0f),
          core(store, pool.data())
    {
        setE(ID::MIX_OSC2, 0.0f);
        setE(ID::MIX_SUB, 0.0f);
        setE(ID::MIX_NOISE, 0.0f);
        setOpt(ID::OSC1_WAVE, (float)(int)Wave::Sine);
        setE(ID::ENV_AMP_ATTACK, 0.0f);
        setE(ID::ENV_AMP_SUSTAIN, 1.0f);
        for (int i = 0; i < 20; ++i) block();   // drain defaults
    }
    void setE(uint16_t id, float eng)  { store.setEngineering(id, eng, Origin::Ui); }
    void setOpt(uint16_t id, float o)
    { store.set(id, Curves::toNorm(kParams[ParameterStore::indexOf(id)], o), Origin::Ui); }
    void block() { float L[kBlockSize], R[kBlockSize]; core.renderBlock(L, R, kBlockSize); }
    std::vector<float> grab(int blocks)
    {
        std::vector<float> out;
        float L[kBlockSize], R[kBlockSize];
        for (int b = 0; b < blocks; ++b) {
            core.renderBlock(L, R, kBlockSize);
            out.insert(out.end(), L, L + kBlockSize);
        }
        return out;
    }
    // Settle a few blocks (parameter and bend events drain at block start),
    // then measure.
    float semisAfterSettle(float refHz)
    {
        for (int i = 0; i < 4; ++i) block();
        const auto v = grab(120);                     // ~0.32 s at 48 kHz
        size_t c = 0;
        for (size_t i = 1; i < v.size(); ++i) if (v[i - 1] <= 0.0f && v[i] > 0.0f) ++c;
        const float hz = (float)((double)c * (double)kSampleRate / (double)v.size());
        return 12.0f * std::log2(hz / refHz);
    }
};

constexpr float kA4 = 440.0f;
constexpr uint16_t kBendUp = 16383, kBendDown = 0, kBendCentre = 8192;

} // namespace

TEST_CASE("pitch bend: up and down use their own ranges")
{
    Rig r;
    r.setE(ID::VOICE_BEND_RANGE, 7.0f);         // up
    r.setE(ID::VOICE_BEND_RANGE_DOWN, 5.0f);    // down
    r.block();
    r.core.noteOn(69, 100);
    CHECK(r.semisAfterSettle(kA4) == doctest::Approx(0.0f).epsilon(0.15));
    r.core.pitchBend(kBendUp);
    CHECK(r.semisAfterSettle(kA4) == doctest::Approx(7.0f).epsilon(0.02));
    r.core.pitchBend(kBendDown);
    CHECK(r.semisAfterSettle(kA4) == doctest::Approx(-5.0f).epsilon(0.03));
    r.core.pitchBend(kBendCentre);
    CHECK(std::fabs(r.semisAfterSettle(kA4)) < 0.15f);
}

TEST_CASE("pitch bend: a range change re-scales a HELD bend at once")
{
    Rig r;
    r.setE(ID::VOICE_BEND_RANGE, 2.0f);
    r.block();
    r.core.noteOn(69, 100);
    r.core.pitchBend(kBendUp);
    CHECK(r.semisAfterSettle(kA4) == doctest::Approx(2.0f).epsilon(0.05));
    r.setE(ID::VOICE_BEND_RANGE, 12.0f);        // no new bend message
    CHECK(r.semisAfterSettle(kA4) == doctest::Approx(12.0f).epsilon(0.02));
    r.core.pitchBend(kBendDown);                // down still uses its own range
    CHECK(r.semisAfterSettle(kA4) == doctest::Approx(-2.0f).epsilon(0.05));
    r.setE(ID::VOICE_BEND_RANGE_DOWN, 12.0f);
    CHECK(r.semisAfterSettle(kA4) == doctest::Approx(-12.0f).epsilon(0.02));
}

TEST_CASE("pitch bend: a zero down range ignores downward bends only")
{
    Rig r;
    r.setE(ID::VOICE_BEND_RANGE, 12.0f);
    r.setE(ID::VOICE_BEND_RANGE_DOWN, 0.0f);
    r.block();
    r.core.noteOn(69, 100);
    r.core.pitchBend(kBendDown);
    CHECK(std::fabs(r.semisAfterSettle(kA4)) < 0.15f);
    r.core.pitchBend(kBendUp);
    CHECK(r.semisAfterSettle(kA4) == doctest::Approx(12.0f).epsilon(0.02));
}

TEST_CASE("mod wheel gates LFO2: silent at 0, modulating at 127")
{
    Rig r;
    r.setE(ID::LFO2_PITCH_DEPTH, 0.5f);
    r.setE(ID::LFO2_FREQ, 5.0f);
    r.setOpt(ID::LFO2_MOD_WHEEL, 1.0f);         // armed (also the table default)
    r.block();
    r.core.noteOn(69, 100);
    // Pitch spread across 30 ms windows: the wheel-down case must be flat.
    // Period-by-period pitch from linearly interpolated upward zero crossings
    // (sub-sample accurate), so the spread is the LFO, not quantisation.
    auto spread = [&r]() {
        const auto v = r.grab(150);                   // ~0.4 s: 2 cycles at 5 Hz
        float lo = 1e9f, hi = -1e9f, prev = -1.0f;
        for (size_t i = 1; i < v.size(); ++i) {
            if (v[i - 1] <= 0.0f && v[i] > 0.0f) {
                const float t = (float)(i - 1) + v[i - 1] / (v[i - 1] - v[i]);
                if (prev >= 0.0f) {
                    const float hz = kSampleRate / (t - prev);
                    lo = std::min(lo, hz); hi = std::max(hi, hz);
                }
                prev = t;
            }
        }
        return 12.0f * std::log2(hi / lo);
    };
    r.core.modWheel(0);
    for (int i = 0; i < 4; ++i) r.block();
    CHECK(spread() < 0.05f);                         // flat: LFO2 gated off
    r.core.modWheel(127);
    for (int i = 0; i < 4; ++i) r.block();
    CHECK(spread() > 2.0f);
}

TEST_CASE("LFO2 -> pitch follows JpModLaw (JE +32 at full wheel: 13.6 st p-p)")
{
    Rig r;
    const float d = 32.0f / 63.0f;                   // JP +32
    r.setE(ID::LFO2_PITCH_DEPTH, d);
    r.setE(ID::LFO2_FREQ, 2.0f);                     // slow: clean per-period reads
    r.setOpt(ID::LFO2_MOD_WHEEL, 1.0f);
    r.block();
    r.core.noteOn(69, 100);
    r.core.modWheel(127);
    for (int i = 0; i < 8; ++i) r.block();
    const auto v = r.grab(400);                      // ~1.07 s: two LFO cycles
    float lo = 1e9f, hi = -1e9f, prev = -1.0f;
    for (size_t i = 1; i < v.size(); ++i) {
        if (v[i - 1] <= 0.0f && v[i] > 0.0f) {
            const float t = (float)(i - 1) + v[i - 1] / (v[i - 1] - v[i]);
            if (prev >= 0.0f) { const float hz = kSampleRate / (t - prev); lo = std::min(lo, hz); hi = std::max(hi, hz); }
            prev = t;
        }
    }
    const float ppSemis = 12.0f * std::log2(hi / lo);
    // Curve: 2 x lfoPitchSemis(32/63) = 14.3 st (JE measured 13.56).  Was 7.1
    // with the old linear +-7 st law.
    CHECK(ppSemis == doctest::Approx(14.3f).epsilon(0.06));
}

TEST_CASE("tremolo depth law: index = depth^2 (JE LFO -> amp +32: 4.8 dB p-p)")
{
    Rig r;
    r.setE(ID::LFO2_AMP_DEPTH, 32.0f / 63.0f);
    r.setE(ID::LFO2_FREQ, 2.0f);
    r.setOpt(ID::LFO2_MOD_WHEEL, 1.0f);
    r.block();
    r.core.noteOn(69, 100);
    r.core.modWheel(127);
    for (int i = 0; i < 8; ++i) r.block();
    // RMS over exactly 11 periods of A4 (1200 samples at 48 kHz) so the window
    // itself adds no ripple; slid in 128-sample steps across two LFO cycles.
    const auto v = r.grab(400);
    const size_t w = (size_t)(11.0f * kSampleRate / kA4 + 0.5f);
    float lo = 1e9f, hi = -1e9f;
    for (size_t i = 0; i + w < v.size(); i += kBlockSize) {
        double acc = 0.0;
        for (size_t k = i; k < i + w; ++k) acc += (double)v[k] * (double)v[k];
        const float rms = (float)std::sqrt(acc / (double)w);
        lo = std::min(lo, rms); hi = std::max(hi, rms);
    }
    const float ppDb = 20.0f * std::log10(hi / lo);
    // index (32/63)^2 = 0.258 -> 20 log10(1.258/0.742) = 4.6 dB (JE 4.8); the
    // 25 ms window shaves a little off the peaks of a 2 Hz triangle.
    // The old linear index gave ~9.6 dB.
    CHECK(ppDb == doctest::Approx(4.6f).epsilon(0.12));
}

TEST_CASE("pitch bend reaches layer B in a split (J4)")
{
    // Split with both layers on channel 1 (B defaults to channel 2, so it is
    // set here), split note 60 (default): A5 (note 81) sounds on layer B only.
    // Before J4 the bend went to layer A alone, so this note did not move.
    Rig r;
    for (uint8_t layer = 0; layer < 2; ++layer) {   // same sine voice on both
        r.store.setEngineering(ID::MIX_OSC2, 0.0f, Origin::Ui, layer);
        r.store.setEngineering(ID::MIX_SUB, 0.0f, Origin::Ui, layer);
        r.store.setEngineering(ID::MIX_NOISE, 0.0f, Origin::Ui, layer);
        r.store.set(ID::OSC1_WAVE,
                    Curves::toNorm(kParams[ParameterStore::indexOf(ID::OSC1_WAVE)],
                                   (float)(int)Wave::Sine), Origin::Ui, layer);
        r.store.setEngineering(ID::ENV_AMP_ATTACK, 0.0f, Origin::Ui, layer);
        r.store.setEngineering(ID::ENV_AMP_SUSTAIN, 1.0f, Origin::Ui, layer);
        r.store.setEngineering(ID::VOICE_BEND_RANGE, 2.0f, Origin::Ui, layer);
    }
    r.setOpt(ID::PERF_MIDI_CHANNEL_B, 0.0f);       // option 0 = Ch 1
    r.setOpt(ID::PERF_MODE, (float)PerfRouter::kModeSplit);
    for (int i = 0; i < 20; ++i) r.block();
    constexpr float kA5 = 880.0f;
    r.core.noteOn(81, 100, 1);
    CHECK(r.semisAfterSettle(kA5) == doctest::Approx(0.0f).epsilon(0.15));
    r.core.pitchBend(kBendUp, 1);
    CHECK(r.semisAfterSettle(kA5) == doctest::Approx(2.0f).epsilon(0.05));
    r.core.pitchBend(kBendCentre, 1);
    CHECK(std::fabs(r.semisAfterSettle(kA5)) < 0.15f);
}
