// =============================================================================
// test_wire_centre.cpp — 7-bit centre detent for symmetric bipolar params
// =============================================================================
// Sign-off 1a (2026-10-25): wire code 64 — sent alone (MSB) or as the plugin's
// 14-bit expansion 64·129 = 8256 — lands on EXACT centre (norm 0.5 = eng 0)
// for Continuous + Lin params with min == -max, and nowhere else.  Found by
// the AI AI AI build-up: OSC DETUNE + FREQ DC + FINE at code 64 added +29 c.
// =============================================================================
#include "doctest.h"

#include "core/MidiParamTransport.h"
#include "core/ParameterStore.h"
#include "core/dsp/Curves.h"
#include "gen/ParamTable.h"

using namespace JT;
using namespace JT::Params;

namespace {
void selectNrpn(MidiParamTransport& t, uint16_t id)
{
    CHECK(t.handleControlChange(99, nrpnMsb(id)));
    CHECK(t.handleControlChange(98, nrpnLsb(id)));
}
float eng(ParameterStore& s, uint16_t id)
{
    return Curves::toEngineering(kParams[ParameterStore::indexOf(id)], s.get(id));
}
} // namespace

TEST_CASE("wire centre: code 64 is exact zero on the bipolar pitch params (7-bit and 14-bit)")
{
    const uint16_t ids[] = { ID::OSC1_DETUNE, ID::OSC1_FREQ_DC, ID::OSC1_FINE_TUNE,
                             ID::OSC2_DETUNE, ID::OSC2_FREQ_DC, ID::OSC2_FINE_TUNE,
                             ID::MIX_BALANCE, ID::ENV_PITCH_DEPTH };
    for (uint16_t id : ids) {
        INFO("param ", kParams[ParameterStore::indexOf(id)].key);
        ParameterStore store;
        MidiParamTransport t(store, Origin::MidiUsbDev);
        selectNrpn(t, id);
        CHECK(t.handleControlChange(6, 64));                 // MSB only
        CHECK(store.get(id) == 0.5f);
        CHECK(eng(store, id) == 0.0f);
        CHECK(t.handleControlChange(38, (uint8_t)((64u * 129u) & 0x7Fu)));   // plugin's 64·129
        CHECK(store.get(id) == 0.5f);
        CHECK(eng(store, id) == 0.0f);
    }
}

TEST_CASE("wire centre: neighbours, unipolar, log and select params are untouched")
{
    ParameterStore store;
    MidiParamTransport t(store, Origin::MidiUsbDev);

    selectNrpn(t, ID::OSC1_DETUNE);
    CHECK(t.handleControlChange(6, 63));
    CHECK(store.get(ID::OSC1_DETUNE) == doctest::Approx(63.0f / 127.0f));
    CHECK(t.handleControlChange(6, 65));
    CHECK(store.get(ID::OSC1_DETUNE) == doctest::Approx(65.0f / 127.0f));

    // Unipolar Lin (0..1), Log (Hz) and Select keep the plain 64/127.
    const uint16_t others[] = { ID::LFO1_PITCH_DEPTH, ID::FILTER_CUTOFF, ID::LFO1_FREQ };
    for (uint16_t id : others) {
        INFO("param ", kParams[ParameterStore::indexOf(id)].key);
        selectNrpn(t, id);
        CHECK(t.handleControlChange(6, 64));
        CHECK(store.get(id) == doctest::Approx(64.0f / 127.0f));
    }
}

TEST_CASE("wire centre: Curves::snapWireCentre applies only to Continuous+Lin+symmetric")
{
    int snapped = 0;
    for (size_t i = 0; i < kParamCount; ++i) {
        const ParamDesc& d = kParams[i];
        const float out = Curves::snapWireCentre(d, 64.0f / 127.0f);
        const bool sym = d.type == Type::Continuous && d.curve == Curve::Lin && d.min == -d.max;
        INFO("param ", d.key);
        CHECK((out == 0.5f) == sym);
        snapped += sym ? 1 : 0;
    }
    CHECK(snapped == 14);   // the list in the 2026-10-25 delivery note
}
