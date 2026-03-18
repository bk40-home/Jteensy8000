// =============================================================================
// StepSequencer.cpp — Block-rate step sequencer implementation
//
// CPU COST:
//   When disabled or depth == 0: one branch, immediate return (~0 cycles).
//   When active: one float comparison, one possible step advance (rare),
//   one lerp, one multiply.  Negligible vs. any AudioStream::update().
//
// TIMING:
//   All timing is wall-clock based (milliseconds from SynthEngine's micros()
//   delta).  No sample counting, no audio-rate work.
// =============================================================================

#include "StepSequencer.h"

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

StepSequencer::StepSequencer() {
    // Initialise all steps to midpoint (CC 64 = 0.0 output)
    for (int i = 0; i < SEQ_MAX_STEPS; ++i) {
        _stepValues[i] = 64;
    }
    recalcDuration();
}

// ─────────────────────────────────────────────────────────────────────────────
// Tick — main update, called once per SynthEngine::update() iteration
// ─────────────────────────────────────────────────────────────────────────────

void StepSequencer::tick(float deltaMs) {
    // ── Early exit: nothing to compute ───────────────────────────────────
    if (!_enabled || _depth <= 0.0f || _stepCount < 1) {
        _output = 0.0f;
        _gateOpen = false;
        return;
    }

    // ── Accumulate phase within current step ─────────────────────────────
    _phaseMs += deltaMs;

    // ── Step advance: has the current step's duration elapsed? ────────────
    //    Use a while loop to handle the (rare) case where deltaMs spans
    //    more than one full step (e.g. very fast rate or long frame gap).
    while (_phaseMs >= _stepDurationMs) {
        _phaseMs -= _stepDurationMs;
        advanceStep();
    }

    // ── Phase fraction within current step (0.0 … 1.0) ──────────────────
    const float phaseFrac = (_stepDurationMs > 0.0f)
                          ? (_phaseMs / _stepDurationMs)
                          : 0.0f;

    // ── Gate check ───────────────────────────────────────────────────────
    _gateOpen = (phaseFrac < _gateLength);

    if (!_gateOpen) {
        // Gate closed — output is zero (bipolar midpoint)
        _output = 0.0f;
        return;
    }

    // ── Compute raw bipolar value for current step ───────────────────────
    const float currentVal = ccToBipolar(_stepValues[_currentStep]);

    float raw;
    if (_slide <= 0.0f) {
        // No slide — hard step value
        raw = currentVal;
    } else {
        // Slide: linear interpolation toward next step's value.
        // The slide fraction ramps from currentVal to nextVal over the
        // gate-open portion of the step.  At slide = 1.0, the value
        // reaches the next step's target exactly when the gate closes
        // (or when the step ends if gate = 100%).
        const float nextVal = ccToBipolar(_stepValues[getNextStepIndex()]);

        // Normalise phaseFrac to the gate-open region (0.0 … 1.0)
        const float gateFrac = (_gateLength > 0.0f)
                             ? (phaseFrac / _gateLength)
                             : 0.0f;

        // Blend: at gateFrac=0 → currentVal, at gateFrac=1 → nextVal
        // Scale by _slide so partial slide only moves part-way.
        const float t = gateFrac * _slide;
        raw = currentVal + t * (nextVal - currentVal);
    }

    // ── Apply master depth scaling ───────────────────────────────────────
    _output = raw * _depth;
}

// ─────────────────────────────────────────────────────────────────────────────
// Step advance — direction-dependent
// ─────────────────────────────────────────────────────────────────────────────

void StepSequencer::advanceStep() {
    switch (_direction) {

    case SEQ_DIR_FORWARD:
        _currentStep = (_currentStep + 1) % _stepCount;
        break;

    case SEQ_DIR_REVERSE:
        _currentStep = (_currentStep - 1 + _stepCount) % _stepCount;
        break;

    case SEQ_DIR_BOUNCE:
        // Ping-pong: reverse direction at boundaries
        _currentStep += _bounceDir;
        if (_currentStep >= _stepCount) {
            _currentStep = _stepCount - 2;  // Step back inside range
            _bounceDir = -1;
            if (_currentStep < 0) _currentStep = 0;  // Single-step safety
        } else if (_currentStep < 0) {
            _currentStep = 1;
            _bounceDir = 1;
            if (_currentStep >= _stepCount) _currentStep = 0;
        }
        break;

    case SEQ_DIR_RANDOM:
        // Random step within active range (avoid repeating same step)
        if (_stepCount > 1) {
            int next;
            do {
                next = random(0, _stepCount);
            } while (next == _currentStep);
            _currentStep = next;
        }
        break;

    default:
        _currentStep = (_currentStep + 1) % _stepCount;
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Next step index — peek ahead for slide interpolation
// ─────────────────────────────────────────────────────────────────────────────

int StepSequencer::getNextStepIndex() const {
    switch (_direction) {

    case SEQ_DIR_FORWARD:
        return (_currentStep + 1) % _stepCount;

    case SEQ_DIR_REVERSE:
        return (_currentStep - 1 + _stepCount) % _stepCount;

    case SEQ_DIR_BOUNCE: {
        int next = _currentStep + _bounceDir;
        // If the next step would exceed bounds, the direction will flip
        if (next >= _stepCount) return _stepCount - 2 >= 0 ? _stepCount - 2 : 0;
        if (next < 0)          return 1 < _stepCount ? 1 : 0;
        return next;
    }

    case SEQ_DIR_RANDOM:
        // Random mode: can't predict, use forward wrap as best guess
        return (_currentStep + 1) % _stepCount;

    default:
        return (_currentStep + 1) % _stepCount;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Bipolar conversion — CC 0–127 → −1.0 … +1.0
// ─────────────────────────────────────────────────────────────────────────────

float StepSequencer::ccToBipolar(uint8_t cc) {
    // Dead-zone at midpoint: CC 64 → exactly 0.0
    // This avoids the "no integer midpoint" issue with (cc/127)*2−1.
    if (cc == 64) return 0.0f;
    return (static_cast<float>(cc) / 127.0f) * 2.0f - 1.0f;
}

// ─────────────────────────────────────────────────────────────────────────────
// Parameter setters
// ─────────────────────────────────────────────────────────────────────────────

void StepSequencer::setEnabled(bool on) {
    _enabled = on;
    if (!on) {
        _output = 0.0f;
        _gateOpen = false;
    }
}

void StepSequencer::setStepCount(int count) {
    _stepCount = constrain(count, 1, SEQ_MAX_STEPS);
    // Clamp current step if it's now out of range
    if (_currentStep >= _stepCount) {
        _currentStep = 0;
    }
}

void StepSequencer::setStepValue(int step, uint8_t value) {
    if (step < 0 || step >= SEQ_MAX_STEPS) return;
    _stepValues[step] = constrain(value, 0, 127);
}

uint8_t StepSequencer::getStepValue(int step) const {
    if (step < 0 || step >= SEQ_MAX_STEPS) return 64;  // Midpoint default
    return _stepValues[step];
}

void StepSequencer::setGateLength(float fraction) {
    _gateLength = constrain(fraction, 0.0f, 1.0f);
}

void StepSequencer::setSlide(float fraction) {
    _slide = constrain(fraction, 0.0f, 1.0f);
}

void StepSequencer::setDirection(SeqDirection dir) {
    _direction = (dir < NUM_SEQ_DIRECTIONS) ? dir : SEQ_DIR_FORWARD;
    // Reset bounce direction when switching modes
    _bounceDir = 1;
}

void StepSequencer::setDepth(float d) {
    _depth = constrain(d, 0.0f, 1.0f);
    if (_depth <= 0.0f) {
        _output = 0.0f;
    }
}

void StepSequencer::setRate(float hz) {
    _rateHz = constrain(hz, 0.05f, 50.0f);
    if (_timingMode == TIMING_FREE) {
        recalcDuration();
    }
}

void StepSequencer::setTimingMode(TimingMode mode) {
    _timingMode = mode;
    if (mode == TIMING_FREE) {
        recalcDuration();
    }
    // BPM-synced duration is set by updateFromBPMClock()
}

void StepSequencer::updateFromBPMClock(const BPMClockManager& clock) {
    if (_timingMode == TIMING_FREE) return;

    // Each step occupies one musical division
    // e.g. TIMING_1_16 at 120 BPM → 125 ms per step
    float ms = clock.getTimeForMode(_timingMode);
    if (ms > 0.0f) {
        _stepDurationMs = ms;
    }
}

void StepSequencer::setRetrigger(bool on) {
    _retrigger = on;
}

// ─────────────────────────────────────────────────────────────────────────────
// Reset — called on noteOn when retrigger is enabled
// ─────────────────────────────────────────────────────────────────────────────

void StepSequencer::reset() {
    _currentStep = 0;
    _phaseMs     = 0.0f;
    _bounceDir   = 1;
    _gateOpen    = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal: recalculate step duration from free-running rate
// ─────────────────────────────────────────────────────────────────────────────

void StepSequencer::recalcDuration() {
    // Duration = 1000ms / rateHz  (one step per cycle)
    if (_rateHz > 0.0f) {
        _stepDurationMs = 1000.0f / _rateHz;
    } else {
        _stepDurationMs = 1000.0f;  // Fallback: 1 Hz
    }
}
