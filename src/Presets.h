/*
 * Presets.h — JT-8000 Preset API
 *
 * Thin wrapper around FactoryBank. Keeps the same public API so
 * PresetBrowser, .ino, and UI code don't need changes.
 *
 * Removed: loadInitTemplateByWave, loadRawPatchViaCC,
 *          loadMicrospherePreset, loadTUSPreset, templateName
 */

#pragma once
#include <Arduino.h>
#include "SynthEngine.h"

namespace Presets {

    // Total number of factory presets (inits + factory sounds)
    int presets_totalCount();

    // Load a preset by global index [0 .. totalCount-1], wraps around
    void presets_loadByGlobalIndex(SynthEngine& synth, int globalIdx, uint8_t midiCh = 1);

    // Display name for a preset index
    const char* presets_nameByGlobalIndex(int globalIdx);

    // Number of init-waveform presets at the start of the bank
    int presets_initCount();

    // Backward-compatible alias (used by PresetBrowser)
    inline int presets_templateCount() { return presets_initCount(); }

} // namespace Presets