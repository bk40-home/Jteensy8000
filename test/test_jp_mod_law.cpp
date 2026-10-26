// =============================================================================
// test_jp_mod_law.cpp — the JP-8000 modulation-depth curve (JpModLaw)
// =============================================================================
// Pins the measured JE-8086 points (2026-10-24 capture) and the unit
// conventions the engine relies on (sign-off 4b / 5b, 2026-10-25).
// =============================================================================
#include "doctest.h"

#include "core/dsp/JpModLaw.h"

using namespace JT;

TEST_CASE("JpModLaw: LFO1 pitch follows the measured JE-8086 points")
{
    // JE cents at JP depth 16 / 32 / 48 / 63 (normalised d/63).
    struct { float d, cents; } pts[] = { {16, 187.33f}, {32, 721.21f}, {48, 1491.05f}, {63, 2380.44f} };
    for (auto p : pts) {
        INFO("depth ", p.d);
        CHECK(JpModLaw::lfoPitchSemis(p.d / 63.0f) * 100.0f == doctest::Approx(p.cents).epsilon(0.03));
    }
    CHECK(JpModLaw::lfoPitchSemis(0.0f) == 0.0f);
}

TEST_CASE("JpModLaw: pitch env is twice the LFO curve; sign is kept")
{
    // JE: env +16 / +32 / +63 = +377 / +1445 / +4768 c.
    CHECK(JpModLaw::envPitchSemis(16.0f / 63.0f) * 100.0f == doctest::Approx(377.0f).epsilon(0.03));
    CHECK(JpModLaw::envPitchSemis(32.0f / 63.0f) * 100.0f == doctest::Approx(1445.0f).epsilon(0.02));
    CHECK(JpModLaw::envPitchSemis(1.0f) * 100.0f == doctest::Approx(4768.0f).epsilon(0.01));
    CHECK(JpModLaw::envPitchSemis(-0.5f) == doctest::Approx(-JpModLaw::envPitchSemis(0.5f)));
    CHECK(JpModLaw::lfoPitchSemis(-0.5f) == doctest::Approx(-JpModLaw::lfoPitchSemis(0.5f)));
}

TEST_CASE("JpModLaw: env into X-MOD adds D/18 knob steps, LFO1 D/16.25")
{
    // JE: env +32 adds 39-41 knob steps, +63 adds ~133 (clamps at 127);
    // LFO1 +32 peaks at 42.8-46.0 (clamped-triangle fit).  Normalised: /127.
    const float lfo32 = JpModLaw::lfoPitchSemis(32.0f / 63.0f) * JpModLaw::kXmodNormPerLfoSemi * 127.0f;
    const float env32 = JpModLaw::envPitchSemis(32.0f / 63.0f) * JpModLaw::kXmodNormPerEnvSemi * 127.0f;
    const float env63 = JpModLaw::envPitchSemis(1.0f)          * JpModLaw::kXmodNormPerEnvSemi * 127.0f;
    CHECK(lfo32 == doctest::Approx(44.1f).epsilon(0.05));
    CHECK(env32 == doctest::Approx(40.0f).epsilon(0.05));
    CHECK(env63 == doctest::Approx(132.0f).epsilon(0.02));
}
