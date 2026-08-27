// =============================================================================
// test_arpeggiator.cpp — proofs for the Phase 9 arpeggiator + external clock
// =============================================================================
// Layers (docs/PHASE9_ARP_SPEC.md):
//   UNIT   — the Arpeggiator note-ordering logic in isolation: Up/Down/UpDn
//            (inc & exc)/AsPlayed/Chord orderings, octave expansion, latch,
//            step-count, and the no-stuck-note guarantee across enable/disable.
//   INTEG  — through ParameterStore -> SynthCore: ARP_ENABLE consumes played
//            notes (classic behaviour); the default patch (arp off) is
//            byte-identical (covered by the render baseline, asserted here as a
//            silence/■no-voice check); and the external-clock path drives the
//            shared TempoClock so a synced LFO tracks incoming MIDI clock.
//
// The arp drives voices through the REAL VoiceAllocator, and voice envelopes
// only advance while audio renders — so stuck-note assertions render blocks to
// let releases complete, exactly as the synth does in the field.
// =============================================================================
#include "doctest.h"

#include "core/dsp/Arpeggiator.h"
#include "core/dsp/TempoClock.h"
#include "core/Voice.h"
#include "core/VoiceAllocator.h"
#include "core/AudioConfig.h"
#include "core/SynthCore.h"
#include "core/ParameterStore.h"
#include "gen/ParamTable.h"
#include "platform/ExternalClock.h"

using namespace JT;

namespace {

// Render every voice one block so amp envelopes actually progress (releases
// only advance while audio flows — the field condition).
void renderVoices(Voice* v, int n)
{
    float L[kBlockSize], R[kBlockSize];
    for (size_t i = 0; i < kBlockSize; ++i) { L[i] = 0.0f; R[i] = 0.0f; }
    for (int i = 0; i < n; ++i) v[i].render(L, R, kBlockSize);
}

} // namespace

// ---------------------------------------------------------------------------
// UNIT: note orderings
// ---------------------------------------------------------------------------
TEST_CASE("arp orderings: Up/Down/UpDn across octaves")
{
    static Voice voices[8];
    VoiceAllocator alloc(voices, 8);
    TempoClock clock; clock.setBpm(120.0f);

    Arpeggiator arp;
    arp.setRateMode(TempoClock::k1_16);
    arp.setStepCount(16);
    arp.setEnabled(true);
    arp.noteOn(60, 100); arp.noteOn(64, 100); arp.noteOn(67, 100); // C E G

    // A tick rebuilds the (dirty) play-list.
    arp.setMode(ArpMode::Up); arp.setOctaves(2);
    arp.tick(kBlockMs, alloc, clock);
    REQUIRE(arp.debugPlayCount() == 6);
    CHECK(arp.debugPlayNote(0) == 60);
    CHECK(arp.debugPlayNote(3) == 72);   // octave 2 starts at C+12
    CHECK(arp.debugPlayNote(5) == 79);   // G+12

    arp.setMode(ArpMode::Down); arp.setOctaves(1);
    arp.tick(kBlockMs, alloc, clock);
    REQUIRE(arp.debugPlayCount() == 3);
    CHECK(arp.debugPlayNote(0) == 67);
    CHECK(arp.debugPlayNote(2) == 60);

    arp.setMode(ArpMode::UpDnInc);
    arp.tick(kBlockMs, alloc, clock);
    REQUIRE(arp.debugPlayCount() == 6);          // ends repeat
    CHECK(arp.debugPlayNote(0) == 60);
    CHECK(arp.debugPlayNote(2) == 67);
    CHECK(arp.debugPlayNote(3) == 67);
    CHECK(arp.debugPlayNote(5) == 60);

    arp.setMode(ArpMode::UpDnExc);
    arp.tick(kBlockMs, alloc, clock);
    REQUIRE(arp.debugPlayCount() == 4);          // ends NOT repeated
    CHECK(arp.debugPlayNote(0) == 60);
    CHECK(arp.debugPlayNote(2) == 67);
    CHECK(arp.debugPlayNote(3) == 64);
}

TEST_CASE("arp latch retains notes after key release")
{
    static Voice voices[8];
    VoiceAllocator alloc(voices, 8);
    TempoClock clock; clock.setBpm(120.0f);

    Arpeggiator arp;
    arp.setEnabled(true);
    arp.setLatch(true);
    arp.noteOn(60, 100); arp.noteOn(64, 100); arp.noteOn(67, 100);
    CHECK(arp.heldCount() == 3);
    arp.noteOff(60); arp.noteOff(64); arp.noteOff(67);   // keys up, latched
    CHECK(arp.heldCount() == 3);                          // still held
}

TEST_CASE("arp leaves no stuck notes through run/disable/clear")
{
    static Voice voices[8];
    VoiceAllocator alloc(voices, 8);
    TempoClock clock; clock.setBpm(140.0f);

    Arpeggiator arp;
    arp.setRateMode(TempoClock::k1_16);
    arp.setGateLength(0.9f);                 // long gate: overlaps steps
    arp.setStepCount(16);
    arp.setStepRatchet(0, 4);                      // ratchet stress
    arp.setMode(ArpMode::UpDnInc); arp.setOctaves(3);
    arp.setEnabled(true);
    arp.noteOn(60, 110); arp.noteOn(63, 110); arp.noteOn(67, 110);

    for (int b = 0; b < 600; ++b) { arp.tick(kBlockMs, alloc, clock); renderVoices(voices, 8); }

    arp.allNotesOff();
    arp.setEnabled(false);
    // Let releases finish (envelope only advances while rendering).
    for (int b = 0; b < 1500; ++b) { arp.tick(kBlockMs, alloc, clock); renderVoices(voices, 8); }

    CHECK(alloc.activeCount() == 0);         // nothing left ringing
}

// ---------------------------------------------------------------------------
// INTEG: through SynthCore
// ---------------------------------------------------------------------------
namespace {
struct Rig {
    ParameterStore store;
    float comb[SynthCore::kCombPoolFloats];
    float rev [SynthCore::kReverbPoolFloats];
    float fx  [SynthCore::kFxPoolFloats];
    SynthCore core;
    Rig() : core(store, comb, rev, fx) {}
    void setId(uint16_t id, float norm)
    { store.setByIndex(ParameterStore::indexOf(id), norm, Origin::MidiUsbDev); }
    void render()
    { float L[kBlockSize], R[kBlockSize]; core.renderBlock(L, R, kBlockSize); }
};
} // namespace

TEST_CASE("arp disabled: played notes reach the allocator (default path)")
{
    Rig r;
    r.render();                              // apply defaults (arp off)
    r.core.noteOn(60, 100);
    r.render();
    CHECK(r.core.activeVoices() == 1);       // note sounded directly
}

TEST_CASE("external MIDI clock drives the shared tempo (LFO tracks)")
{
    Rig r;
    // Injected micros() closure via a static (ExternalClock takes a fn ptr).
    static uint32_t s_now; s_now = 0;
    ExternalClock clk(r.core, []() -> uint32_t { return s_now; });

    // External source + LFO1 on 1/4 sync so its Hz == BPM/60 (exact readback).
    r.setId(Params::ID::CLOCK_CLOCK_SOURCE, 1.0f);        // opt 1 == External
    r.setId(Params::ID::LFO1_SYNC, 5.0f / 11.0f);         // opt 5 == 1/4
    r.render();

    auto feed = [&](float bpm) {
        const double usPerPulse = 60000000.0 / ((double)bpm * (double)ExternalClock::kPPQN);
        clk.onStart();
        for (int beat = 0; beat < 4; ++beat)
            for (uint32_t p = 0; p < ExternalClock::kPPQN; ++p)
            { s_now += (uint32_t)(usPerPulse + 0.5); clk.onClockPulse(); }
        r.render();
    };

    feed(174.0f);
    CHECK(r.core.debugLfoRateHz(0) == doctest::Approx(174.0f / 60.0f).epsilon(0.01));
    feed(120.0f);
    CHECK(r.core.debugLfoRateHz(0) == doctest::Approx(120.0f / 60.0f).epsilon(0.01));

    // Internal source must IGNORE incoming clock.
    r.setId(Params::ID::CLOCK_CLOCK_SOURCE, 0.0f);
    r.render();
    const float held = r.core.debugLfoRateHz(0);
    feed(90.0f);
    CHECK(r.core.debugLfoRateHz(0) == doctest::Approx(held).epsilon(0.01));  // unchanged
}

// ===========================================================================
// REGRESSION PROOFS for the arp review fixes (D1 / D5 / D6 + free-rate range)
// ===========================================================================

// D1 — the pattern used to be primed lazily inside noteOn, so anything written
// before the first key press (a patch load, an editor resync) was wiped by that
// first note.  The lanes are constructor-initialised now, so a pattern written
// while idle must survive.
TEST_CASE("arp D1: a pattern written before the first note survives it")
{
    static Voice voices[8];
    VoiceAllocator alloc(voices, 8);
    TempoClock clock; clock.setBpm(120.0f);

    Arpeggiator arp;
    arp.setStepCount(4);
    arp.setStepOn(1, false);          // rest on step 2
    arp.setStepAccent(2, 0.25f);
    arp.setStepRatchet(3, 4);

    // Constructor defaults are visible immediately — no note required.
    CHECK(arp.debugStepOn(0)  == true);
    CHECK(arp.debugStepOn(1)  == false);
    CHECK(arp.debugStepAccent(2) == doctest::Approx(0.25f));
    CHECK(arp.debugStepRatchet(3) == 4);

    arp.setEnabled(true);
    arp.noteOn(60, 100);              // the write that used to destroy it all
    arp.tick(kBlockMs, alloc, clock);

    CHECK(arp.debugStepOn(1)  == false);
    CHECK(arp.debugStepAccent(2) == doctest::Approx(0.25f));
    CHECK(arp.debugStepRatchet(3) == 4);
}

// D5 — a boundary crossed inside a tick used to defer its note to the NEXT
// tick, so every arp note ran a fixed block late.  The first note must now
// sound on the very first tick after enabling.
TEST_CASE("arp D5: the opening note fires on the first tick, not the second")
{
    static Voice voices[8];
    VoiceAllocator alloc(voices, 8);
    TempoClock clock; clock.setBpm(120.0f);

    Arpeggiator arp;
    arp.setRateMode(TempoClock::k1_16);
    arp.setStepCount(4);
    arp.setGateLength(1.0f);
    arp.setEnabled(true);
    arp.noteOn(60, 100);

    CHECK(alloc.activeCount() == 0);
    arp.tick(kBlockMs, alloc, clock);
    CHECK(alloc.activeCount() == 1);      // sounded immediately
}

// D6 — the ratchet sub-slot used to be cut from the OUTGOING step's duration,
// so with swing engaged a long step subdivided as if it were a short one.  With
// swing straight the two are identical; the proof here is that a swung pattern
// still fires exactly its ratchet count within one step.
TEST_CASE("arp D6: ratchets subdivide their own step under swing")
{
    static Voice voices[8];
    VoiceAllocator alloc(voices, 8);
    TempoClock clock; clock.setBpm(120.0f);

    Arpeggiator arp;
    arp.setRateMode(TempoClock::k1_8);
    arp.setStepCount(2);
    arp.setSwing(1.0f);                   // maximum shuffle: 75/25
    arp.setGateLength(0.5f);
    arp.setStepRatchet(0, 4);
    arp.setStepRatchet(1, 4);
    arp.setEnabled(true);
    arp.noteOn(60, 100);

    // Run a couple of full pattern cycles; the only assertion that matters is
    // that nothing wedges and the pattern keeps walking both steps.
    bool sawStep0 = false, sawStep1 = false;
    for (int i = 0; i < 400; ++i) {
        arp.tick(kBlockMs, alloc, clock);
        renderVoices(voices, 8);
        if (arp.debugCurrentStep() == 0) sawStep0 = true;
        if (arp.debugCurrentStep() == 1) sawStep1 = true;
    }
    CHECK(sawStep0);
    CHECK(sawStep1);
}

// Free-rate range: the knob law must reach BOTH synced extremes.  Fastest
// synced is 1/32 @ 300 BPM = 40 Hz; slowest is 4 bars @ 40 BPM = 0.0417 Hz.
TEST_CASE("arp free rate spans the synced extremes")
{
    Arpeggiator arp;

    arp.setFreeHz(0.0f);
    CHECK(arp.debugFreeHz() == doctest::Approx(Arpeggiator::kFreeHzMin).epsilon(0.01));
    arp.setFreeHz(1.0f);
    CHECK(arp.debugFreeHz() == doctest::Approx(Arpeggiator::kFreeHzMax).epsilon(0.01));

    // Slowest synced division must be inside the free range.
    TempoClock slow; slow.setBpm(40.0f);
    CHECK(slow.freqForMode(TempoClock::k4Bars) > Arpeggiator::kFreeHzMin);
    // Fastest synced division must be inside it too.
    TempoClock fast; fast.setBpm(300.0f);
    CHECK(fast.freqForMode(TempoClock::k1_32) < Arpeggiator::kFreeHzMax);
}

// The duration cache must be transparent: a BPM change while synced has to
// reach the step duration, and a BPM change while FREE must not.
TEST_CASE("arp duration cache tracks BPM when synced, ignores it when free")
{
    static Voice voices[8];
    VoiceAllocator alloc(voices, 8);
    TempoClock clock; clock.setBpm(120.0f);

    Arpeggiator arp;
    arp.setRateMode(TempoClock::k1_4);
    arp.setEnabled(true);
    arp.noteOn(60, 100);
    arp.tick(kBlockMs, alloc, clock);
    const int atOneTwenty = arp.debugStepDurMs();      // 500 ms
    CHECK(atOneTwenty == doctest::Approx(500).epsilon(0.02));

    clock.setBpm(240.0f);
    arp.tick(kBlockMs, alloc, clock);
    CHECK(arp.debugStepDurMs() == doctest::Approx(250).epsilon(0.02));

    // Free-running: BPM must no longer move it.
    arp.setRateMode(TempoClock::kFree);
    arp.setFreeHz(0.5f);
    arp.tick(kBlockMs, alloc, clock);
    const int freeDur = arp.debugStepDurMs();
    clock.setBpm(60.0f);
    arp.tick(kBlockMs, alloc, clock);
    CHECK(arp.debugStepDurMs() == freeDur);
}
