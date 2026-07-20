// =============================================================================
// test_filter_section.cpp — proofs for core/FilterSection (Pass 5, VA engine)
// =============================================================================
// Priorities, in order: (1) the v1 STABILITY lessons hold at the extremes —
// Korg35's NL-bounded loop and MoogDV's cutoff ceiling; (2) every type does
// its filtering job directionally (LP darkens, HP brightens, notch cuts,
// AP preserves); (3) type thrash under signal stays finite; (4) the store
// wiring reaches the section with norm semantics.
// =============================================================================
#include "doctest.h"

#include <cmath>
#include <initializer_list>
#include <vector>

#include "core/FilterSection.h"
#include "core/SynthCore.h"
#include "core/dsp/Curves.h"
#include "gen/ParamTable.h"

using namespace JT;
using namespace JT::Params;

namespace {

// A deterministic harmonically-rich test source: naive saw at 220 Hz.
void fillSaw(float* buf, size_t n, float& phase)
{
    const float inc = 220.0f / kSampleRate;
    for (size_t i = 0; i < n; ++i) {
        buf[i] = 2.0f * phase - 1.0f;
        phase += inc;
        if (phase >= 1.0f) phase -= 1.0f;
    }
}

// Run 'blocks' blocks of saw through the section, RMS over the last half
// (skips the filter's settling transient).
float sawRmsThrough(FilterSection& f, int blocks)
{
    float buf[kBlockSize];
    float phase = 0.0f;
    double acc = 0.0;
    size_t counted = 0;
    for (int b = 0; b < blocks; ++b) {
        fillSaw(buf, kBlockSize, phase);
        f.process(buf, kBlockSize);
        if (b >= blocks / 2) {
            for (size_t i = 0; i < kBlockSize; ++i)
                acc += (double)buf[i] * (double)buf[i];
            counted += kBlockSize;
        }
    }
    return (float)std::sqrt(acc / (double)counted);
}

// Set the cutoff knob with both views, like SynthCore does: the norm plus
// its Hz on the global 20..20000 log curve (VA uses norm, OBXa uses Hz).
void setKnob(FilterSection& f, float norm)
{
    f.setCutoff(norm, 20.0f * std::pow(1000.0f, norm));
}

} // namespace

TEST_CASE("every LP type darkens with the knob; every HP type thins the lows")
{
    // LP types at knob 1.0 vs 0.15: closing must cost significant RMS.
    for (int type : { 0, 5, 6, 8, 9, 11, 13, 14 }) {   // all LP variants
        CAPTURE(type);
        FilterSection f;
    f.setEngine(1);                          // VA engine under test
        f.setVaType(type);
        setKnob(f, 1.0f);
        const float open = sawRmsThrough(f, 40);
        f.reset();
        setKnob(f, 0.15f);
        const float dark = sawRmsThrough(f, 40);
        CHECK(dark < open * 0.7f);
        CHECK(open > 0.05f);
    }

    // HP types: raising the knob must cost the (dominant) low harmonics.
    for (int type : { 1, 10, 12, 15 }) {
        CAPTURE(type);
        FilterSection f;
    f.setEngine(1);                          // VA engine under test
        f.setVaType(type);
        setKnob(f, 0.0f);
        const float full = sawRmsThrough(f, 40);
        f.reset();
        setKnob(f, 1.0f);
        const float thin = sawRmsThrough(f, 40);
        CHECK(thin < full * 0.7f);
    }
}

TEST_CASE("notch cuts its centre; allpass preserves level")
{
    // Notch tuned onto 220 Hz: knob position for 220 in the notch row
    // (100..8000): n = ln(220/100)/ln(80) ≈ 0.18.
    FilterSection notch;
    notch.setEngine(1);                          // VA engine under test
    notch.setVaType(3);
    notch.setResonanceNorm(0.8f);
    setKnob(notch, 0.18f);
    float buf[kBlockSize];
    float phase = 0.0f;
    // Pure-ish tone: use the saw's fundamental region by measuring against
    // a wide-open notch elsewhere.
    const float onTone  = sawRmsThrough(notch, 40);
    notch.reset();
    setKnob(notch, 0.9f);                       // notch parked at HF
    const float offTone = sawRmsThrough(notch, 40);
    CHECK(onTone < offTone);                         // fundamental attenuated

    FilterSection ap;
    ap.setEngine(1);                          // VA engine under test
    ap.setVaType(4);
    ap.setResonanceNorm(0.5f);
    setKnob(ap, 0.5f);
    (void)buf; (void)phase;
    const float apRms = sawRmsThrough(ap, 40);
    // Allpass: |H| = 1 at every frequency — saw RMS 0.577 survives intact.
    CHECK(apRms == doctest::Approx(0.577f).epsilon(0.05));
}

TEST_CASE("V1 LESSON: Korg35 at maximum res and cutoff stays bounded (NL_SAT)")
{
    // This exact corner diverged in v1 with linear feedback — the reason
    // the bank forces VA_NL_SAT on Korg35.  Both variants, worst corner,
    // sustained drive: everything finite and within the output clamp.
    for (int type : { 9, 10 }) {
        CAPTURE(type);
        FilterSection f;
    f.setEngine(1);                          // VA engine under test
        f.setVaType(type);
        setKnob(f, 1.0f);
        f.setResonanceNorm(1.0f);
        float buf[kBlockSize];
        float phase = 0.0f;
        for (int b = 0; b < 400; ++b) {
            fillSaw(buf, kBlockSize, phase);
            f.process(buf, kBlockSize);
            for (float s : buf) {
                REQUIRE(std::isfinite(s));
                REQUIRE(std::fabs(s) <= 1.0f);
            }
        }
    }
}

TEST_CASE("V1 LESSON: MoogDV honours its cutoff ceiling and still filters")
{
    // Knob fully open on a MoogDV type: the section must clamp to the
    // model's ~5.8 kHz ceiling internally — output remains a working LP
    // (finite, and STILL darker than a wide-open SVF at the same knob).
    FilterSection dv;
    dv.setEngine(1);                          // VA engine under test
    dv.setVaType(13);
    setKnob(dv, 1.0f);
    const float dvOpen = sawRmsThrough(dv, 40);
    CHECK(dvOpen > 0.05f);

    FilterSection svf;
    svf.setEngine(1);                          // VA engine under test
    svf.setVaType(0);
    setKnob(svf, 1.0f);
    const float svfOpen = sawRmsThrough(svf, 40);
    CHECK(dvOpen < svfOpen * 1.05f);                 // never brighter than 14k SVF

    // And max resonance at the ceiling stays finite (physical self-osc
    // saturates, never diverges — the model's defining property).
    dv.setResonanceNorm(1.0f);
    float buf[kBlockSize];
    float phase = 0.0f;
    for (int b = 0; b < 400; ++b) {
        fillSaw(buf, kBlockSize, phase);
        dv.process(buf, kBlockSize);
        for (float s : buf) REQUIRE(std::isfinite(s));
    }
}

TEST_CASE("type thrash under sustained signal: every switch lands clean")
{
    // Random-walk the type selector every few blocks with resonance high —
    // the v1 reset-on-switch must leave no NaN or stale-state blowups.
    FilterSection f;
    f.setEngine(1);                          // VA engine under test
    f.setResonanceNorm(0.95f);
    setKnob(f, 0.8f);
    uint32_t rng = 17;
    float buf[kBlockSize];
    float phase = 0.0f;
    for (int b = 0; b < 600; ++b) {
        rng = rng * 1664525u + 1013904223u;
        if ((b & 3) == 0) f.setVaType((int)((rng >> 8) % 17u));
        if ((rng & 15u) == 0)
            setKnob(f, (float)((rng >> 8) & 255u) / 255.0f);
        fillSaw(buf, kBlockSize, phase);
        f.process(buf, kBlockSize);
        for (float s : buf) {
            REQUIRE(std::isfinite(s));
            REQUIRE(std::fabs(s) <= 1.0f);
        }
    }
}

TEST_CASE("engine wiring: type/engine/knobs reach the section via the store")
{
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);
    float L[kBlockSize], R[kBlockSize];

    auto windowRms = [&](int blocks) {
        double acc = 0.0;
        for (int i = 0; i < blocks; ++i) {
            core.renderBlock(L, R, kBlockSize);
            for (size_t k = 0; k < kBlockSize; ++k)
                acc += (double)L[k] * (double)L[k];
        }
        return (float)std::sqrt(acc / (double)(blocks * (int)kBlockSize));
    };

    store.setEngineering(ID::ENV_AMP_SUSTAIN, 1.0f, Origin::Ui);
    store.setEngineering(ID::MIX_OSC2, 0.0f, Origin::Ui);
    // Engine select accepted (falls back to VA this step — flagged).
    store.set(ID::FILTER_ENGINE, 1.0f, Origin::MidiUsbDev);           // "VA"
    core.noteOn(45, 127);
    const float svfLp = windowRms(30);

    // Switch to Moog LP4 by option index and close the knob — both writes
    // must land: character changes AND darkening tracks the knob.
    const size_t idxT = ParameterStore::indexOf(ID::FILTER_VA_TYPE);
    store.set(ID::FILTER_VA_TYPE,
              Curves::toNorm(kParams[idxT], 5.0f), Origin::MidiUsbDev);
    store.set(ID::FILTER_CUTOFF, 0.1f, Origin::MidiUsbDev);
    const float moogDark = windowRms(30);
    CHECK(moogDark < svfLp * 0.8f);
    CHECK(moogDark > 0.0f);
}

// =============================================================================
// OBXa engine (Pass 5.2)
// =============================================================================

TEST_CASE("OBXa 4-pole: darkens with the knob; multimode morphs toward 1-pole")
{
    FilterSection f;                             // OBXa is the default engine
    setKnob(f, 1.0f);
    const float open = sawRmsThrough(f, 40);
    f.reset();
    setKnob(f, 0.1f);
    const float dark = sawRmsThrough(f, 40);
    CHECK(dark < open * 0.5f);                   // 24 dB/oct bites hard

    // Multimode sweeps y4 (LP4) toward y1 (LP1): at the same low cutoff a
    // 1-pole passes far more of the saw than a 4-pole — RMS must rise.
    f.reset();
    f.setObxaMultimode(1.0f);
    const float onePole = sawRmsThrough(f, 40);
    CHECK(onePole > dark * 1.5f);
}

TEST_CASE("OBXa 2-pole modes: distinct from 4-pole, push and BP-blend live")
{
    // Same knob position: a 2-pole passes more HF than the 4-pole.
    FilterSection p4, p2;
    setKnob(p4, 0.3f); p4.setObxaMode(0);
    setKnob(p2, 0.3f); p2.setObxaMode(1);
    const float r4 = sawRmsThrough(p4, 40);
    const float r2 = sawRmsThrough(p2, 40);
    CHECK(r2 > r4 * 1.1f);

    // BP blend at multimode 0.5 is the band-pass centre: less total energy
    // than the LP end of the same blend (a band keeps less of a saw).
    FilterSection bp;
    setKnob(bp, 0.4f);
    bp.setObxaMode(2);
    bp.setObxaMultimode(0.0f);                   // LP end of the blend
    const float lpEnd = sawRmsThrough(bp, 40);
    bp.reset();
    bp.setObxaMultimode(0.5f);                   // BP centre
    const float bpMid = sawRmsThrough(bp, 40);
    CHECK(bpMid < lpEnd);

    // Push mode: same topology with the diode-pair bias — output differs
    // from plain 2-pole at high resonance (deterministic A/B).
    FilterSection plain, push;
    setKnob(plain, 0.4f); plain.setObxaMode(1); plain.setResonanceNorm(0.9f);
    setKnob(push,  0.4f); push.setObxaMode(3);  push.setResonanceNorm(0.9f);
    float a[kBlockSize], b[kBlockSize];
    float pa = 0.0f, pb = 0.0f;
    double diff = 0.0;
    for (int blk = 0; blk < 40; ++blk) {
        fillSaw(a, kBlockSize, pa);
        fillSaw(b, kBlockSize, pb);
        plain.process(a, kBlockSize);
        push.process(b, kBlockSize);
        for (size_t i = 0; i < kBlockSize; ++i)
            diff += ((double)a[i] - (double)b[i]) * ((double)a[i] - (double)b[i]);
    }
    CHECK(diff > 1e-4);
}

TEST_CASE("OBXa Xpander: HP rows thin a saw's lows, LP4 row keeps them")
{
    // Decisive corner: cutoff FAR above the 220 Hz saw (knob 0.8 ≈ 5 kHz).
    // LP4 then passes essentially the whole saw; HP2 keeps only the weak
    // harmonics above 5 kHz.  (At a cutoff NEAR the fundamental the
    // comparison inverts legitimately — a steep LP crushes its corner while
    // the HP keeps the whole harmonic series above it.  First version of
    // this test learned that the hard way.)
    FilterSection lp, hp;
    setKnob(lp, 0.8f); lp.setObxaMode(4); lp.setObxaXpanderMode(0);    // LP4
    setKnob(hp, 0.8f); hp.setObxaMode(4); hp.setObxaXpanderMode(5);    // HP2
    const float lpR = sawRmsThrough(lp, 40);
    const float hpR = sawRmsThrough(hp, 40);
    CHECK(hpR < lpR * 0.3f);

    // Every row, worst-corner drive: finite and clamped (matrix rows with
    // ±6 gains are the stress case for the state guard).
    for (int mode = 0; mode < 15; ++mode) {
        CAPTURE(mode);
        FilterSection f;
        setKnob(f, 0.9f);
        f.setObxaMode(5);                        // "Xpander M" alias
        f.setObxaXpanderMode(mode);
        f.setResonanceNorm(1.0f);                // clamps to 0.97 inside
        float buf[kBlockSize];
        float phase = 0.0f;
        for (int b2 = 0; b2 < 150; ++b2) {
            fillSaw(buf, kBlockSize, phase);
            f.process(buf, kBlockSize);
            for (float s : buf) {
                REQUIRE(std::isfinite(s));
                REQUIRE(std::fabs(s) <= 1.0f);
            }
        }
    }
}

TEST_CASE("OBXa: engine switching resets state; mode thrash stays finite")
{
    FilterSection f;
    setKnob(f, 0.7f);
    f.setResonanceNorm(0.95f);
    uint32_t rng = 23;
    float buf[kBlockSize];
    float phase = 0.0f;
    for (int b = 0; b < 600; ++b) {
        rng = rng * 1664525u + 1013904223u;
        if ((b & 7) == 0) f.setEngine((int)((rng >> 8) & 1u));   // OBXa<->VA
        if ((b & 3) == 1) f.setObxaMode((int)((rng >> 9) % 6u));
        if ((b & 3) == 2) f.setVaType((int)((rng >> 10) % 17u));
        if ((rng & 15u) == 0) f.setObxaMultimode((float)((rng >> 8) & 255u) / 255.0f);
        fillSaw(buf, kBlockSize, phase);
        f.process(buf, kBlockSize);
        for (float s : buf) {
            REQUIRE(std::isfinite(s));
            REQUIRE(std::fabs(s) <= 1.0f);
        }
    }
}

TEST_CASE("OBXa engine wiring: mode/multimode/xpander land via the store")
{
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);
    float L[kBlockSize], R[kBlockSize];

    auto windowRms = [&](int blocks) {
        double acc = 0.0;
        for (int i = 0; i < blocks; ++i) {
            core.renderBlock(L, R, kBlockSize);
            for (size_t k = 0; k < kBlockSize; ++k)
                acc += (double)L[k] * (double)L[k];
        }
        return (float)std::sqrt(acc / (double)(blocks * (int)kBlockSize));
    };

    store.setEngineering(ID::ENV_AMP_SUSTAIN, 1.0f, Origin::Ui);
    store.setEngineering(ID::MIX_OSC2, 0.0f, Origin::Ui);
    // Cutoff BELOW the note's 110 Hz fundamental: pole count now dominates
    // the outcome (at cutoffs a few times above the fundamental, LP1 and
    // LP4 differ only in the small harmonic tail — measured, not guessed).
    store.setEngineering(ID::FILTER_CUTOFF, 60.0f, Origin::MidiUsbDev);
    core.noteOn(45, 127);                        // default engine == OBXa 4P
    const float lp4 = windowRms(30);

    // Multimode to 1.0 selects the y1 tap (LP1): -6 dB/oct spares the
    // fundamental that -24 dB/oct was crushing.
    store.setEngineering(ID::FILTER_OBXA_MULTIMODE, 1.0f, Origin::MidiUsbDev);
    const float lp1 = windowRms(30);
    CHECK(lp1 > lp4 * 1.5f);

    // Xpander HP2 row via mode + sub-mode options: lows thinned.
    const size_t idxM = ParameterStore::indexOf(ID::FILTER_MODE);
    const size_t idxX = ParameterStore::indexOf(ID::FILTER_OBXA_XPANDER_MODE);
    store.set(ID::FILTER_MODE, Curves::toNorm(kParams[idxM], 4.0f), Origin::MidiUsbDev);
    store.set(ID::FILTER_OBXA_XPANDER_MODE,
              Curves::toNorm(kParams[idxX], 5.0f), Origin::MidiUsbDev);
    const float hp2 = windowRms(30);
    // At a 60 Hz knob the HP2 row passes the WHOLE 110 Hz saw while LP1
    // attenuates it — louder is the correct proof the row switched in.
    CHECK(hp2 > lp1 * 1.3f);
}
