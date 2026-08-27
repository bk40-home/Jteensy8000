// =============================================================================
// ParamBroadcast.cpp — implementation
// =============================================================================
// Wire format, suppression and pacing rules live in ParamBroadcast.h.
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "core/ParamBroadcast.h"

#include "core/AudioConfig.h"   // JT_COLD
#include "core/dsp/Curves.h"
#include "gen/ParamTable.h"

namespace JT {

JT_COLD bool ParamBroadcast::addSink(NrpnSink& sink, Origin origin)
{
    if (_sinkCount >= kMaxSinks) return false;
    _sinks[_sinkCount]   = &sink;
    _origins[_sinkCount] = origin;
    ++_sinkCount;
    return true;
}

JT_COLD void ParamBroadcast::requestFullResync(uint8_t layer)
{
    _resyncLayer = (layer != 0u) ? 1u : 0u;
    // Restart from the top even if a resync is already running — the caller
    // (a just-connected editor) wants everything, and re-sends are idempotent.
    _resyncCursor = 0;
}

// -----------------------------------------------------------------------------
// One parameter to every eligible sink.  'suppress' distinguishes normal
// dirty-bit traffic (skip the last writer's port) from resync (send to all).
// -----------------------------------------------------------------------------
JT_COLD void ParamBroadcast::emitParam(size_t index, bool suppress, uint8_t layer)
{
    // Layer travels in the ADDRESS, bit 13 — the same encoding the transport
    // decodes on the way in, so an editor sees its own edits mirrored back with
    // the layer it sent them for.  Shared and performance parameters are never
    // banked, so they are always emitted as layer A whatever is asked for:
    // tagging them would invite an editor to keep two copies of one value.
    const bool banked = Params::isBanked(index);
    const uint8_t lay = (banked && layer != 0u) ? 1u : 0u;

    const uint16_t id  = (uint16_t)(Params::kParams[index].id |
                                    (lay ? 0x2000u : 0x0000u));
    const uint16_t v14 = Curves::normTo14bit(_store.getByIndex(index, lay));
    const Origin   who = _store.originByIndex(index, lay);

    // Bytes are identical for every sink — build once, send per sink.
    const uint8_t idMsb = (uint8_t)((id >> 7) & 0x7Fu);
    const uint8_t idLsb = (uint8_t)( id       & 0x7Fu);
    const uint8_t dMsb  = (uint8_t)((v14 >> 7) & 0x7Fu);
    const uint8_t dLsb  = (uint8_t)( v14       & 0x7Fu);

    for (size_t s = 0; s < _sinkCount; ++s) {
        if (suppress && _origins[s] == who) continue;   // never echo back
        NrpnSink& out = *_sinks[s];
        out.sendCC(99, idMsb);   // NRPN select…
        out.sendCC(98, idLsb);
        out.sendCC( 6, dMsb);    // …coarse, effective immediately at receiver
        out.sendCC(38, dLsb);    // …then refined to full 14-bit
        _sinkTouched[s] = true;
        ++_sent;
    }
}

// One RPN-null deselect per sink that transmitted this pass — parks the
// receiver's data-entry state so a later stray CC6 can't land on our last
// selection (spec §5: once per pass, not per parameter).
JT_COLD void ParamBroadcast::emitDeselect()
{
    for (size_t s = 0; s < _sinkCount; ++s) {
        if (!_sinkTouched[s]) continue;
        _sinks[s]->sendCC(101, 127);
        _sinks[s]->sendCC(100, 127);
        _sinkTouched[s] = false;
    }
}

// One reserved-address message, self-contained (address + data + park).  Both
// status feeds go through here so the 6-CC form is written down exactly once.
void ParamBroadcast::emitReserved(uint16_t addr, uint16_t value14)
{
    for (size_t s = 0; s < _sinkCount; ++s) {
        if (!_sinks[s]) continue;
        NrpnSink& out = *_sinks[s];
        out.sendCC(99, static_cast<uint8_t>((addr >> 7) & 0x7F));
        out.sendCC(98, static_cast<uint8_t>(addr & 0x7F));
        out.sendCC(6,  static_cast<uint8_t>((value14 >> 7) & 0x7F));
        out.sendCC(38, static_cast<uint8_t>(value14 & 0x7F));
        // Self-contained park — see the header note.
        out.sendCC(101, 127);
        out.sendCC(100, 127);
    }
    ++_sent;
}

void ParamBroadcast::sendStatusIfChanged(uint16_t status14)
{
    if (status14 == _lastStatus) return;   // idle cost: one compare
    _lastStatus = status14;
    emitReserved(kStatusAddr, status14);
}

void ParamBroadcast::sendArpStatusIfChanged(uint16_t status14)
{
    if (status14 == _lastArpStatus) return;   // idle cost: one compare
    _lastArpStatus = status14;
    emitReserved(kArpStatusAddr, status14);
}

JT_COLD void ParamBroadcast::drain()
{
    size_t budget = kMaxPerPass;

    // Resync first: it supersedes ordinary traffic (any dirty bits set
    // meanwhile stay set and drain on later passes with the newest value).
    while (budget > 0 && _resyncCursor < ParameterStore::kCount) {
        emitParam(_resyncCursor, /*suppress=*/false, _resyncLayer);
        ++_resyncCursor;
        --budget;
    }

    // Ordinary changed-parameter traffic.  The bitset is drained even with
    // zero sinks so bits never accumulate stale (emitParam then loops over
    // an empty sink table — no message construction, per the CPU contract).
    while (budget > 0) {
        const size_t slot = _store.takeNextTxDirty();
        if (slot == ParameterStore::kInvalidIndex) break;   // clean — idle path
        emitParam(Params::paramOfSlot(slot), /*suppress=*/true,
                  Params::layerOfSlot(slot));
        --budget;
    }

    emitDeselect();
}

} // namespace JT
