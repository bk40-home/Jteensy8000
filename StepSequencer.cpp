// =============================================================================
// StepSequencer.cpp — Block-rate step sequencer (v2 — unipolar steps)
//
// KEY CHANGES FROM v1:
//   1. ccToUnipolar() replaces ccToBipolar() — CC 0→0.0, CC 127→1.0
//   2. Depth is bipolar (-1.0 to +1.0), output = step * depth
//   3. Gate close uses a 2ms linear ramp (SEQ_GATE_RAMP_MS) instead of
//      instant snap to zero. This eliminates the click artefact on Amp mode.
//   4. Steps default to 0 (CC 0 = no modulation) instead of 64 (old midpoint)
//
// CPU COST: Unchanged — one branch, one lerp, one multiply when active.
// The ramp adds one comparison + one multiply per tick during the 2ms window.
// =============================================================================

#include "StepSequencer.h"

// ─────────────────────────────────────────────────────────────────────────────
// Constructor — steps default to 0 (no modulation)
// ─────────────────────────────────────────────────────────────────────────────

StepSequencer::StepSequencer() {
    for (int i = 0; i < SEQ_MAX_STEPS; ++i) {
        _stepValues[i] = 0;   // CC 0 = 0.0 output (was 64 in v1)
    }
    recalcDuration();
}

// ─────────────────────────────────────────────────────────────────────────────
// Tick
// ─────────────────────────────────────────────────────────────────────────────

void StepSequencer::tick(float deltaMs) {
    // ── Early exit ────────────────────────────────────────────────────────
    if (!_enabled || _stepCount < 1) {
        // If we were ramping, let the ramp finish to avoid a click on disable
        if (_ramping) {
            _rampValue -= deltaMs / SEQ_GATE_RAMP_MS;
            if (_rampValue <= 0.0f) {
                _rampValue = 0.0f;
                _ramping   = false;
            }
            _output *= _rampValue;  // fade out the last held value
            return;
        }
        _output   = 0.0f;
        _gateOpen = false;
        return;
    }

    // Depth zero = no output, but still advance the sequencer so it stays
    // in sync (user might increase depth later and expect correct position)
    const bool hasOutput = (_depth != 0.0f);

    // ── Accumulate phase ──────────────────────────────────────────────────
    _phaseMs += deltaMs;

    while (_phaseMs >= _stepDurationMs) {
        _phaseMs -= _stepDurationMs;
        advanceStep();
    }

    // ── Phase fraction (0.0 … 1.0) ───────────────────────────────────────
    const float phaseFrac = (_stepDurationMs > 0.0f)
                          ? (_phaseMs / _stepDurationMs)
                          : 0.0f;

    // ── Gate check ────────────────────────────────────────────────────────
    const bool wasOpen = _gateOpen;
    _gateOpen = (phaseFrac < _gateLength);

    if (_gateOpen) {
        // Gate is open — compute the step value
        _ramping   = false;
        _rampValue = 1.0f;

        if (!hasOutput) {
            _output = 0.0f;
            return;
        }

        const float currentVal = ccToUnipolar(_stepValues[_currentStep]);
        float raw;

        if (_slide <= 0.0f) {
            raw = currentVal;
        } else {
            // Slide: interpolate toward next step
            const float nextVal = ccToUnipolar(_stepValues[getNextStepIndex()]);
            const float gateFrac = (_gateLength > 0.0f)
                                 ? (phaseFrac / _gateLength)
                                 : 0.0f;
            const float t = gateFrac * _slide;
            raw = currentVal + t * (nextVal - currentVal);
        }

        // Output = unipolar step value × bipolar depth
        _output = raw * _depth;

    } else {
        // Gate closed — ramp output to zero over SEQ_GATE_RAMP_MS
        if (wasOpen && !_gateOpen) {
            // Gate just closed this tick — start the ramp
            _ramping   = true;
            _rampValue = 1.0f;
        }

        if (_ramping) {
            _rampValue -= deltaMs / SEQ_GATE_RAMP_MS;
            if (_rampValue <= 0.0f) {
                _rampValue = 0.0f;
                _ramping   = false;
                _output    = 0.0f;
            } else {
                // Hold the last computed value but fade it
                _output *= _rampValue;
            }
        } else {
            _output = 0.0f;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Step advance
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
        _currentStep += _bounceDir;
        if (_currentStep >= _stepCount) {
            _currentStep = _stepCount - 2;
            _bounceDir = -1;
            if (_currentStep < 0) _currentStep = 0;
        } else if (_currentStep < 0) {
            _currentStep = 1;
            _bounceDir = 1;
            if (_currentStep >= _stepCount) _currentStep = 0;
        }
        break;

    case SEQ_DIR_RANDOM:
        if (_stepCount > 1) {
            int next;
            do { next = random(0, _stepCount); } while (next == _currentStep);
            _currentStep = next;
        }
        break;

    default:
        _currentStep = (_currentStep + 1) % _stepCount;
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Next step index
// ─────────────────────────────────────────────────────────────────────────────

int StepSequencer::getNextStepIndex() const {
    switch (_direction) {
    case SEQ_DIR_FORWARD:
        return (_currentStep + 1) % _stepCount;
    case SEQ_DIR_REVERSE:
        return (_currentStep - 1 + _stepCount) % _stepCount;
    case SEQ_DIR_BOUNCE: {
        int next = _currentStep + _bounceDir;
        if (next >= _stepCount) return _stepCount - 2 >= 0 ? _stepCount - 2 : 0;
        if (next < 0)          return 1 < _stepCount ? 1 : 0;
        return next;
    }
    case SEQ_DIR_RANDOM:
        return (_currentStep + 1) % _stepCount;
    default:
        return (_currentStep + 1) % _stepCount;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Unipolar conversion — CC 0-127 → 0.0 … 1.0
// ─────────────────────────────────────────────────────────────────────────────

float StepSequencer::ccToUnipolar(uint8_t cc) {
    return static_cast<float>(cc) / 127.0f;
}

// ─────────────────────────────────────────────────────────────────────────────
// Parameter setters
// ─────────────────────────────────────────────────────────────────────────────

void StepSequencer::setEnabled(bool on) {
    if (!on && _enabled) {
        // Starting disable — begin ramp-down instead of hard cut
        _ramping = true;
        _rampValue = 1.0f;
    }
    _enabled = on;
    if (!on) {
        _gateOpen = false;
    }
}

void StepSequencer::setStepCount(int count) {
    _stepCount = constrain(count, 1, SEQ_MAX_STEPS);
    if (_currentStep >= _stepCount) _currentStep = 0;
}

void StepSequencer::setStepValue(int step, uint8_t value) {
    if (step < 0 || step >= SEQ_MAX_STEPS) return;
    _stepValues[step] = constrain(value, 0, 127);
}

uint8_t StepSequencer::getStepValue(int step) const {
    if (step < 0 || step >= SEQ_MAX_STEPS) return 0;
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
    _bounceDir = 1;
}

void StepSequencer::setDepth(float d) {
    // Bipolar: -1.0 to +1.0
    _depth = constrain(d, -1.0f, 1.0f);
}

void StepSequencer::setRate(float hz) {
    _rateHz = constrain(hz, 0.05f, 50.0f);
    if (_timingMode == TIMING_FREE) recalcDuration();
}

void StepSequencer::setTimingMode(TimingMode mode) {
    _timingMode = mode;
    if (mode == TIMING_FREE) recalcDuration();
}

void StepSequencer::updateFromBPMClock(const BPMClockManager& clock) {
    if (_timingMode == TIMING_FREE) return;
    float ms = clock.getTimeForMode(_timingMode);
    if (ms > 0.0f) _stepDurationMs = ms;
}

void StepSequencer::setRetrigger(bool on) {
    _retrigger = on;
}

void StepSequencer::reset() {
    _currentStep = 0;
    _phaseMs     = 0.0f;
    _bounceDir   = 1;
    _gateOpen    = false;
    _ramping     = false;
    _rampValue   = 0.0f;
    _output      = 0.0f;
}

void StepSequencer::recalcDuration() {
    if (_rateHz > 0.0f) {
        _stepDurationMs = 1000.0f / _rateHz;
    } else {
        _stepDurationMs = 1000.0f;
    }
}
