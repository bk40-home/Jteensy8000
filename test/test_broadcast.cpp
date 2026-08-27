// =============================================================================
// test_broadcast.cpp — ParamBroadcast host tests (Phase B' spec §9)
// =============================================================================
// Proves the outbound half of the live-view contract:
//   1. loopback round-trip is value-exact for every parameter
//   2. echo suppression (per-port) and non-port origins (broadcast to all)
//   3. coalescing: N writes between drains -> exactly one broadcast, latest
//   4. full resync: everyone gets everything, pacing respected, self-clears
//   5. publish()/tx interaction under the modelled preemption hook
// =============================================================================
#include "doctest.h"


#include "core/ParameterStore.h"
#include "core/ParamBroadcast.h"
#include "core/MidiParamTransport.h"
#include "core/dsp/Curves.h"
#include "gen/ParamTable.h"

using namespace JT;
using namespace JT::Params;

namespace {

// Capture sink: records the raw CC stream a real port would transmit.
// Fixed capacity (no heap).  The largest stream any test produces is a full
// resync: kCount*4 NRPN CCs plus a deselect pair per pass.  Sized from the
// table rather than a magic number, because it silently overflowed the moment
// the explicit step arrays pushed kCount past 256.
struct CaptureSink : NrpnSink {
    struct Msg { uint8_t cc, value; };
    static constexpr size_t kCap =
        Params::kParamCount * 4 + (Params::kParamCount / ParamBroadcast::kMaxPerPass + 2) * 2 + 64;
    Msg    log[kCap];
    size_t size = 0;
    void sendCC(uint8_t cc, uint8_t value) override {
        REQUIRE(size < kCap);                // overflow == test bug, fail loud
        log[size++] = {cc, value};
    }
    size_t paramCount() const {              // CC99 starts each param
        size_t n = 0;
        for (size_t i = 0; i < size; ++i) if (log[i].cc == 99) ++n;
        return n;
    }
    void clear() { size = 0; }
};

// Drain until the broadcaster goes quiet (bounded — a wedge fails the test).
void drainAll(ParamBroadcast& b, int maxPasses = 64) {
    for (int i = 0; i < maxPasses; ++i) b.drain();
}

} // namespace

// -----------------------------------------------------------------------------
TEST_CASE("broadcast: loopback round-trip is value-exact for all params") {
    ParameterStore txStore, rxStore;
    ParamBroadcast bc(txStore);
    CaptureSink    wire;
    REQUIRE(bc.addSink(wire, Origin::MidiSerial));

    // Editor-side receiver: the REAL inbound transport, fed the raw stream.
    MidiParamTransport rx(rxStore, Origin::MidiSerial);

    // Distinct, non-default value per param (skip 0/1 extremes on purpose;
    // toggles and selects quantise via the same 14-bit map on both sides).
    for (size_t i = 0; i < ParameterStore::kCount; ++i)
        txStore.setByIndex(i, 0.1f + 0.8f * (float)i / (float)ParameterStore::kCount,
                           Origin::Ui);                 // Ui: broadcast to all

    bc.requestFullResync();     // deterministic full sweep beats bit order
    txStore.clearTxDirty();     // the Ui writes above also set tx bits — the
                                // resync already covers them (dedup for test)
    drainAll(bc);

    for (size_t m = 0; m < wire.size; ++m)
        rx.handleControlChange(wire.log[m].cc, wire.log[m].value);

    for (size_t i = 0; i < ParameterStore::kCount; ++i) {
        // EXACT equality, deliberately not Approx: both directions share
        // Curves::normTo/From14bit, so the round trip must land on the
        // identical 14-bit lattice float — any epsilon would mask a drift
        // bug in one side's mapping, which is the whole point of this test.
        const float expect = Curves::normFrom14bit(
                                 Curves::normTo14bit(txStore.getByIndex(i)));
        CHECK(rxStore.getByIndex(i) == expect);
    }
    CHECK(rx.unknownIdCount() == 0);
}

// -----------------------------------------------------------------------------
TEST_CASE("broadcast: echo suppression per port; non-port origins reach all") {
    ParameterStore store;
    store.clearTxDirty();                    // discard construction bits
    ParamBroadcast bc(store);
    CaptureSink dev, host, ser;
    REQUIRE(bc.addSink(dev,  Origin::MidiUsbDev));
    REQUIRE(bc.addSink(host, Origin::MidiUsbHost));
    REQUIRE(bc.addSink(ser,  Origin::MidiSerial));

    SUBCASE("write from a port skips exactly that port") {
        store.set(ID::FILTER_CUTOFF, 0.42f, Origin::MidiSerial);
        drainAll(bc);
        CHECK(ser.paramCount()  == 0);       // never echo to the producer
        CHECK(dev.paramCount()  == 1);
        CHECK(host.paramCount() == 1);
    }
    SUBCASE("Ui / PatchLoad / Sequencer writes reach every port") {
        store.set(ID::FILTER_CUTOFF,    0.1f, Origin::Ui);
        store.set(ID::FILTER_RESONANCE, 0.2f, Origin::PatchLoad);
        store.set(ID::LFO1_FREQ,        0.3f, Origin::Sequencer);
        drainAll(bc);
        CHECK(dev.paramCount()  == 3);
        CHECK(host.paramCount() == 3);
        CHECK(ser.paramCount()  == 3);
    }
    SUBCASE("RPN-null deselect: once per pass per touched sink, trailing") {
        store.set(ID::FILTER_CUTOFF, 0.9f, Origin::Ui);
        bc.drain();                          // single pass
        REQUIRE(dev.size == 6);        // 4 NRPN CCs + CC101 + CC100
        CHECK(dev.log[4].cc == 101); CHECK(dev.log[4].value == 127);
        CHECK(dev.log[5].cc == 100); CHECK(dev.log[5].value == 127);
        dev.clear();
        bc.drain();                          // idle pass: nothing at all
        CHECK(dev.size == 0);
    }
}

// -----------------------------------------------------------------------------
TEST_CASE("broadcast: writes between drains coalesce to one latest-value send") {
    ParameterStore store;
    store.clearTxDirty();
    ParamBroadcast bc(store);
    CaptureSink dev;
    REQUIRE(bc.addSink(dev, Origin::MidiUsbDev));

    for (int k = 0; k < 25; ++k)             // a knob sweep between passes
        store.set(ID::FILTER_CUTOFF, (float)k / 25.0f, Origin::Ui);
    drainAll(bc);

    REQUIRE(dev.paramCount() == 1);          // one bit -> one broadcast
    const uint16_t v14 = (uint16_t)((dev.log[2].value << 7) | dev.log[3].value);
    CHECK(v14 == Curves::normTo14bit(store.get(ID::FILTER_CUTOFF)));  // latest
}

// -----------------------------------------------------------------------------
TEST_CASE("broadcast: full resync reaches the requester, paced, self-clearing") {
    ParameterStore store;
    store.clearTxDirty();
    ParamBroadcast bc(store);
    CaptureSink dev, ser;
    REQUIRE(bc.addSink(dev, Origin::MidiUsbDev));
    REQUIRE(bc.addSink(ser, Origin::MidiSerial));

    bc.requestFullResync();
    CHECK(bc.resyncActive());

    bc.drain();                              // pacing: exactly one pass
    CHECK(dev.paramCount() == ParamBroadcast::kMaxPerPass);

    drainAll(bc);
    CHECK(!bc.resyncActive());               // cursor self-cleared
    CHECK(dev.paramCount() == ParameterStore::kCount);   // everything…
    CHECK(ser.paramCount() == ParameterStore::kCount);   // …to everyone
}

// -----------------------------------------------------------------------------
TEST_CASE("broadcast: tx bits survive the modelled publish() preemption") {
    // The tx bitset is fed inside publish() between the documented preempt
    // points; prove a fake audio ISR firing there cannot lose or duplicate
    // broadcast traffic (spec §9 item 5).
    ParameterStore store;
    store.clearTxDirty();

    static ParameterStore* sp = &store;
    store.testPreemptHook = [](void*) {
        (void)sp->acquireSnapshot();         // ISR-shaped touch: snapshot +
        (void)sp->takeNextDirty();           // audio-side dirty pop
    };

    ParamBroadcast bc(store);
    CaptureSink dev;
    REQUIRE(bc.addSink(dev, Origin::MidiUsbDev));

    store.set(ID::FILTER_CUTOFF,    0.25f, Origin::Ui);
    store.set(ID::FILTER_RESONANCE, 0.75f, Origin::Ui);
    drainAll(bc);

    CHECK(dev.paramCount() == 2);            // both, exactly once each
    store.testPreemptHook = nullptr;
}

// ===========================================================================
// Reserved-address status feeds (review item 2a)
// ===========================================================================
// The sequencer status word already spends 13 of its 14 bits
// (voiceMask:8 | seqStep:4 | seqRunning:1), so the arp playhead could not be
// packed alongside it — it gets its OWN reserved address, 0x3FFE.  Before this
// the controller had no arp playhead at all and drew the SEQUENCER's on the
// arp lane, or none whenever the sequencer was stopped.
TEST_CASE("broadcast: arp status is a separate reserved address, change-only") {
    ParameterStore store;
    ParamBroadcast bc(store);
    CaptureSink sink;
    bc.addSink(sink, Origin::MidiUsbDev);

    // Each reserved message is SELF-CONTAINED: address + data + RPN-null park,
    // six CCs, because it is sporadic and must never depend on nor disturb
    // whatever address a parameter drain last latched.
    sink.size = 0;
    bc.sendArpStatusIfChanged(0x001F);
    REQUIRE(sink.size == 6);
    CHECK(sink.log[0].cc == 99);
    CHECK(sink.log[0].value == (uint8_t)((ParamBroadcast::kArpStatusAddr >> 7) & 0x7F));
    CHECK(sink.log[1].cc == 98);
    CHECK(sink.log[1].value == (uint8_t)(ParamBroadcast::kArpStatusAddr & 0x7F));
    CHECK(sink.log[4].cc == 101);
    CHECK(sink.log[5].cc == 100);

    // Change-only: an identical word costs one compare and sends nothing.
    sink.size = 0;
    bc.sendArpStatusIfChanged(0x001F);
    CHECK(sink.size == 0);

    // The two feeds are independent — one must not suppress the other.
    sink.size = 0;
    bc.sendStatusIfChanged(0x001F);
    CHECK(sink.size == 6);
    CHECK(sink.log[1].value == (uint8_t)(ParamBroadcast::kStatusAddr & 0x7F));

    // The heartbeat clears BOTH last-sent words together, so a 1 Hz beat
    // re-proves the wire for the arp lane as well as the sequencer's.
    bc.invalidateStatus();
    sink.size = 0;
    bc.sendArpStatusIfChanged(0x001F);
    CHECK(sink.size == 6);

    // Neither reserved address may ever collide with a real ParamID.
    CHECK(ParameterStore::indexOf(ParamBroadcast::kArpStatusAddr)
          == ParameterStore::kInvalidIndex);
    CHECK(ParameterStore::indexOf(ParamBroadcast::kStatusAddr)
          == ParameterStore::kInvalidIndex);
}
