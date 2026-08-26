// =============================================================================
// test_param_slew.cpp — filter.cutoff smoothing (SlewedValue at layer level)
// =============================================================================
#include "doctest.h"
#include <cmath>
#include <initializer_list>
#include "core/SynthCore.h"
#include "core/ParameterStore.h"
#include "core/dsp/SlewedValue.h"
#include "gen/ParamTable.h"
#include "core/dsp/Curves.h"
using namespace JT; using namespace JT::Params;

TEST_CASE("SlewedValue: block advance matches the per-sample recurrence")
{
    // The whole point of tickBlock() is that it is EXACT, not an approximation.
    // Run one smoother per-sample for a block and another a whole block at a
    // time, and they must agree.
    SlewedValue perSample, perBlock;
    for (SlewedValue* s : {&perSample, &perBlock}) {
        s->setSampleRate(kSampleRate);
        s->setBlockSize((int)kBlockSize);
        s->setTimeMs(5.0f);
        s->reset(0.0f);
        s->setTarget(1.0f);
    }
    for (int b = 0; b < 6; ++b) {
        for (size_t i = 0; i < kBlockSize; ++i) perSample.tick();
        perBlock.tickBlock();
        CHECK(perBlock.current() == doctest::Approx(perSample.current()).epsilon(1e-4));
    }
}

TEST_CASE("SlewedValue: settles, and a settled smoother is free")
{
    SlewedValue s;
    s.setSampleRate(kSampleRate); s.setBlockSize((int)kBlockSize);
    s.setTimeMs(5.0f); s.reset(0.0f);
    CHECK(s.isSettled());                       // idle from the start
    s.setTarget(0.5f);
    CHECK_FALSE(s.isSettled());
    int blocks = 0;
    while (!s.isSettled() && blocks < 200) { s.tickBlock(); ++blocks; }
    CHECK(s.isSettled());
    CHECK(s.current() == doctest::Approx(0.5f));
    CHECK(blocks < 20);                         // ~5 ms, not a long tail
    // Re-targeting to the value it already holds must NOT wake it up.
    s.setTarget(0.5f);
    CHECK(s.isSettled());
}

TEST_CASE("cutoff: first write snaps, later writes glide")
{
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);
    float L[kBlockSize], R[kBlockSize];

    // First write: a patch load must land immediately, not sweep up from
    // wherever the previous patch sat.
    store.setEngineering(ID::FILTER_CUTOFF, 800.0f, Origin::Ui);
    core.renderBlock(L, R, kBlockSize);
    const float afterLoad = core.debugSlewCur(ID::FILTER_CUTOFF);

    // Second write: a knob move must glide.  One block in, we must be BETWEEN
    // the old and new values — that is the whole behaviour under test.
    store.setEngineering(ID::FILTER_CUTOFF, 8000.0f, Origin::MidiUsbDev);
    core.renderBlock(L, R, kBlockSize);
    const float oneBlockIn = core.debugSlewCur(ID::FILTER_CUTOFF);
    CHECK(oneBlockIn > afterLoad);              // moved toward the target
    const float target = Curves::toNorm(*Params::find(ID::FILTER_CUTOFF), 8000.0f);
    CHECK(oneBlockIn < target);                 // but has NOT arrived: it glided

    // And it does arrive.
    for (int b = 0; b < 40; ++b) core.renderBlock(L, R, kBlockSize);
    CHECK(core.debugSlewCur(ID::FILTER_CUTOFF) == doctest::Approx(target).epsilon(0.01));
}

TEST_CASE("cutoff: a detent is spread over several blocks")
{
    // The panel's 1/128 detent is ~77 cents on the filter row.  Confirm the
    // smoother breaks that into steps small enough not to read as a staircase.
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);
    float L[kBlockSize], R[kBlockSize];

    store.set(ID::FILTER_CUTOFF, 0.5f, Origin::Ui);
    core.renderBlock(L, R, kBlockSize);
    float prev = core.debugSlewCur(ID::FILTER_CUTOFF);

    store.set(ID::FILTER_CUTOFF, 0.5f + 1.0f / 128.0f, Origin::MidiUsbDev);
    float biggestStep = 0.0f;
    int   movingBlocks = 0;
    for (int b = 0; b < 20; ++b) {
        core.renderBlock(L, R, kBlockSize);
        const float now  = core.debugSlewCur(ID::FILTER_CUTOFF);
        const float step = std::fabs(now - prev);
        if (step > 1e-7f) ++movingBlocks;
        if (step > biggestStep) biggestStep = step;
        prev = now;
    }
    CHECK(movingBlocks >= 3);                   // spread, not landed whole
    CHECK(biggestStep < (1.0f / 128.0f));       // no single block takes it all
}

TEST_CASE("resonance: first write snaps, later writes glide")
{
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);
    float L[kBlockSize], R[kBlockSize];

    store.set(ID::FILTER_RESONANCE, 0.2f, Origin::Ui);
    core.renderBlock(L, R, kBlockSize);
    CHECK(core.debugSlewCur(ID::FILTER_RESONANCE) == doctest::Approx(0.2f));   // snapped

    store.set(ID::FILTER_RESONANCE, 0.9f, Origin::MidiUsbDev);
    core.renderBlock(L, R, kBlockSize);
    const float oneBlockIn = core.debugSlewCur(ID::FILTER_RESONANCE);
    CHECK(oneBlockIn > 0.2f);
    CHECK(oneBlockIn < 0.9f);                                    // glided
    for (int b = 0; b < 40; ++b) core.renderBlock(L, R, kBlockSize);
    CHECK(core.debugSlewCur(ID::FILTER_RESONANCE) == doctest::Approx(0.9f).epsilon(0.01));
}

TEST_CASE("cutoff and resonance glide independently, in one pass")
{
    // Both moving at once must not interfere: each tracks its own target.
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);
    float L[kBlockSize], R[kBlockSize];

    store.set(ID::FILTER_CUTOFF,    0.2f, Origin::Ui);
    store.set(ID::FILTER_RESONANCE, 0.8f, Origin::Ui);
    core.renderBlock(L, R, kBlockSize);

    // Move them in OPPOSITE directions in the same block.
    store.set(ID::FILTER_CUTOFF,    0.9f, Origin::MidiUsbDev);
    store.set(ID::FILTER_RESONANCE, 0.1f, Origin::MidiUsbDev);
    for (int b = 0; b < 40; ++b) core.renderBlock(L, R, kBlockSize);
    CHECK(core.debugSlewCur(ID::FILTER_CUTOFF) == doctest::Approx(0.9f).epsilon(0.01));
    CHECK(core.debugSlewCur(ID::FILTER_RESONANCE)    == doctest::Approx(0.1f).epsilon(0.01));
}

TEST_CASE("a settled filter costs nothing and drifts nowhere")
{
    // Regression guard: with neither knob moving, the smoothers must hold
    // EXACTLY, not creep. A slow drift here would detune every patch over time.
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);
    float L[kBlockSize], R[kBlockSize];

    store.set(ID::FILTER_CUTOFF,    0.42f, Origin::Ui);
    store.set(ID::FILTER_RESONANCE, 0.63f, Origin::Ui);
    for (int b = 0; b < 50; ++b) core.renderBlock(L, R, kBlockSize);
    const float c0 = core.debugSlewCur(ID::FILTER_CUTOFF);
    const float r0 = core.debugSlewCur(ID::FILTER_RESONANCE);
    for (int b = 0; b < 500; ++b) core.renderBlock(L, R, kBlockSize);
    CHECK(core.debugSlewCur(ID::FILTER_CUTOFF) == c0);   // bit-exact, not approx
    CHECK(core.debugSlewCur(ID::FILTER_RESONANCE)    == r0);
}

// ── Generic bank: the behaviours only the shared mechanism can have ──────────

TEST_CASE("every smooth_ms > 0 param is Continuous")
{
    // Safety invariant, asserted rather than assumed.  The bank glides ANY
    // param whose row says smooth_ms > 0.  If a Select or Toggle ever acquired
    // a non-zero smooth_ms, gliding it would walk the engine through every
    // intermediate option — stepping through filter types on the way to the one
    // you asked for.  This test is the guard on that.
    for (size_t i = 0; i < kParamCount; ++i) {
        const ParamDesc& d = kParams[i];
        if (d.smoothMs > 0) {
            CAPTURE(d.key);
            CHECK(d.type == Type::Continuous);
        }
    }
}

TEST_CASE("velocity sens params declare no smoothing")
{
    // They are latched at note-on (Voice.cpp:84/97/101) and held as per-note
    // DC, so a slew could never run on them.  Their rows used to claim 5 ms.
    for (uint16_t id : {ID::VELOCITY_AMP_SENS, ID::VELOCITY_FILTER_SENS,
                        ID::VELOCITY_ENV_SENS})
        CHECK(Params::find(id)->smoothMs == 0);
}

TEST_CASE("smooth_ms 0 params still step immediately")
{
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);
    float L[kBlockSize], R[kBlockSize];
    // osc1.wave is a Select at smooth_ms 0 — it must land on the block it
    // arrives, with no intermediate values ever reaching the engine.
    store.set(ID::OSC1_WAVE, 0.0f, Origin::Ui);
    core.renderBlock(L, R, kBlockSize);
    store.set(ID::OSC1_WAVE, 1.0f, Origin::MidiUsbDev);
    core.renderBlock(L, R, kBlockSize);
    CHECK(core.debugSlewCur(ID::OSC1_WAVE) == 1.0f);
    CHECK(core.debugSlewActiveCount() == 0);      // never entered the bank
}

TEST_CASE("many params glide at once, all reach target")
{
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);
    float L[kBlockSize], R[kBlockSize];

    const uint16_t ids[] = {
        ID::FILTER_CUTOFF, ID::FILTER_RESONANCE, ID::FILTER_ENV_AMOUNT,
        ID::FILTER_KEY_TRACK, ID::FILTER_OCTAVE_CONTROL, ID::MIX_OSC1,
        ID::MIX_OSC2, ID::MIX_SUB, ID::MIX_NOISE, ID::OSC1_RING_MIX,
        ID::OSC2_RING_MIX, ID::REVERB_MIX, ID::FX_DRY_MIX,
    };
    for (uint16_t id : ids) store.set(id, 0.25f, Origin::Ui);   // first = snap
    core.renderBlock(L, R, kBlockSize);
    CHECK(core.debugSlewActiveCount() == 0);

    for (uint16_t id : ids) store.set(id, 0.75f, Origin::MidiUsbDev);
    core.renderBlock(L, R, kBlockSize);
    CHECK(core.debugSlewActiveCount() > 0);                     // all gliding
    for (uint16_t id : ids) {
        CAPTURE(Params::find(id)->key);
        CHECK(core.debugSlewCur(id) > 0.25f);
        CHECK(core.debugSlewCur(id) < 0.75f);
    }

    for (int b = 0; b < 60; ++b) core.renderBlock(L, R, kBlockSize);
    for (uint16_t id : ids) {
        CAPTURE(Params::find(id)->key);
        CHECK(core.debugSlewCur(id) == doctest::Approx(0.75f).epsilon(0.005));
    }
    // Everything settled EXACTLY and left the active list — no permanent creep.
    CHECK(core.debugSlewActiveCount() == 0);
}

TEST_CASE("master.volume uses the slow class")
{
    // The only smooth_ms 20 row in the table.  It must take visibly longer to
    // arrive than a 5 ms param driven over the same distance.
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);
    float L[kBlockSize], R[kBlockSize];

    store.set(ID::MASTER_VOLUME,  0.1f, Origin::Ui);
    store.set(ID::FILTER_CUTOFF,  0.1f, Origin::Ui);
    core.renderBlock(L, R, kBlockSize);
    store.set(ID::MASTER_VOLUME,  0.9f, Origin::MidiUsbDev);
    store.set(ID::FILTER_CUTOFF,  0.9f, Origin::MidiUsbDev);
    core.renderBlock(L, R, kBlockSize);
    // Same distance, same block: the slow one must have travelled less.
    CHECK(core.debugSlewCur(ID::MASTER_VOLUME) < core.debugSlewCur(ID::FILTER_CUTOFF));
    CHECK(Params::find(ID::MASTER_VOLUME)->smoothMs == 20);
}
