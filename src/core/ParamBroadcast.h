// =============================================================================
// ParamBroadcast.h — ParameterStore -> NRPN-out mirror for JT-8000 v2
// =============================================================================
//
// ROLE IN THE ARCHITECTURE (Phase B' spec §3, §5)
//   The firmware is the single source of truth; every connected editor is a
//   live view of it.  This class makes that true in the outbound direction:
//   each parameter change accepted by the ParameterStore is re-emitted as an
//   NRPN message to every editor port EXCEPT the one that produced it.
//
//   Inbound (MidiParamTransport) and outbound (this class) deliberately use
//   the identical wire format and the identical Curves::norm<->14bit mapping,
//   so an editor's loopback (send a value, receive the echo) is bit-exact.
//
// WIRE FORMAT (4 CCs per parameter — mirror of MidiParamTransport in)
//   CC99 = ParamID >> 7     (NRPN MSB == section)
//   CC98 = ParamID & 0x7F   (NRPN LSB == index)
//   CC6  = value14 >> 7     (data MSB)
//   CC38 = value14 & 0x7F   (data LSB)
//   After a drain pass that sent anything, ONE RPN-null deselect
//   (CC101=127, CC100=127) per active sink — guards the receiver's data-entry
//   state without tripling per-parameter traffic (spec §5).
//
// ECHO SUPPRESSION (spec §3)
//   The store records the Origin of each parameter's last writer.  A sink
//   registered with that same Origin is skipped; all non-port origins (Ui,
//   PatchLoad, Sequencer, Init, SysEx) suppress nothing.  Coalescing is free:
//   the tx bitset holds at most one bit per parameter, and the value read at
//   drain time is always the latest.
//
// PACING (spec §5)
//   At most kMaxPerPass parameters per drain() call, bounding loop() latency.
//   A full resync (140 params) completes in ceil(140/8) = 18 passes — tens of
//   milliseconds of wall time, invisible next to the UART itself at 1 Mbaud.
//
// CPU CONTRACT ("do not calculate if not required")
//   Idle cost is one bitset scan of kDirtyWords words, all zero.  With no
//   sinks registered the bitset is still drained (bits must not accumulate
//   stale) but no messages are constructed.
//
// CONTEXT: control plane (loop()) only — never call from the audio ISR.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "core/ParameterStore.h"

namespace JT {

// Byte-pair sink one MIDI-out port implements.  Kept abstract so host tests
// register capture sinks and main.cpp registers thin adapters over
// usbMIDI / USBHost / Serial1 senders.  Virtual dispatch is fine here: this
// is the control plane, a handful of calls per changed parameter.
struct NrpnSink {
    virtual ~NrpnSink() = default;
    virtual void sendCC(uint8_t cc, uint8_t value) = 0;
};

class ParamBroadcast {
public:
    static constexpr size_t  kMaxSinks   = 3;   // USB dev / USB host / Serial1
    static constexpr size_t  kMaxPerPass = 8;   // params per drain() (spec §5)

    explicit ParamBroadcast(ParameterStore& store) : _store(store) {}

    // Register a port.  'origin' must be the same identity its inbound
    // MidiParamTransport tags writes with — that pairing IS the suppression.
    // Returns false when the sink table is full (programming error).
    bool addSink(NrpnSink& sink, Origin origin);

    // Emit the ENTIRE table to every sink, suppression ignored — the reply
    // to an editor's kNrpnResyncRequest, and idempotent for everyone else.
    // Paced like normal drains; ongoing dirty-bit traffic resumes after.
    // 'layer' is which layer's values the dump should carry — an editor
    // showing layer B asks for B.  Banked parameters are tagged with the
    // address bit so the editor can tell them apart; unbanked ones always
    // arrive as layer A, because there is only one of them.
    void requestFullResync(uint8_t layer = 0);

    // Call once per loop() pass, after store writes for the pass are done.
    void drain();

    // ---- Status feed (Phase F5) -------------------------------------------
    // Engine telemetry for the controller: one reserved NRPN address, sent
    // only when the packed word changes. NOT a parameter — no store ordinal,
    // no dirty bit, no echo suppression (there is no writer to suppress) —
    // so it bypasses the param bitset entirely. Sent SELF-CONTAINED (address
    // + data + RPN-null park, 6 CCs) because it is sporadic: it must never
    // depend on, nor disturb, whatever address a param drain left latched.
    static constexpr uint16_t kStatusAddr = 0x3FFF;  // reserved, never a ParamID
    void sendStatusIfChanged(uint16_t status14);

    // ARP status, on its OWN reserved address.  The sequencer word above is
    // already 13 of its 14 bits (voiceMask:8 | seqStep:4 | seqRunning:1), so
    // the arp playhead cannot be packed alongside it — hence a second address
    // rather than a wider word.  Layout:
    //
    //     [13..5] reserved 0   [4..1] arpStep:4   [0] arpRunning:1
    //
    // Without this the controller drew the SEQUENCER's playhead on the arp
    // lane, or none at all when the sequencer was stopped.
    static constexpr uint16_t kArpStatusAddr = 0x3FFE;  // reserved, never a ParamID
    void sendArpStatusIfChanged(uint16_t status14);

    // Heartbeat support: forget the last-sent words so the next
    // send*IfChanged() transmits even if nothing changed. The caller owns the
    // clock (core stays platform-free); main.cpp ticks this once a second so
    // the controller's LINK indicator means "wire alive", not merely
    // "something changed lately".
    void invalidateStatus() { _lastStatus = 0xFFFF; _lastArpStatus = 0xFFFF; }

    // Diagnostics for the 1 Hz status line.
    uint32_t sentCount() const { return _sent; }
    bool     resyncActive() const { return _resyncCursor < ParameterStore::kCount; }

private:
    void emitParam(size_t index, bool suppress, uint8_t layer = 0);
    void emitDeselect();                           // RPN null to sinks that sent
    // One SELF-CONTAINED reserved-address message: address + data + RPN-null
    // park, 6 CCs.  Shared by both status feeds so the wire format can only be
    // defined once.
    void emitReserved(uint16_t addr, uint16_t value14);

    ParameterStore& _store;

    NrpnSink* _sinks[kMaxSinks]  = {nullptr, nullptr, nullptr};
    Origin    _origins[kMaxSinks] = {Origin::Init, Origin::Init, Origin::Init};
    size_t    _sinkCount = 0;

    uint16_t  _lastStatus    = 0xFFFF;  // impossible value -> first word always sends
    uint16_t  _lastArpStatus = 0xFFFF;

    // kCount == "no resync in progress"; otherwise the next index to emit.
    size_t    _resyncCursor = ParameterStore::kCount;
    uint8_t   _resyncLayer  = 0;

    bool      _sinkTouched[kMaxSinks] = {false, false, false};  // per-pass
    uint32_t  _sent = 0;
};

} // namespace JT
