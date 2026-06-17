#pragma once
// =============================================================================
// Performance.h — JP-8080-style dual-layer performance container
// =============================================================================
//
// A Performance holds:
//   - Two Patch objects (Layer A and Layer B)
//   - Performance-level settings (mode, voice split, split note, balance)
//   - Per-layer MIDI channel assignment (for future MIDI channel routing)
//
// DESIGN:
//   - Patch is the existing CC-snapshot struct (Patch.h). No changes needed.
//   - Performance wraps two patches with the performance envelope.
//   - Loading a single patch targets a specific layer via LayerManager.
//   - Loading a performance restores both patches + all perf settings.
//   - Serialisation uses the same JSON approach as Patch for consistency.
//   - MIDI channel is stored per-layer but not yet routed (future Phase).
//
// STORAGE FORMATS:
//   - Single patch: existing Patch::toJson() / fromJson() — unchanged.
//   - Performance: Performance::toJson() / fromJson() — wraps two patches.
//   - Factory presets: existing Presets:: loaders work unchanged — just pass
//     the correct SynthEngine& reference (layers.layerA() or layerB()).
//
// USAGE:
//   Performance perf;
//   perf.captureFrom(layers);           // snapshot both layers + perf settings
//   perf.applyTo(layers);               // restore both layers + perf settings
//
//   // Load a single patch to a specific layer:
//   Patch p;
//   p.fromJson(jsonString);
//   Performance::loadPatchToLayer(layers, p, EditTarget::LAYER_B);
//
//   // Load a factory preset to the active edit target:
//   Performance::loadPresetToActive(layers, presetIndex);
// =============================================================================

#include <Arduino.h>
#include "Patch.h"
#include "LayerManager.h"

struct Performance {
    // =========================================================================
    // Patch data — one per layer
    // =========================================================================
    Patch patchA;
    Patch patchB;

    // =========================================================================
    // Performance-level settings
    // =========================================================================
    char    name[24]     = "Init Perf";
    uint8_t version      = 1;

    // Mode: 0=SINGLE, 1=LAYER, 2=SPLIT (maps to PerfMode enum)
    uint8_t perfMode     = 0;

    // Voice allocation: how many voices Layer A gets (1..MAX_VOICES-1).
    // Layer B gets MAX_VOICES - voicesA.  In SINGLE mode this is MAX_VOICES.
    uint8_t voicesA      = MAX_VOICES;

    // Split point: MIDI note number (0..127). Only used in SPLIT mode.
    uint8_t splitNote    = 60;

    // Layer balance: 0=A only, 64=equal, 127=B only.
    uint8_t balance      = 64;

    // Per-layer MIDI channel (1-16, JP-8000 convention — no omni).
    // Both layers set to the same channel  = "Dual" behaviour: both Parts
    //   receive notes from one controller, split/layer rules decide which
    //   Part plays the note.
    // Different channels = strict multitimbral: only the matching Part
    //   receives the event (split/layer rules then apply within the match).
    // LayerManager strictly filters notes, pitch bend, and CCs by channel.
    uint8_t midiChannelA = 1;
    uint8_t midiChannelB = 1;

    // =========================================================================
    // Global reverb state — moved here from PatchState (Phase 3).
    // Reverb is shared between layers via GlobalFX, so its parameters are
    // performance-scope. Loaded on applyTo() into mgr.getGlobalFX(); captured
    // from the same instance on captureFrom(). Serialised under the "rv"
    // JSON object for grouping.
    // =========================================================================
    float   reverbRoomSize  = 0.5f;
    float   reverbHiDamp    = 0.5f;
    float   reverbLoDamp    = 0.5f;
    float   reverbMixL      = 0.0f;   // master wet level L
    float   reverbMixR      = 0.0f;   // master wet level R
    float   reverbShimmer   = 0.0f;
    bool    reverbFrozen    = false;
    float   reverbLowpass   = 0.0f;
    float   reverbHipass    = 0.0f;
    bool    reverbBypass    = false;

    // =========================================================================
    // Lifecycle
    // =========================================================================
    Performance() { clear(); }

    void clear() {
        patchA.clear();
        patchB.clear();
        strncpy(name, "Init Perf", sizeof(name));
        version      = 1;
        perfMode     = 0;
        voicesA      = MAX_VOICES;
        splitNote    = 60;
        balance      = 64;
        midiChannelA = 1;
        midiChannelB = 1;

        // Global reverb defaults — matches GlobalFX's ctor defaults so a
        // freshly-cleared Performance applies cleanly (no audible change
        // when the engine's current reverb already matches these values).
        reverbRoomSize = 0.5f;
        reverbHiDamp   = 0.5f;
        reverbLoDamp   = 0.5f;
        reverbMixL     = 0.0f;
        reverbMixR     = 0.0f;
        reverbShimmer  = 0.0f;
        reverbFrozen   = false;
        reverbLowpass  = 0.0f;
        reverbHipass   = 0.0f;
        reverbBypass   = false;
    }

    // =========================================================================
    // Capture — snapshot the current state of both layers + perf settings
    // =========================================================================
    void captureFrom(LayerManager& mgr) {
        // Capture each layer's patch from its engine
        patchA.captureFrom(mgr.layerA());
        patchB.captureFrom(mgr.layerB());

        // Capture performance settings
        perfMode     = (uint8_t)mgr.getPerfMode();
        voicesA      = mgr.getVoiceSplit();
        splitNote    = mgr.getSplitNote();
        balance      = mgr.getBalance();
        midiChannelA = mgr.getMidiChannelA();
        midiChannelB = mgr.getMidiChannelB();

        // Capture global reverb (Phase 3).
        const GlobalFX& gfx = mgr.getGlobalFX();
        reverbRoomSize = gfx.getReverbRoomSize();
        reverbHiDamp   = gfx.getReverbHiDamping();
        reverbLoDamp   = gfx.getReverbLoDamping();
        reverbMixL     = gfx.getReverbMixL();
        reverbMixR     = gfx.getReverbMixR();
        reverbShimmer  = gfx.getReverbShimmer();
        reverbFrozen   = gfx.getReverbFreeze();
        reverbLowpass  = gfx.getReverbLowpass();
        reverbHipass   = gfx.getReverbHipass();
        reverbBypass   = gfx.getReverbBypass();
    }

    // =========================================================================
    // Apply — restore both layers + perf settings to the LayerManager
    // =========================================================================
    void applyTo(LayerManager& mgr, uint8_t midiCh = 1) const {
        // Set performance mode first (this configures voice ranges)
        mgr.setPerfMode((PerfMode)perfMode);

        // Set voice split before loading patches (voices must be assigned)
        if (perfMode != (uint8_t)PerfMode::SINGLE) {
            mgr.setVoiceSplit(voicesA);
        }
        mgr.setSplitNote(splitNote);
        mgr.setBalance(balance);

        // Per-layer MIDI channel assignment. Must be applied BEFORE note
        // events resume; the LayerManager filter uses these values on every
        // incoming note/bend/CC. Clamp defensively (file corruption safety).
        mgr.setMidiChannelA(midiChannelA);
        mgr.setMidiChannelB(midiChannelB);

        // Global reverb state (Phase 3). Pushed into GlobalFX in a single
        // batch — setReverbMix() is called LAST because it's the only call
        // that triggers the auto-bypass check; doing it last lets the other
        // params settle before the bypass decision is re-evaluated.
        GlobalFX& gfx = mgr.getGlobalFX();
        gfx.setReverbRoomSize(reverbRoomSize);
        gfx.setReverbHiDamping(reverbHiDamp);
        gfx.setReverbLoDamping(reverbLoDamp);
        gfx.setReverbShimmer(reverbShimmer);
        gfx.setReverbFreeze(reverbFrozen);
        gfx.setReverbLowpass(reverbLowpass);
        gfx.setReverbHipass(reverbHipass);
        gfx.setReverbBypass(reverbBypass);
        gfx.setReverbMix(reverbMixL, reverbMixR);

        // Apply patches to their respective engines. Preset-load CCs go
        // directly through Patch::applyTo → engine.handleControlChange,
        // bypassing LayerManager's channel filter entirely — this is
        // intentional. The filter is strictly for LIVE MIDI input.
        patchA.applyTo(mgr.layerA(), midiCh, true);

        // Only apply Layer B patch if we're in a dual-layer mode
        if (perfMode != (uint8_t)PerfMode::SINGLE) {
            patchB.applyTo(mgr.layerB(), midiCh, true);
        }
    }

    // =========================================================================
    // JSON serialisation
    // =========================================================================
    // Format:
    //   {
    //     "name": "My Performance",
    //     "v": 1,
    //     "mode": 1,
    //     "voicesA": 4,
    //     "split": 60,
    //     "bal": 64,
    //     "chA": 0,
    //     "chB": 0,
    //     "a": { ...patch A JSON... },
    //     "b": { ...patch B JSON... }
    //   }
    //
    // The "a" and "b" fields use the existing Patch::toJson() format.

    String toJson() const {
        String js = "{\"name\":\"";
        js += name;
        js += "\",\"v\":";
        js += version;
        js += ",\"mode\":";
        js += perfMode;
        js += ",\"voicesA\":";
        js += voicesA;
        js += ",\"split\":";
        js += splitNote;
        js += ",\"bal\":";
        js += balance;
        js += ",\"chA\":";
        js += midiChannelA;
        js += ",\"chB\":";
        js += midiChannelB;

        // Global reverb (Phase 3). Grouped under "rv" for readability
        // and so a future format revision can extend it without touching
        // the outer keys. Values are raw floats / bools.
        js += ",\"rv\":{";
        js += "\"sz\":";  js += String(reverbRoomSize, 4);
        js += ",\"hd\":"; js += String(reverbHiDamp,   4);
        js += ",\"ld\":"; js += String(reverbLoDamp,   4);
        js += ",\"mL\":"; js += String(reverbMixL,     4);
        js += ",\"mR\":"; js += String(reverbMixR,     4);
        js += ",\"sh\":"; js += String(reverbShimmer,  4);
        js += ",\"fz\":"; js += (reverbFrozen ? "1" : "0");
        js += ",\"lp\":"; js += String(reverbLowpass,  4);
        js += ",\"hp\":"; js += String(reverbHipass,   4);
        js += ",\"bp\":"; js += (reverbBypass ? "1" : "0");
        js += "}";

        js += ",\"a\":";
        js += patchA.toJson();
        js += ",\"b\":";
        js += patchB.toJson();
        js += "}";
        return js;
    }

    bool fromJson(const String& js) {
        // Parse performance-level fields
        // Name
        int ni = js.indexOf("\"name\":\"");
        if (ni >= 0) {
            ni += 8;
            int ne = js.indexOf('"', ni);
            if (ne > ni) {
                String n = js.substring(ni, ne);
                n.toCharArray(name, sizeof(name));
            }
        }

        // Simple integer fields — extract with helper
        perfMode     = (uint8_t)_extractInt(js, "\"mode\":", 0);
        voicesA      = (uint8_t)_extractInt(js, "\"voicesA\":", MAX_VOICES);
        splitNote    = (uint8_t)_extractInt(js, "\"split\":", 60);
        balance      = (uint8_t)_extractInt(js, "\"bal\":", 64);

        // MIDI channels: valid 1..16 per JP-8000 convention. Anything outside
        // that range (including a pre-refactor '0' omni marker in older JSON)
        // is clamped to 1 so old files load as a sensible default.
        const int rawChA = _extractInt(js, "\"chA\":", 1);
        const int rawChB = _extractInt(js, "\"chB\":", 1);
        midiChannelA = (uint8_t)((rawChA >= 1 && rawChA <= 16) ? rawChA : 1);
        midiChannelB = (uint8_t)((rawChB >= 1 && rawChB <= 16) ? rawChB : 1);

        // Global reverb state (Phase 3). Grouped under "rv" in JSON. If the
        // file predates the refactor and has no "rv" block, _extractObject
        // returns an empty string and the per-key extractors all fall back
        // to the defaults below — which match clear()'s initial values, so
        // old Performances load cleanly with a silent reverb tank.
        const String rv = _extractObject(js, "\"rv\":");
        reverbRoomSize = _extractFloat(rv, "\"sz\":", 0.5f);
        reverbHiDamp   = _extractFloat(rv, "\"hd\":", 0.5f);
        reverbLoDamp   = _extractFloat(rv, "\"ld\":", 0.5f);
        reverbMixL     = _extractFloat(rv, "\"mL\":", 0.0f);
        reverbMixR     = _extractFloat(rv, "\"mR\":", 0.0f);
        reverbShimmer  = _extractFloat(rv, "\"sh\":", 0.0f);
        reverbLowpass  = _extractFloat(rv, "\"lp\":", 0.0f);
        reverbHipass   = _extractFloat(rv, "\"hp\":", 0.0f);
        // Booleans stored as 0/1 ints — any non-zero value reads as true.
        reverbFrozen   = (_extractInt(rv, "\"fz\":", 0) != 0);
        reverbBypass   = (_extractInt(rv, "\"bp\":", 0) != 0);

        // Defensive clamp — file corruption or third-party editors could
        // produce out-of-range floats. GlobalFX setters clamp too, but the
        // stored-struct values should also be in range for save round-trip.
        reverbRoomSize = constrain(reverbRoomSize, 0.0f, 1.0f);
        reverbHiDamp   = constrain(reverbHiDamp,   0.0f, 1.0f);
        reverbLoDamp   = constrain(reverbLoDamp,   0.0f, 1.0f);
        reverbMixL     = constrain(reverbMixL,     0.0f, 1.0f);
        reverbMixR     = constrain(reverbMixR,     0.0f, 1.0f);
        reverbShimmer  = constrain(reverbShimmer,  0.0f, 1.0f);
        reverbLowpass  = constrain(reverbLowpass,  0.0f, 1.0f);
        reverbHipass   = constrain(reverbHipass,   0.0f, 1.0f);

        // Extract nested patch JSON objects
        patchA.clear();
        patchB.clear();
        String patchAJson = _extractObject(js, "\"a\":");
        String patchBJson = _extractObject(js, "\"b\":");
        if (patchAJson.length() > 0) patchA.fromJson(patchAJson);
        if (patchBJson.length() > 0) patchB.fromJson(patchBJson);

        return true;
    }

    // =========================================================================
    // Static helpers — load patches to specific layers
    // =========================================================================

    // Load a single Patch to a specific layer.
    // Does not change performance mode or voice allocation.
    static void loadPatchToLayer(LayerManager& mgr, const Patch& p,
                                  EditTarget target, uint8_t midiCh = 1)
    {
        switch (target) {
            case EditTarget::LAYER_A:
                p.applyTo(mgr.layerA(), midiCh, true);
                break;
            case EditTarget::LAYER_B:
                p.applyTo(mgr.layerB(), midiCh, true);
                break;
            case EditTarget::BOTH:
                p.applyTo(mgr.layerA(), midiCh, true);
                p.applyTo(mgr.layerB(), midiCh, true);
                break;
        }
    }

    // Load a factory preset by global index to the current edit target.
    // Uses the existing Presets:: loader infrastructure unchanged.
    static void loadPresetToActive(LayerManager& mgr, int globalIdx,
                                    uint8_t midiCh = 1)
    {
        SynthEngine& engine = mgr.activeEngine();
        Presets::presets_loadByGlobalIndex(engine, globalIdx, midiCh);
    }

    // Load a factory preset to a specific layer by target.
    static void loadPresetToLayer(LayerManager& mgr, int globalIdx,
                                   EditTarget target, uint8_t midiCh = 1)
    {
        switch (target) {
            case EditTarget::LAYER_A:
                Presets::presets_loadByGlobalIndex(mgr.layerA(), globalIdx, midiCh);
                break;
            case EditTarget::LAYER_B:
                Presets::presets_loadByGlobalIndex(mgr.layerB(), globalIdx, midiCh);
                break;
            case EditTarget::BOTH:
                Presets::presets_loadByGlobalIndex(mgr.layerA(), globalIdx, midiCh);
                Presets::presets_loadByGlobalIndex(mgr.layerB(), globalIdx, midiCh);
                break;
        }
    }

private:
    // =========================================================================
    // Minimal JSON parsing helpers (no external library dependency)
    // =========================================================================

    // Extract an integer value after a key like "\"mode\":" from JSON string.
    static int _extractInt(const String& js, const char* key, int defaultVal) {
        int pos = js.indexOf(key);
        if (pos < 0) return defaultVal;
        pos += strlen(key);
        // Skip whitespace
        while (pos < (int)js.length() && js[pos] == ' ') ++pos;
        // Parse integer (handles negative)
        return js.substring(pos).toInt();
    }

    // Same as _extractInt but for floats. Uses Arduino's String::toFloat()
    // which returns 0.0 on parse failure — so the default is applied only
    // when the key is absent, not when the value is malformed. That's
    // acceptable for patch loading where a malformed float means "treat
    // this parameter as zero".
    static float _extractFloat(const String& js, const char* key, float defaultVal) {
        int pos = js.indexOf(key);
        if (pos < 0) return defaultVal;
        pos += strlen(key);
        while (pos < (int)js.length() && js[pos] == ' ') ++pos;
        return js.substring(pos).toFloat();
    }

    // Extract a nested JSON object {...} after a key like "\"a\":".
    // Returns the substring including braces, or empty string if not found.
    static String _extractObject(const String& js, const char* key) {
        int pos = js.indexOf(key);
        if (pos < 0) return "";
        pos += strlen(key);
        // Skip whitespace
        while (pos < (int)js.length() && js[pos] == ' ') ++pos;
        if (pos >= (int)js.length() || js[pos] != '{') return "";

        // Find matching closing brace (handles nesting)
        int depth = 0;
        int start = pos;
        for (int i = pos; i < (int)js.length(); ++i) {
            if (js[i] == '{') ++depth;
            else if (js[i] == '}') {
                --depth;
                if (depth == 0) {
                    return js.substring(start, i + 1);
                }
            }
        }
        return "";  // malformed — no matching brace
    }
};
