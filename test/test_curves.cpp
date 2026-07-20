// =============================================================================
// test_curves.cpp — proofs for core/dsp/Curves
// =============================================================================
// These tests replace v1's ad-hoc Python simulations (bucket-midpoint
// round-trip checks) with permanent, CI-run proofs.  Where a property must
// hold for EVERY parameter, the test iterates the whole generated table —
// adding a parameter to params.yaml automatically extends the proof.
// =============================================================================
#include "doctest.h"

#include <cmath>

#include "core/dsp/Curves.h"
#include "gen/ParamTable.h"

using namespace JT;
using namespace JT::Params;

// Relative tolerance for float round trips through expf/logf.  1e-4 relative
// is far tighter than 14-bit wire resolution (1/16383 ≈ 6.1e-5 of full scale
// in the NORMALIZED domain; log-domain engineering values are what we bound
// here), so passing this guarantees conversions never audibly drift.
static bool closeRel(float a, float b, float rel = 1e-4f)
{
    const float scale = std::fmax(std::fabs(a), std::fabs(b));
    return std::fabs(a - b) <= rel * std::fmax(scale, 1e-6f);
}

TEST_CASE("every parameter: norm -> engineering -> norm round trip")
{
    // 33 evenly spaced probe points covers both segment halves of Seg2 and
    // the extremes where clamping bugs live.
    for (size_t i = 0; i < kParamCount; ++i) {
        const ParamDesc& d = kParams[i];
        for (int s = 0; s <= 32; ++s) {
            const float t   = (float)s / 32.0f;
            const float eng = Curves::toEngineering(d, t);
            const float t2  = Curves::toNorm(d, eng);
            CAPTURE(d.key); CAPTURE(t); CAPTURE(eng);
            CHECK(closeRel(t2, t, 2e-4f));
        }
    }
}

TEST_CASE("every parameter: endpoints and defaults are exact and in range")
{
    for (size_t i = 0; i < kParamCount; ++i) {
        const ParamDesc& d = kParams[i];
        CAPTURE(d.key);

        // Endpoints must map exactly — the engine relies on t=0/t=1 hitting
        // min/max so a fully-open filter really is 20 kHz, not 19.97.
        CHECK(closeRel(Curves::toEngineering(d, 0.0f), d.min, 1e-5f));
        CHECK(closeRel(Curves::toEngineering(d, 1.0f), d.max, 1e-5f));

        // The table default converts to a valid normalized value.
        const float nd = Curves::toNorm(d, d.def);
        CHECK(nd >= 0.0f);
        CHECK(nd <= 1.0f);
        CHECK(closeRel(Curves::toEngineering(d, nd), d.def, 2e-4f));
    }
}

TEST_CASE("every parameter: curve is monotonically non-decreasing")
{
    // A non-monotonic curve would make a knob reverse direction mid-travel.
    for (size_t i = 0; i < kParamCount; ++i) {
        const ParamDesc& d = kParams[i];
        float prev = Curves::toEngineering(d, 0.0f);
        for (int s = 1; s <= 64; ++s) {
            const float cur = Curves::toEngineering(d, (float)s / 64.0f);
            CAPTURE(d.key); CAPTURE(s);
            CHECK(cur >= prev - 1e-6f);
            prev = cur;
        }
    }
}

TEST_CASE("Seg2 hits 'mid' exactly at centre — v1 envelope-slope parity")
{
    // v1 mapped CC 64 -> slope 1.0 (linear envelopes).  v2's contract is
    // t = 0.5 -> mid.  Verify on the real envelope-slope params.
    const ParamDesc* d = find(ID::ENV_AMP_ATTACK_CURVE);
    REQUIRE(d != nullptr);
    CHECK(d->curve == Curve::Seg2);
    CHECK(Curves::toEngineering(*d, 0.5f) == doctest::Approx(1.0f));
    CHECK(Curves::toEngineering(*d, 0.0f) == doctest::Approx(0.15f));
    CHECK(Curves::toEngineering(*d, 1.0f) == doctest::Approx(5.0f));
    CHECK(Curves::toNorm(*d, 1.0f)        == doctest::Approx(0.5f));
}

TEST_CASE("known engineering values — cutoff and envelope time curves")
{
    // Spot-check the log curves against v1 Mapping.h maths.
    const ParamDesc* cut = find(ID::FILTER_CUTOFF);
    REQUIRE(cut != nullptr);
    CHECK(Curves::toEngineering(*cut, 0.0f) == doctest::Approx(20.0f));
    CHECK(Curves::toEngineering(*cut, 1.0f) == doctest::Approx(20000.0f));
    // Geometric midpoint of a log curve: sqrt(20 * 20000) ≈ 632.5 Hz.
    CHECK(Curves::toEngineering(*cut, 0.5f) == doctest::Approx(632.455f).epsilon(0.001));

    const ParamDesc* rel = find(ID::ENV_AMP_RELEASE);
    REQUIRE(rel != nullptr);
    // v1 default: CC 20 -> 1 * 11880^(20/127) ≈ 4.38 ms — the seed converted
    // it through the same curve, so the table default must reproduce it.
    CHECK(rel->def == doctest::Approx(4.38f).epsilon(0.01));
}

TEST_CASE("select params: option index round trip is exact for every option")
{
    // The property v1's bucket-midpoint formula needed a Python simulation
    // to establish — trivially true in v2, but keep the proof forever.
    for (size_t i = 0; i < kParamCount; ++i) {
        const ParamDesc& d = kParams[i];
        if (d.type != Type::Select) continue;
        for (int idx = 0; idx < (int)d.optionCount; ++idx) {
            const float n = Curves::normFromOptionIndex(d, idx);
            CAPTURE(d.key); CAPTURE(idx);
            CHECK(Curves::toOptionIndex(d, n) == idx);
            // ...and it must survive 7-bit CC quantisation too, because a
            // curated CC might one day carry a select param.
            CHECK(Curves::toOptionIndex(d, Curves::normFrom7bit(Curves::normTo7bit(n))) == idx);
        }
    }
}

TEST_CASE("wire quantisation: toXbit(fromXbit(v)) identity over ALL codes")
{
    for (unsigned v = 0; v <= 16383u; ++v)
        REQUIRE(Curves::normTo14bit(Curves::normFrom14bit((uint16_t)v)) == v);
    for (unsigned v = 0; v <= 127u; ++v)
        REQUIRE(Curves::normTo7bit(Curves::normFrom7bit((uint8_t)v)) == v);
}

TEST_CASE("garbage in, silence out: NaN and out-of-range inputs are contained")
{
    const ParamDesc* cut = find(ID::FILTER_CUTOFF);
    REQUIRE(cut != nullptr);
    const float nan = std::nanf("");

    // A corrupt transport packet must clamp, never propagate NaN.
    CHECK(Curves::clamp01(nan) == 0.0f);
    CHECK(Curves::toEngineering(*cut, nan)   == doctest::Approx(20.0f));
    CHECK(Curves::toEngineering(*cut, -5.0f) == doctest::Approx(20.0f));
    CHECK(Curves::toEngineering(*cut,  5.0f) == doctest::Approx(20000.0f));
    CHECK(Curves::toNorm(*cut, nan) == 0.0f);
    CHECK(std::isfinite(Curves::toNorm(*cut, 1e30f)));
}
