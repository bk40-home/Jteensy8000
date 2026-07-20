// =============================================================================
// test_performance.cpp — proofs for the Phase 4 PERFORMANCE subsystem
// =============================================================================
// Covers the six ParamTable §11 params wired this pass (see
// docs/PHASE4_PERFORMANCE_SPEC.md): GLIDE_ENABLE / GLIDE_TIME, VOICE_POLY_MODE
// (Poly/Mono/Unison), VOICE_UNISON_DETUNE, VOICE_BEND_RANGE (+ SynthCore::
// pitchBend), and VOICE_AMP_LEVEL (= v1 AMP_MOD_FIXED_LEVEL, the VCA-mod base).
//
// MEASUREMENT: sine carriers + rising-zero-crossing counts read directly as Hz
// (the test_pitch_env / test_lfo technique).  Glide and bend both ride the
// same octave-space FM path, so a numeric Hz read-out is the honest proof.
//
// WARM-UP: renderBlock drains note events BEFORE dirty params, so params are
// written, then ONE warm-up block is rendered, THEN the note-on — the Pass 8
// lesson recorded in test_velocity / test_lfo / test_bpmclock.
// =============================================================================
#include "doctest.h"

#include <cmath>
#include <vector>

#include "core/SynthCore.h"
#include "core/VoiceAllocator.h"
#include "core/dsp/Curves.h"
#include "gen/ParamTable.h"

using namespace JT;
using namespace JT::Params;

namespace {

// Each Rig owns a FRESH comb pool (heap): a shared static pool let one test's
// feedback-comb residue corrupt the next test's waveform (miscounted crossings).
// Per-Rig isolation makes every case independent.  Sine osc1, osc2 muted,
// instant attack, full sustain — a clean steady tone a crossing count reads.
struct Rig {
    ParameterStore     store;
    std::vector<float> pool;
    SynthCore          core;
    Rig()
        : pool((size_t)SynthCore::kCombPoolFloats, 0.0f),
          core(store, pool.data())
    {
        setN(ID::MIX_OSC2, 0.0f);
        setN(ID::MIX_SUB, 0.0f);
        setN(ID::MIX_NOISE, 0.0f);
        setOpt(ID::OSC1_WAVE, (float)(int)Wave::Sine);
        setE(ID::ENV_AMP_ATTACK, 0.0f);
        setE(ID::ENV_AMP_SUSTAIN, 1.0f);
        setE(ID::ENV_AMP_RELEASE, 5.0f);
    }
    void setE(uint16_t id, float eng)  { store.setEngineering(id, eng, Origin::Ui); }
    void setN(uint16_t id, float eng)  { store.setEngineering(id, eng, Origin::Ui); }
    void setOpt(uint16_t id, float o)
    { store.set(id, Curves::toNorm(kParams[ParameterStore::indexOf(id)], o), Origin::Ui); }
    void setNorm(uint16_t id, float n) { store.set(id, n, Origin::Ui); }

    void block() { float L[kBlockSize], R[kBlockSize]; core.renderBlock(L, R, kBlockSize); }
};

// Warm-up: flush the initial default-param drain so tone is fully established.
void settle(Rig& r) { for (int i = 0; i < 20; ++i) r.block(); }

size_t risingCrossings(const std::vector<float>& v, size_t begin, size_t end)
{
    size_t c = 0;
    for (size_t i = (begin == 0 ? 1 : begin); i < end; ++i)
        if (v[i - 1] <= 0.0f && v[i] > 0.0f) ++c;
    return c;
}
float freqHz(const std::vector<float>& v, size_t begin, size_t end)
{
    const size_t cycles = risingCrossings(v, begin, end);
    const double secs   = (double)(end - begin) / (double)kSampleRate;
    return (float)((double)cycles / secs);
}

// Collect `blocks` mono blocks (L channel) into a flat vector.
std::vector<float> grab(Rig& r, int blocks)
{
    std::vector<float> out;
    out.reserve((size_t)blocks * kBlockSize);
    float L[kBlockSize], R[kBlockSize];
    for (int b = 0; b < blocks; ++b) {
        r.core.renderBlock(L, R, kBlockSize);
        for (size_t k = 0; k < kBlockSize; ++k) out.push_back(L[k]);
    }
    return out;
}

float rms(const std::vector<float>& v, size_t begin, size_t end)
{
    double acc = 0.0;
    for (size_t i = begin; i < end; ++i) acc += (double)v[i] * (double)v[i];
    return (float)std::sqrt(acc / (double)(end - begin));
}

} // namespace

// =============================================================================
// GLIDE
// =============================================================================

TEST_CASE("performance: glide OFF jumps to the new note immediately")
{
    Rig r;
    settle(r);
    r.core.allSoundOff();
    r.block();

    // Note A4 (440), let it settle, then note A5 (880) with glide OFF (default).
    r.core.noteOn(69, 100);
    for (int i = 0; i < 20; ++i) r.block();
    r.core.noteOn(81, 100);           // +12 st
    const auto v = grab(r, 40);
    // First ~90 ms after the new note should ALREADY be ~880 Hz (no ramp).
    const float early = freqHz(v, 1000, 5000);
    CHECK(early == doctest::Approx(880.0f).epsilon(0.06));
}

TEST_CASE("performance: glide ON ramps from the previous note toward the new one")
{
    Rig r;
    settle(r);
    r.core.allSoundOff();
    r.setNorm(ID::GLIDE_ENABLE, 1.0f);     // toggle on
    // Small time norm: even so, v1's per-BLOCK rate (the 128x quirk) keeps the
    // slew slow, so we prove the RAMP (climb) not full arrival in a short window.
    r.setNorm(ID::GLIDE_TIME, 0.1f);       // ~2.6 ms literal; slow via the quirk
    r.block();                             // warm-up: drains params before note

    r.core.noteOn(69, 100);                // establish A4 (440) as previous pitch
    for (int i = 0; i < 20; ++i) r.block();
    r.core.noteOn(81, 100);                // glide toward A5 (880)

    // v1's rate is applied once per BLOCK (the documented ~128x quirk), so the
    // slew is slow: prove the RAMP — start near 440, climb monotonically, not
    // yet arrived — rather than full arrival inside a short window.
    auto w0 = grab(r, 40);
    auto w1 = grab(r, 40);
    auto w2 = grab(r, 40);
    const float f0 = freqHz(w0, 500, w0.size());
    const float f1 = freqHz(w1, 0, w1.size());
    const float f2 = freqHz(w2, 0, w2.size());
    CHECK(f0 > 440.0f);                     // has left the start pitch
    CHECK(f0 < 860.0f);                     // but not yet at target
    CHECK(f1 > f0);                         // climbing
    CHECK(f2 > f1);                         // still climbing (monotone slew)
}

TEST_CASE("performance: longer GLIDE_TIME climbs slower than a shorter one")
{
    // Same fixed window for both: the SLOWER (longer-time) glide has moved LESS
    // far up from 440 toward 880 by the end.  The "not arrived" check confirms
    // the v1 per-block quirk is in force (a literal few-ms glide still crawls).
    auto pitchAfter = [](float timeNorm) {
        Rig r;
        settle(r);
        r.core.allSoundOff();
        r.setNorm(ID::GLIDE_ENABLE, 1.0f);
        r.setNorm(ID::GLIDE_TIME, timeNorm);
        r.block();
        r.core.noteOn(69, 100);            // A4 440
        for (int i = 0; i < 20; ++i) r.block();
        r.core.noteOn(81, 100);            // glide to A5 880
        auto v = grab(r, 120);
        return freqHz(v, v.size() - 4000, v.size());   // pitch at window end
    };
    const float fast = pitchAfter(0.1f);
    const float slow = pitchAfter(0.5f);
    CHECK(fast > slow);                     // shorter time => further up by now
    CHECK(slow > 440.0f);                   // the slow one still moved a little
    CHECK(fast < 880.0f);                   // even the fast one hasn't arrived
}

// =============================================================================
// MONO
// =============================================================================

TEST_CASE("performance: Mono uses a single voice with last-note legato return")
{
    Rig r;
    settle(r);
    r.core.allSoundOff();
    r.setOpt(ID::VOICE_POLY_MODE, (float)(int)PolyMode::Mono);
    r.block();

    r.core.noteOn(60, 100);               // C4
    for (int i = 0; i < 8; ++i) r.block();
    r.core.noteOn(64, 100);               // E4 on top — still ONE voice
    for (int i = 0; i < 8; ++i) r.block();
    CHECK(r.core.activeVoices() == 1);

    // Measure E4 (~330 Hz) sounding now.
    auto v1 = grab(r, 40);
    const float top = freqHz(v1, 2000, v1.size());
    CHECK(top == doctest::Approx(329.63f).epsilon(0.05));

    // Release E4 -> legato return to the still-held C4 (~262 Hz).
    r.core.noteOff(64);
    for (int i = 0; i < 8; ++i) r.block();
    auto v2 = grab(r, 40);
    const float ret = freqHz(v2, 2000, v2.size());
    CHECK(ret == doctest::Approx(261.63f).epsilon(0.05));

    // Release C4 too -> silence.
    r.core.noteOff(60);
    for (int i = 0; i < 40; ++i) r.block();
    CHECK(r.core.activeVoices() == 0);
}

// =============================================================================
// UNISON
// =============================================================================

TEST_CASE("performance: Unison lights all voices; detune spreads their pitch")
{
    Rig r;
    settle(r);
    r.core.allSoundOff();
    r.setOpt(ID::VOICE_POLY_MODE, (float)(int)PolyMode::Unison);
    r.setNorm(ID::VOICE_UNISON_DETUNE, 0.0f);   // perfect unison first
    r.block();

    r.core.noteOn(69, 100);                    // one key
    for (int i = 0; i < 8; ++i) r.block();
    CHECK(r.core.activeVoices() == VoiceAllocator::kMaxVoices);   // all 8

    // Perfect unison: the summed voices are all 440 Hz -> a clean 440 tone.
    auto vClean = grab(r, 40);
    CHECK(freqHz(vClean, 2000, vClean.size()) == doctest::Approx(440.0f).epsilon(0.03));

    // Now spread them: with detune > 0 the eight slightly-different pitches
    // beat, so the summed RMS is no longer a single steady sinusoid — the
    // simplest robust proof is that the waveform is NO LONGER a clean 440
    // (crossing count drifts) while still audibly present.
    r.setNorm(ID::VOICE_UNISON_DETUNE, 1.0f);
    r.core.noteOn(69, 100);                    // re-strike applies the spread
    for (int i = 0; i < 8; ++i) r.block();
    auto vWide = grab(r, 80);
    CHECK(rms(vWide, 0, vWide.size()) > 0.0f);  // still sounding
    // Detuned stack beats: energy is spread, so a mid-window crossing count
    // differs from the perfect-unison 440 by more than rounding.
    const float wide = freqHz(vWide, 4000, vWide.size() - 4000);
    CHECK(wide != doctest::Approx(440.0f).epsilon(0.005));
}

// =============================================================================
// PITCH BEND
// =============================================================================

TEST_CASE("performance: pitch bend scales with VOICE_BEND_RANGE")
{
    auto bentHz = [](float rangeSemis, uint16_t value14) {
        Rig r;
        settle(r);
        r.core.allSoundOff();
        r.setE(ID::VOICE_BEND_RANGE, rangeSemis);
        r.block();
        r.core.noteOn(69, 100);            // A4 440
        for (int i = 0; i < 8; ++i) r.block();
        r.core.pitchBend(value14);
        for (int i = 0; i < 8; ++i) r.block();
        auto v = grab(r, 40);
        return freqHz(v, 2000, v.size());
    };

    // Centre wheel (8192) at any range = no shift (v2 corrects v1's centre bug).
    CHECK(bentHz(2.0f, 8192) == doctest::Approx(440.0f).epsilon(0.03));

    // Centred form: normalised = (value-8192)/8192.  value 12288 -> +0.5.
    // range 2: +0.5·2 = +1 st -> 440·2^(1/12) = 466.16 Hz.
    CHECK(bentHz(2.0f, 12288) == doctest::Approx(466.16f).epsilon(0.03));

    // Same wheel, range 12: +0.5·12 = +6 st -> 440·2^(6/12) = 622.25 Hz.
    CHECK(bentHz(12.0f, 12288) == doctest::Approx(622.25f).epsilon(0.03));

    // Full down (value 0): normalised -1 -> -range.  range 2 -> -2 st ->
    // 440·2^(-2/12) = 391.995 Hz.
    CHECK(bentHz(2.0f, 0) == doctest::Approx(391.995f).epsilon(0.03));
}

// =============================================================================
// AMP LEVEL (v1 AMP_MOD_FIXED_LEVEL — the VCA-mod DC base)
// =============================================================================

TEST_CASE("performance: VOICE_AMP_LEVEL scales output linearly, independent of master")
{
    auto levelRms = [](float ampLevel) {
        Rig r;
        settle(r);
        r.core.allSoundOff();
        r.setE(ID::MASTER_VOLUME, 0.8f);        // fixed master
        r.setE(ID::VOICE_AMP_LEVEL, ampLevel);
        r.block();
        r.core.noteOn(69, 100);
        for (int i = 0; i < 12; ++i) r.block();
        auto v = grab(r, 40);
        return rms(v, 2000, v.size());
    };
    const float full = levelRms(1.0f);
    const float half = levelRms(0.5f);
    CHECK(full > 0.0f);
    // The VCA base scales the whole mix: half the level ≈ half the RMS.
    CHECK(half == doctest::Approx(full * 0.5f).epsilon(0.02));
}

// =============================================================================
// NO-SILENT-CHANGE GUARD (CLAUDE.md rule 2)
// =============================================================================

TEST_CASE("performance: default patch is byte-identical to before the perf wiring")
{
    // With glide off, Poly, unison detune 0, bend range 2 but NO bend message,
    // amp level 1.0 (all defaults), writing the perf params must not perturb a
    // single output sample vs never writing them.  Mirrors test_bpmclock.
    auto render = [](bool writePerf) {
        ParameterStore store;
        std::vector<float> pool((size_t)SynthCore::kCombPoolFloats, 0.0f);
        SynthCore core(store, pool.data());
        if (writePerf) {
            store.set(ID::GLIDE_ENABLE, 0.0f, Origin::Ui);
            store.set(ID::GLIDE_TIME,  0.0f, Origin::Ui);
            store.set(ID::VOICE_POLY_MODE,
                      Curves::toNorm(kParams[ParameterStore::indexOf(ID::VOICE_POLY_MODE)],
                                     (float)(int)PolyMode::Poly), Origin::Ui);
            store.setEngineering(ID::VOICE_UNISON_DETUNE, 0.0f, Origin::Ui);
            store.setEngineering(ID::VOICE_BEND_RANGE, 2.0f, Origin::Ui);
            store.setEngineering(ID::VOICE_AMP_LEVEL, 1.0f, Origin::Ui);
        }
        core.noteOn(57, 100);
        std::vector<float> out;
        float L[kBlockSize], R[kBlockSize];
        for (int b = 0; b < 100; ++b) {
            core.renderBlock(L, R, kBlockSize);
            for (size_t k = 0; k < kBlockSize; ++k) { out.push_back(L[k]); out.push_back(R[k]); }
        }
        return out;
    };
    const auto a = render(false);
    const auto b = render(true);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) CHECK(a[i] == b[i]);
}
