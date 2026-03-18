// =============================================================================
// StepSequencer.h — Block-rate step sequencer for JT-8000
//
// OVERVIEW:
//   A 1–16 step analogue-style sequencer that outputs a single bipolar float
//   (−1.0 … +1.0) each time tick() is called.  The output can be routed to
//   any modulation destination (Pitch, Filter, PWM, Amp) by SynthEngine.
//
//   This class is pure computation — it owns no AudioStream objects.
//   SynthEngine owns the DC generator(s) and mixer gain routing.
//
// STEP VALUES:
//   Each step stores a raw CC value (0–127).  64 = midpoint (output 0.0).
//   Below 64 → negative, above 64 → positive.  Bipolar encoding matches
//   the project convention for detune and pitch bend.
//
// GATE:
//   Gate length (0–100%) controls what fraction of a step the value is held.
//   After the gate closes, the output snaps to 0.0 (midpoint / silence).
//   100% gate = full legato (no gap between steps).
//
// SLIDE:
//   Slide amount (0–100%) controls linear interpolation between the current
//   step target and the next step target.  0% = hard step, 100% = full
//   portamento across the step duration.  Slide is applied during the gate-
//   open portion only; once the gate closes, output is 0.0 regardless.
//
// TIMING:
//   Free-running (Hz) or BPM-synced via the shared TimingMode enum.
//   When BPM-synced, step duration is derived from BPMClockManager.
//
// DIRECTION:
//   Forward / Reverse / Bounce (ping-pong) / Random.
//
// RETRIGGER:
//   When enabled, noteOn resets the sequencer to step 0 and restarts the
//   phase accumulator.  When disabled, the sequencer free-runs continuously.
//
// MULTI-CHANNEL FUTURE:
//   This class is self-contained and stateless with respect to routing.
//   SynthEngine can instantiate multiple StepSequencer objects (seq1, seq2…)
//   each with its own CC bank.  The class itself has no knowledge of
//   destinations or audio wiring.
// =============================================================================

#pragma once
#include <Arduino.h>
#include "BPMClockManager.h"  // TimingMode enum, BPMClockManager class

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int   SEQ_MAX_STEPS = 16;
static constexpr float SEQ_BIPOLAR_MID = 64.0f;  // CC midpoint → 0.0 output

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

// Re-use LFODestination for routing — same targets, same enum.
// Declared in LFOBlock.h: LFO_DEST_NONE, LFO_DEST_PITCH, LFO_DEST_FILTER,
//                          LFO_DEST_PWM, LFO_DEST_AMP.

// ─────────────────────────────────────────────────────────────────────────────
// StepSequencer class
// ─────────────────────────────────────────────────────────────────────────────

class StepSequencer {
public:
    StepSequencer();

    // =====================================================================
    // Tick — call once per SynthEngine::update() loop iteration
    // =====================================================================

    /**
     * @brief Advance the sequencer by deltaMs milliseconds and compute output.
     *
     * @param deltaMs  Time since last tick in milliseconds.
     *                 SynthEngine tracks this via micros() delta.
     *
     * Call this every loop iteration.  When the sequencer is disabled or
     * depth is zero, the function returns immediately (no computation).
     */
    void tick(float deltaMs);

    // =====================================================================
    // Output — read after tick()
    // =====================================================================

    /**
     * @brief Current sequencer output, scaled by depth.
     * @return Bipolar float in range [−depth … +depth].
     *         0.0 when disabled, depth is zero, or gate is closed.
     */
    float getOutput() const { return _output; }

    /**
     * @brief Current step index (0-based).  Useful for UI display.
     */
    int getCurrentStep() const { return _currentStep; }

    /**
     * @brief Whether the gate is currently open on the active step.
     */
    bool isGateOpen() const { return _gateOpen; }

    // =====================================================================
    // Parameter setters
    // =====================================================================

    /** @brief Enable or disable the sequencer.  When disabled, output = 0. */
    void setEnabled(bool on);
    bool isEnabled() const { return _enabled; }

    /** @brief Set the number of active steps (clamped 1–16). */
    void setStepCount(int count);
    int  getStepCount() const { return _stepCount; }

    /**
     * @brief Set the raw CC value for a single step.
     * @param step  Step index (0–15).
     * @param value Raw CC value (0–127).  64 = zero output.
     */
    void setStepValue(int step, uint8_t value);
    uint8_t getStepValue(int step) const;

    /** @brief Gate length as a fraction (0.0 = trigger, 1.0 = legato). */
    void  setGateLength(float fraction);
    float getGateLength() const { return _gateLength; }

    /** @brief Slide/glide amount (0.0 = hard step, 1.0 = full portamento). */
    void  setSlide(float fraction);
    float getSlide() const { return _slide; }

    /** @brief Step traversal direction. */
    void         setDirection(SeqDirection dir);
    SeqDirection getDirection() const { return _direction; }

    /** @brief Master output depth (0.0–1.0).  Scales the bipolar output. */
    void  setDepth(float d);
    float getDepth() const { return _depth; }

    /** @brief Free-running rate in Hz (used when timingMode == TIMING_FREE). */
    void  setRate(float hz);
    float getRate() const { return _rateHz; }

    /** @brief Timing mode — free-running or BPM-synced division. */
    void       setTimingMode(TimingMode mode);
    TimingMode getTimingMode() const { return _timingMode; }

    /**
     * @brief Update step duration from BPM clock.
     *        Call from SynthEngine::updateBPMSync() when in synced mode.
     */
    void updateFromBPMClock(const BPMClockManager& clock);

    /** @brief If true, noteOn resets to step 0. */
    void setRetrigger(bool on);
    bool getRetrigger() const { return _retrigger; }

    /** @brief Reset to step 0 and restart phase.  Called on noteOn if retrigger is enabled. */
    void reset();

private:
    // ─── Step data ───────────────────────────────────────────────────────
    uint8_t _stepValues[SEQ_MAX_STEPS];  // Raw CC 0–127 per step
    int     _stepCount  = 8;             // Active steps (1–16)

    // ─── Playback state ──────────────────────────────────────────────────
    bool    _enabled    = false;
    int     _currentStep = 0;            // Current step index
    float   _phaseMs    = 0.0f;          // Milliseconds elapsed within current step
    float   _stepDurationMs = 500.0f;    // Duration of one step in ms
    bool    _gateOpen   = false;         // Is the gate currently open?
    int     _bounceDir  = 1;             // +1 forward, −1 reverse (bounce mode)

    // ─── Parameters ──────────────────────────────────────────────────────
    float        _gateLength = 1.0f;     // 0.0–1.0 fraction of step
    float        _slide      = 0.0f;     // 0.0–1.0 glide amount
    float        _depth      = 0.0f;     // 0.0–1.0 master output scale
    float        _rateHz     = 2.0f;     // Free-running step rate
    SeqDirection _direction  = SEQ_DIR_FORWARD;
    TimingMode   _timingMode = TIMING_FREE;
    bool         _retrigger  = true;

    // ─── Output ──────────────────────────────────────────────────────────
    float _output = 0.0f;                // Scaled bipolar output

    // ─── Internal helpers ────────────────────────────────────────────────

    /** @brief Advance to the next step based on direction mode. */
    void advanceStep();

    /** @brief Convert raw CC value (0–127) to bipolar float (−1.0 … +1.0). */
    static float ccToBipolar(uint8_t cc);

    /** @brief Recalculate _stepDurationMs from current rate/timing settings. */
    void recalcDuration();

    /**
     * @brief Get the step index that follows a given step.
     *        Used for slide interpolation (peek at next target).
     */
    int getNextStepIndex() const;
};
