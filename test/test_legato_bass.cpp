// =============================================================================
// test_legato_bass.cpp — proofs for bass-seq Phase 2 (engine integration)
// =============================================================================
// Covers, through the real SynthCore:
//   * voice.legato (decision 8): Mono/Unison pitch-only note changes, Mono
//     return-to-held-key legato (2a); OFF keeps the v1 restart.
//   * The bass sequencer as note source #3: keys consumed, root transpose,
//     key release stops it, bass wins over the arp (4a).
//   * SLIDE steps: one continuous note (no envelope restart), any poly mode.
//   * ACCENT: bass.accent_amp boosts accented steps.
//
// RESTART PROBE.  A note that RESTARTS on an audible voice goes through the
// steal fade (Voice.h): one block ramps the gain to 0, the next restarts it —
// so the per-block RMS DIPS to ~0.58 of the steady level (a linear ramp's RMS
// is 1/sqrt(3)).  A legato change keeps the gain flat: no dip.  Sine osc,
// attack 0, sustain 1, so the steady RMS does not depend on the pitch.
// The probe notes are ~2 kHz (C7 and up) on purpose: a 128-sample block then
// holds 5+ cycles, so block RMS is steady to a few %.  At ~260 Hz a block is
// under one cycle and its RMS swings with phase — that first attempt measured
// the phase, not the envelope.
// =============================================================================
#include "doctest.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/SynthCore.h"
#include "core/VoiceAllocator.h"
#include "core/dsp/Curves.h"
#include "gen/ParamTable.h"

using namespace JT;
using namespace JT::Params;

namespace {

// Fresh comb pool per Rig (test_performance lesson: no shared residue).
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
        setE(ID::ENV_AMP_RELEASE, 5.0f);
        for (int i = 0; i < 20; ++i) block();   // drain defaults
    }
    void setE(uint16_t id, float eng)  { store.setEngineering(id, eng, Origin::Ui); }
    void setOpt(uint16_t id, float o)
    { store.set(id, Curves::toNorm(kParams[ParameterStore::indexOf(id)], o), Origin::Ui); }
    void setNorm(uint16_t id, float n) { store.set(id, n, Origin::Ui); }
    void block() { float L[kBlockSize], R[kBlockSize]; core.renderBlock(L, R, kBlockSize); }

    // Per-block RMS of the left channel for `blocks` blocks.
    std::vector<float> blockRms(int blocks)
    {
        std::vector<float> out;
        float L[kBlockSize], R[kBlockSize];
        for (int b = 0; b < blocks; ++b) {
            core.renderBlock(L, R, kBlockSize);
            double acc = 0.0;
            for (size_t k = 0; k < kBlockSize; ++k) acc += (double)L[k] * (double)L[k];
            out.push_back((float)std::sqrt(acc / (double)kBlockSize));
        }
        return out;
    }
    // Flat sample capture, for pitch by zero crossings.
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
};

float freqHz(const std::vector<float>& v, size_t begin, size_t end)
{
    size_t c = 0;
    for (size_t i = begin + 1; i < end; ++i)
        if (v[i - 1] <= 0.0f && v[i] > 0.0f) ++c;
    return (float)((double)c * (double)kSampleRate / (double)(end - begin));
}

// Smallest block RMS around a note change, relative to the steady level just
// before it.  ~0.58 = restart (steal fade); ~1.0 = legato.
float dipRatio(Rig& r, uint8_t secondNote)
{
    const auto before = r.blockRms(8);
    const float steady = before.back();
    r.core.noteOn(secondNote, 100);
    const auto after = r.blockRms(6);
    return *std::min_element(after.begin(), after.end()) / steady;
}

void bassBasics(Rig& r)
{
    r.setOpt(ID::BASS_ENABLE, 1.0f);           // on_off select: On
    r.setE(ID::BASS_DENSITY, 1.0f);            // every step plays
    r.setE(ID::BASS_OCTAVE, 0.0f);
    r.setE(ID::BASS_SEED, 77.0f);
}

} // namespace

// =============================================================================
// LEGATO SWITCH
// =============================================================================

TEST_CASE("legato OFF (default): Mono second key restarts the note (v1 unchanged)")
{
    Rig r;
    r.setOpt(ID::VOICE_POLY_MODE, (float)(int)PolyMode::Mono);
    r.block();
    r.core.noteOn(96, 100);                    // C7
    CHECK(dipRatio(r, 100) < 0.75f);           // the steal-fade dip is there
}

TEST_CASE("legato ON: Mono second key moves pitch only — no restart, new pitch")
{
    Rig r;
    r.setOpt(ID::VOICE_POLY_MODE, (float)(int)PolyMode::Mono);
    r.setNorm(ID::VOICE_LEGATO, 1.0f);
    r.block();
    r.core.noteOn(96, 100);                    // C7  2093 Hz
    CHECK(dipRatio(r, 100) > 0.9f);            // E7: no dip, envelope kept running
    const auto v = r.grab(40);
    CHECK(freqHz(v, 1000, v.size()) == doctest::Approx(2637.0f).epsilon(0.03));
    CHECK(r.core.activeVoices() == 1);

    // Return to the still-held C4 on release: legato too (decision 2a).
    const float steady = r.blockRms(4).back();
    r.core.noteOff(100);
    const auto after = r.blockRms(6);
    CHECK(*std::min_element(after.begin(), after.end()) / steady > 0.9f);
    const auto w = r.grab(40);
    CHECK(freqHz(w, 1000, w.size()) == doctest::Approx(2093.0f).epsilon(0.03));
}

TEST_CASE("legato ON: a key after full release still restarts (not legato)")
{
    Rig r;
    r.setOpt(ID::VOICE_POLY_MODE, (float)(int)PolyMode::Mono);
    r.setE(ID::ENV_AMP_RELEASE, 2000.0f);      // long tail: still audible on re-strike
    r.setNorm(ID::VOICE_LEGATO, 1.0f);
    r.block();
    r.core.noteOn(96, 100);
    (void)r.blockRms(8);
    r.core.noteOff(96);                        // released (tail sounding)
    (void)r.blockRms(2);
    const float tail = r.blockRms(1).back();
    r.core.noteOn(100, 100);
    const auto after = r.blockRms(4);
    // A releasing voice cannot legato: the strike restarts through the fade.
    CHECK(*std::min_element(after.begin(), after.end()) / tail < 0.75f);
}

TEST_CASE("legato ON: Unison moves every voice without a restart")
{
    Rig r;
    r.setOpt(ID::VOICE_POLY_MODE, (float)(int)PolyMode::Unison);
    r.setNorm(ID::VOICE_LEGATO, 1.0f);
    r.block();
    r.core.noteOn(96, 100);
    CHECK(dipRatio(r, 103) > 0.9f);
    CHECK(r.core.activeVoices() == (int)VoiceAllocator::kMaxVoices);
}

TEST_CASE("legato ON: Poly is unaffected (separate keys are separate voices)")
{
    Rig r;
    r.setNorm(ID::VOICE_LEGATO, 1.0f);
    r.block();
    r.core.noteOn(60, 100);
    (void)r.blockRms(8);
    r.core.noteOn(64, 100);
    (void)r.blockRms(4);
    CHECK(r.core.activeVoices() == 2);
}

// =============================================================================
// BASS SEQUENCER IN THE ENGINE
// =============================================================================

TEST_CASE("bass: keys are consumed; the held key is the root; release stops it")
{
    Rig r;
    bassBasics(r);
    r.setE(ID::BASS_RANGE, 1.0f);              // degree always 0 -> every note = root
    r.setE(ID::BASS_GATE_LENGTH, 0.9f);
    r.block();

    r.core.noteOn(57, 100);                    // A3 = 220 Hz
    const auto v = r.grab(30);                 // inside the first 1/16 (~88 ms @170)
    CHECK(freqHz(v, 1000, v.size()) == doctest::Approx(220.0f).epsilon(0.05));
    CHECK(r.core.activeVoices() >= 1);

    r.core.noteOff(57);
    for (int i = 0; i < 200; ++i) r.block();   // release (5 ms) + margin
    CHECK(r.core.activeVoices() == 0);
}

TEST_CASE("bass: slide steps are one continuous note (no restart), pitch moves")
{
    Rig r;
    bassBasics(r);
    r.setE(ID::BASS_SLIDE, 1.0f);              // every step slides into the next
    // Range 5 degrees of minor = root .. +7 st.  The line must stay in a band
    // where the default filter is flat: a 15-degree range from C7 reached
    // ~8 kHz, where the roll-off made block RMS follow PITCH (first attempt).
    r.setE(ID::BASS_RANGE, 5.0f);
    r.setE(ID::BASS_SLIDE_TIME, 10.0f);        // quick, so pitches settle per step
    r.block();

    r.core.noteOn(90, 100);                    // F#6 root, 1480..2217 Hz: 4+ cycles/block
    (void)r.blockRms(4);                       // past the first strike's attack
    const auto rmsRun = r.blockRms(33 * 8);    // ~8 steps of 1/16 @ 170 BPM
    const float med = rmsRun[rmsRun.size() / 2];
    const float lo  = *std::min_element(rmsRun.begin(), rmsRun.end());
    CHECK(lo / med > 0.85f);                   // never a restart dip (~0.58)
    CHECK(r.core.activeVoices() == 1);         // Poly mode, still ONE moving voice

    // The line actually moves pitch: collect per-step frequencies.
    int distinct = 0; float last = -1.0f;
    for (int s = 0; s < 8; ++s) {
        const auto w = r.grab(33);
        const float f = freqHz(w, 2000, w.size());
        if (std::fabs(f - last) > 3.0f) ++distinct;
        last = f;
    }
    CHECK(distinct >= 2);
}

TEST_CASE("bass: accent_amp makes accented steps louder (x2 at max)")
{
    auto peak = [](float accAmp) {
        Rig r;
        bassBasics(r);
        r.setE(ID::BASS_SLIDE, 0.0f);
        r.setE(ID::BASS_ACCENT, 1.0f);         // every step accented
        r.setE(ID::BASS_ACCENT_AMP, accAmp);
        r.setE(ID::BASS_RANGE, 1.0f);
        r.block();
        r.core.noteOn(57, 100);
        const auto v = r.blockRms(20);
        return *std::max_element(v.begin(), v.end());
    };
    const float plain = peak(0.0f);
    const float acc   = peak(1.0f);
    REQUIRE(plain > 0.0f);
    CHECK(acc / plain == doctest::Approx(2.0f).epsilon(0.05));
}

TEST_CASE("bass wins over the arp; turning bass off hands the keys back")
{
    Rig r;
    r.setNorm(ID::ARP_ENABLE, 1.0f);
    bassBasics(r);
    r.block();

    r.core.noteOn(48, 100);
    for (int i = 0; i < 40; ++i) r.block();
    CHECK((r.core.arpStatusWord() & 1u) == 0u);   // arp has no keys: not running
    CHECK(r.core.activeVoices() >= 1);            // the bass line is playing

    r.core.noteOff(48);
    r.setOpt(ID::BASS_ENABLE, 0.0f);              // bass off -> arp owns keys again
    for (int i = 0; i < 20; ++i) r.block();
    r.core.noteOn(50, 100);
    for (int i = 0; i < 40; ++i) r.block();
    CHECK((r.core.arpStatusWord() & 1u) == 1u);   // arp running on the new key
}

TEST_CASE("bass off (default): keys play the voices directly, as before")
{
    Rig r;
    r.core.noteOn(60, 100);
    (void)r.blockRms(4);
    CHECK(r.core.activeVoices() == 1);
    const auto v = r.grab(40);
    CHECK(freqHz(v, 1000, v.size()) == doctest::Approx(261.63f).epsilon(0.05));
}
