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
    fx.setDelayTime(1.0f * 1500.0f); // norm 1.0 -> 1500 ms
    CHECK(fx.debugDelayMs() == doctest::Approx(1500.0f));
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
