// =============================================================================
// MidiParamTransport.h — MIDI CC/NRPN -> ParameterStore bridge for JT-8000 v2
// =============================================================================
//
// ROLE IN THE ARCHITECTURE (design brief §5)
//   One instance per MIDI source (USB device, USB host, Serial1).  It
//   consumes Control Change bytes and turns the parameter-carrying ones into
//   ParameterStore writes tagged with THIS PORT's Origin — that tag is what
//   lets ParamBroadcast echo the change to every OTHER port and not back to
//   this one (Phase B' D1).  It deliberately handles ONLY
//   parameters:
//     * the NRPN cluster    (CC 99/98 select, CC 6/38 data, RPN deselect)
//     * curated performance CCs (74 cutoff, 71 resonance, ... from the table)
//   Everything else (sustain 64, channel-mode 120–127, mod wheel 1) returns
//   'false' = not-mine, so main.cpp routes them to the allocator / panic /
//   mod matrix.  That routing split is what guarantees a DAW panic really
//   panics instead of editing a parameter — the exact v1 failure mode.
//
// NRPN SEMANTICS IMPLEMENTED (brief §5.2)
//   CC99 msb + CC98 lsb  select parameter (NRPN number == ParamID)
//   CC6  data MSB        applies a 7-BIT COARSE value immediately
//   CC38 data LSB        refines the last CC6 to full 14-BIT
//   CC101/100 (RPN)      deselects NRPN (we implement no RPNs; their data
//                        entry is swallowed so it can't misroute)
//   NRPN 127/127         the standard "null" — deselects
//
//   A fine-resolution set therefore lands as coarse-then-fine, two store
//   writes ~1 ms apart.  That is how NRPN works on every hardware synth;
//   per-param smoothing absorbs the step.
//
// CPU
//   Curated CC lookup is a 128-entry direct table built once in the
//   constructor (one array read per CC) instead of scanning 140 ParamDescs
//   per event — negligible RAM (256 B), zero search cost.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>

#include "core/ParameterStore.h"

namespace JT {

class MidiParamTransport {
public:
    // Reserved NRPN number (never a ParamID: MSB 126 is above every section)
    // an editor sends — with any CC6 data byte — to request a full-state
    // push.  main.cpp polls consumeResyncRequest() and forwards to
    // ParamBroadcast::requestFullResync().  Phase B' D4.
    static constexpr uint16_t kNrpnResyncRequest = (126u << 7) | 0u; // 0x3F00

    // 'origin' identifies the port this instance serves (MidiUsbDev /
    // MidiUsbHost / MidiSerial) — every store write is tagged with it.
    MidiParamTransport(ParameterStore& store, Origin origin);

    // Feed every incoming Control Change here first.
    // Returns true  -> the event was parameter traffic and was consumed.
    // Returns false -> not parameter traffic; caller must route it
    //                  (sustain, channel mode, mod sources...).
    bool handleControlChange(uint8_t cc, uint8_t value);

    // A new MIDI connection or a Reset All Controllers should clear any
    // half-assembled NRPN selection so stale state can't misroute data.
    void resetState();

    // Diagnostics for the bring-up serial console (OFFLINE_TESTING.md S3):
    // how many parameter writes landed, and how many NRPN data bytes
    // arrived for an id this firmware doesn't have.
    uint32_t appliedCount() const { return _applied; }
    uint32_t unknownIdCount() const { return _unknownId; }

    // True exactly once per received resync request (read-and-clear).
    bool consumeResyncRequest()
    {
        const bool r = _resyncRequested;
        _resyncRequested = false;
        return r;
    }

private:
    enum class Selected : uint8_t { None, Nrpn, Rpn };

    void applyNrpn(float norm);

    ParameterStore& _store;
    const Origin    _origin;              // this port's identity (D1)
    bool            _resyncRequested = false;

    // --- NRPN assembly state (per MIDI source, hence per instance) ---
    Selected _selected  = Selected::None;
    uint8_t  _nrpnMsb   = 127;   // 127/127 == null selection
    uint8_t  _nrpnLsb   = 127;
    uint8_t  _dataMsb   = 0;
    bool     _dataMsbValid = false;   // CC38 without a prior CC6 is ignored

    // --- curated CC fast map: cc number -> dense param index, -1 = none ---
    int16_t  _ccToIndex[128];

    uint32_t _applied   = 0;
    uint32_t _unknownId = 0;
};

} // namespace JT
