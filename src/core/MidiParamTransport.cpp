// =============================================================================
// MidiParamTransport.cpp — implementation
// =============================================================================
// Protocol semantics and routing contract live in MidiParamTransport.h.
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "core/MidiParamTransport.h"

#include "core/AudioConfig.h"   // JT_COLD

#include "core/dsp/Curves.h"
#include "gen/ParamTable.h"

namespace JT {

MidiParamTransport::MidiParamTransport(ParameterStore& store, Origin origin,
                                       const PerfRouter* router)
    : _store(store), _router(router), _origin(origin)
{
    // Build the curated-CC direct map once.  The generator's validator
    // guarantees no parameter binds a reserved CC, so entries for 64,
    // 120–127 etc. stay -1 and those events fall through to the caller.
    for (int cc = 0; cc < 128; ++cc) _ccToIndex[cc] = -1;
    for (size_t i = 0; i < Params::kParamCount; ++i) {
        const int16_t cc = Params::kParams[i].cc;
        if (cc >= 0 && cc < 128) _ccToIndex[cc] = (int16_t)i;
    }
}

JT_COLD void MidiParamTransport::resetState()
{
    _selected     = Selected::None;
    _nrpnMsb      = 127;
    _nrpnLsb      = 127;
    _dataMsbValid = false;
}

JT_COLD void MidiParamTransport::applyNrpn(float norm)
{
    const uint16_t raw = Params::idFromNrpn(_nrpnMsb, _nrpnLsb);

    // Reserved protocol id, not a parameter: an editor asking for the full
    // state (Phase B' D4).  The data byte is ignored; main.cpp polls the
    // flag.  Checked BEFORE the store so it can never count as unknown-id.
    //
    // Tested on the RAW number, before the layer bit is stripped.  It has to
    // be: 126 << 7 is 0x3F00, which HAS bit 13 set, so masking first would
    // turn every resync request into a layer-B write to id 0x1F00.
    if (raw == kNrpnResyncRequest) {
        _resyncRequested = true;
        // The payload names the layer to dump.  Anything non-zero means B, so
        // an editor may send 1 or the full 14-bit 1 and both work.
        _resyncLayer = (norm > 0.0f) ? 1u : 0u;
        return;
    }

    // Layer travels in the address (header, LAYER ADDRESSING).  A shared or
    // performance-scope parameter written "as layer B" folds back onto its
    // single slot inside the store — see Params::slotFor — so an editor need
    // not know which parameters bank.
    const uint8_t  layer = (raw & kLayerBit) ? 1u : 0u;
    const uint16_t id    = raw & kIdMask;

    if (!_store.set(id, norm, _origin, layer)) {
        // Unknown ParamID: a controller mapped to newer firmware, or a
        // typo in a DAW template.  Count it for the bring-up console and
        // move on — one bad assignment must not disturb the rest.
        ++_unknownId;
        return;
    }
    ++_applied;
}

JT_COLD bool MidiParamTransport::handleControlChange(uint8_t cc, uint8_t value,
                                                     uint8_t channel1to16)
{
    value &= 0x7Fu;   // defensive: strip a stray status bit from bad cables

    switch (cc) {
        // --- parameter selection -------------------------------------------------
        case 99:   // NRPN MSB
            _selected     = Selected::Nrpn;
            _nrpnMsb      = value;
            _dataMsbValid = false;   // fresh selection, stale data invalid
            return true;

        case 98:   // NRPN LSB
            _selected     = Selected::Nrpn;
            _nrpnLsb      = value;
            _dataMsbValid = false;
            return true;

        case 101:  // RPN MSB — we implement no RPNs, but the selection must
        case 100:  // RPN LSB   still deselect NRPN so following CC6 data
                   //           can't land on a stale parameter.
            _selected     = Selected::Rpn;
            _dataMsbValid = false;
            return true;

        // --- data entry ----------------------------------------------------------
        case 6:    // Data Entry MSB
            if (_selected == Selected::Rpn) return true;   // swallow, see above
            if (_selected != Selected::Nrpn) return false; // stray: not ours
            if (_nrpnMsb == 127 && _nrpnLsb == 127) return true;  // null sel
            _dataMsb      = value;
            _dataMsbValid = true;
            // Coarse 7-bit apply, effective immediately — a knob mapped to
            // MSB-only NRPN (most controllers) works with no LSB at all.
            applyNrpn(Curves::normFrom7bit(value));
            return true;

        case 38:   // Data Entry LSB — refine the last MSB to 14 bits.
            if (_selected == Selected::Rpn) return true;
            if (_selected != Selected::Nrpn) return false;
            if (!_dataMsbValid) return true;   // LSB with no MSB: spec-noise
            applyNrpn(Curves::normFrom14bit(
                (uint16_t)(((uint16_t)_dataMsb << 7) | value)));
            return true;

        // --- data increment / decrement -------------------------------------------
        case 96:   // Data Increment  (RP-018: +1 on the 14-bit value)
        case 97: { // Data Decrement
            if (_selected != Selected::Nrpn) return _selected == Selected::Rpn;
            const uint16_t id  = Params::idFromNrpn(_nrpnMsb, _nrpnLsb);
            const size_t   idx = ParameterStore::indexOf(id);
            if (idx == ParameterStore::kInvalidIndex) { ++_unknownId; return true; }
            const uint16_t cur = Curves::normTo14bit(_store.getByIndex(idx));
            const uint16_t nxt =
                (cc == 96) ? (uint16_t)((cur < 16383u) ? cur + 1u : 16383u)
                           : (uint16_t)((cur > 0u)     ? cur - 1u : 0u);
            _store.setByIndex(idx, Curves::normFrom14bit(nxt), _origin);
            ++_applied;
            return true;
        }

        default:
            break;
    }

    // --- curated performance CCs (74 cutoff, 71 resonance, ...) ---------------
    const int16_t idx = _ccToIndex[cc & 0x7Fu];
    if (idx >= 0) {
        // A plain CC has nowhere to put an address bit, so its layer comes
        // from the receive channel.  The router is consulted HERE and not
        // earlier, so NRPN traffic — the overwhelming majority — never pays
        // for a routing decision it does not use.
        const float norm = Curves::normFrom7bit(value);
        const LayerMask dest = (_router != nullptr) ? _router->forChannel(channel1to16)
                                                    : LayerMask::A;

        // A CC on a channel that matches NEITHER layer is not ours: return
        // false so the caller can still give it a standard MIDI meaning
        // rather than having it silently vanish into a parameter write.
        if (dest == LayerMask::None) return false;

        // In Layer mode both layers commonly share one channel, so one CC
        // legitimately edits both.  Each write is a separate slot and a
        // separate dirty bit; for a SHARED parameter the two writes fold onto
        // the same slot, which costs one redundant store and no wrong state.
        if (has(dest, LayerMask::A)) _store.setByIndex((size_t)idx, norm, _origin, 0u);
        if (has(dest, LayerMask::B)) _store.setByIndex((size_t)idx, norm, _origin, 1u);
        ++_applied;
        return true;
    }

    return false;   // not parameter traffic — caller routes it
}

} // namespace JT
