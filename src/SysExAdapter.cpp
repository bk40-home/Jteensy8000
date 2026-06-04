// =============================================================================
// SysExAdapter.cpp — JT-8000 editor SysEx receive path (Phase 1)
//
// See SysExAdapter.h for architecture overview and SyxProtocol.h for the wire
// format.
//
// © 2025 Kris Bishop — MIT licensed.
// =============================================================================

#include "SysExAdapter.h"
#include "SyxProtocol.h"
#include "ParamMap.h"
#include "CCCache.h"
#include "LayerManager.h"
#include "ParamDefs.h"
#include "Mapping.h"        // Hi-res inverse converters (cutoff_hz_to_cc, time_ms_to_cc, etc.)
#include "DebugTrace.h"  // JT_LOGF — same logging path the rest of the firmware uses
#include <Arduino.h>     // AudioNoInterrupts / AudioInterrupts

// =============================================================================
// Helpers — keep them static so they don't pollute global symbol space.
// =============================================================================

namespace {

// Variation count for each FX Mod / FX Delay slot. These match the Phase 0
// param-map "enum-N" rows for FX_MOD_EFFECT (11 variations) and
// FX_DELAY_EFFECT (5 variations). Centralised here so they don't drift.
static constexpr uint8_t kModVariationCount   = 11;
static constexpr uint8_t kDelayVariationCount = 5;

// Clamp a float to a CC byte 0..127 with explicit rounding. Inline because it
// gets called once per SET_PARAM and we want zero overhead.
inline uint8_t clampToCC(float v) {
    if (v <= 0.0f)   return 0;
    if (v >= 127.0f) return 127;
    return (uint8_t)(v + 0.5f);   // round-to-nearest
}

} // namespace

// =============================================================================
// Construction
// =============================================================================
SysExAdapter::SysExAdapter(LayerManager& lm) : _lm(lm) {}

// =============================================================================
// handleSysEx — top-level entry point. Validates envelope, dispatches by msg.
//
// Returns true if the message was for us (regardless of whether dispatch
// succeeded). Returns false only if it's a different manufacturer's SysEx.
// =============================================================================
bool SysExAdapter::handleSysEx(const uint8_t* data, size_t len) {
    // Diagnostic log — fires once per inbound SysEx, safe at UI rate.
    JT_LOGF("[SyxAdpt] handleSysEx len=%u\n", (unsigned)len);
    // Minimum size for any valid JT-8000 message = GET_PARAM (10 bytes).
    if (len < SyxProto::kMsgGetParamLen) return false;

    // Envelope must be: F0 7D 4A 54 ...
    if (data[0]                        != 0xF0)              return false;
    if (data[SyxProto::kOffsetMfrId]   != SyxProto::kMfrId)  return false;
    if (data[SyxProto::kOffsetSubIdJ]  != SyxProto::kSubIdJ) return false;
    if (data[SyxProto::kOffsetSubIdT]  != SyxProto::kSubIdT) return false;

    // Device ID — accept default and broadcast.
    const uint8_t devId = data[SyxProto::kOffsetDeviceId];
    if (devId != SyxProto::kDeviceDefault && devId != SyxProto::kDeviceBroadcast) {
        // Addressed to a different device. Not malformed — just not us.
        return true;   // returning true means "we saw it, leave it alone"
    }

    // Last byte must be F7.
    if (data[len - 1] != 0xF7) {
        JT_LOGF("[SyxAdpt] missing F7 (len=%u)\n", (unsigned)len);
        return true;
    }

    const uint8_t msg = data[SyxProto::kOffsetMsgType];

    switch (msg) {
        case SyxProto::kMsgSetParam: {
            if (len != SyxProto::kMsgSetParamLen) {
                JT_LOGF("[SyxAdpt] SET_PARAM bad len %u (want %u)\n",
                        (unsigned)len, (unsigned)SyxProto::kMsgSetParamLen);
                return true;
            }
            const uint8_t  layer  = data[SyxProto::kOffsetLayer];
            const uint16_t pid    = SyxProto::paramIdFromBytes(
                                        data[SyxProto::kOffsetPidHi],
                                        data[SyxProto::kOffsetPidLo]);
            const float    value  = SyxProto::decodeFloat(&data[SyxProto::kOffsetFloat]);
            _handleSetParam(layer, pid, value);
            return true;
        }

        case SyxProto::kMsgGetParam: {
            if (len != SyxProto::kMsgGetParamLen) {
                JT_LOGF("[SyxAdpt] GET_PARAM bad len %u (want %u)\n",
                        (unsigned)len, (unsigned)SyxProto::kMsgGetParamLen);
                return true;
            }
            const uint8_t  layer = data[SyxProto::kOffsetLayer];
            const uint16_t pid   = SyxProto::paramIdFromBytes(
                                       data[SyxProto::kOffsetPidHi],
                                       data[SyxProto::kOffsetPidLo]);
            _handleGetParam(layer, pid);
            return true;
        }

        case SyxProto::kMsgParamValue:
            // PARAM_VALUE is firmware -> editor only. We never receive it.
            // Drop silently — could be an editor bug or a loopback test.
            return true;

        case SyxProto::kMsgBankDumpRequest:
            // Editor wants a snapshot of the live state. No payload to parse.
            // Length should be exactly kMsgBankDumpReqLen (header + msg + F7).
            if (len != SyxProto::kMsgBankDumpReqLen) {
                JT_LOGF("[SyxAdpt] BANK_DUMP_REQUEST bad len %u\n", (unsigned)len);
                return true;
            }
            _handleBankDumpRequest();
            return true;

        case SyxProto::kMsgBankDump:
            // Editor pushed us a bank dump — apply every entry under one
            // AudioNoInterrupts block to avoid mid-load audio glitches.
            _handleIncomingBankDump(data, len);
            return true;

        case SyxProto::kMsgExtCC: {
            // Extended CC from ESP32 hardware controller — carries CC
            // numbers above the MIDI 0..127 range (envelope curves 147-155,
            // performance params 140-146) inside SysEx so they survive the
            // 7-bit MIDI wire limitation.
            //
            // Layout: F0 7D 4A 54 00 20 <ch> <cc_hi> <cc_lo> <val> F7
            //         0  1  2  3  4  5   6    7       8       9     10
            if (len != SyxProto::kMsgExtCCLen) {
                JT_LOGF("[SyxAdpt] EXT_CC bad len %u (want %u)\n",
                        (unsigned)len, (unsigned)SyxProto::kMsgExtCCLen);
                return true;
            }
            const uint8_t ch  = data[6];
            // Reconstruct full CC number from 7-bit hi/lo split
            const uint8_t cc  = (uint8_t)(((data[7] & 0x7F) << 7) | (data[8] & 0x7F));
            const uint8_t val = data[9];
            JT_LOGF("[SyxAdpt] EXT_CC ch=%u cc=%u val=%u\n",
                    (unsigned)ch, (unsigned)cc, (unsigned)val);
            // Route through the same path as standard MIDI CC — LayerManager
            // already has case statements for perf CCs (140-146) and
            // SynthEngine handles envelope curves (147-155).
            _lm.handleControlChange(ch, cc, val);
            return true;
        }

        default:
            JT_LOGF("[SyxAdpt] unknown msg type 0x%02X\n", (unsigned)msg);
            return true;
    }
}

// =============================================================================
// _handleSetParam — dispatch a single SET_PARAM message
// =============================================================================
void SysExAdapter::_handleSetParam(uint8_t layer, uint16_t paramId, float value) {
    // Diagnostic — one line per SET_PARAM, safe at UI rate.
    JT_LOGF("[SyxAdpt] SET_PARAM layer=0x%02X pid=0x%04X val=%.4f\n",
            (unsigned)layer, (unsigned)paramId, (double)value);
    // Validate layer ID up front — saves a lookup if it's bogus.
    const bool engineLayer = (layer == SyxProto::kLayerA   ||
                              layer == SyxProto::kLayerB   ||
                              layer == SyxProto::kLayerBoth);
    const bool managerLayer = (layer == SyxProto::kLayerPerf ||
                               layer == SyxProto::kLayerGlobalFx);
    if (!engineLayer && !managerLayer) {
        JT_LOGF("[SyxAdpt] SET_PARAM bad layer 0x%02X\n", (unsigned)layer);
        return;
    }

    // SysEx-only abstraction params are routed before the table lookup —
    // their entries exist (with kSysExOnly scope) but the adapter handles
    // them entirely on its own.
    //
    // Two SysEx-only ranges:
    //   0x0C80..0x0C87  — FX Mod/Delay abstractions (global; layer byte ignored)
    //   0x0504..0x0506  — Amp envelope curve exponents
    //   0x0604..0x0606  — Filter envelope curve exponents
    //   0x0705..0x0707  — Pitch envelope curve exponents
    const bool isSyxOnly =
        (paramId >= 0x0C80 && paramId <= 0x0C87) ||
        (paramId >= 0x0504 && paramId <= 0x0506) ||
        (paramId >= 0x0604 && paramId <= 0x0606) ||
        (paramId >= 0x0705 && paramId <= 0x0707);

    if (isSyxOnly) {
        _handleSyxOnlySet(paramId, value, layer);
        return;
    }

    const ParamMap::Entry* e = ParamMap::find(paramId);
    if (!e) {
        JT_LOGF("[SyxAdpt] unknown ParamID 0x%04X\n", (unsigned)paramId);
        return;
    }

    // Convert SysEx float -> CC byte.
    //
    // Hi-res params arrive as native units (Hz, ms, 0-1) from the JUCE plugin's
    // ccByteToNativeFloat. We convert back to a CC byte using the same curves
    // the firmware's CC handlers use (Mapping.h). Non-hi-res params arrive as
    // CC bytes cast to float — just clamp.
    uint8_t ccValue;
    switch (paramId) {
        case 0x0400: // Filter Cutoff → Hz
            ccValue = JT8000Map::cutoff_hz_to_cc(value);
            break;
        case 0x0401: // Filter Resonance → 0..0.97 (OBXa) or 0..1 (VA)
            // Use OBXa curve as default — matches JUCE side assumption.
            ccValue = JT8000Map::obxa_res01_to_cc(value);
            break;
        case 0x0500: // Amp Attack → ms
        case 0x0501: // Amp Decay → ms
        case 0x0503: // Amp Release → ms
        case 0x0600: // Filter Env Attack → ms
        case 0x0601: // Filter Env Decay → ms
        case 0x0603: // Filter Env Release → ms
        case 0x0700: // Pitch Env Attack → ms
        case 0x0701: // Pitch Env Decay → ms
        case 0x0703: // Pitch Env Release → ms
            ccValue = JT8000Map::time_ms_to_cc(value);
            break;
        case 0x0800: // LFO1 Frequency → Hz
        case 0x0900: // LFO2 Frequency → Hz
            ccValue = JT8000Map::lfo_hz_to_cc(value);
            break;
        default:
            // Non-hi-res: value is a CC byte as float.
            ccValue = _floatToCC(e->type, value);
            break;
    }
    JT_LOGF("[SyxAdpt] dispatch pid=0x%04X scope=%u ccAlias=%u ccValue=%u\n",
            (unsigned)paramId, (unsigned)e->scope, (unsigned)e->ccAlias, (unsigned)ccValue);

    switch (e->scope) {
        case ParamMap::kPatch:
            // Engine-scoped param. Apply to the requested layer(s).
            if (engineLayer) {
                _applyEngineCC(layer, e->ccAlias, ccValue);
            } else {
                JT_LOGF("[SyxAdpt] Patch param 0x%04X needs engine layer "
                        "(got 0x%02X)\n", (unsigned)paramId, (unsigned)layer);
            }
            break;

        case ParamMap::kPerf:
            // Performance scope is global — silently accept any layer; the
            // perf-CC switch in LayerManager handles it locally.
            _applyManagerCC(e->ccAlias, ccValue);
            break;

        case ParamMap::kGlobalFx:
            // Global FX scope is global — same as Perf.
            _applyManagerCC(e->ccAlias, ccValue);
            break;

        case ParamMap::kSysExOnly:
            // Already handled above. Defensive — should never reach here.
            break;
    }
}

// =============================================================================
// _handleGetParam — query current value, send a PARAM_VALUE reply
// =============================================================================
// Reads the per-layer / per-scope CC cache held by LayerManager, converts the
// CC byte back to a SysEx float using the param's value type, and emits a
// PARAM_VALUE reply.
//
// Returns silently (no reply) for unknown ParamIDs or unset cache slots —
// the editor times out on no-reply rather than getting bogus data.
// =============================================================================
void SysExAdapter::_handleGetParam(uint8_t layer, uint16_t paramId) {
    float value;
    if (_readParamValue(layer, paramId, value)) {
        _sendParamValue(layer, paramId, value);
    } else {
        JT_LOGF("[SyxAdpt] GET_PARAM 0x%04X layer=0x%02X — no cached value\n",
                (unsigned)paramId, (unsigned)layer);
    }
}

// =============================================================================
// _readParamValue — pull the current value for (layer, paramId)
//
// Looks up the param in ParamMap, then reads from the appropriate cache. The
// cache lives in LayerManager; we use its public getCachedCC() accessor.
// SysEx-only abstractions are read directly from the adapter's local state.
// Returns false for unknown ParamIDs or unset cache slots.
// =============================================================================
bool SysExAdapter::_readParamValue(uint8_t layer, uint16_t paramId, float& outValue) const {
    // SysEx-only abstraction params are global; ignore layer.
    // Matches the same ranges checked in _handleSetParam above.
    const bool isSyxOnly =
        (paramId >= 0x0C80 && paramId <= 0x0C87) ||
        (paramId >= 0x0504 && paramId <= 0x0506) ||
        (paramId >= 0x0604 && paramId <= 0x0606) ||
        (paramId >= 0x0705 && paramId <= 0x0707);

    if (isSyxOnly) {
        outValue = _handleSyxOnlyGet(paramId);
        return true;
    }

    const ParamMap::Entry* e = ParamMap::find(paramId);
    if (!e) return false;
    if (e->scope == ParamMap::kSysExOnly) return false;  // shouldn't reach

    // Read the cache. For Patch scope we honour the layer byte; for Perf and
    // GlobalFX the scope is global so the layer byte is ignored.
    uint8_t cc = e->ccAlias;
    uint8_t cv = CCCache::kUnset;

    switch (e->scope) {
        case ParamMap::kPatch: {
            // For kLayerBoth, prefer Layer A (it's what the editor will display
            // when both are equal). If they're not equal, the editor should
            // request each layer separately.
            uint8_t l = (layer == SyxProto::kLayerBoth) ? SyxProto::kLayerA : layer;
            cv = _lm.getCachedCC(l, cc);
        } break;
        case ParamMap::kPerf:
            cv = _lm.getCachedCC(SyxProto::kLayerPerf, cc);
            break;
        case ParamMap::kGlobalFx:
            cv = _lm.getCachedCC(SyxProto::kLayerGlobalFx, cc);
            break;
        default:
            return false;
    }

    if (cv == CCCache::kUnset) return false;
    outValue = _ccToFloat(e->type, cv);
    return true;
}

// =============================================================================
// _ccToFloat — inverse of _floatToCC. Maps a CC byte 0..127 into the SysEx
// float native form, by value type.
//
// For most types this is the literal inverse. For continuous params (kCont)
// the editor receives the raw CC value as float — Phase 1's design choice
// was to keep continuous params on the CC quantised path until Phase 3 adds
// hi-res direct setters.
// =============================================================================
// _ccToFloat — convert a CC byte to the SysEx float sent to the editor.
//
// CURRENT STATE: The JUCE plugin expects raw CC bytes as floats for all
// non-hi-res params (nativeFloatToCCByte default path rounds the float to
// an integer CC byte). So we just cast and return.
//
// The type-specific normalised decoding (bipolar → -1..+1, norm01 → 0..1)
// was designed for a future protocol. Enabling it requires matching changes
// in the JUCE plugin's nativeFloatToCCByte. Until then, all types pass through.
//
// Exception: hi-res params have dedicated ccToXxx converters on the JUCE side
// (ccByteToNativeFloat), so their PARAM_VALUE echoes carry native units
// (Hz, ms, etc.) — those params never hit _ccToFloat.
// =============================================================================
float SysExAdapter::_ccToFloat(uint8_t /*valueType*/, uint8_t ccValue) const {
    return (float)ccValue;
}

// =============================================================================
// _sendParamValue — build and emit a PARAM_VALUE message (used by GET_PARAM
// replies in Phase 2, and for any unsolicited push from the firmware later)
// =============================================================================
void SysExAdapter::_sendParamValue(uint8_t layer, uint16_t paramId, float value) {
    if (!_send) return;

    uint8_t buf[SyxProto::kMsgParamValueLen];
    buf[0]                          = 0xF0;
    buf[SyxProto::kOffsetMfrId]     = SyxProto::kMfrId;
    buf[SyxProto::kOffsetSubIdJ]    = SyxProto::kSubIdJ;
    buf[SyxProto::kOffsetSubIdT]    = SyxProto::kSubIdT;
    buf[SyxProto::kOffsetDeviceId]  = SyxProto::kDeviceDefault;
    buf[SyxProto::kOffsetMsgType]   = SyxProto::kMsgParamValue;
    buf[SyxProto::kOffsetLayer]     = layer;
    buf[SyxProto::kOffsetPidHi]     = SyxProto::paramIdHi(paramId);
    buf[SyxProto::kOffsetPidLo]     = SyxProto::paramIdLo(paramId);
    SyxProto::encodeFloat(value, &buf[SyxProto::kOffsetFloat]);
    buf[SyxProto::kMsgParamValueLen - 1] = 0xF7;

    _send(buf, sizeof(buf));
}

// =============================================================================
// notifyLocalCC — echo a local CC change to the editor as PARAM_VALUE
//
// Called from onCCHandled() in Jteensy8000.cpp when a TFT/encoder/preset-load
// CC change should be reflected back to the JUCE plugin.
//
// Flow: CC number → ParamMap::findByCC → _ccToFloat → _sendParamValue.
// Safe at UI rate. No-ops if the CC has no ParamID mapping (e.g. internal
// CCs above the map range) or if no sender is registered.
// =============================================================================
void SysExAdapter::notifyLocalCC(uint8_t layer, uint8_t cc, uint8_t val) {
    if (!_send) return;

    const ParamMap::Entry* e = ParamMap::findByCC(cc);
    if (!e) return;  // CC not in the map — nothing to echo

    const float nativeVal = _ccToFloat(e->type, val);
    _sendParamValue(layer, e->paramId, nativeVal);
}

// =============================================================================
// _floatToCC — convert SysEx float to a CC byte 0..127, by value type
//
// CURRENT STATE: The JUCE plugin sends CC byte values as floats for all
// non-hi-res params (ccByteToNativeFloat default path returns (float)ccValue).
// So the incoming float IS already a CC byte — just clamp and return.
//
// The type-specific normalised encoding (bipolar -1..+1, norm01 0..1, etc.)
// was designed for a future protocol where the editor sends true native values.
// That path is not active — enabling it requires matching changes in the JUCE
// plugin's ccByteToNativeFloat. Until then, all types use clampToCC.
//
// Exception: hi-res params bypass _floatToCC entirely (Phase 3 fast path in
// _handleSetParam), so they are unaffected by this simplification.
// =============================================================================
uint8_t SysExAdapter::_floatToCC(uint8_t /*valueType*/, float value) const {
    return clampToCC(value);
}

// =============================================================================
// _applyEngineCC — patch-scope dispatch with explicit layer
//
// Bypasses the channel filter (LayerManager::handleControlChange would only
// accept CCs on the matching MIDI channel; our SysEx routing is explicit).
//
// Cache update: because we're bypassing LayerManager's main dispatch path,
// we have to update the cache here directly. Otherwise SysEx writes wouldn't
// show up in subsequent BANK_DUMP / GET_PARAM reads.
// =============================================================================
void SysExAdapter::_applyEngineCC(uint8_t layer, uint8_t cc, uint8_t ccValue) {
    JT_LOGF("[SyxAdpt] _applyEngineCC layer=0x%02X cc=%u val=%u\n",
            (unsigned)layer, (unsigned)cc, (unsigned)ccValue);

    // Route through the same path the TFT uses. This also fires the notifier
    // (TFT repaint + PARAM_VALUE echo) and respects edit target.
    // Cache first so BANK_DUMP reads reflect the new value.
    switch (layer) {
        case SyxProto::kLayerA:
            _lm.cacheCC(SyxProto::kLayerA, cc, ccValue);
            break;
        case SyxProto::kLayerB:
            _lm.cacheCC(SyxProto::kLayerB, cc, ccValue);
            break;
        case SyxProto::kLayerBoth:
            _lm.cacheCC(SyxProto::kLayerA, cc, ccValue);
            _lm.cacheCC(SyxProto::kLayerB, cc, ccValue);
            break;
        default: break;
    }
    _lm.setCC(cc, ccValue);
}

// =============================================================================
// _applyManagerCC — Performance / GlobalFX dispatch
//
// Goes through LayerManager::handleControlChange() because both perf-CCs and
// global-FX-CCs are intercepted in that handler before the engine-channel
// filter applies. Channel "1" is a placeholder — it only matters for
// patch-scope CCs, and these aren't.
// =============================================================================
void SysExAdapter::_applyManagerCC(uint8_t cc, uint8_t ccValue) {
    JT_LOGF("[SyxAdpt] _applyManagerCC cc=%u val=%u\n",
            (unsigned)cc, (unsigned)ccValue);
    _lm.handleControlChange(1, cc, ccValue);
}

// =============================================================================
// _handleBankDumpRequest — compose and send a BANK_DUMP of all live params
//
// Walks every entry in ParamMap that has a CC alias, reads the cached value
// per scope (and per-layer for Patch scope), encodes each as an entry, and
// emits one big SysEx. SysEx-only abstractions are emitted at the end.
//
// For engine-scope params (Patch) we emit two entries — one for Layer A and
// one for Layer B — so the editor gets the full state of both engines in one
// shot. Performance and GlobalFX are emitted once each (they're global).
//
// Total bytes written: header (8) + entries (8 each) + F7 (1).
// Worst case ~1648 bytes for the current 138 ParamIDs. Teensy USB MIDI breaks
// long SysEx into 64-byte packets transparently — no special chunking needed.
// =============================================================================
void SysExAdapter::_handleBankDumpRequest() {
    if (!_send) {
        JT_LOGF("[SyxAdpt] BANK_DUMP_REQUEST but no sender registered\n");
        return;
    }

    // Buffer sized for worst case. Static so we don't blow the stack — this
    // function is called from a MIDI callback context.
    // With 126 ParamIDs × up to 2 layers (engine-scope + curve params) we
    // comfortably fit within kBankDumpMaxEntries (256). If that limit is ever
    // hit, increase SyxProto::kBankDumpMaxEntries and rebuild.
    static uint8_t buf[SyxProto::kBankDumpMaxLen];

    size_t pos = 0;
    buf[pos++] = 0xF0;
    buf[pos++] = SyxProto::kMfrId;
    buf[pos++] = SyxProto::kSubIdJ;
    buf[pos++] = SyxProto::kSubIdT;
    buf[pos++] = SyxProto::kDeviceDefault;
    buf[pos++] = SyxProto::kMsgBankDump;
    // Reserve 2 bytes for count — we'll backfill once we know the total.
    const size_t countOffset = pos;
    buf[pos++] = 0;
    buf[pos++] = 0;

    uint16_t entryCount = 0;
    const size_t paramCount = ParamMap::entryCount();

    // Helper to append one entry. Returns true if there was room.
    auto appendEntry = [&](uint8_t layer, uint16_t pid, float value) -> bool {
        if (pos + SyxProto::kBankDumpEntrySize + 1 > sizeof(buf)) return false; // +1 for F7
        buf[pos++] = layer;
        buf[pos++] = SyxProto::paramIdHi(pid);
        buf[pos++] = SyxProto::paramIdLo(pid);
        SyxProto::encodeFloat(value, &buf[pos]);
        pos += SyxProto::kFloatBytes;
        ++entryCount;
        return true;
    };

    // Walk the ParamMap once. Per scope:
    //   kPatch     -> emit one entry per layer (A, B)
    //   kPerf      -> emit once with kLayerPerf
    //   kGlobalFx  -> emit once with kLayerGlobalFx
    //   kSysExOnly -> split: curve params emit per-layer (A, B);
    //                        FX abstractions emit once with kLayerA (global)
    for (size_t i = 0; i < paramCount; ++i) {
        const ParamMap::Entry* e = ParamMap::entryAt(i);
        if (!e) continue;

        if (e->scope == ParamMap::kPatch) {
            // Emit Layer A and Layer B entries separately. If a layer's cache
            // is unset we skip that entry — editor sees partial state.
            uint8_t cv;
            cv = _lm.getCachedCC(SyxProto::kLayerA, e->ccAlias);
            if (cv != CCCache::kUnset) {
                if (!appendEntry(SyxProto::kLayerA, e->paramId, _ccToFloat(e->type, cv))) goto done;
            }
            cv = _lm.getCachedCC(SyxProto::kLayerB, e->ccAlias);
            if (cv != CCCache::kUnset) {
                if (!appendEntry(SyxProto::kLayerB, e->paramId, _ccToFloat(e->type, cv))) goto done;
            }
        }
        else if (e->scope == ParamMap::kPerf) {
            const uint8_t cv = _lm.getCachedCC(SyxProto::kLayerPerf, e->ccAlias);
            if (cv != CCCache::kUnset) {
                if (!appendEntry(SyxProto::kLayerPerf, e->paramId, _ccToFloat(e->type, cv))) goto done;
            }
        }
        else if (e->scope == ParamMap::kGlobalFx) {
            const uint8_t cv = _lm.getCachedCC(SyxProto::kLayerGlobalFx, e->ccAlias);
            if (cv != CCCache::kUnset) {
                if (!appendEntry(SyxProto::kLayerGlobalFx, e->paramId, _ccToFloat(e->type, cv))) goto done;
            }
        }
        else if (e->scope == ParamMap::kSysExOnly) {
            using namespace ParamMap::SysExOnlyIds;
            const uint16_t pid = e->paramId;

            // Envelope curve params are per-engine — emit one entry per layer.
            const bool isCurve =
                (pid == kAmpAttackCurve    || pid == kAmpDecayCurve    || pid == kAmpReleaseCurve   ||
                 pid == kFilterAttackCurve || pid == kFilterDecayCurve || pid == kFilterReleaseCurve ||
                 pid == kPitchAttackCurve  || pid == kPitchDecayCurve  || pid == kPitchReleaseCurve);

            if (isCurve) {
                // Read each engine's PatchState directly — no CC cache involved.
                const float vA = _handleSyxOnlyGet(pid);   // reads layerA()
                if (!appendEntry(SyxProto::kLayerA, pid, vA)) goto done;

                // Layer B: temporarily switch the getter context by reading
                // layerB() directly via its getter methods.
                float vB = 0.0f;
                switch (pid) {
                    case kAmpAttackCurve:     vB = _lm.layerB().getAmpAttackCurve();     break;
                    case kAmpDecayCurve:      vB = _lm.layerB().getAmpDecayCurve();      break;
                    case kAmpReleaseCurve:    vB = _lm.layerB().getAmpReleaseCurve();    break;
                    case kFilterAttackCurve:  vB = _lm.layerB().getFilterAttackCurve();  break;
                    case kFilterDecayCurve:   vB = _lm.layerB().getFilterDecayCurve();   break;
                    case kFilterReleaseCurve: vB = _lm.layerB().getFilterReleaseCurve(); break;
                    case kPitchAttackCurve:   vB = _lm.layerB().getPitchEnvAttackCurve();  break;
                    case kPitchDecayCurve:    vB = _lm.layerB().getPitchEnvDecayCurve();   break;
                    case kPitchReleaseCurve:  vB = _lm.layerB().getPitchEnvReleaseCurve(); break;
                    default: break;
                }
                if (!appendEntry(SyxProto::kLayerB, pid, vB)) goto done;
            } else {
                // FX abstractions — global state, emit once with kLayerA.
                const float v = _handleSyxOnlyGet(pid);
                if (!appendEntry(SyxProto::kLayerA, pid, v)) goto done;
            }
        }
    }

done:
    // Backfill count (14-bit MSB/LSB).
    buf[countOffset]     = (uint8_t)((entryCount >> 7) & 0x7F);
    buf[countOffset + 1] = (uint8_t)( entryCount       & 0x7F);
    buf[pos++] = 0xF7;

    JT_LOGF("[SyxAdpt] BANK_DUMP sending %u entries, %u bytes\n",
            (unsigned)entryCount, (unsigned)pos);
    _send(buf, pos);
}

// =============================================================================
// _handleIncomingBankDump — apply a BANK_DUMP pushed from the editor
//
// One AudioNoInterrupts() guard wraps every entry application. This is what
// replaces the per-CC spray in Patch::applyTo(): instead of 90 separate CC
// dispatches over the wire, the editor sends one SysEx and the firmware
// applies it atomically.
//
// Each entry is the same 8-byte format as the dump-out direction:
//   <layer> <pid_hi> <pid_lo> <val[0..4]>
//
// Bad entries (unknown ParamID, layer mismatch with scope) are logged and
// skipped — partial application is acceptable; never crash on bad data.
// =============================================================================
void SysExAdapter::_handleIncomingBankDump(const uint8_t* data, size_t len) {
    // Sanity: minimum size = header (8) + F7 (1) = 9 bytes (zero entries).
    if (len < SyxProto::kBankDumpHeaderSize + 1) {
        JT_LOGF("[SyxAdpt] BANK_DUMP too short (%u bytes)\n", (unsigned)len);
        return;
    }
    if (data[len - 1] != 0xF7) {
        JT_LOGF("[SyxAdpt] BANK_DUMP missing F7\n");
        return;
    }

    // Decode 14-bit count.
    const uint16_t countHi = data[SyxProto::kOffsetMsgType + 1] & 0x7F;
    const uint16_t countLo = data[SyxProto::kOffsetMsgType + 2] & 0x7F;
    const uint16_t entryCount = (uint16_t)((countHi << 7) | countLo);

    // Verify length matches declared count.
    const size_t expectedLen = SyxProto::kBankDumpHeaderSize
                             + (size_t)entryCount * SyxProto::kBankDumpEntrySize
                             + 1;
    if (len != expectedLen) {
        JT_LOGF("[SyxAdpt] BANK_DUMP len mismatch: got %u want %u (count=%u)\n",
                (unsigned)len, (unsigned)expectedLen, (unsigned)entryCount);
        return;
    }

    JT_LOGF("[SyxAdpt] BANK_DUMP applying %u entries\n", (unsigned)entryCount);

    // Wrap the whole apply in one AudioNoInterrupts so audio doesn't see a
    // partial parameter set. This is the same protection Patch::applyTo()
    // uses; combined with the single-message wire format it eliminates the
    // mid-load knob-sweep audible artifacts.
    AudioNoInterrupts();

    const uint8_t* p = data + SyxProto::kBankDumpHeaderSize;
    for (uint16_t i = 0; i < entryCount; ++i) {
        const uint8_t  layer = p[0];
        const uint16_t pid   = SyxProto::paramIdFromBytes(p[1], p[2]);
        const float    value = SyxProto::decodeFloat(&p[3]);
        p += SyxProto::kBankDumpEntrySize;

        // Re-use the SET_PARAM path. It already validates layer + ParamID
        // and handles SysEx-only abstractions correctly.
        _handleSetParam(layer, pid, value);
    }

    AudioInterrupts();
}

// =============================================================================
// snoopCC — called from LayerManager::handleControlChange() to update local
// memory of conflated CCs. Lets the SysEx-only abstraction view stay
// consistent when hardware controllers move the underlying CC.
// =============================================================================
void SysExAdapter::snoopCC(uint8_t cc, uint8_t value) {
    switch (cc) {
        case CC::FX_MOD_EFFECT:
            if (value > 0) _lastModEffectCC = value;
            // value == 0 means OFF — don't lose memory of the last variation.
            _modEnabled = (value > 0);
            break;
        case CC::FX_MOD_FEEDBACK:
            if (value > 0) _lastModFbCC = value;
            _modFbOverride = (value > 0);
            break;
        case CC::FX_JPFX_DELAY_EFFECT:
            if (value > 0) _lastDelayEffectCC = value;
            _delayEnabled = (value > 0);
            break;
        case CC::FX_JPFX_DELAY_FEEDBACK:
            if (value > 0) _lastDelayFbCC = value;
            _delayFbOverride = (value > 0);
            break;
        default: break;
    }
}

// =============================================================================
// SysEx-only abstraction handling (section 12b of Phase 0 map)
//
// Each virtual ParamID projects onto the underlying conflated CC. Setting
// "enabled = true" emits the last-known variation CC; setting "enabled =
// false" emits CC = 0 (OFF) but keeps the variation memory intact.
// =============================================================================
void SysExAdapter::_handleSyxOnlySet(uint16_t paramId, float value, uint8_t layer) {
    using namespace ParamMap::SysExOnlyIds;

    // ---- Envelope curve exponents ------------------------------------------
    // Per-engine: fan out to the engine(s) indicated by the layer byte.
    // Each setter writes PatchState and fans out to all voices on that engine.
    // No CC quantisation — the float is applied directly.
    //
    // A lambda avoids repeating the A/B/Both branch for every one of the 9 params.
    // clampCurve mirrors AudioEffectEnvelopeJT::constrainCurve (0.05..10.0).
    auto applyCurve = [&](auto setterA, auto setterB) {
        // Clamp to the same [0.05, 10.0] range the engine enforces so the
        // PatchState value is always in a known safe range regardless of source.
        float v = value;
        if (v < 0.05f) v = 0.05f;
        if (v > 10.0f) v = 10.0f;
        switch (layer) {
            case SyxProto::kLayerA:
                (_lm.layerA().*setterA)(v);
                break;
            case SyxProto::kLayerB:
                (_lm.layerB().*setterB)(v);
                break;
            case SyxProto::kLayerBoth:
            default:
                (_lm.layerA().*setterA)(v);
                (_lm.layerB().*setterB)(v);
                break;
        }
    };

    switch (paramId) {
        // Amp envelope curves
        case kAmpAttackCurve:
            applyCurve(&SynthEngine::setAmpAttackCurve, &SynthEngine::setAmpAttackCurve);
            return;
        case kAmpDecayCurve:
            applyCurve(&SynthEngine::setAmpDecayCurve, &SynthEngine::setAmpDecayCurve);
            return;
        case kAmpReleaseCurve:
            applyCurve(&SynthEngine::setAmpReleaseCurve, &SynthEngine::setAmpReleaseCurve);
            return;
        // Filter envelope curves
        case kFilterAttackCurve:
            applyCurve(&SynthEngine::setFilterAttackCurve, &SynthEngine::setFilterAttackCurve);
            return;
        case kFilterDecayCurve:
            applyCurve(&SynthEngine::setFilterDecayCurve, &SynthEngine::setFilterDecayCurve);
            return;
        case kFilterReleaseCurve:
            applyCurve(&SynthEngine::setFilterReleaseCurve, &SynthEngine::setFilterReleaseCurve);
            return;
        // Pitch envelope curves
        case kPitchAttackCurve:
            applyCurve(&SynthEngine::setPitchEnvAttackCurve, &SynthEngine::setPitchEnvAttackCurve);
            return;
        case kPitchDecayCurve:
            applyCurve(&SynthEngine::setPitchEnvDecayCurve, &SynthEngine::setPitchEnvDecayCurve);
            return;
        case kPitchReleaseCurve:
            applyCurve(&SynthEngine::setPitchEnvReleaseCurve, &SynthEngine::setPitchEnvReleaseCurve);
            return;

        // ---- FX Mod / Delay abstractions (global; layer byte ignored) ------
        default: break;
    }

    // FX Mod / Delay abstractions — global state, layer byte not relevant.
    switch (paramId) {

        case kFxModEnabled: {
            const bool en = (value >= 0.5f);
            _modEnabled = en;
            // Recall last variation if we have one; otherwise variation 0
            // (i.e. CC 1 — the lowest non-OFF position).
            const uint8_t cc = en ? (_lastModEffectCC ? _lastModEffectCC : 1) : 0;
            _applyManagerCC(CC::FX_MOD_EFFECT, cc);
        } break;

        case kFxModVariation: {
            // Editor sends 0..N-1 as float. Encode to CC 1..127.
            const uint8_t var = clampToCC(value);
            const uint8_t cc  = _variationToCC(var, kModVariationCount);
            _lastModEffectCC  = cc;       // update memory
            _modEnabled       = true;     // setting variation implies "on"
            _applyManagerCC(CC::FX_MOD_EFFECT, cc);
        } break;

        case kFxModFbOverride: {
            const bool en = (value >= 0.5f);
            _modFbOverride = en;
            const uint8_t cc = en ? (_lastModFbCC ? _lastModFbCC : 64) : 0;
            _applyManagerCC(CC::FX_MOD_FEEDBACK, cc);
        } break;

        case kFxModFbValue: {
            // Editor sends 0..1 normalised; map to CC 1..127.
            float v = value;
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            const uint8_t cc = (uint8_t)(1.0f + v * 126.0f + 0.5f);
            _lastModFbCC    = cc;
            _modFbOverride  = true;
            _applyManagerCC(CC::FX_MOD_FEEDBACK, cc);
        } break;

        case kFxDelayEnabled: {
            const bool en = (value >= 0.5f);
            _delayEnabled = en;
            const uint8_t cc = en ? (_lastDelayEffectCC ? _lastDelayEffectCC : 1) : 0;
            _applyManagerCC(CC::FX_JPFX_DELAY_EFFECT, cc);
        } break;

        case kFxDelayVariation: {
            const uint8_t var = clampToCC(value);
            const uint8_t cc  = _variationToCC(var, kDelayVariationCount);
            _lastDelayEffectCC = cc;
            _delayEnabled      = true;
            _applyManagerCC(CC::FX_JPFX_DELAY_EFFECT, cc);
        } break;

        case kFxDelayFbOverride: {
            const bool en = (value >= 0.5f);
            _delayFbOverride = en;
            const uint8_t cc = en ? (_lastDelayFbCC ? _lastDelayFbCC : 64) : 0;
            _applyManagerCC(CC::FX_JPFX_DELAY_FEEDBACK, cc);
        } break;

        case kFxDelayFbValue: {
            float v = value;
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            const uint8_t cc = (uint8_t)(1.0f + v * 126.0f + 0.5f);
            _lastDelayFbCC    = cc;
            _delayFbOverride  = true;
            _applyManagerCC(CC::FX_JPFX_DELAY_FEEDBACK, cc);
        } break;

        default:
            JT_LOGF("[SyxAdpt] unknown SysEx-only ParamID 0x%04X\n", (unsigned)paramId);
            break;
    }
}

float SysExAdapter::_handleSyxOnlyGet(uint16_t paramId) const {
    using namespace ParamMap::SysExOnlyIds;

    // ---- Envelope curve exponents — read from Layer A as representative ----
    // For GET_PARAM / BANK_DUMP the editor typically requests Layer A or B
    // explicitly; the bank dump walker calls this once per PID and uses kLayerA.
    // We read from layerA() here; the BANK_DUMP walker emits one entry per layer.
    switch (paramId) {
        case kAmpAttackCurve:     return _lm.layerA().getAmpAttackCurve();
        case kAmpDecayCurve:      return _lm.layerA().getAmpDecayCurve();
        case kAmpReleaseCurve:    return _lm.layerA().getAmpReleaseCurve();
        case kFilterAttackCurve:  return _lm.layerA().getFilterAttackCurve();
        case kFilterDecayCurve:   return _lm.layerA().getFilterDecayCurve();
        case kFilterReleaseCurve: return _lm.layerA().getFilterReleaseCurve();
        case kPitchAttackCurve:   return _lm.layerA().getPitchEnvAttackCurve();
        case kPitchDecayCurve:    return _lm.layerA().getPitchEnvDecayCurve();
        case kPitchReleaseCurve:  return _lm.layerA().getPitchEnvReleaseCurve();
        default: break;
    }

    // ---- FX Mod / Delay abstractions ----------------------------------------
    switch (paramId) {
        case kFxModEnabled:       return _modEnabled       ? 1.0f : 0.0f;
        case kFxModVariation:     return (float)_ccToVariation(_lastModEffectCC, kModVariationCount);
        case kFxModFbOverride:    return _modFbOverride    ? 1.0f : 0.0f;
        case kFxModFbValue:       return _lastModFbCC ? ((_lastModFbCC - 1) / 126.0f) : 0.0f;
        case kFxDelayEnabled:     return _delayEnabled     ? 1.0f : 0.0f;
        case kFxDelayVariation:   return (float)_ccToVariation(_lastDelayEffectCC, kDelayVariationCount);
        case kFxDelayFbOverride:  return _delayFbOverride  ? 1.0f : 0.0f;
        case kFxDelayFbValue:     return _lastDelayFbCC ? ((_lastDelayFbCC - 1) / 126.0f) : 0.0f;
        default:                  return 0.0f;
    }
}

// =============================================================================
// Variation <-> CC encoding for the conflated FX_MOD_EFFECT / FX_DELAY_EFFECT
// CCs. Firmware uses ((value - 1) * N) / 127 to decode CC 1..127 to 0..N-1.
// We encode the inverse: bucket-midpoint within the 1..127 range.
// =============================================================================
uint8_t SysExAdapter::_variationToCC(uint8_t variation, uint8_t n) const {
    if (n == 0) return 1;
    if (variation >= n) variation = n - 1;
    // Place each variation at the midpoint of its bucket within 1..127.
    // Bucket size = 127 / N; midpoint = (variation + 0.5) * (127 / N) + 1.
    const uint16_t cc = (uint16_t)((((uint32_t)variation * 127u) + (127u / 2u)) / n) + 1u;
    return (cc > 127) ? 127 : (uint8_t)cc;
}

uint8_t SysExAdapter::_ccToVariation(uint8_t cc, uint8_t n) const {
    if (cc == 0 || n == 0) return 0;
    // Mirror firmware's ((cc - 1) * N) / 127 decode.
    const uint16_t var = (uint16_t)(((cc - 1u) * (uint32_t)n) / 127u);
    return (var >= n) ? (n - 1) : (uint8_t)var;
}