// =============================================================================
// test_pattern_presets.cpp — the generated pattern presets, proven on the engine
// =============================================================================
// The presets live in jt8000-params (params.yaml -> gen/PatternPresets.h) and
// are applied by the ESP32 / JUCE editors, never by the firmware.  These tests
// apply them HERE, through the same ParameterStore engineering-unit path the
// editors' NRPN lands on, and check the engine does what the YAML promised:
//
//   1. every preset applies cleanly and renders finite audio (both families);
//   2. the seq_gate INVERTED encoding really chops: rests are near-silent and
//      hits are open — the property the whole family rests on;
//   3. an arp rest really rests: no new note in a '.' step;
//   4. presets are complete states: applying B after A equals applying B alone.
//
// Header-only include of the generated table is test-only — the firmware build
// does not depend on it (see PatternPresets.h).
// =============================================================================
#include "doctest.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "core/SynthCore.h"
#include "core/ParameterStore.h"
#include "core/dsp/Curves.h"
#include "core/dsp/TempoClock.h"
#include "gen/ParamTable.h"
#include "../jt8000-params/gen/PatternPresets.h"

using namespace JT;
using namespace JT::Params;
namespace PP = JT::PatternPresets;

namespace {

struct Rig {
    ParameterStore     store;
    std::vector<float> comb, rev, fx;
    SynthCore          core;
    Rig()
        : comb(SynthCore::kCombPoolFloats, 0.0f),
          rev (SynthCore::kReverbPoolFloats, 0.0f),
          fx  (SynthCore::kFxPoolFloats, 0.0f),
          core(store, comb.data(), rev.data(), fx.data())
    {
        // A plain, steady test tone: one sine, instant attack, full sustain,
        // short release, so amplitude reads the gate directly.
        setE(ID::MIX_OSC2, 0.0f);
        setE(ID::MIX_SUB, 0.0f);
        setE(ID::MIX_NOISE, 0.0f);
        setOpt(ID::OSC1_WAVE, 0.0f);                    // SIN
        setE(ID::ENV_AMP_ATTACK, 0.0f);
        setE(ID::ENV_AMP_SUSTAIN, 1.0f);
        setE(ID::ENV_AMP_RELEASE, 5.0f);
        setE(ID::CLOCK_TEMPO, 120.0f);
    }
    void setE(uint16_t id, float eng) { store.setEngineering(id, eng, Origin::Ui); }
    void setOpt(uint16_t id, float o)
    { store.set(id, Curves::toNorm(kParams[ParameterStore::indexOf(id)], o), Origin::Ui); }

    // Apply a preset exactly as an editor would: engineering value per write.
    void apply(const PP::Preset& p)
    { for (uint16_t i = 0; i < p.count; ++i) setE(p.writes[i].id, p.writes[i].eng); }

    // Render n samples (rounded up to blocks), mono-summed into out.
    void render(std::vector<float>& out, size_t samples)
    {
        float L[kBlockSize], R[kBlockSize];
        while (out.size() < samples) {
            core.renderBlock(L, R, kBlockSize);
            for (size_t k = 0; k < kBlockSize; ++k) out.push_back(L[k] + R[k]);
        }
    }
};

float rms(const std::vector<float>& v, size_t from, size_t to)
{
    double acc = 0.0;
    for (size_t i = from; i < to && i < v.size(); ++i) acc += (double)v[i] * (double)v[i];
    return (to > from) ? (float)std::sqrt(acc / (double)(to - from)) : 0.0f;
}

const PP::Preset* find(PP::FamilyId f, const char* name)
{
    const PP::Family& fam = PP::family(f);
    for (uint8_t i = 0; i < fam.count; ++i)
        if (std::strcmp(fam.presets[i].name, name) == 0) return &fam.presets[i];
    return nullptr;
}

} // namespace

TEST_CASE("presets: every preset in every family applies and renders finite audio")
{
    for (uint8_t f = 0; f < (uint8_t)PP::FamilyId::Count; ++f) {
        const PP::Family& fam = PP::family((PP::FamilyId)f);
        REQUIRE(fam.count > 0);
        for (uint8_t i = 0; i < fam.count; ++i) {
            CAPTURE(fam.presets[i].name);
            Rig r;
            r.apply(fam.presets[i]);
            std::vector<float> out;
            r.render(out, kBlockSize);                 // params land
            r.core.noteOn(57, 100);
            r.core.noteOn(60, 100);                    // a dyad: exercises Chord mode
            r.render(out, 48000);
            bool finite = true;
            for (float x : out) finite = finite && std::isfinite(x);
            CHECK(finite);
            CHECK(rms(out, 0, out.size()) > 0.0f);     // something sounded
        }
    }
}

TEST_CASE("presets: seq_gate inverted encoding chops — rests closed, hits open")
{
    // "Gate 16ths" = "X.X. X.X. ..." at 1/32, 120 BPM: 62.5 ms per step.
    // Even steps are hits (step value 0 -> ampMul 1), odd steps rests
    // (step 1.0 x depth -1 -> ampMul 0).  Retrigger resets to step 0 on the
    // note, so step k occupies [k*stepLen, (k+1)*stepLen) after note-on.
    const PP::Preset* p = find(PP::FamilyId::SeqGate, "Gate 16ths");
    REQUIRE(p != nullptr);

    Rig r;
    r.apply(*p);
    std::vector<float> warm;
    r.render(warm, kBlockSize);
    REQUIRE(r.core.debugSeq().synced());
    const size_t stepLen = (size_t)std::lround(r.core.debugSeq().debugStepDurMs()
                                               * 0.001f * kSampleRate);
    CHECK(r.core.debugSeq().debugStepDurMs() == doctest::Approx(62.5f));

    r.core.noteOn(57, 100);
    std::vector<float> out;
    r.render(out, stepLen * 9);

    // Middle half of each step: clear of the per-block gain ramp at the edges
    // and of any block-quantisation of the step boundary.
    for (int k = 1; k < 8; ++k) {
        CAPTURE(k);
        const size_t a = (size_t)k * stepLen + stepLen / 4;
        const size_t b = (size_t)k * stepLen + (3 * stepLen) / 4;
        const float lvl = rms(out, a, b);
        if (k % 2 == 0) CHECK(lvl > 0.05f);            // hit: open
        else            CHECK(lvl < 1e-4f);            // rest: closed
    }
}

TEST_CASE("presets: an arp rest step starts no note")
{
    // "Offbeat 8" = "..X. ..X. ..." at 1/16, 120 BPM: 125 ms per step, gate
    // 0.6.  Steps 0/1 rest, step 2 hits.  The level in step 1 must be ~0 and
    // step 2 clearly sounding.
    const PP::Preset* p = find(PP::FamilyId::Arp, "Offbeat 8");
    REQUIRE(p != nullptr);

    Rig r;
    r.apply(*p);
    std::vector<float> warm;
    r.render(warm, kBlockSize);
    r.core.noteOn(57, 100);

    const size_t stepLen = (size_t)(0.125f * kSampleRate);
    std::vector<float> out;
    r.render(out, stepLen * 4);
    const float rest = rms(out, stepLen + stepLen / 4, stepLen + stepLen / 2);
    const float hit  = rms(out, 2 * stepLen + stepLen / 8, 2 * stepLen + stepLen / 2);
    CHECK(rest < 1e-4f);
    CHECK(hit  > 0.05f);
}

TEST_CASE("presets: a preset is a complete state — B after A equals B alone")
{
    // Tresillo sets swing 0.55 and gate 0.7; Straight 16 relies on the family
    // `fixed` block to put them back to 0.5.  If fixed ever stopped covering a
    // global, B-after-A would inherit A's value and this would diverge.
    const PP::Preset* a = find(PP::FamilyId::Arp, "Tresillo");
    const PP::Preset* b = find(PP::FamilyId::Arp, "Straight 16");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    Rig r1, r2;
    r1.apply(*a);
    r1.apply(*b);
    r2.apply(*b);
    for (size_t i = 0; i < kParamCount; ++i) {
        CAPTURE(kParams[i].key);
        CHECK(r1.store.getByIndex(i) == r2.store.getByIndex(i));
    }
}
