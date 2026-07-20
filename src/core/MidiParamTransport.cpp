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

MidiParamTransport::MidiParamTransport(ParameterStore& store, Origin origin)
    : _store(store), _origin(origin)
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
    const uint16_t id = Params::idFromNrpn(_nrpnMsb, _nrpnLsb);

    // Reserved protocol id, not a parameter: an editor asking for the full
    // state (Phase B' D4).  The data byte is ignored; main.cpp polls the
    // flag.  Checked BEFORE the store so it can never count as unknown-id.
    if (id == kNrpnResyncRequest) {
        _resyncRequested = true;
        return;
    }

    if (!_store.set(id, norm, _origin)) {
        // Unknown ParamID: a controller mapped to newer firmware, or a
        // typo in a DAW template.  Count it for the bring-up console and
        // move on — one bad assignment must not disturb the rest.
        ++_unknownId;
        return;
    }
    ++_applied;
}

JT_COLD bool MidiParamTransport::handleControlChange(uint8_t cc, uint8_t value)
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
        _store.setByIndex((size_t)idx, Curves::normFrom7bit(value), _origin);
        ++_applied;
        return true;
    }

    return false;   // not parameter traffic — caller routes it
}

} // namespace JT
