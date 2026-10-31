// =============================================================================
// test_jp_env.cpp — JP envelope time law (voice.env_law = JpEnv, sign-off 1a)
// =============================================================================
// Targets are the JE-8086 measurements (fm1-port scene 18 / 19 analyses):
//   * one tau(byte) table; decay & release exponential; attack charges toward
//     1.25 and stops at full (0.79 of full at t = tau, full at 1.61 tau);
//   * LFO1 fade = the attack generator on the depth;
//   * through SynthCore, an amp decay at byte 72 falls with tau = 0.424 s.
// =============================================================================
#include "doctest.h"

#include <cmath>
#include <vector>

#include "core/SynthCore.h"
#include "core/dsp/Curves.h"
#include "core/dsp/EnvGen.h"
#include "core/dsp/JpEnvLaw.h"
#include "core/dsp/Lfo.h"
#include "gen/ParamTable.h"

using namespace JT;
using namespace JT::Params;

namespace {
constexpr float kBlockS = kBlockMs * 0.001f;
float nb(float byte) { return byte / 127.0f; }
}

TEST_CASE("JpEnvLaw: measured knots and the JP's own steps")
{
    CHECK(JpEnvLaw::tauFromNorm(nb(12))  == doctest::Approx(0.0147f).epsilon(0.001));
    CHECK(JpEnvLaw::tauFromNorm(nb(32))  == doctest::Approx(0.0578f).epsilon(0.001));
    CHECK(JpEnvLaw::tauFromNorm(nb(72))  == doctest::Approx(0.424f).epsilon(0.001));
    CHECK(JpEnvLaw::tauFromNorm(nb(112)) == doctest::Approx(2.61f).epsilon(0.001));
    CHECK(JpEnvLaw::tauFromNorm(nb(116)) == doctest::Approx(2.98f).epsilon(0.001));
    CHECK(JpEnvLaw::tauFromNorm(nb(117)) == doctest::Approx(7.85f).epsilon(0.001));  // the step
    CHECK(JpEnvLaw::tauFromNorm(nb(118)) == doctest::Approx(7.85f).epsilon(0.001));  // pair
    CHECK(JpEnvLaw::tauFromNorm(nb(120)) == doctest::Approx(8.68f).epsilon(0.001));
    CHECK(JpEnvLaw::tauFromNorm(nb(127)) == doctest::Approx(15.1f).epsilon(0.001));
    float prev = 0.0f;                                   // never decreasing
    for (int b = 0; b <= 127; ++b) {
        const float t = JpEnvLaw::tauFromNorm(nb((float)b));
        CHECK(t >= prev);
        prev = t;
    }
}

TEST_CASE("EnvGen JpEnv: decay and release are exponential with the table tau")
{
    EnvGen e;
    e.setJpLaw(true);
    e.setJpAttackNorm(0.0f);                             // instant
    e.setJpDecayNorm(nb(72));                            // tau 0.424 s
    e.setJpReleaseNorm(nb(56));                          // tau 0.233 s
    e.setSustain(0.0f);
    e.noteOn();
    while (e.tickBlock() < 1.0f) {}                      // attack lands on exactly 1.0
    const int n = (int)(0.424f / kBlockS);               // ~1 tau
    float lv = 1.0f;
    for (int i = 0; i < n; ++i) lv = e.tickBlock();
    CHECK(lv == doctest::Approx(std::exp(-(float)n * kBlockS / 0.424f)).epsilon(0.01));

    EnvGen r;
    r.setJpLaw(true);
    r.setJpAttackNorm(0.0f);
    r.setJpReleaseNorm(nb(56));
    r.setSustain(1.0f);
    r.noteOn();
    for (int i = 0; i < 4; ++i) r.tickBlock();           // at sustain 1.0
    r.noteOff();
    const int m = (int)(0.233f / kBlockS);
    for (int i = 0; i < m; ++i) lv = r.tickBlock();
    CHECK(lv == doctest::Approx(std::exp(-(float)m * kBlockS / 0.233f)).epsilon(0.01));
    int guard = 0;                                       // ends below -60 dB
    while (r.isActive() && guard++ < 100000) r.tickBlock();
    CHECK_FALSE(r.isActive());
    CHECK(r.level() == 0.0f);
}

TEST_CASE("EnvGen JpEnv: attack charges toward 1.25 and stops at full")
{
    EnvGen e;
    e.setJpLaw(true);
    e.setJpAttackNorm(nb(104));                          // tau 1.73 s
    e.setSustain(1.0f);
    e.noteOn();
    const float tau = 1.73f;
    const int atTau = (int)(tau / kBlockS);
    float lv = 0.0f;
    for (int i = 0; i < atTau; ++i) lv = e.tickBlock();
    CHECK(lv == doctest::Approx(1.25f * (1.0f - std::exp(-(float)atTau * kBlockS / tau))).epsilon(0.01));
    int blocks = atTau;
    while (lv < 1.0f) { lv = e.tickBlock(); ++blocks; }
    CHECK((float)blocks * kBlockS == doctest::Approx(tau * std::log(5.0f)).epsilon(0.01));  // 1.61 tau
    CHECK(lv == 1.0f);
}

TEST_CASE("Lfo JP fade: the attack generator on the depth gain")
{
    Lfo l(1u);
    l.setWave(1);                                        // TRI
    l.setRateHz(5.0f);
    l.setJpFade(true);
    const float tau = JpEnvLaw::tauFromNorm(nb(104));
    l.setJpFadeCoef(JpEnvLaw::blockCoef(tau));
    l.retrigger();
    const int atTau = (int)(tau / kBlockS);
    for (int i = 0; i < atTau; ++i) l.tickBlock();
    CHECK(l.delayGain() == doctest::Approx(1.25f * (1.0f - std::exp(-(float)atTau * kBlockS / tau))).epsilon(0.01));
    for (int i = 0; i < atTau; ++i) l.tickBlock();       // 2 tau > 1.61 tau
    CHECK(l.delayGain() == 1.0f);
}

TEST_CASE("SynthCore env_law = JpEnv: amp decay at byte 72 falls with tau 0.424 s")
{
    ParameterStore     store;
    std::vector<float> pool((size_t)SynthCore::kCombPoolFloats, 0.0f);
    SynthCore          core(store, pool.data());
    auto setOpt = [&](uint16_t id, float o)
    { store.set(id, Curves::toNorm(kParams[ParameterStore::indexOf(id)], o), Origin::Ui); };
    store.setEngineering(ID::MIX_OSC2, 0.0f, Origin::Ui);
    setOpt(ID::OSC1_WAVE, (float)(int)Wave::Sine);
    setOpt(ID::VOICE_ENV_LAW, 1.0f);                     // JpEnv
    store.set(ID::ENV_AMP_ATTACK, nb(0), Origin::Ui);    // knob positions, as a JP import sends
    store.set(ID::ENV_AMP_DECAY,  nb(72), Origin::Ui);
    store.setEngineering(ID::ENV_AMP_SUSTAIN, 0.0f, Origin::Ui);
    float L[kBlockSize], R[kBlockSize];
    for (int i = 0; i < 20; ++i) core.renderBlock(L, R, kBlockSize);
    core.noteOn(69, 100);
    // Block RMS (dB) -> straight-line fit over the first ~20 dB.
    std::vector<float> db;
    for (int b = 0; b < (int)(0.6f / kBlockS); ++b) {
        core.renderBlock(L, R, kBlockSize);
        double acc = 0.0;
        for (size_t k = 0; k < kBlockSize; ++k) acc += (double)L[k] * (double)L[k];
        db.push_back(10.0f * std::log10((float)(acc / (double)kBlockSize) + 1e-20f));
    }
    size_t pk = 0;
    for (size_t i = 1; i < db.size(); ++i) if (db[i] > db[pk]) pk = i;
    double sx = 0, sy = 0, sxx = 0, sxy = 0; int cnt = 0;
    for (size_t i = pk + 4; i < db.size() && db[pk] - db[i] < 20.0f; ++i) {
        const double x = (double)(i - pk) * (double)kBlockS, y = db[i];
        sx += x; sy += y; sxx += x * x; sxy += x * y; ++cnt;
    }
    REQUIRE(cnt > 10);
    const double slope = (cnt * sxy - sx * sy) / (cnt * sxx - sx * sx);   // dB per s
    const float tau = (float)(-20.0 / std::log(10.0) / slope);
    CHECK(tau == doctest::Approx(0.424f).epsilon(0.05));
}
