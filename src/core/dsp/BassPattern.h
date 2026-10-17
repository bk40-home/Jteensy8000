// =============================================================================
// BassPattern.h — the bass sequencer's PATTERN GENERATOR, standalone
// =============================================================================
//
// WHY THIS IS ITS OWN FILE
//   The generator is the one piece of the bass sequencer that every front end
//   needs, not just the engine: the JUCE plugin and the ESP32 controller both
//   draw the RESOLVED pattern (generated steps + hand edits) without asking the
//   engine for it.  Because the generator is deterministic, a front end running
//   the same code with the same parameters gets exactly the steps the engine
//   plays — no wire traffic, instant feedback while a knob turns.
//
//   For that to be safe the code must be the SAME code everywhere, so this
//   pair depends on nothing but <stdint.h> and is COPIED VERBATIM into the
//   ESP32 sketch (the same rule as ParamTable.h).  Never edit a copy — edit
//   this file and re-copy.  The playback side (clock, held keys, events) stays
//   in BassSequencer, which owns one of these.
//
// DESIGN (bass-seq/phase0-proposal.md, decisions 1, 6)
//   Clean-room (L1).  "Rolls, not results": one seeded xorshift32 stream fills
//   five uint16 rolls per step; the step flags are plain threshold compares
//   against the amounts, so raising an amount only ever ADDS (monotonic) and
//   an amount change costs one 32-step compare pass, run lazily.
//   The whole pattern is defined by (seed, mutation count) + amounts + the
//   hand edits — all ordinary saved parameters.
//
// CPU: generation work only on a parameter change (dirty flags); reading a
//   resolved step is an array read.  No float in the resolve pass.
//
// (c) 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>

namespace JT {

class BassPattern {
public:
    // ---- Capacities -------------------------------------------------------
    static constexpr int      kMaxSteps     = 32;
    static constexpr uint16_t kSeedMax      = 0x3FFF; // 14-bit NRPN value limit
    static constexpr uint16_t kMaxMutation  = 255;    // replay cost ceiling
    static constexpr int      kMaxRange     = 15;     // scale degrees of spread

    // Fraction of steps re-rolled by one mutate pass (x/65536 compare).
    static constexpr uint32_t kMutateThr = 0x4000u;   // 25 %

    // ---- Packed step format (fits one 14-bit NRPN value; FROZEN in patches)
    //   bits 0-4  scale degree 0..31
    //   bit  5    gate
    //   bit  6    accent
    //   bit  7    slide
    //   bits 8-9  octave: 0 = -1, 1 = 0, 2 = +1  (3 is treated as 0)
    //   bit  10   USER: this step is a hand edit (generator skips it)
    //   bits 11-13 spare
    static constexpr uint16_t kDegreeMask = 0x001Fu;
    static constexpr uint16_t kGateBit    = 1u << 5;
    static constexpr uint16_t kAccentBit  = 1u << 6;
    static constexpr uint16_t kSlideBit   = 1u << 7;
    static constexpr int      kOctShift   = 8;
    static constexpr uint16_t kOctMask    = 0x0300u;
    static constexpr uint16_t kUserBit    = 1u << 10;
    static constexpr uint16_t kPackedMask = 0x07FFu;  // all defined bits

    BassPattern();

    // ---- Generator inputs (clamp at the boundary; no-op when unchanged) ----
    void setSeed(uint16_t seed);        // 0..kSeedMax; does NOT reset mutation
    void setMutation(uint16_t count);   // 0..kMaxMutation
    void setDensity(float amt01);       // gate probability
    void setAccentAmount(float amt01);  // accent probability
    void setSlideAmount(float amt01);   // slide probability
    void setOctaveAmount(float amt01);  // octave-jump probability
    void setRange(int degrees);         // 1..kMaxRange
    void setLength(int steps);          // 1..kMaxSteps (the slide rule wraps here)

    // ---- Actions.  Each returns the new state for write-back to the store.
    uint16_t newRandomSeed();           // fresh seed, mutation -> 0; returns seed
    uint16_t mutate();                  // one more pass; returns new count
    bool     undo();                    // restore previous (seed, mutation)

    // ---- Hand edits ---------------------------------------------------------
    void setStepPacked(int step, uint16_t packed);  // kUserBit set = override
    void clearAllEdits();

    // ---- Resolved pattern -------------------------------------------------
    // Non-const: resolves lazily if an input changed since the last resolve.
    uint16_t        stepPacked(int step);
    // All kMaxSteps resolved steps (playback hot path: one dirty check, then
    // plain array reads).  Valid until the next setter call.
    const uint16_t* resolved() { ensureResolved(); return _steps; }

    uint16_t seed()       const { return _seed; }
    uint16_t mutation()   const { return _mutation; }
    uint32_t editedMask() const { return _editedMask; }
    int      length()     const { return _length; }

#ifdef JT_TESTING
    uint16_t debugRollPitch(int s) const
    { return (s >= 0 && s < kMaxSteps) ? _rolls[s].pitch : 0; }
    uint16_t debugRollGate(int s) const
    { return (s >= 0 && s < kMaxSteps) ? _rolls[s].gate : 0; }
#endif

private:
    // Five independent random values per step.  uint16 so a threshold compare
    // is a plain integer compare (no float in the resolve pass).
    struct Rolls { uint16_t gate, accent, slide, octave, pitch; };

    void rebuildRolls();                // seed + replay mutation passes
    void resolveTable();                // rolls + amounts + edits -> _steps
    void ensureResolved()
    {
        if (_rollsDirty) { rebuildRolls(); _rollsDirty = false; _tableDirty = true; }
        if (_tableDirty) { resolveTable(); _tableDirty = false; }
    }

    // House xorshift32 (same as StepSequencer / Arpeggiator).  State never 0.
    static uint32_t xorshift(uint32_t& s)
    { uint32_t x = s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; s = x; return x; }
    static uint32_t streamState(uint16_t seed, uint32_t salt);
    static void     rollStep(uint32_t& st, Rolls& r);
    static uint32_t amtToThr(float amt01);   // 0..1 -> 0..65536 compare threshold

    Rolls    _rolls[kMaxSteps];
    uint16_t _steps[kMaxSteps];          // resolved packed steps (incl. edits)
    uint16_t _edits[kMaxSteps];          // hand-edited values (valid where mask bit set)
    uint32_t _editedMask  = 0;
    bool     _rollsDirty  = true;        // seed/mutation changed
    bool     _tableDirty  = true;        // an amount / length / edit changed

    uint16_t _seed         = 0;
    uint16_t _mutation     = 0;
    uint16_t _undoSeed     = 0;
    uint16_t _undoMutation = 0;
    bool     _undoValid    = false;

    // Amount thresholds (0..65536; 65536 means "always").  Defaults are set in
    // the constructor to the params.yaml defaults.
    uint32_t _densityThr  = 0;
    uint32_t _accentThr   = 0;
    uint32_t _slideThr    = 0;
    uint32_t _octaveThr   = 0;
    int      _range       = 7;
    int      _length      = 16;

    uint32_t _seedRng = 0x9B05688Cu;     // newRandomSeed entropy
};

} // namespace JT
