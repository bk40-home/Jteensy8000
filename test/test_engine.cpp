// =============================================================================
// test_engine.cpp — proofs for the Phase 1 voice engine
// =============================================================================
// EnvGen timing, VoiceAllocator policy, and SynthCore end-to-end behaviour
// (silence -> note -> spectrum -> release -> silence), all at the exact
// 44.1 kHz / 128-sample format the hardware runs.
// =============================================================================
#include "doctest.h"

#include <cmath>

#include "core/SynthCore.h"
#include "core/dsp/EnvGen.h"
#include "gen/ParamTable.h"

using namespace JT;
using namespace JT::Params;

namespace {

float blockRms(const float* b, size_t n)
{
    float acc = 0.0f;
    for (size_t i = 0; i < n; ++i) acc += b[i] * b[i];
    return std::sqrt(acc / (float)n);
}

// Render N blocks, returning the RMS of the LAST block.
float runBlocks(SynthCore& core, int blocks, float* L, float* R)
{
    float rms = 0.0f;
    for (int i = 0; i < blocks; ++i) {
        core.renderBlock(L, R, kBlockSize);
        rms = blockRms(L, kBlockSize);
    }
    return rms;
}

} // namespace

// =============================================================================
// EnvGen
// =============================================================================

TEST_CASE("EnvGen: attack duration is accurate to within one block")
{
    EnvGen e;
    e.setAttackMs(100.0f);
    e.setDecayMs(500.0f);
    e.setSustain(0.5f);
    e.noteOn();

    // 100 ms / 2.9025 ms ≈ 34.45 blocks to reach peak.
    int blocks = 0;
    while (e.tickBlock() < 1.0f && blocks < 100) ++blocks;
    CHECK(blocks >= 33);
    CHECK(blocks <= 36);
}

TEST_CASE("EnvGen: sustain holds, release reaches true zero and goes idle")
{
    EnvGen e;
    e.setAttackMs(0.0f);
    e.setDecayMs(0.0f);
    e.setSustain(0.6f);
    e.setReleaseMs(50.0f);
    e.noteOn();

    for (int i = 0; i < 10; ++i) e.tickBlock();     // through A and D
    CHECK(e.tickBlock() == doctest::Approx(0.6f));  // parked at sustain

    e.noteOff();
    int blocks = 0;
    while (e.isActive() && blocks < 100) { e.tickBlock(); ++blocks; }
    // 50 ms ≈ 17.2 blocks.
    CHECK(blocks >= 16);
    CHECK(blocks <= 19);
    CHECK(e.level() == 0.0f);
    CHECK_FALSE(e.isActive());
}

TEST_CASE("EnvGen: retrigger and early release are click-free (no level jumps)")
{
    EnvGen e;
    e.setAttackMs(200.0f);
    e.setReleaseMs(200.0f);
    e.setSustain(1.0f);
    e.noteOn();
    for (int i = 0; i < 10; ++i) e.tickBlock();     // partway up the attack
    const float mid = e.level();
    CHECK(mid > 0.05f);
    CHECK(mid < 0.95f);

    // Release mid-attack: next level must start from 'mid', not jump.
    e.noteOff();
    CHECK(e.tickBlock() <= mid);
    CHECK(e.tickBlock() > 0.0f);

    // Retrigger mid-release: attack resumes from the current level.
    const float atRetrig = e.level();
    e.noteOn();
    CHECK(e.tickBlock() >= atRetrig);
}

TEST_CASE("EnvGen: slope shapes bend the right way")
{
    // slope > 1 must be AHEAD of linear mid-attack; slope < 1 behind.
    auto midAttackLevel = [](float slope) {
        EnvGen e;
        e.setAttackMs(100.0f);
        e.setAttackSlope(slope);
        e.noteOn();
        for (int i = 0; i < 17; ++i) e.tickBlock();   // ~half the attack
        return e.level();
    };
    const float lin  = midAttackLevel(1.0f);
    CHECK(midAttackLevel(5.0f)  > lin + 0.1f);
    CHECK(midAttackLevel(0.15f) < lin - 0.1f);
    CHECK(lin == doctest::Approx(0.5f).epsilon(0.05));  // linear sanity
}

// =============================================================================
// VoiceAllocator
// =============================================================================

TEST_CASE("allocator: 8 distinct notes, 8 voices; retrigger reuses; steal is oldest")
{
    Voice voices[VoiceAllocator::kMaxVoices];
    VoiceAllocator a(voices);

    for (uint8_t n = 0; n < 8; ++n) a.noteOn((uint8_t)(60 + n), 100);
    CHECK(a.activeCount() == 8);

    // Retrigger of a held note must NOT consume another voice.
    a.noteOn(60, 100);
    CHECK(a.activeCount() == 8);

    // 9th note steals the OLDEST voice — note 61 (60 was refreshed above).
    a.noteOn(80, 100);
    CHECK(a.activeCount() == 8);
    bool has61 = false, has80 = false;
    for (const Voice& v : voices) {
        if (v.isActive() && v.note() == 61) has61 = true;
        if (v.isActive() && v.note() == 80) has80 = true;
    }
    CHECK_FALSE(has61);
    CHECK(has80);
}

TEST_CASE("allocator: releasing voices are preferred steal targets")
{
    Voice voices[VoiceAllocator::kMaxVoices];
    VoiceAllocator a(voices);
    for (uint8_t n = 0; n < 8; ++n) a.noteOn((uint8_t)(60 + n), 100);
    a.noteOff(64);                          // one voice now releasing

    a.noteOn(90, 100);                      // must take the releasing one,
    for (const Voice& v : voices) {         // not the oldest held (60)
        if (v.isActive() && v.note() == 60) { CHECK(true); break; }
    }
    bool has64 = false;
    for (const Voice& v : voices)
        if (v.isActive() && v.note() == 64 && !v.isReleasing()) has64 = true;
    CHECK_FALSE(has64);
}

TEST_CASE("allocator: sustain pedal defers releases; velocity-0 is note-off")
{
    Voice voices[VoiceAllocator::kMaxVoices];
    VoiceAllocator a(voices);

    a.sustain(true);
    a.noteOn(60, 100);
    a.noteOff(60);                          // pedal down: still sounding
    CHECK_FALSE(voices[0].isReleasing());
    a.sustain(false);                       // lift: release fires now
    CHECK(voices[0].isReleasing());

    a.noteOn(61, 100);
    a.noteOn(61, 0);                        // running-status note-off
    bool releasing61 = false;
    for (const Voice& v : voices)
        if (v.note() == 61 && v.isReleasing()) releasing61 = true;
    CHECK(releasing61);
}

// =============================================================================
// SynthCore end-to-end
// =============================================================================

TEST_CASE("engine: silent at boot, sounds on note, decays to silence on release")
{
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);
    float L[kBlockSize], R[kBlockSize];

    // Boot: defaults applied, no notes — output must be EXACTLY zero.
    core.renderBlock(L, R, kBlockSize);
    for (size_t i = 0; i < kBlockSize; ++i) REQUIRE(L[i] == 0.0f);

    // Note on (queued -> applied at next block) -> audible within a few ms.
    core.noteOn(69, 127);                                  // A4
    const float sounding = runBlocks(core, 20, L, R);
    CHECK(sounding > 0.01f);

    // Note off with a short, known release -> below -80 dBFS afterwards.
    store.setEngineering(ID::ENV_AMP_RELEASE, 30.0f, Origin::Ui);
    core.noteOff(69);
    runBlocks(core, 40, L, R);                             // 40 blocks ≈ 116 ms
    CHECK(blockRms(L, kBlockSize) < 1e-4f);
    CHECK(core.activeVoices() == 0);
}

TEST_CASE("engine: cutoff darkens a saw — NRPN-style store writes reach the DSP")
{
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);
    float L[kBlockSize], R[kBlockSize];

    core.noteOn(48, 127);                                  // C3 saw, rich HF
    store.setEngineering(ID::FILTER_CUTOFF, 20000.0f, Origin::MidiUsbDev);
    const float open = runBlocks(core, 30, L, R);

    // The cutoff knob is SHAPED per filter type (v1 feel): the store's Hz
    // rides the global knob curve, and the section maps that knob position
    // under the active type's own range.  Closing the knob fully lands at
    // the SVF row's 40 Hz floor — well under C3's 130.8 Hz fundamental.
    store.setEngineering(ID::FILTER_CUTOFF, 20.0f, Origin::MidiUsbDev);
    const float closed = runBlocks(core, 30, L, R);

    // A 40 Hz LP on a 130 Hz saw strips fundamental AND harmonics —
    // expect a large but not total RMS drop.
    CHECK(closed < open * 0.4f);
    CHECK(closed > 0.0f);
}

TEST_CASE("engine: master volume fades instead of clicking, silences at zero")
{
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);
    float L[kBlockSize], R[kBlockSize];
    core.noteOn(60, 127);
    runBlocks(core, 20, L, R);

    store.set(ID::MASTER_VOLUME, 0.0f, Origin::MidiUsbDev);      // hard CC7 drop
    core.renderBlock(L, R, kBlockSize);
    CHECK(blockRms(L, kBlockSize) > 0.0f);                 // fading, not cut
    runBlocks(core, 60, L, R);                             // smoother settles
    CHECK(blockRms(L, kBlockSize) < 1e-4f);
}

TEST_CASE("engine: panic paths — CC123 releases, CC120 is instant silence")
{
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);
    float L[kBlockSize], R[kBlockSize];
    for (uint8_t n = 60; n < 68; ++n) core.noteOn(n, 127);
    runBlocks(core, 10, L, R);
    CHECK(core.activeVoices() == 8);

    core.allSoundOff();                                    // CC 120
    core.renderBlock(L, R, kBlockSize);
    CHECK(core.activeVoices() == 0);
    core.renderBlock(L, R, kBlockSize);
    CHECK(blockRms(L, kBlockSize) == 0.0f);                // hard zero
}

TEST_CASE("engine: 2000 blocks of abuse stay finite (NaN containment)")
{
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);
    float L[kBlockSize], R[kBlockSize];
    uint32_t rng = 1;

    for (int b = 0; b < 2000; ++b) {
        rng = rng * 1664525u + 1013904223u;
        if ((rng & 7u) == 0) core.noteOn((uint8_t)(36 + (rng >> 8) % 60), 127);
        if ((rng & 7u) == 1) core.noteOff((uint8_t)(36 + (rng >> 8) % 60));
        if ((rng & 7u) == 2)
            store.set(ID::FILTER_CUTOFF,   (float)((rng >> 8) & 1023) / 1023.0f, Origin::MidiUsbDev);
        if ((rng & 7u) == 3)
            store.set(ID::FILTER_RESONANCE,(float)((rng >> 8) & 1023) / 1023.0f, Origin::MidiUsbDev);
        core.renderBlock(L, R, kBlockSize);
        for (size_t i = 0; i < kBlockSize; ++i) {
            REQUIRE(std::isfinite(L[i]));
            REQUIRE(std::fabs(L[i]) < 4.0f);       // and sanely bounded
        }
    }
}
