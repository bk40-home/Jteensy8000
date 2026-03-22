#pragma once
#include <Arduino.h>
#include "SynthEngine.h"

// ---------------------------------------------------------------------------
// Presets.h — JT-8000 preset loading interface
//
// Three preset banks:
//   1. Init templates  (9 waveform-based defaults)
//   2. Microsphere     (32 patches from JP-8000 SysEx, 64-byte raw format)
//   3. TUS             (The Usual Suspects, full CC-array format)
//
// Global index = flat numbering across all banks for UI scrolling.
// ---------------------------------------------------------------------------
namespace Presets {

    // -- Init templates (one per waveform type) -------------------------------
    void loadInitTemplateByWave(SynthEngine& synth, uint8_t waveIndex);

    inline void loadTemplateByIndex(SynthEngine& synth, uint8_t idx) {
        loadInitTemplateByWave(synth, idx);
    }

    // -- Microsphere bank (64-byte raw SysEx → CC via Mapping.h) --------------
    void loadRawPatchViaCC(SynthEngine& synth, const uint8_t data[64], uint8_t midiCh = 1);
    void loadMicrospherePreset(SynthEngine& synth, int index, uint8_t midiCh = 1);

    // -- TUS bank (direct CC-array, no transform) -----------------------------
    void loadTUSPreset(SynthEngine& synth, int index);

    // -- Global index helpers -------------------------------------------------
    int         presets_templateCount();
    int         presets_totalCount();
    void        presets_loadByGlobalIndex(SynthEngine& synth, int globalIdx, uint8_t midiCh = 1);
    const char* presets_nameByGlobalIndex(int globalIdx);

    // -- Display name for init templates --------------------------------------
    const char* templateName(uint8_t idx);
}
