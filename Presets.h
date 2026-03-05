#pragma once
#include <Arduino.h>
#include "SynthEngine.h"

// Nine basic templates: OSC1 full mix, tuning mid, filter open no res,
// drive=1, gain=1, ENV: A=0 D=0 S=1 R=0, no FX, no LFO.
namespace Presets {

  // Load the “init” template for a given OSC1 waveform index (0..8).
  // Applies by sending CC values through SynthEngine::handleControlChange
  // to keep everything aligned with your current CC pipeline.
  void loadInitTemplateByWave(SynthEngine& synth, uint8_t waveIndex);

  // Convenience: load by a simple 0..8 number (same as waveIndex).
  inline void loadTemplateByIndex(SynthEngine& synth, uint8_t idx) {
    loadInitTemplateByWave(synth, idx);
  }

  // Apply a 64-byte JT patch via CC map
  void loadRawPatchViaCC(SynthEngine& synth, const uint8_t data[64], uint8_t midiCh = 1);

  // Load one of the Microsphere bank patches by index [0..31]
  void loadMicrospherePreset(SynthEngine& synth, int index, uint8_t midiCh = 1);

  // Load one of the TUS (The Usual Suspects) JP-8000 patches by index [0..33]
  void loadTUSPreset(SynthEngine& synth, int index);

// Number of your existing init/templates (whatever you already expose)
int presets_templateCount();   // returns N (existing templates)

// Total = templates + Microsphere bank (32)
int presets_totalCount();

// Load by a single global index [0 .. totalCount-1]
void presets_loadByGlobalIndex(SynthEngine& synth, int globalIdx, uint8_t midiCh = 1);

// Optional: helper to get a display name for current index
const char* presets_nameByGlobalIndex(int globalIdx);


  // Optional: get a friendly name for UI/debug (“Init Saw”, etc.)
  const char* templateName(uint8_t idx);
}
