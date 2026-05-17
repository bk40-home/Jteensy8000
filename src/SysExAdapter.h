// =============================================================================
// SysExAdapter.h — JT-8000 editor SysEx receive path
//
// Sits between the MIDI input (USB device, USB host, DIN) and LayerManager.
// Decodes SysEx messages in the JT-8000 editor format (see SyxProtocol.h) and
// dispatches each SET_PARAM / GET_PARAM / BANK_DUMP_REQUEST / BANK_DUMP to
// the right scope.
//
// Phase 2 additions over Phase 1:
//   - GET_PARAM reply via PARAM_VALUE message (reads from LayerManager cache)
//   - BANK_DUMP_REQUEST -> BANK_DUMP reply (live state of all cached params)
//   - BANK_DUMP receive (apply all entries under one AudioNoInterrupts block)
//
// Dispatch strategy unchanged:
//   - Patch-scope ParamIDs with a CC alias  -> LayerManager::layerX().handleControlChange()
//   - Performance ParamIDs (CC alias)       -> LayerManager::handleControlChange()
//   - GlobalFX ParamIDs (CC alias)          -> LayerManager::handleControlChange()
//   - SysEx-only abstractions (0x0C8X)      -> handled locally, project onto
//                                              a conflated CC (Option D)
//
// CC snoop:
//   LayerManager::handleControlChange() calls SysExAdapter::snoopCC() at the
//   top of every CC dispatch (P3 sign-off). The adapter watches CCs 103/106/
//   107/109 to keep its "last variation" / "last fb override" memory aligned
//   when hardware controllers move the underlying conflated CC.
//
// © 2025 Kris Bishop — MIT licensed.
// =============================================================================

#pragma once

#include <stdint.h>
#include <stddef.h>

class LayerManager;

class SysExAdapter {
public:
    // Output sender — registered by Jteensy8000.cpp at construction so the
    // adapter can reply to GET_PARAM and BANK_DUMP_REQUEST without depending
    // on a specific MIDI port. Function takes (data, length) and writes the
    // bytes (F0..F7 inclusive).
    typedef void (*SendFn)(const uint8_t* data, size_t len);

    explicit SysExAdapter(LayerManager& lm);

    // Register the outgoing-SysEx sender. Pass nullptr to disable replies.
    void setSender(SendFn fn) { _send = fn; }

    // -------------------------------------------------------------------------
    // Incoming MIDI hooks
    // -------------------------------------------------------------------------

    // Called by Jteensy8000.cpp's onSysEx() callback for any port. Returns true
    // if the message was a valid JT-8000 message and was handled (either
    // dispatched or rejected for protocol reasons). Returns false if the
    // message was not addressed to this device (different manufacturer ID).
    //
    // Bad messages (length / format / unknown ParamID) are logged and dropped;
    // never crashes, never partial-applies.
    bool handleSysEx(const uint8_t* data, size_t len);

    // Called from LayerManager::handleControlChange() at the top of dispatch
    // so the adapter sees every CC traffic event, including those generated
    // internally (e.g. by Patch::applyTo()). Used to keep the SysEx-only
    // abstraction state (FX Mod/Delay enable + variation memory) in sync.
    void snoopCC(uint8_t cc, uint8_t value);

    // -------------------------------------------------------------------------
    // Outbound notifications (firmware → editor)
    // -------------------------------------------------------------------------

    // Called from onCCHandled() when a local CC change (TFT, encoder, preset
    // load) should be echoed to the editor as a SysEx PARAM_VALUE message.
    //
    // `layer` is the SyxProto layer ID (kLayerA/B/Perf/GlobalFx).
    // `cc` and `val` come from the notifier callback (uint8_t each).
    //
    // Internally: looks up the ParamID and value type via ParamMap::findByCC(),
    // converts the CC byte to a native float, then calls _sendParamValue().
    // No-ops silently if: no sender registered, CC not in ParamMap, or cc is
    // a SysEx-only param.
    void notifyLocalCC(uint8_t layer, uint8_t cc, uint8_t val);

private:
    // -------------------------------------------------------------------------
    // Phase 1 message handlers
    // -------------------------------------------------------------------------
    void _handleSetParam  (uint8_t layer, uint16_t paramId, float value);
    void _handleGetParam  (uint8_t layer, uint16_t paramId);
    void _sendParamValue  (uint8_t layer, uint16_t paramId, float value);

    // -------------------------------------------------------------------------
    // Phase 2 message handlers
    // -------------------------------------------------------------------------
    // BANK_DUMP_REQUEST -> compose and send a BANK_DUMP with current live state.
    void _handleBankDumpRequest();

    // BANK_DUMP from editor -> apply every entry under one AudioNoInterrupts.
    void _handleIncomingBankDump(const uint8_t* data, size_t len);

    // -------------------------------------------------------------------------
    // Read paths — pull current parameter value from LayerManager cache, then
    // convert from CC byte to SysEx float using the param's value type.
    // Returns false if the param has no value cached yet.
    // -------------------------------------------------------------------------
    bool _readParamValue(uint8_t layer, uint16_t paramId, float& outValue) const;

    // CC-byte -> SysEx float (inverse of _floatToCC). Used by GET_PARAM reply
    // and BANK_DUMP composition.
    float _ccToFloat(uint8_t valueType, uint8_t ccValue) const;

    // -------------------------------------------------------------------------
    // Phase 1 dispatch helpers (unchanged signature)
    // -------------------------------------------------------------------------
    uint8_t _floatToCC(uint8_t valueType, float value) const;
    void _applyEngineCC(uint8_t layer, uint8_t cc, uint8_t ccValue);
    void _applyManagerCC(uint8_t cc, uint8_t ccValue);

    // -------------------------------------------------------------------------
    // SysEx-only abstraction (section 12b of Phase 0 map) — handlers
    // -------------------------------------------------------------------------
    void  _handleSyxOnlySet(uint16_t paramId, float value);
    float _handleSyxOnlyGet(uint16_t paramId) const;
    uint8_t _variationToCC (uint8_t variation, uint8_t n) const;
    uint8_t _ccToVariation (uint8_t cc, uint8_t n) const;

private:
    LayerManager& _lm;
    SendFn        _send = nullptr;

    // ---- SysEx-only abstraction state (section 12b) -----------------------
    // Last non-zero CC value seen on each conflated CC. Updated by snoopCC()
    // and by our own _applyManagerCC() / _applyEngineCC() emissions. Used so
    // toggling "enabled" off then back on recalls the previous variation.
    uint8_t _lastModEffectCC   = 0; // CC 103
    uint8_t _lastModFbCC       = 0; // CC 106
    uint8_t _lastDelayEffectCC = 0; // CC 107
    uint8_t _lastDelayFbCC     = 0; // CC 109

    // Whether each "enabled" / "fb override" virtual toggle is on. Tracked
    // here because the conflated CC = 0 means OFF on both axes simultaneously.
    bool _modEnabled       = false;
    bool _delayEnabled     = false;
    bool _modFbOverride    = false;
    bool _delayFbOverride  = false;
};
