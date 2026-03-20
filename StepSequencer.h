// =============================================================================
// StepSequencer.h — Block-rate step sequencer for JT-8000 (v2 — unipolar)
//
// CHANGES FROM v1 (bipolar):
//   - Step values are UNIPOLAR: CC 0-127 → 0.0 … 1.0 (was bipolar ±1.0)
//   - Depth is BIPOLAR: CC 0=-1.0, CC 64=0.0, CC 127=+1.0 (was unipolar 0-1)
//   - Output = stepValue * depth  (range ±1.0)
//   - Gate close uses a 2ms linear ramp to zero (was instant snap → click)
//
// WHY:
//   Bipolar step values caused noise on the Amp destination because negative
//   step values subtracted from the amp mod DC level, creating zero-crossings
//   and phase inversions. With unipolar steps, the pattern is always 0-to-1
//   (how much), and the depth sign controls direction (add or subtract).
//   Pitch/Filter/PWM destinations still get full ±1.0 range via depth sign.
//
// STEP VALUES:
//   CC 0 = 0.0 (minimum), CC 127 = 1.0 (maximum). No midpoint convention.
//   All steps default to 0 (CC 0 = silent/no modulation).
//
// GATE RAMP:
//   When the gate closes, instead of snapping output to 0.0 (which creates
//   a discontinuity = click), the output ramps linearly to 0.0 over RAMP_MS.
//   This is imperceptible musically but eliminates the audible artefact.
//
// Everything else (timing, direction, slide, retrigger) is unchanged.
// =============================================================================

#pragma once
#include <Arduino.h>
#include "BPMClockManager.h"

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int   SEQ_MAX_STEPS = 16;
static constexpr float SEQ_GATE_RAMP_MS = 2.0f;  // anti-click ramp duration

// ─────────────────────────────────────────────────────────────────────────────
// Step direction modes
// ─────────────────────────────────────────────────────────────────────────────

enum SeqDirection : uint8_t {
    SEQ_DIR_FORWARD = 0,
    SEQ_DIR_REVERSE,
    SEQ_DIR_BOUNCE,
    SEQ_DIR_RANDOM,
    NUM_SEQ_DIRECTIONS
};

static const char* SeqDirectionNames[NUM_SEQ_DIRECTIONS]
    __attribute__((unused)) = {
    "Forward",
    "Reverse",
    "Bounce",
    "Random"
};

// ─────────────────────────────────────────────────────────────────────────────
// StepSequencer class
// ─────────────────────────────────────────────────────────────────────────────

class StepSequencer {
public:
    StepSequencer();

    // =====================================================================
    // Tick — call once per SynthEngine::update() loop iteration
    // =====================================================================
    void tick(float deltaMs);

    // =====================================================================
    // Output — read after tick()
    // =====================================================================

    /** Current output, scaled by depth. Range [−|depth| … +|depth|]. */
    float getOutput() const { return _output; }

    /** Current step index (0-based). */
    int getCurrentStep() const { return _currentStep; }

    /** Whether the gate is currently open. */
    bool isGateOpen() const { return _gateOpen; }

    // =====================================================================
    // Parameter setters
    // =====================================================================

    void setEnabled(bool on);
    bool isEnabled() const { return _enabled; }

    void setStepCount(int count);
    int  getStepCount() const { return _stepCount; }

    /**
     * Set the raw CC value for a single step.
     * CC 0 = 0.0 output, CC 127 = 1.0 output (UNIPOLAR).
     */
    void setStepValue(int step, uint8_t value);
    uint8_t getStepValue(int step) const;

    void  setGateLength(float fraction);
    float getGateLength() const { return _gateLength; }

    void  setSlide(float fraction);
    float getSlide() const { return _slide; }

    void         setDirection(SeqDirection dir);
    SeqDirection getDirection() const { return _direction; }

    /**
     * Depth — BIPOLAR: -1.0 to +1.0.
     * Positive = pattern adds to destination.
     * Negative = pattern subtracts from destination.
     * CC 0 = -1.0, CC 64 = 0.0, CC 127 = +1.0.
     */
    void  setDepth(float d);
    float getDepth() const { return _depth; }

    void  setRate(float hz);
    float getRate() const { return _rateHz; }

    void       setTimingMode(TimingMode mode);
    TimingMode getTimingMode() const { return _timingMode; }

    void updateFromBPMClock(const BPMClockManager& clock);

    void setRetrigger(bool on);
    bool getRetrigger() const { return _retrigger; }

    void reset();

private:
    // ─── Step data ───────────────────────────────────────────────────────
    uint8_t _stepValues[SEQ_MAX_STEPS];
    int     _stepCount  = 8;

    // ─── Playback state ──────────────────────────────────────────────────
    bool    _enabled     = false;
    int     _currentStep = 0;
    float   _phaseMs     = 0.0f;
    float   _stepDurationMs = 500.0f;
    bool    _gateOpen    = false;
    int     _bounceDir   = 1;

    // ─── Gate ramp (anti-click) ──────────────────────────────────────────
    float   _rampValue   = 0.0f;    // current ramp level (1.0 when gate open, ramps to 0)
    bool    _ramping     = false;   // true during the 2ms ramp-down after gate close

    // ─── Parameters ──────────────────────────────────────────────────────
    float        _gateLength = 1.0f;
    float        _slide      = 0.0f;
    float        _depth      = 0.0f;     // BIPOLAR: -1.0 to +1.0
    float        _rateHz     = 2.0f;
    SeqDirection _direction  = SEQ_DIR_FORWARD;
    TimingMode   _timingMode = TIMING_FREE;
    bool         _retrigger  = true;

    // ─── Output ──────────────────────────────────────────────────────────
    float _output = 0.0f;

    // ─── Internal helpers ────────────────────────────────────────────────
    void advanceStep();
    int  getNextStepIndex() const;
    void recalcDuration();

    /** Convert CC 0-127 to unipolar 0.0-1.0. */
    static float ccToUnipolar(uint8_t cc);
};
