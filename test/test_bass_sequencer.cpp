// =============================================================================
// test_bass_sequencer.cpp — proofs for the Phase 1 BassSequencer core class
// =============================================================================
// Covers the bass-seq/phase0-proposal.md host test list (items 1-6; item 7,
// the default-patch render gate, belongs to Phase 2 integration) plus the
// Phase 1 refinements: (seed, mutation) reproducibility and playback-time
// slide guarding for non-forward directions.
//
// Pure logic tests: no SynthCore, no voices, no audio.  Time is driven in 1 ms
// ticks so event times can be asserted to the millisecond.
// =============================================================================
#include "doctest.h"

#include <vector>

#include "core/dsp/BassSequencer.h"
#include "core/dsp/TempoClock.h"

using namespace JT;

namespace {

using BS = BassSequencer;

// One recorded event with the tick (ms) it was emitted on.
struct Ev { int t; BassEvent e; };

// A sequencer playing a held C2 (48) at a free 8 Hz (125 ms steps).
struct Rig {
    BS seq;
    Rig(uint16_t seed = 0x0123)
    {
        seq.setSeed(seed);
        seq.setFreeHz(8.0f);
        seq.setEnabled(true);
    }
    // Tick ms times at 1 ms, collecting every event.
    std::vector<Ev> run(int ms)
    {
        std::vector<Ev> out;
        for (int t = 0; t < ms; ++t) {
            seq.tick(1.0f);
            for (int i = 0; i < seq.eventCount(); ++i) out.push_back({ t, seq.event(i) });
        }
        return out;
    }
};

std::vector<uint16_t> table(BS& s)
{
    std::vector<uint16_t> v;
    for (int i = 0; i < BS::kMaxSteps; ++i) v.push_back(s.stepPacked(i));
    return v;
}

bool gated(uint16_t p) { return (p & BS::kGateBit) != 0; }

} // namespace

// ---- 1. Determinism --------------------------------------------------------
TEST_CASE("BassSequencer: same seed + amounts -> identical step table")
{
    BS a, b;
    a.setSeed(0x2A5F); b.setSeed(0x2A5F);
    a.setDensity(0.6f); b.setDensity(0.6f);
    CHECK(table(a) == table(b));

    BS c; c.setSeed(0x2A60); c.setDensity(0.6f);
    CHECK(table(a) != table(c));   // neighbouring seed is a different pattern
}

// ---- 2. Density monotonic (and the end points are exact) --------------------
TEST_CASE("BassSequencer: raising density only adds gates")
{
    for (uint16_t seed : { 0, 1, 77, 0x1234, 0x3FFF }) {
        BS s; s.setSeed(seed);
        std::vector<uint16_t> prev;
        for (int k = 0; k <= 64; ++k) {
            s.setDensity((float)k / 64.0f);
            const auto cur = table(s);
            if (k == 0)  for (uint16_t p : cur) CHECK_FALSE(gated(p));
            if (k == 64) for (uint16_t p : cur) CHECK(gated(p));
            if (!prev.empty())
                for (int i = 0; i < BS::kMaxSteps; ++i)
                    if (gated(prev[i])) CHECK(gated(cur[i]));
            prev = cur;
        }
    }
}

// ---- 3. Flag rules ----------------------------------------------------------
TEST_CASE("BassSequencer: no accent/slide on a rest; no slide into a rest")
{
    for (uint16_t seed = 0; seed < 200; ++seed) {
        BS s; s.setSeed(seed);
        s.setDensity(0.5f); s.setAccentAmount(0.7f); s.setSlideAmount(0.9f);
        s.setLength(16);
        const auto t = table(s);
        for (int i = 0; i < 16; ++i) {
            if (!gated(t[i])) {
                CHECK((t[i] & (BS::kAccentBit | BS::kSlideBit)) == 0);
            }
            if (t[i] & BS::kSlideBit) CHECK(gated(t[(i + 1) % 16]));
        }
    }
}

// ---- 4. Hand edits win ------------------------------------------------------
TEST_CASE("BassSequencer: edited steps survive amounts, mutate and new seed")
{
    BS s; s.setSeed(42);
    const uint16_t edit = BS::kUserBit | BS::kGateBit | BS::kAccentBit | 5u;
    s.setStepPacked(3, edit);
    CHECK(s.stepPacked(3) == (edit & BS::kPackedMask));

    s.setDensity(0.0f);         CHECK(s.stepPacked(3) == (edit & BS::kPackedMask));
    (void)s.mutate();           CHECK(s.stepPacked(3) == (edit & BS::kPackedMask));
    (void)s.newRandomSeed();    CHECK(s.stepPacked(3) == (edit & BS::kPackedMask));
    CHECK(s.editedMask() == (1u << 3));

    // Handing the step back (USER bit clear) restores generation.
    s.setStepPacked(3, 0u);
    CHECK(s.editedMask() == 0u);
    CHECK_FALSE(gated(s.stepPacked(3)));   // density is 0 -> generated rest

    // An edited rest cannot carry accent or slide.
    s.setStepPacked(4, BS::kUserBit | BS::kAccentBit | BS::kSlideBit);
    CHECK((s.stepPacked(4) & (BS::kAccentBit | BS::kSlideBit)) == 0);
}

// ---- 5. Mutate / undo / reproducibility --------------------------------------
TEST_CASE("BassSequencer: mutate rerolls ~25% of steps; undo is exact; replay is exact")
{
    BS s; s.setSeed(0x0BAD);
    (void)s.stepPacked(0);                     // force a resolve

    long changed = 0, total = 0;
    for (int m = 0; m < 100; ++m) {
        uint16_t before[BS::kMaxSteps];
        for (int i = 0; i < BS::kMaxSteps; ++i) before[i] = s.debugRollPitch(i);
        (void)s.mutate();
        (void)s.stepPacked(0);
        for (int i = 0; i < BS::kMaxSteps; ++i) { changed += (s.debugRollPitch(i) != before[i]); ++total; }
    }
    const double frac = (double)changed / (double)total;
    CHECK(frac > 0.20);
    CHECK(frac < 0.30);

    // Undo returns the exact previous table.
    const auto t100 = table(s);
    (void)s.mutate();
    CHECK(table(s) != t100);
    CHECK(s.undo());
    CHECK(table(s) == t100);
    CHECK_FALSE(s.undo());                     // one level only

    // (seed, mutation) fully defines the pattern -> a patch reload reproduces it.
    BS r; r.setSeed(0x0BAD); r.setMutation(s.mutation());
    CHECK(table(r) == t100);

    // New seed resets mutation; undo restores the old pair.
    const uint16_t ns = s.newRandomSeed();
    CHECK(ns != 0x0BAD);
    CHECK(s.mutation() == 0);
    CHECK(s.undo());
    CHECK(s.seed() == 0x0BAD);
    CHECK(table(s) == t100);
}

TEST_CASE("BassSequencer: changing length never reshuffles the remaining steps")
{
    BS s; s.setSeed(99); s.setSlideAmount(0.0f);   // slide wraps at length; exclude it
    s.setLength(32);
    const auto a = table(s);
    s.setLength(8);
    const auto b = table(s);
    for (int i = 0; i < 8; ++i) CHECK(a[i] == b[i]);
}

// ---- 6. Gate timing and slide events ------------------------------------------
TEST_CASE("BassSequencer: 303 gate — NoteOn on the step, NoteOff at gate_len")
{
    Rig r;
    r.seq.setDensity(1.0f); r.seq.setSlideAmount(0.0f); r.seq.setAccentAmount(0.0f);
    r.seq.setGateLength(0.5f);
    r.seq.noteOn(48, 100);
    const auto ev = r.run(260);

    // Step period 125 ms, gate 62.5 ms.  Tick k has accumulated k ms (the first
    // tick fires the step at phase 0), so the off lands on the first tick at or
    // past 62.5 ms: t = 63.  (Block-boundary timing, as the arp.)
    REQUIRE(ev.size() >= 5);
    CHECK(ev[0].t == 0);   CHECK(ev[0].e.type == BassEvent::NoteOn);
    CHECK(ev[1].t == 63);  CHECK(ev[1].e.type == BassEvent::NoteOff);
    CHECK(ev[1].e.note == ev[0].e.note);
    CHECK(ev[2].t == 125); CHECK(ev[2].e.type == BassEvent::NoteOn);
    CHECK(ev[3].t == 188); CHECK(ev[3].e.type == BassEvent::NoteOff);
    CHECK(ev[4].t == 250); CHECK(ev[4].e.type == BassEvent::NoteOn);
    CHECK(ev[0].e.velocity == 100);
}

TEST_CASE("BassSequencer: slide steps overlap — new NoteOn before old NoteOff, no gap")
{
    Rig r(0x0777);
    r.seq.setDensity(1.0f); r.seq.setSlideAmount(1.0f); r.seq.setRange(15);
    r.seq.noteOn(48, 90);
    const auto ev = r.run(16 * 125);

    int sounding = 0;
    for (size_t i = 0; i < ev.size(); ++i) {
        const BassEvent& e = ev[i].e;
        if (e.type == BassEvent::NoteOn) {
            ++sounding;
            if (e.flags & BassEvent::kSlide) {
                // The matching release of the previous note follows immediately.
                REQUIRE(i + 1 < ev.size());
                CHECK(ev[i + 1].e.type == BassEvent::NoteOff);
                CHECK(ev[i + 1].t == ev[i].t);
            }
        } else {
            --sounding;
            // With every step sliding, a NoteOff only ever follows a slide NoteOn.
            REQUIRE(i > 0);
            CHECK(ev[i - 1].e.type == BassEvent::NoteOn);
            CHECK((ev[i - 1].e.flags & BassEvent::kSlide) != 0);
        }
        CHECK(sounding >= 0);
        // Mono line: at the END of each tick at most one note is down.  Within a
        // tick a slide briefly holds two (NoteOn new, then NoteOff old) — that
        // overlap is the legato, so the check is taken at tick boundaries.
        const bool lastInTick = (i + 1 == ev.size()) || (ev[i + 1].t != ev[i].t);
        if (lastInTick) CHECK(sounding <= 1);
    }
}

TEST_CASE("BassSequencer: reverse playback never slides into a rest")
{
    Rig r;
    r.seq.setLength(4);
    r.seq.setDirection(BassDir::Reverse);
    r.seq.setGateLength(0.5f);
    // Forward-legal pattern: step 0 slides into step 1.  Reversed, step 0 is
    // followed by step 3, a rest — the slide must NOT hold through.
    r.seq.setStepPacked(0, BS::kUserBit | BS::kGateBit | BS::kSlideBit | 0u);
    r.seq.setStepPacked(1, BS::kUserBit | BS::kGateBit | 2u);
    r.seq.setStepPacked(2, BS::kUserBit | BS::kGateBit | 4u);
    r.seq.setStepPacked(3, BS::kUserBit | 0u);
    r.seq.noteOn(48, 100);
    const auto ev = r.run(4 * 125 + 100);

    // Order 3(rest) 2 1 0 3 ... : step 0 plays at t=375 and must be released
    // at its gate, 375 + 63 = 438, well before step 3's boundary at 500.
    bool sawOn0 = false, sawOff0 = false;
    for (const Ev& x : ev) {
        if (x.t == 375 && x.e.type == BassEvent::NoteOn) sawOn0 = true;
        if (x.t == 438 && x.e.type == BassEvent::NoteOff) sawOff0 = true;
    }
    CHECK(sawOn0);
    CHECK(sawOff0);
}

// ---- Play / stop behaviour ----------------------------------------------------
TEST_CASE("BassSequencer: disabled is silent; disable mid-note releases it")
{
    Rig r; r.seq.setDensity(1.0f); r.seq.setGateLength(0.9f);
    r.seq.setEnabled(false);
    r.seq.noteOn(48, 100);
    CHECK(r.run(300).empty());

    r.seq.setEnabled(true);
    auto ev = r.run(10);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].e.type == BassEvent::NoteOn);
    r.seq.setEnabled(false);
    ev = r.run(2);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].e.type == BassEvent::NoteOff);
}

TEST_CASE("BassSequencer: key release stops the line unless latched")
{
    Rig r; r.seq.setDensity(1.0f); r.seq.setGateLength(0.9f);
    r.seq.noteOn(48, 100);
    (void)r.run(10);
    r.seq.noteOff(48);
    auto ev = r.run(300);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].e.type == BassEvent::NoteOff);
    CHECK_FALSE(r.seq.running());

    r.seq.setLatch(true);
    r.seq.noteOn(50, 100);
    (void)r.run(10);
    r.seq.noteOff(50);
    ev = r.run(300);
    int ons = 0;
    for (const Ev& x : ev) ons += (x.e.type == BassEvent::NoteOn);
    CHECK(ons >= 2);           // still stepping on the latched root
    CHECK(r.seq.running());
}

// ---- Pitch: root transpose and scale lock --------------------------------------
TEST_CASE("BassSequencer: held key transposes; notes stay in the scale")
{
    auto notesFor = [](uint8_t root, BassScale sc) {
        Rig r(0x1ACD);
        r.seq.setDensity(1.0f); r.seq.setSlideAmount(0.0f); r.seq.setOctaveAmount(0.0f);
        r.seq.setRange(15); r.seq.setScale(sc);
        r.seq.noteOn(root, 100);
        std::vector<int> n;
        for (const Ev& x : r.run(16 * 125))
            if (x.e.type == BassEvent::NoteOn) n.push_back(x.e.note);
        return n;
    };
    const auto a = notesFor(48, BassScale::Minor);
    const auto b = notesFor(60, BassScale::Minor);
    REQUIRE(a.size() == b.size());
    REQUIRE(!a.empty());
    for (size_t i = 0; i < a.size(); ++i) CHECK(b[i] - a[i] == 12);

    const int minor[7] = { 0, 2, 3, 5, 7, 8, 10 };
    for (int n : a) {
        const int pc = (n - 48) % 12;
        bool in = false;
        for (int m : minor) in = in || (pc == m);
        CHECK(in);
    }
}

// ---- Tempo sync ---------------------------------------------------------------
TEST_CASE("BassSequencer: synced 1/16 follows BPM; Free falls back to free rate")
{
    TempoClock clk; clk.setBpm(120.0f);
    BS s;
    s.setTimingMode(TempoClock::k1_16, clk);
    CHECK(s.debugStepDurMs() == doctest::Approx(125.0f));
    clk.setBpm(140.0f); s.updateFromClock(clk);
    CHECK(s.debugStepDurMs() == doctest::Approx(1000.0f / (140.0f / 60.0f / 0.25f)));
    s.setFreeHz(4.0f);
    s.setTimingMode(TempoClock::kFree, clk);
    CHECK(s.debugStepDurMs() == doctest::Approx(250.0f));
}
