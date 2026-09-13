// =============================================================================
// test_fxchain.cpp — proofs for the Phase 6 PER-PATCH FX CHAIN subsystem
// =============================================================================
// Covers the 14 ParamTable §9 params wired this pass (see
// docs/PHASE6_FXCHAIN_SPEC.md): FX_BASS_GAIN/TREBLE_GAIN/DRIVE, FX_MOD_*,
// FX_DELAY_* (SYNC deferred), FX_DRY_MIX/JPFX_MIX — folded into
// SynthCore::renderBlock after the voice sum and before the global reverb, and
// the ported FxChain (JP-8000 JPFX) processor itself.
//
// TWO TEST LAYERS:
//   1. Mapping unit tests on FxChain directly (via #ifdef JT_TESTING debug
//      probes) — guard the ported v1 constants + the D-1/Q3/D-5 decodes.
//   2. Behavioural tests through SynthCore — engaged-gate guard (Q6, the
//      byte-identical proof), saturation/delay/chorus audibility, stability.
//
// POOLS: a Rig owns a comb pool AND an fx pool on the heap.  The default
// SynthCore ctor leaves fxPool null (chain inert) — FX tests MUST pass a real
// pool or the chain never runs.
// =============================================================================
#include "doctest.h"
#include "core/dsp/TempoClock.h"
#include <cmath>

#include <cmath>
#include <vector>

#include "core/SynthCore.h"
#include "core/dsp/FxChain.h"
#include "core/dsp/Curves.h"
#include "gen/ParamTable.h"

using namespace JT;
using namespace JT::Params;

namespace {

// Same clean steady-tone rig as test_reverb, plus an fx pool so the chain runs.
struct Rig {
    ParameterStore     store;
    std::vector<float> combPool;
    std::vector<float> fxPool;
    SynthCore          core;
    Rig()
        : combPool((size_t)SynthCore::kCombPoolFloats, 0.0f),
          fxPool((size_t)SynthCore::kFxPoolFloats, 0.0f),
          core(store, combPool.data(), /*reverbPool*/ nullptr, fxPool.data())
    {
        setE(ID::MIX_OSC2, 0.0f);
        setE(ID::MIX_SUB, 0.0f);
        setE(ID::MIX_NOISE, 0.0f);
        setOpt(ID::OSC1_WAVE, (float)(int)Wave::Sine);
        setE(ID::ENV_AMP_ATTACK, 0.0f);
        setE(ID::ENV_AMP_SUSTAIN, 1.0f);
        setE(ID::ENV_AMP_RELEASE, 5.0f);
    }
    void setE(uint16_t id, float eng) { store.setEngineering(id, eng, Origin::Ui); }
    void setNorm(uint16_t id, float n) { store.set(id, n, Origin::Ui); }
    void setOpt(uint16_t id, float o)
    { store.set(id, Curves::toNorm(kParams[ParameterStore::indexOf(id)], o), Origin::Ui); }

    void block(float* L, float* R) { core.renderBlock(L, R, kBlockSize); }
};

std::vector<float> run(Rig& rig, int blocks, int onBlocks = -1, int note = 57, int vel = 100)
{
    std::vector<float> out;
    float L[kBlockSize], R[kBlockSize];
    // Warm-up block so params land before the note (renderBlock drains notes
    // BEFORE dirty params — the suite-wide lesson).
    rig.block(L, R);
    rig.core.noteOn((uint8_t)note, (uint8_t)vel);
    for (int b = 0; b < blocks; ++b) {
        if (onBlocks >= 0 && b == onBlocks) rig.core.noteOff((uint8_t)note);
        rig.block(L, R);
        for (size_t k = 0; k < kBlockSize; ++k) { out.push_back(L[k]); out.push_back(R[k]); }
    }
    return out;
}

double rms(const std::vector<float>& v, size_t from, size_t to)
{
    double acc = 0.0; size_t n = 0;
    for (size_t i = from; i < to && i < v.size(); ++i) { const double s = (double)v[i]; acc += s * s; ++n; }
    return n ? std::sqrt(acc / (double)n) : 0.0;
}

bool allFinite(const std::vector<float>& v)
{
    for (float x : v) if (!std::isfinite(x)) return false;
    return true;
}

float peakAbs(const std::vector<float>& v)
{
    float m = 0.0f;
    for (float x : v) { const float a = std::fabs(x); if (a > m) m = a; }
    return m;
}

// Option-index norm for a Select param (so setNorm lands the exact bucket).
float optNorm(uint16_t id, int optIndex)
{ return Curves::normFromOptionIndex(kParams[ParameterStore::indexOf(id)], optIndex); }

} // namespace

// =============================================================================
// LAYER 1 — mapping unit tests on FxChain directly (debug probes)
// =============================================================================
TEST_CASE("fxchain: tone dB mapping — norm 0/0.5/1 -> -12/0/+12 dB")
{
    // Reproduce SynthCore's applyParam maths: dB = norm*24 - 12.
    FxChain fx;
    std::vector<float> pool((size_t)FxChain::kPoolFloats, 0.0f);
    fx.begin(pool.data());

    fx.setBassGain(0.0f * 24.0f - 12.0f);
    CHECK(fx.debugBassDb() == doctest::Approx(-12.0f));
    fx.setBassGain(0.5f * 24.0f - 12.0f);
    CHECK(fx.debugBassDb() == doctest::Approx(0.0f));
    fx.setTrebleGain(1.0f * 24.0f - 12.0f);
    CHECK(fx.debugTrebleDb() == doctest::Approx(12.0f));
}

TEST_CASE("fxchain: mod rate & delay time endpoints")
{
    FxChain fx;
    std::vector<float> pool((size_t)FxChain::kPoolFloats, 0.0f);
    fx.begin(pool.data());

    // Rate override only takes effect with an active mod effect (updateLfoIncrements
    // reads the preset otherwise), but the stored override is what we probe.
    fx.setModEffect(0);              // CHORUS1
    fx.setModRate(1.0f * 20.0f);     // norm 1.0 -> 20 Hz
    CHECK(fx.debugModRateHz() == doctest::Approx(20.0f));

    fx.setDelayEffect(1);            // MONO_LONG (so ratio resolves)
    fx.setDelayTime(1500.0f);        // an ordinary explicit time, mid-sweep now
    CHECK(fx.debugDelayMs() == doctest::Approx(1500.0f));
}

TEST_CASE("fxchain: preset-change mute lasts the read depth, not a buffer lap")
{
    FxChain fx;
    std::vector<float> pool((size_t)FxChain::kPoolFloats, 0.0f);
    fx.begin(pool.data());

    const float msToSamp = 0.001f * 44100.0f;
    float blkL[128] = {0.0f}, blkR[128] = {0.0f};

    // MONO_SHORT and MONO_LONG both have an L/R ratio of 1.0, so the deepest
    // tap is exactly the time set — the panning presets multiply the R tap and
    // would make these bounds a statement about the ratio, not about the clamp.
    fx.setDelayEffect(0);               // MONO_SHORT
    fx.setDelayTime(100.0f);            // 100 ms -> 4410 samples
    fx.setDelayMix(1.0f);

    const uint32_t shortNeeded = (uint32_t)(100.0f * msToSamp) + 2u;

    // Run well past what a 100 ms tap needs, but FAR short of a full buffer
    // lap.  Before the clamp this armed 441002 samples (10.00 s) and the mute
    // would still be running when this loop ends.
    for (uint32_t done = 0; done < shortNeeded + 256u; done += 128u)
        fx.processBlock(blkL, blkR, 128);
    CHECK(fx.debugDelayMuteCounter() == 0u);

    // The clamp must only ever LOWER the counter: a genuinely deep tap still
    // gets the flush it needs, or the previous preset's audio leaks into the
    // feedback loop — the thing the mute exists to prevent.
    fx.setDelayEffect(1);               // MONO_LONG, ratio 1.0
    fx.setDelayTime(4000.0f);           // 4 s
    fx.processBlock(blkL, blkR, 128);

    const uint32_t longNeeded = (uint32_t)(4000.0f * msToSamp) + 2u;
    CHECK(fx.debugDelayMuteCounter() >  shortNeeded);
    CHECK(fx.debugDelayMuteCounter() <= longNeeded);
    CHECK(fx.debugDelayMuteCounter() >= longNeeded - 128u);   // one block consumed
}

TEST_CASE("fxchain: delay time — E2 explicit-only, clamped to the Log range")
{
    FxChain fx;
    std::vector<float> pool((size_t)FxChain::kPoolFloats, 0.0f);
    fx.begin(pool.data());
    fx.setDelayEffect(1);            // MONO_LONG (so the L/R ratio resolves)

    // The top of the new sweep must actually be reachable — this is the whole
    // point of the 10 s buffer, and a stale kDelayLen would clamp it here.
    fx.setDelayTime(FxChain::kMaxDelayMs);
    CHECK(fx.debugDelayMs() == doctest::Approx(FxChain::kMaxDelayMs));

    // E2: 0 no longer means "use the preset time".  It used to install a -1
    // sentinel; it must now clamp to kMinDelayMs like any other out-of-range
    // value, so the bottom of the knob is a real 10 ms delay.
    fx.setDelayTime(0.0f);
    CHECK(fx.debugDelayMs() == doctest::Approx(FxChain::kMinDelayMs));

    // Over-range is clamped rather than wrapped — a wrapped read pointer would
    // be an out-of-bounds access on the delay buffer, not merely a wrong time.
    fx.setDelayTime(FxChain::kMaxDelayMs * 10.0f);
    CHECK(fx.debugDelayMs() == doctest::Approx(FxChain::kMaxDelayMs));
}

TEST_CASE("fxchain: feedback sentinel — 0 -> use preset (-1), else norm*0.99")
{
    FxChain fx;
    std::vector<float> pool((size_t)FxChain::kPoolFloats, 0.0f);
    fx.begin(pool.data());

    // SynthCore does: norm<=0 ? -1 : norm*0.99  (D-5)
    fx.setModFeedback(-1.0f);                       // norm 0 path
    CHECK(fx.debugModFb() == doctest::Approx(-1.0f));
    fx.setModFeedback(1.0f * 0.99f);
    CHECK(fx.debugModFb() == doctest::Approx(0.99f));

    fx.setDelayFeedback(-1.0f);
    CHECK(fx.debugDelayFb() == doctest::Approx(-1.0f));
    fx.setDelayFeedback(0.5f * 0.99f);
    CHECK(fx.debugDelayFb() == doctest::Approx(0.495f));
}

TEST_CASE("fxchain: Select off-by-one decode (Q3) — opt-1 = v1 type")
{
    FxChain fx;
    std::vector<float> pool((size_t)FxChain::kPoolFloats, 0.0f);
    fx.begin(pool.data());

    // Mirror applyParam: setModEffect(opt - 1).
    fx.setModEffect(0 - 1);   CHECK(fx.debugModType() == -1);   // "OFF"
    CHECK(fx.modActive() == false);
    fx.setModEffect(1 - 1);   CHECK(fx.debugModType() == 0);    // preset 0
    CHECK(fx.modActive() == true);
    fx.setModEffect(11 - 1);  CHECK(fx.debugModType() == 10);   // preset 10

    fx.setDelayEffect(0 - 1); CHECK(fx.debugDelayType() == -1); // "OFF"
    CHECK(fx.delayActive() == false);
    fx.setDelayEffect(5 - 1); CHECK(fx.debugDelayType() == 4);  // preset 4
    CHECK(fx.delayActive() == true);
}

TEST_CASE("fxchain: drive mode Select (D-1) — 0/1/2 = OFF/Soft/Hard")
{
    FxChain fx;
    std::vector<float> pool((size_t)FxChain::kPoolFloats, 0.0f);
    fx.begin(pool.data());

    fx.setDriveMode(0); CHECK(fx.debugDriveMode() == 0); CHECK(fx.driveActive() == false);
    fx.setDriveMode(1); CHECK(fx.debugDriveMode() == 1); CHECK(fx.driveActive() == true);
    fx.setDriveMode(2); CHECK(fx.debugDriveMode() == 2); CHECK(fx.driveActive() == true);
}

// =============================================================================
// LAYER 2 — NO-SILENT-CHANGE GUARD (CLAUDE.md rule 2) — the gate on Q6
// =============================================================================
TEST_CASE("fxchain: default patch is byte-identical to never touching FX params")
{
    // Table defaults: drive OFF, mod OFF, delay OFF => chain disengaged =>
    // renderBlock skips processBlock entirely.  Writing every FX param at its
    // default must not perturb one output sample versus never writing them.
    auto render = [](bool writeFx) {
        Rig rig;
        if (writeFx) {
            rig.setNorm(ID::FX_BASS_GAIN, 0.0f);
            rig.setNorm(ID::FX_TREBLE_GAIN, 0.0f);
            rig.setNorm(ID::FX_DRIVE, optNorm(ID::FX_DRIVE, 0));        // OFF
            rig.setNorm(ID::FX_MOD_EFFECT, optNorm(ID::FX_MOD_EFFECT, 0));   // OFF
            rig.setNorm(ID::FX_MOD_MIX, 0.0f);
            rig.setNorm(ID::FX_MOD_RATE, 0.5f);
            rig.setNorm(ID::FX_MOD_FEEDBACK, 0.0f);
            rig.setNorm(ID::FX_DELAY_EFFECT, optNorm(ID::FX_DELAY_EFFECT, 0)); // OFF
            rig.setNorm(ID::FX_DELAY_TIME, 0.5f);
            rig.setNorm(ID::FX_DELAY_MIX, 0.0f);
            rig.setNorm(ID::FX_DELAY_FEEDBACK, 0.0f);
            rig.setNorm(ID::FX_DELAY_SYNC, optNorm(ID::FX_DELAY_SYNC, 0));
            rig.setNorm(ID::FX_DRY_MIX, 1.0f);
            rig.setNorm(ID::FX_JPFX_MIX, 1.0f);
        }
        return run(rig, 60, 30);
    };
    const auto a = render(false);
    const auto b = render(true);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) CHECK(a[i] == b[i]);
}

// =============================================================================
// LAYER 2 — audible-effect + stability proofs through SynthCore
// =============================================================================
TEST_CASE("fxchain: hard drive alters the signal and stays within the limiter ceiling")
{
    Rig dry;                       // chain disengaged
    Rig wet;
    wet.setNorm(ID::FX_DRIVE, optNorm(ID::FX_DRIVE, 2));   // Hard

    const auto a = run(dry, 40, -1);
    const auto b = run(wet, 40, -1);
    REQUIRE(a.size() == b.size());

    // Different from dry (saturation present).
    double diff = 0.0;
    for (size_t i = 0; i < a.size(); ++i) diff += std::fabs((double)a[i] - (double)b[i]);
    CHECK(diff > 0.0);

    // Finite and within the float limiter ceiling (0.97) — no int16 clip, D-7.
    CHECK(allFinite(b));
    CHECK(peakAbs(b) <= 0.97f + 1e-4f);
}

TEST_CASE("fxchain: MONO_LONG delay leaves a tail after the note stops")
{
    // v1 CLICK-FREE TRANSITION (AudioEffectJPFX.cpp:528-534, ported verbatim):
    // on a delay preset change the wet output is MUTED for one full buffer lap
    // (_delayMuteCounter = kDelayLen = 66152 samples ≈ 517 blocks) while fresh
    // audio overwrites stale PSRAM.  So the wet tail only appears AFTER ~517
    // blocks.  A short window would sit entirely inside the mute lap and read
    // zero — that is correct behaviour, not a dead delay.  We therefore run well
    // past the mute lap, holding the note the whole time so the buffer fills,
    // then release and measure the tail.
    Rig rig;
    rig.setNorm(ID::FX_DELAY_EFFECT, optNorm(ID::FX_DELAY_EFFECT, 2)); // idx2 = MONO_LONG (opt-1=1)
    rig.setNorm(ID::FX_DELAY_MIX, 0.6f);
    rig.setNorm(ID::FX_DELAY_FEEDBACK, 0.6f);

    const int muteBlocks = (int)(FxChain::kDelayLen / kBlockSize) + 2; // ~518
    const int onBlocks   = muteBlocks + 20;                            // release after mute drains
    const int total      = onBlocks + 200;
    const auto out = run(rig, total, onBlocks);
    const size_t blkF = kBlockSize * 2;

    // Tail window: 120 blocks after note-off, once wet is live and decaying.
    const double tail = rms(out, (size_t)(onBlocks + 40) * blkF,
                                 (size_t)(onBlocks + 160) * blkF);
    CHECK(tail > 1e-5);
    CHECK(allFinite(out));
}

TEST_CASE("fxchain: chorus changes the signal without blowing up")
{
    Rig dry;
    Rig wet;
    wet.setNorm(ID::FX_MOD_EFFECT, optNorm(ID::FX_MOD_EFFECT, 1)); // idx1 -> v1 type 0 = CHORUS1
    wet.setNorm(ID::FX_MOD_MIX, 1.0f);

    const auto a = run(dry, 60, -1);
    const auto b = run(wet, 60, -1);
    REQUIRE(a.size() == b.size());

    double diff = 0.0;
    for (size_t i = 0; i < a.size(); ++i) diff += std::fabs((double)a[i] - (double)b[i]);
    CHECK(diff > 0.0);
    CHECK(allFinite(b));
    CHECK(peakAbs(b) <= 0.97f + 1e-4f);
}

TEST_CASE("fxchain: jpfx mix 0 with delay engaged returns the dry bus (scaled by dry mix)")
{
    // Engage delay (so the chain runs) but null the wet return; with dry mix 1.0
    // the output must track the pre-FX bus closely (only the mono-sum + blend
    // path runs, wet contributes nothing).
    Rig ref;                        // chain disengaged: true dry reference
    Rig cut;
    cut.setNorm(ID::FX_DELAY_EFFECT, optNorm(ID::FX_DELAY_EFFECT, 1)); // MONO_SHORT engaged
    cut.setNorm(ID::FX_JPFX_MIX, 0.0f);
    cut.setNorm(ID::FX_DRY_MIX, 1.0f);

    const auto a = run(ref, 30, -1);
    const auto b = run(cut, 30, -1);
    REQUIRE(a.size() == b.size());

    // Close (not necessarily bit-identical: the engaged path still sums L+R to
    // mono internally, but jpfxMix=0 discards it, so left/right = dry*1.0).
    double diff = 0.0;
    for (size_t i = 0; i < a.size(); ++i) diff += std::fabs((double)a[i] - (double)b[i]);
    CHECK(diff == doctest::Approx(0.0).epsilon(1e-6));
}

TEST_CASE("fxchain: worst case (hard drive + max-fb delay + chorus) stays finite")
{
    Rig rig;
    rig.setNorm(ID::FX_DRIVE, optNorm(ID::FX_DRIVE, 2));                  // Hard
    rig.setNorm(ID::FX_MOD_EFFECT, optNorm(ID::FX_MOD_EFFECT, 11));       // Super Chorus
    rig.setNorm(ID::FX_MOD_MIX, 1.0f);
    rig.setNorm(ID::FX_DELAY_EFFECT, optNorm(ID::FX_DELAY_EFFECT, 2));    // MONO_LONG
    rig.setNorm(ID::FX_DELAY_MIX, 1.0f);
    rig.setNorm(ID::FX_DELAY_FEEDBACK, 1.0f);                             // 0.99 after map

    const auto out = run(rig, 200, 100);
    CHECK(allFinite(out));
    CHECK(peakAbs(out) <= 0.97f + 1e-4f);
}

// ===========================================================================
// Aux-lane destinations (review item 6c)
// ===========================================================================
// The aux destination at index 4 was LABELLED "Drive" while the engine
// implemented a bass<->treble tone TILT.  The label was the thing that was
// wrong, so it is now "Tone" — and a real drive-amount destination was
// appended at index 5.  These prove the two are genuinely different effects.

TEST_CASE("fx: tone tilt engages the chain on its own and colours the signal")
{
    // toneTiltActive() exists so SynthCore can run the chain for the tilt
    // alone.  Without it, selecting the aux Tone destination on an all-OFF
    // chain did nothing whatsoever: _fxEngaged only tracks drive/mod/delay.
    FxChain fx;
    std::vector<float> pool((size_t)FxChain::kPoolFloats, 0.0f);
    fx.begin(pool.data());

    CHECK(fx.driveActive()   == false);
    CHECK(fx.modActive()     == false);
    CHECK(fx.delayActive()   == false);
    CHECK(fx.toneTiltActive() == false);   // nothing would run the chain

    fx.setToneTiltMod(1.0f);
    CHECK(fx.toneTiltActive() == true);    // now something does
}

TEST_CASE("fx: tone tilt changes the output, drive mod does not without drive")
{
    auto render = [](float tilt, float driveMod, int driveMode) {
        FxChain fx;
        std::vector<float> pool((size_t)FxChain::kPoolFloats, 0.0f);
        fx.begin(pool.data());
        fx.setDriveMode(driveMode);
        fx.setToneTiltMod(tilt);
        fx.setDriveAmountMod(driveMod);

        std::vector<float> out;
        float L[kBlockSize], R[kBlockSize];
        double ph = 0.0;
        for (int b = 0; b < 40; ++b) {              // long enough to settle both
            for (size_t i = 0; i < kBlockSize; ++i) {   // 220 Hz test tone
                const float x = 0.3f * (float)std::sin(ph);
                ph += 2.0 * 3.14159265358979 * 220.0 / (double)kSampleRate;
                L[i] = x; R[i] = x;
            }
            fx.processBlock(L, R, kBlockSize);
            for (size_t i = 0; i < kBlockSize; ++i) out.push_back(L[i]);
        }
        return out;
    };
    auto diff = [](const std::vector<float>& a, const std::vector<float>& b) {
        double d = 0.0;
        for (size_t i = 0; i < a.size(); ++i) d += std::fabs((double)a[i] - (double)b[i]);
        return d;
    };

    const auto flat = render(0.0f, 0.0f, 0);

    // Tone tilt colours the signal whatever the drive mode is — drive OFF here.
    CHECK(diff(flat, render(1.0f, 0.0f, 0)) > 0.0);

    // Drive mod with drive OFF is inaudible: applySaturation bypasses entirely
    // in that mode, so the aux lane cannot conjure a saturator that is not
    // running.  This is a documented limitation, asserted so it stays honest.
    CHECK(diff(flat, render(0.0f, 1.0f, 0)) == doctest::Approx(0.0));

    // With a drive mode selected, the SAME mod is clearly audible.
    const auto driven = render(0.0f, 0.0f, 1);
    CHECK(diff(driven, render(0.0f, 1.0f, 1)) > 0.0);
}

// =============================================================================
// T2 — tempo-synced delay (closes deferral D-2)
// =============================================================================

TEST_CASE("tempo clock: dotted divisions are 1.5x the plain note")
{
    TempoClock c;
    c.setBpm(120.0f);

    // ms for a division == 1000 / freqForMode.  At 120 BPM a quarter is 500 ms,
    // so the dotted quarter must be 750 ms — the arithmetic the delay relies on.
    auto ms = [&](int mode) { return 1000.0f / c.freqForMode(mode); };

    CHECK(ms(TempoClock::k1_4)   == doctest::Approx(500.0f));
    CHECK(ms(TempoClock::k1_4D)  == doctest::Approx(750.0f));
    CHECK(ms(TempoClock::k1_8)   == doctest::Approx(250.0f));
    CHECK(ms(TempoClock::k1_8D)  == doctest::Approx(375.0f));
    CHECK(ms(TempoClock::k1_16)  == doctest::Approx(125.0f));
    CHECK(ms(TempoClock::k1_16D) == doctest::Approx(187.5f));

    // Each dotted value must be exactly 1.5x its plain note — the property the
    // hand-written kMult[] entries could get wrong without this failing.
    CHECK(ms(TempoClock::k1_4D)  == doctest::Approx(ms(TempoClock::k1_4)  * 1.5f));
    CHECK(ms(TempoClock::k1_8D)  == doctest::Approx(ms(TempoClock::k1_8)  * 1.5f));
    CHECK(ms(TempoClock::k1_16D) == doctest::Approx(ms(TempoClock::k1_16) * 1.5f));
}

TEST_CASE("tempo clock: appending dotted modes did not move the existing ones")
{
    // The whole reason the dotted family was appended rather than inserted:
    // these indices are what saved patches store for lfo/seq/arp/delay rate.
    // If this test fails, every existing patch has silently changed division.
    CHECK((int)TempoClock::kFree  ==  0);
    CHECK((int)TempoClock::k1_4   ==  5);
    CHECK((int)TempoClock::k1_16  ==  7);   // arp.rate's default
    CHECK((int)TempoClock::k1_32  ==  8);
    CHECK((int)TempoClock::k1_16T == 11);
    CHECK((int)TempoClock::k1_4D  == 12);   // first appended entry
    CHECK((int)TempoClock::kNumModes == 15);
}

TEST_CASE("tempo clock: out-of-range modes report 'not synced' rather than reading past kMult")
{
    TempoClock c;
    c.setBpm(120.0f);
    CHECK(c.freqForMode(TempoClock::kNumModes) <= 0.0f);
    CHECK(c.freqForMode(-1) <= 0.0f);
    CHECK(c.freqForMode(TempoClock::kFree) <= 0.0f);
}

// =============================================================================
// D4 — tape delay engine
// =============================================================================
namespace {

// Two chains driven with identical input; returns true only if every output
// sample matches BIT for bit.  Approx() would hide exactly the drift this is
// meant to catch, so the comparison is exact on purpose.
struct TapeRig {
    std::vector<float> pool;
    FxChain fx;
    TapeRig() : pool((size_t)FxChain::kPoolFloats, 0.0f) { fx.begin(pool.data()); }
};

// A short burst followed by silence: the burst fills the line, the silence
// lets the repeats be observed on their own.
void feed(FxChain& fx, float* outL, float* outR, size_t n, bool excite)
{
    for (size_t i = 0; i < n; i += 128) {
        float bL[128], bR[128];
        for (size_t k = 0; k < 128; ++k) {
            // Alternating +/- is full-scale broadband — the harshest test for
            // both the feedback filter and the saturator.
            const float v = excite ? ((k & 1u) ? 0.5f : -0.5f) : 0.0f;
            bL[k] = v; bR[k] = v;
        }
        fx.processBlock(bL, bR, 128);
        for (size_t k = 0; k < 128 && i + k < n; ++k) {
            outL[i + k] = bL[k];
            outR[i + k] = bR[k];
        }
    }
}

float rms(const float* v, size_t n)
{
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i) acc += (double)v[i] * (double)v[i];
    return (float)std::sqrt(acc / (double)n);
}

} // namespace

TEST_CASE("D4: Digital engine is bit-identical with every tape control moved")
{
    // THE headline claim of D4: a Digital patch pays one branch and nothing
    // else, so the tape controls must be provably unable to touch its output.
    TapeRig a, b;
    for (FxChain* fx : { &a.fx, &b.fx }) {
        fx->setDelayEffect(1);
        fx->setDelayTime(120.0f);
        fx->setDelayMix(0.8f);
        fx->setDelayFeedback(0.7f);
    }
    // b gets every tape control pushed to an extreme — while staying Digital.
    b.fx.setDelayEngine(0);
    b.fx.setDelayTone(500.0f);
    b.fx.setDelaySat(1.0f);
    b.fx.setDelayWow(1.0f);
    b.fx.setDelayFlutter(1.0f);

    const size_t N = 4096;
    std::vector<float> aL(N), aR(N), bL(N), bR(N);
    feed(a.fx, aL.data(), aR.data(), N, true);
    feed(b.fx, bL.data(), bR.data(), N, true);

    bool identical = true;
    for (size_t i = 0; i < N; ++i)
        if (aL[i] != bL[i] || aR[i] != bR[i]) { identical = false; break; }
    CHECK(identical);
    CHECK_FALSE(b.fx.debugTapeActive());
}

TEST_CASE("D4: Tape with every control at zero also stays off the tape path")
{
    // "Engine == Tape" is not sufficient to arm the per-sample work — a patch
    // that selects Tape but leaves the controls alone should cost nothing.
    TapeRig r;
    r.fx.setDelayEffect(1);
    r.fx.setDelayTime(120.0f);
    r.fx.setDelayMix(0.8f);
    r.fx.setDelayEngine(1);
    r.fx.setDelayTone(12000.0f);        // maximum == filter off
    r.fx.setDelaySat(0.0f);
    r.fx.setDelayWow(0.0f);
    r.fx.setDelayFlutter(0.0f);

    float bL[128] = {0.0f}, bR[128] = {0.0f};
    r.fx.processBlock(bL, bR, 128);
    CHECK_FALSE(r.fx.debugTapeActive());

    // Lowering the tone alone must arm it — proving the gate tracks the
    // controls rather than being stuck off.
    r.fx.setDelayTone(3000.0f);
    r.fx.processBlock(bL, bR, 128);
    CHECK(r.fx.debugTapeActive());
    CHECK(r.fx.debugFbLpG() < 1.0f);    // g == 1 is the bypass value
}

TEST_CASE("D4: tone darkens the repeats progressively")
{
    // The filter is INSIDE the feedback loop, so the tail must lose energy
    // faster with the tone down than with it open.  Comparing late-tail RMS
    // between two otherwise identical chains isolates that.
    TapeRig open_, dark;
    for (FxChain* fx : { &open_.fx, &dark.fx }) {
        fx->setDelayEffect(0);          // MONO_SHORT, 80 ms
        fx->setDelayTime(80.0f);
        fx->setDelayMix(1.0f);
        fx->setDelayFeedback(0.85f);
        fx->setDelayEngine(1);
    }
    open_.fx.setDelayTone(12000.0f);    // off
    dark .fx.setDelayTone(800.0f);      // heavy damping

    // The open chain has tape ENGAGED but nothing to do, so this also checks
    // that "tone off" really is neutral rather than subtly lossy.
    const size_t N = 8192;
    std::vector<float> oL(N), oR(N), dL(N), dR(N);
    feed(open_.fx, oL.data(), oR.data(), 512, true);
    feed(open_.fx, oL.data(), oR.data(), N,   false);
    feed(dark .fx, dL.data(), dR.data(), 512, true);
    feed(dark .fx, dL.data(), dR.data(), N,   false);

    CHECK(rms(dL.data(), N) < rms(oL.data(), N));
}

TEST_CASE("D4: saturation tames a runaway feedback loop")
{
    // Feedback at the maximum with a hot input: the digital loop grows until
    // the sanitiser clamps it, the saturated loop should settle far lower.
    TapeRig clean, sat;
    for (FxChain* fx : { &clean.fx, &sat.fx }) {
        fx->setDelayEffect(0);
        fx->setDelayTime(50.0f);
        fx->setDelayMix(1.0f);
        fx->setDelayFeedback(0.99f);
        fx->setDelayEngine(1);
        fx->setDelayTone(12000.0f);
    }
    clean.fx.setDelaySat(0.0f);
    sat  .fx.setDelaySat(1.0f);

    const size_t N = 8192;
    std::vector<float> cL(N), cR(N), sL(N), sR(N);
    feed(clean.fx, cL.data(), cR.data(), 2048, true);
    feed(clean.fx, cL.data(), cR.data(), N,    false);
    feed(sat  .fx, sL.data(), sR.data(), 2048, true);
    feed(sat  .fx, sL.data(), sR.data(), N,    false);

    CHECK(rms(sL.data(), N) < rms(cL.data(), N));
    // And it must stay bounded — the point of a limiter in the loop.
    for (size_t i = 0; i < N; ++i) CHECK(std::fabs(sL[i]) < 2.0f);
}

TEST_CASE("D4: wow moves the read head without moving the write head")
{
    // Drift must change the OUTPUT while the delay time setting is untouched;
    // if it were applied to the write index instead, debugDelayMs would move
    // and the effect would be a delay-time change rather than pitch drift.
    TapeRig flat, wow;
    for (FxChain* fx : { &flat.fx, &wow.fx }) {
        fx->setDelayEffect(1);
        fx->setDelayTime(200.0f);   // 8820 samples: clears the mute inside N
        fx->setDelayMix(1.0f);
        fx->setDelayFeedback(0.5f);
        fx->setDelayEngine(1);
        fx->setDelayTone(12000.0f);
    }
    flat.fx.setDelayWow(0.0f);
    wow .fx.setDelayWow(1.0f);

    // Prime PAST the preset-change mute first.  A 200 ms tap owes ~8822
    // samples of flush, so a comparison that starts sooner is comparing two
    // dry signals and passes or fails for the wrong reason.
    const size_t N = 16384;
    std::vector<float> fL(N), fR(N), wL(N), wR(N);
    feed(flat.fx, fL.data(), fR.data(), N, true);
    feed(wow .fx, wL.data(), wR.data(), N, true);
    REQUIRE(flat.fx.debugDelayMuteCounter() == 0u);
    REQUIRE(wow .fx.debugDelayMuteCounter() == 0u);

    // Now the measured window, with the delay line full and the drift running.
    feed(flat.fx, fL.data(), fR.data(), N, true);
    feed(wow .fx, wL.data(), wR.data(), N, true);

    bool differs = false;
    for (size_t i = 0; i < N; ++i) if (fL[i] != wL[i]) { differs = true; break; }
    CHECK(differs);
    CHECK(wow.fx.debugDelayMs() == doctest::Approx(200.0f));   // time unchanged
    for (size_t i = 0; i < N; ++i) CHECK(std::fabs(wL[i]) < 2.0f);
}

// =============================================================================
// X2 — delay-time slew (the crackle fix)
// =============================================================================

TEST_CASE("X2: the slew snaps EXACTLY on arrival")
{
    // The slew must become invisible once the time stops moving, or every
    // downstream bit-identity claim and the render baseline break for the rest
    // of the session.  A one-pole alone never arrives — kSlewSnap is what
    // forces the last step to an exact assignment, and this is that guarantee.
    TapeRig r;
    r.fx.setDelayEffect(1);
    r.fx.setDelayMix(0.8f);
    r.fx.setDelayTime(900.0f);

    float wL[128] = {0.0f}, wR[128] = {0.0f};
    r.fx.processBlock(wL, wR, 128);
    // Primed by snapping on the first block, not ramped in from zero — or the
    // very first note would hear the delay sweep in from nothing.
    CHECK(r.fx.debugTapSamples() == r.fx.debugTapTarget());

    r.fx.setDelayTime(300.0f);
    r.fx.processBlock(wL, wR, 128);
    CHECK(r.fx.debugTapSamples() != r.fx.debugTapTarget());   // ramping

    // A 900 -> 300 ms move is 26460 samples.  Rate-clamped at 0.2 samples per
    // sample it glides for roughly 5 s, so allow generous time and then demand
    // EXACT equality — Approx would defeat the purpose of kSlewSnap.
    for (int i = 0; i < 3000; ++i) r.fx.processBlock(wL, wR, 128);
    CHECK(r.fx.debugTapSamples() == r.fx.debugTapTarget());
}

TEST_CASE("X2: the tap ramps rather than jumping")
{
    // A block-rate jump moved the read pointer by thousands of samples between
    // two consecutive OUTPUT samples.  Cap the per-block movement well below
    // that: 600 ms -> 150 ms is 19845 samples, and one 128-sample block at a
    // 150 ms time constant should cover well under a tenth of it.
    TapeRig r;
    r.fx.setDelayEffect(1);
    r.fx.setDelayTime(600.0f);
    float wL[128] = {0.0f}, wR[128] = {0.0f};
    r.fx.processBlock(wL, wR, 128);

    const float before = r.fx.debugTapSamples();
    r.fx.setDelayTime(150.0f);
    r.fx.processBlock(wL, wR, 128);
    const float moved = before - r.fx.debugTapSamples();

    CHECK(moved > 0.0f);                                  // it IS moving
    CHECK(moved <= 128.0f * FxChain::kTimeSlewMaxStep);   // and rate-clamped
    CHECK(moved < (before - r.fx.debugTapTarget()) * 0.1f);
}

TEST_CASE("X2: changing time under feedback no longer steps the output")
{
    // The bug: the read pointer jumped thousands of samples at a block
    // boundary, which is a click, which feedback then recirculated. Measure the
    // largest sample-to-sample jump across a time change and require it to stay
    // in the range continuous audio produces.
    auto maxStep = [](bool changeTime) {
        TapeRig r;
        r.fx.setDelayEffect(1);
        r.fx.setDelayTime(600.0f);
        r.fx.setDelayMix(1.0f);
        r.fx.setDelayFeedback(0.8f);

        // Prime past the preset mute with a steady tone so the line is full.
        float bL[128], bR[128];
        double ph = 0.0;
        auto block = [&]() {
            for (int k = 0; k < 128; ++k) {
                bL[k] = bR[k] = 0.35f * (float)std::sin(ph);
                ph += 2.0 * 3.14159265 * 220.0 / 44100.0;
            }
            r.fx.processBlock(bL, bR, 128);
        };
        for (int i = 0; i < 400; ++i) block();

        if (changeTime) r.fx.setDelayTime(150.0f);

        float prev = 0.0f, worst = 0.0f;
        bool first = true;
        for (int i = 0; i < 200; ++i) {
            block();
            for (int k = 0; k < 128; ++k) {
                if (!first) { const float d = std::fabs(bL[k] - prev); if (d > worst) worst = d; }
                prev = bL[k]; first = false;
            }
        }
        return worst;
    };

    const float steady  = maxStep(false);
    const float changed = maxStep(true);

    // A 220 Hz tone steps by at most ~0.05 per sample at this level, so a
    // discontinuity shows up as a step far larger than the steady-state one.
    // Before the slew this ratio was enormous; it must now stay close to 1.
    CHECK(changed < steady * 3.0f);
}

// =============================================================================
// W3 — wow specified in cents
// =============================================================================

TEST_CASE("W3: wow depth is delay-time independent")
{
    // THE point of moving from ms to cents. The old ms-based depth gave 5 cents
    // at 200 ms and 40 cents at 1500 ms from one knob position; the pitch
    // wobble must now be the same at any delay time.
    //
    // Measured indirectly: the drift amplitude in SAMPLES should scale with
    // 1/rate and NOT with the delay time, so a long and a short delay at the
    // same knob position must produce the same modulation depth in samples.
    auto driftRange = [](float delayMs) {
        TapeRig r;
        r.fx.setDelayEffect(1);         // MONO_LONG, ratio 1.0
        r.fx.setDelayTime(delayMs);
        r.fx.setDelayMix(1.0f);
        r.fx.setDelayEngine(1);
        r.fx.setDelayTone(12000.0f);
        r.fx.setDelayWow(1.0f);
        r.fx.setDelayWowRate(0.7f);
        r.fx.setDelayFlutter(0.0f);
        float bL[128] = {0.0f}, bR[128] = {0.0f};
        r.fx.processBlock(bL, bR, 128);
        return r.fx.debugWowAmpSamples();
    };

    // 600 ms and 1500 ms are both far above the kDriftMaxFrac clamp, so neither
    // is limited and the two must agree.
    CHECK(driftRange(600.0f) == doctest::Approx(driftRange(1500.0f)).epsilon(0.001));
}

TEST_CASE("W3: wow rate changes speed, not depth in cents")
{
    // A faster capstan needs a SMALLER delay excursion for the same pitch
    // deviation (dev = A * 2*pi*f). Halving the rate must roughly double the
    // amplitude in samples — that relationship is what keeps the depth knob
    // meaning cents rather than milliseconds.
    auto amp = [](float rate) {
        TapeRig r;
        r.fx.setDelayEffect(1);
        r.fx.setDelayTime(2000.0f);     // well clear of the safety clamp
        r.fx.setDelayEngine(1);
        r.fx.setDelayTone(12000.0f);
        r.fx.setDelayWow(1.0f);
        r.fx.setDelayWowRate(rate);
        float bL[128] = {0.0f}, bR[128] = {0.0f};
        r.fx.processBlock(bL, bR, 128);
        return r.fx.debugWowAmpSamples();
    };
    CHECK(amp(0.5f) == doctest::Approx(amp(1.0f) * 2.0f).epsilon(0.01));
}

TEST_CASE("W3: drift is decorrelated between channels")
{
    // The first version added ONE drift to both taps, so the wobble pumped both
    // sides in lockstep and read as flat. The two channels must now differ.
    TapeRig r;
    r.fx.setDelayEffect(1);
    r.fx.setDelayTime(200.0f);
    r.fx.setDelayMix(1.0f);
    r.fx.setDelayFeedback(0.5f);
    r.fx.setDelayEngine(1);
    r.fx.setDelayTone(12000.0f);
    r.fx.setDelayWow(1.0f);

    const size_t N = 32768;
    std::vector<float> oL(N), oR(N);
    feed(r.fx, oL.data(), oR.data(), N, true);
    feed(r.fx, oL.data(), oR.data(), N, true);   // past the preset mute

    bool differs = false;
    for (size_t i = 0; i < N; ++i) if (oL[i] != oR[i]) { differs = true; break; }
    CHECK(differs);
    for (size_t i = 0; i < N; ++i) CHECK(std::fabs(oL[i]) < 2.0f);
}

TEST_CASE("W3: a short delay clamps the drift instead of folding the pointer")
{
    // Deep slow wow on a 40 ms slapback would modulate through zero without the
    // kDriftMaxFrac budget, folding the read pointer past the write head.
    TapeRig r;
    r.fx.setDelayEffect(0);
    r.fx.setDelayTime(40.0f);
    r.fx.setDelayMix(1.0f);
    r.fx.setDelayEngine(1);
    r.fx.setDelayWow(1.0f);
    r.fx.setDelayWowRate(0.2f);      // slowest rate = largest amplitude
    r.fx.setDelayFlutter(1.0f);

    float bL[128] = {0.0f}, bR[128] = {0.0f};
    r.fx.processBlock(bL, bR, 128);

    const float tapSamples = 40.0f * 0.001f * 44100.0f;
    CHECK(r.fx.debugWowAmpSamples() <= tapSamples * FxChain::kDriftMaxFrac + 1.0f);
}
