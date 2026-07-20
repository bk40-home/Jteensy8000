// =============================================================================
// test_midi_transport.cpp — proofs for core/MidiParamTransport
// =============================================================================
#include "doctest.h"

#include "core/MidiParamTransport.h"
#include "core/dsp/Curves.h"
#include "gen/ParamTable.h"

using namespace JT;
using namespace JT::Params;

namespace {
// Send a full NRPN selection for a ParamID.
void selectNrpn(MidiParamTransport& t, uint16_t id)
{
    CHECK(t.handleControlChange(99, nrpnMsb(id)));
    CHECK(t.handleControlChange(98, nrpnLsb(id)));
}
} // namespace

TEST_CASE("NRPN: coarse (MSB-only) and fine (MSB+LSB) both land correctly")
{
    ParameterStore store;
    MidiParamTransport t(store, Origin::MidiUsbDev);

    selectNrpn(t, ID::FILTER_CUTOFF);

    // Coarse: CC6 alone = 7-bit value, applied immediately.
    CHECK(t.handleControlChange(6, 100));
    CHECK(store.get(ID::FILTER_CUTOFF) == doctest::Approx(100.0f / 127.0f));
    CHECK(store.origin(ID::FILTER_CUTOFF) == Origin::MidiUsbDev);

    // Fine: CC38 refines the last CC6 to 14 bits.
    CHECK(t.handleControlChange(38, 45));
    const uint16_t v14 = (uint16_t)((100u << 7) | 45u);
    CHECK(store.get(ID::FILTER_CUTOFF)
          == doctest::Approx(Curves::normFrom14bit(v14)));
    CHECK(t.appliedCount() == 2);
}

TEST_CASE("NRPN: selection persists — many data entries per selection")
{
    // Standard controller behaviour: select once, stream CC6 as the knob
    // turns.  Every value must land without reselection.
    ParameterStore store;
    MidiParamTransport t(store, Origin::MidiUsbDev);
    selectNrpn(t, ID::ENV_AMP_ATTACK);
    for (uint8_t v = 0; v <= 120; v += 10)
        CHECK(t.handleControlChange(6, v));
    CHECK(store.get(ID::ENV_AMP_ATTACK) == doctest::Approx(120.0f / 127.0f));
}

TEST_CASE("NRPN: data entry without selection / after null is inert")
{
    ParameterStore store;
    MidiParamTransport t(store, Origin::MidiUsbDev);
    const float before = store.get(ID::FILTER_CUTOFF);

    // No selection at all: CC6 is not ours (could be someone's legacy map).
    CHECK_FALSE(t.handleControlChange(6, 42));
    // LSB without a prior MSB: consumed but ignored (spec noise).
    selectNrpn(t, ID::FILTER_CUTOFF);
    CHECK(t.handleControlChange(38, 42));
    CHECK(store.get(ID::FILTER_CUTOFF) == doctest::Approx(before));

    // Null selection 127/127 parks the machine: data must not land.
    CHECK(t.handleControlChange(99, 127));
    CHECK(t.handleControlChange(98, 127));
    CHECK(t.handleControlChange(6, 99));
    CHECK(store.get(ID::FILTER_CUTOFF) == doctest::Approx(before));
    CHECK(t.appliedCount() == 0);
}

TEST_CASE("RPN traffic deselects NRPN and swallows its own data")
{
    // A DAW emitting pitch-bend-range RPN (101/100/6) right after our NRPN
    // must neither corrupt the parameter nor leak CC6 to the caller.
    ParameterStore store;
    MidiParamTransport t(store, Origin::MidiUsbDev);
    selectNrpn(t, ID::FILTER_CUTOFF);
    CHECK(t.handleControlChange(6, 64));                 // ours
    const float ours = store.get(ID::FILTER_CUTOFF);

    CHECK(t.handleControlChange(101, 0));                // RPN 0 selected
    CHECK(t.handleControlChange(100, 0));
    CHECK(t.handleControlChange(6, 12));                 // RPN data: swallowed
    CHECK(store.get(ID::FILTER_CUTOFF) == doctest::Approx(ours));
    CHECK(t.appliedCount() == 1);
}

TEST_CASE("NRPN increment / decrement nudge the 14-bit value by one")
{
    ParameterStore store;
    MidiParamTransport t(store, Origin::MidiUsbDev);
    selectNrpn(t, ID::FILTER_CUTOFF);
    t.handleControlChange(6, 64);
    const uint16_t before = Curves::normTo14bit(store.get(ID::FILTER_CUTOFF));

    CHECK(t.handleControlChange(96, 0));                 // increment
    CHECK(Curves::normTo14bit(store.get(ID::FILTER_CUTOFF)) == before + 1);
    CHECK(t.handleControlChange(97, 0));                 // decrement
    CHECK(t.handleControlChange(97, 0));
    CHECK(Curves::normTo14bit(store.get(ID::FILTER_CUTOFF)) == before - 1);
}

TEST_CASE("unknown NRPN id: counted, harmless, neighbours untouched")
{
    ParameterStore store;
    MidiParamTransport t(store, Origin::MidiUsbDev);
    CHECK(t.handleControlChange(99, 0x7F));              // id 0x3F80: unused
    CHECK(t.handleControlChange(98, 0x00));
    CHECK(t.handleControlChange(6, 77));
    CHECK(t.unknownIdCount() == 1);
    CHECK(t.appliedCount() == 0);
}

TEST_CASE("curated CCs land; reserved and unbound CCs fall through")
{
    ParameterStore store;
    MidiParamTransport t(store, Origin::MidiUsbDev);

    // CC 74 -> cutoff, CC 71 -> resonance (the signed-off performance set).
    CHECK(t.handleControlChange(74, 127));
    CHECK(store.get(ID::FILTER_CUTOFF) == doctest::Approx(1.0f));
    CHECK(t.handleControlChange(71, 64));
    CHECK(store.get(ID::FILTER_RESONANCE) == doctest::Approx(64.0f / 127.0f));

    // Sustain, mod wheel, channel mode: NOT parameters — the caller must
    // receive these to route to allocator / mod matrix / panic.
    CHECK_FALSE(t.handleControlChange(64, 127));
    CHECK_FALSE(t.handleControlChange(1, 64));
    CHECK_FALSE(t.handleControlChange(123, 0));
    CHECK_FALSE(t.handleControlChange(120, 0));
}

TEST_CASE("resetState clears a half-assembled selection")
{
    ParameterStore store;
    MidiParamTransport t(store, Origin::MidiUsbDev);
    selectNrpn(t, ID::FILTER_CUTOFF);
    t.resetState();                                      // e.g. cable replug
    // After reset there is NO selection, so CC6 is not parameter traffic:
    // it must fall through to the caller, and nothing may be applied.
    CHECK_FALSE(t.handleControlChange(6, 50));
    CHECK(t.appliedCount() == 0);
}
