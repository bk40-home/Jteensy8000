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
// LAYER ADDRESSING (Performance mode)
//   The two kinds of traffic this class handles are addressed by two
//   DIFFERENT mechanisms, on purpose:
//
//     NRPN         layer travels in the ADDRESS, as bit 13 of the ParamID
//                  (kLayerBit).  Sections run 0..17, so bits 13..11 of a
//                  14-bit ParamID are permanently free.  The receive channel
//                  is IGNORED on this path.
//     curated CC   a plain CC has no spare address bits, so the layer comes
//                  from the RECEIVE CHANNEL via PerfRouter.
//
//   Why not use the channel for both?  Because the ordinary Layer setup gives
//   both layers the SAME channel so one keyboard plays both — at which point
//   channel-addressed edits could no longer name a layer at all.  The address
//   bit is immune to that, costs nothing on the wire, and keeps every NRPN
//   send self-contained at 6 CCs.
//
// CPU
//   Curated CC lookup is a 128-entry direct table built once in the
//   constructor (one array read per CC) instead of scanning 140 ParamDescs
//   per event — negligible RAM (256 B), zero search cost.  The router is
//   consulted ONLY on the curated-CC branch, i.e. after the direct table has
//   already confirmed this CC is a parameter; NRPN traffic never pays for it.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>

#include "core/ParameterStore.h"
#include "core/PerfRouter.h"

namespace JT {

class MidiParamTransport {
public:
    // Reserved NRPN number (never a ParamID: MSB 126 is above every section)
    // an editor sends — with any CC6 data byte — to request a full-state
    // push.  main.cpp polls consumeResyncRequest() and forwards to
    // ParamBroadcast::requestFullResync().  Phase B' D4.
    static constexpr uint16_t kNrpnResyncRequest = (126u << 7) | 0u; // 0x3F00

    // Bit 13 of a received NRPN number means "this edit targets layer B".
    // ParamIDs are (section << 7) | index with sections 0..17, so this bit is
    // free by construction — and an editor that never sets it addresses layer
    // A, the correct default, with no special case anywhere.
    static constexpr uint16_t kLayerBit = 0x2000;
    static constexpr uint16_t kIdMask   = 0x1FFF;   // the id with layer removed

    // 'origin' identifies the port this instance serves (MidiUsbDev /
    // MidiUsbHost / MidiSerial) — every store write is tagged with it.
    //
    // 'router' resolves a receive channel to a layer for curated CCs.  It is
    // optional: a null router means "everything is layer A", which is exactly
    // the pre-Performance behaviour and keeps single-layer callers and unit
    // tests free of a dependency they do not need.
    MidiParamTransport(ParameterStore& store, Origin origin,
                       const PerfRouter* router = nullptr);

    // Feed every incoming Control Change here first.
    // Returns true  -> the event was parameter traffic and was consumed.
    // Returns false -> not parameter traffic; caller must route it
    //                  (sustain, channel mode, mod sources...).
    //
    // 'channel1to16' is used ONLY by the curated-CC branch (see the layer
    // note above); the NRPN cluster deliberately ignores it, so an editor may
    // keep sending its cluster on whatever channel it likes.  It defaults to
    // 1 so existing single-layer call sites read unchanged.
    bool handleControlChange(uint8_t cc, uint8_t value, uint8_t channel1to16 = 1);

    // A new MIDI connection or a Reset All Controllers should clear any
    // half-assembled NRPN selection so stale state can't misroute data.
    void resetState();

    // Diagnostics for the bring-up serial console (OFFLINE_TESTING.md S3):
    // how many parameter writes landed, and how many NRPN data bytes
    // arrived for an id this firmware doesn't have.
    uint32_t appliedCount() const { return _applied; }
    uint32_t unknownIdCount() const { return _unknownId; }

    // Which layer the last resync request asked for.  The reserved resync
    // address carries no parameter, so its 14-bit DATA payload was previously
    // ignored — it now names the layer, which costs nothing on the wire and
    // avoids a second reserved address.  0 for any editor that sends 0, i.e.
    // every existing one.
    uint8_t resyncLayer() const { return _resyncLayer; }

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

    ParameterStore&   _store;
    const PerfRouter* _router;            // null = single-layer (layer A only)
    const Origin      _origin;            // this port's identity (D1)
    bool            _resyncRequested = false;
    uint8_t         _resyncLayer     = 0;

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
