#pragma once
#include <Arduino.h>
//#include "CCMap.h"       // use your existing ccMap pages/rows
#include "SynthEngine.h" // for getters and CC apply
#include "Mapping.h"     // JT8000Map: conversions for capture from engine

// -----------------------------------------------------------------------------
// Patch: CC-centric snapshot of a sound. 
// - Only stores CCs that your UI actually uses (discovered from ccMap).
// - Apply: calls SynthEngine::handleControlChange(ch, cc, val).
// - Capture: converts engine getters -> CC values (uses Mapping.h curves).
// -----------------------------------------------------------------------------
struct Patch {
  // Optional metadata
  char     name[24]   = "Init";
  uint8_t  version    = 1;

  // Storage: for each CC (0..127), whether we store it and the last value
  bool     has[128];            // initialized in clear()
  uint8_t  value[128];          // 0..127

  Patch() { clear(); }

  // Reset contents
  void clear() {
    for (int i = 0; i < 128; ++i) { has[i] = false; value[i] = 0; }
  }

  // Set / get a single CC explicitly
  void setCC(uint8_t cc, uint8_t v) { has[cc] = true; value[cc] = v; }
  bool getCC(uint8_t cc, uint8_t &out) const { if (!has[cc]) return false; out = value[cc]; return true; }

  // Build the set of CCs used by the UI pages (unique set from ccMap)
  // Returns how many CCs were found
  int buildUsedCCList(uint8_t* outList, int maxCount) const;

  // Capture engine state into CCs (only those present in ccMap)
  void captureFrom(SynthEngine& synth);

  // Apply CCs to engine (optionally batch with AudioNoInterrupts)
  void applyTo(SynthEngine& synth, uint8_t midiChannel = 1, bool batch = true) const;

  // Serialize as compact JSON: {"name":"...", "v":1, "cc":{"23":64,"24":80,...}}
  String toJson() const;

  // Parse JSON of the above form (very light parser; tolerant of spacing)
  bool fromJson(const String& js);
};
