/* Audio Library for Teensy
 * Copyright (c) 2025, Kris Bishop, bishopkris40@hotmail.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

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
// BUG FIXES (v2.1):
//   FIX 1 — Ramp compounding (_output *= _rampValue was called every tick,
//            causing a geometric fade instead of a linear one). The output
//            value is now frozen into _lastGateOutput the moment the gate
//            closes, and the ramp uses  _output = _lastGateOutput * _rampValue
//            so each tick fades from the same held value.
//
//   FIX 2 — SEQ_TIMING_MODE CC decode is in SynthEngine.cpp (see that file).
//            The constrain(value, 0, 11) there treated CC value as a direct
//            mode index; it now uses the same proportional bucket decode as
//            LFO1/LFO2/Delay timing modes.
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
// Tick — called once per engine update loop
// ─────────────────────────────────────────────────────────────────────────────

void StepSequencer::tick(float deltaMs) {

    // ── Early exit (disabled or empty) ───────────────────────────────────
    if (!_enabled || _stepCount < 1) {
        if (_ramping) {
            // Let the ramp finish gracefully even after disable,
            // to avoid a click from an abrupt output cut.
            _rampValue -= deltaMs / SEQ_GATE_RAMP_MS;
            if (_rampValue <= 0.0f) {
                _rampValue = 0.0f;
                _ramping   = false;
            }
            // FIX 1: fade from the frozen gate-close value, not from
            // a value that has already been multiplied down by previous ticks.
            _output = _lastGateOutput * _rampValue;
            return;
        }
        _output   = 0.0f;
        _gateOpen = false;
        return;
    }

    // Depth zero = no output, but still advance the sequencer position so it
    // stays in sync (user may increase depth later and expect correct step).
    const bool hasOutput = (_depth != 0.0f);

    // ── Accumulate phase ──────────────────────────────────────────────────
    _phaseMs += deltaMs;

    // Step advance: consume whole step durations, wrapping phase remainder
    while (_phaseMs >= _stepDurationMs) {
        _phaseMs -= _stepDurationMs;
        advanceStep();
    }

    // Phase fraction within current step: 0.0 (start) … 1.0 (end)
    const float phaseFrac = (_stepDurationMs > 0.0f)
                          ? (_phaseMs / _stepDurationMs)
                          : 0.0f;

    // ── Gate check ────────────────────────────────────────────────────────
    const bool wasOpen = _gateOpen;
    _gateOpen = (phaseFrac < _gateLength);

    if (_gateOpen) {

        // Gate open — compute output for this step
        _ramping   = false;
        _rampValue = 1.0f;

        if (!hasOutput) {
            _output = 0.0f;
            return;
        }

        const float currentVal = ccToUnipolar(_stepValues[_currentStep]);
        float raw;

        if (_slide <= 0.0f) {
            // No slide — hold step value flat
            raw = currentVal;
        } else {
            // Slide: linearly interpolate toward the next step value.
            // t reaches _slide at the end of the gate (not the full step),
            // so a slide of 1.0 means the value reaches the next step exactly
            // at gate close.
            const float nextVal  = ccToUnipolar(_stepValues[getNextStepIndex()]);
            const float gateFrac = (_gateLength > 0.0f)
                                 ? (phaseFrac / _gateLength)
                                 : 0.0f;
            const float t = gateFrac * _slide;
            raw = currentVal + t * (nextVal - currentVal);
        }

        // Output = unipolar step value × bipolar depth → range ±1.0
        _output = raw * _depth;

        // FIX 1: freeze the output value so the ramp has a stable reference
        // if the gate closes this tick or on a subsequent tick.
        _lastGateOutput = _output;

    } else {

        // Gate closed — ramp output to zero over SEQ_GATE_RAMP_MS (anti-click)
        if (wasOpen && !_gateOpen) {
            // Gate just closed — begin ramp. _lastGateOutput already holds
            // the final gate-open value set above.
            _ramping   = true;
            _rampValue = 1.0f;
        }

        if (_ramping) {
            _rampValue -= deltaMs / SEQ_GATE_RAMP_MS;

            if (_rampValue <= 0.0f) {
                // Ramp complete
                _rampValue = 0.0f;
                _ramping   = false;
                _output    = 0.0f;
            } else {
                // FIX 1: linear fade from the frozen gate-close value.
                // Previously _output *= _rampValue compounded each tick
                // (geometric decay), making the fade faster than intended.
                _output = _lastGateOutput * _rampValue;
            }
        } else {
            _output = 0.0f;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// advanceStep — move _currentStep forward by one according to direction mode
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
        // Walk forward or backward; reverse direction at each end.
        _currentStep += _bounceDir;
        if (_currentStep >= _stepCount) {
            _currentStep = _stepCount - 2;   // one step back from the end
            _bounceDir   = -1;
            if (_currentStep < 0) _currentStep = 0;  // guard: only 1 step
        } else if (_currentStep < 0) {
            _currentStep = 1;                // one step in from the start
            _bounceDir   = 1;
            if (_currentStep >= _stepCount) _currentStep = 0; // guard
        }
        break;

    case SEQ_DIR_RANDOM:
        // Pick any step other than the current one (avoids immediate repeats).
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
// getNextStepIndex — preview the index that advanceStep() would reach next.
//                    Used by the slide calculation to know the target value.
// ─────────────────────────────────────────────────────────────────────────────

int StepSequencer::getNextStepIndex() const {
    switch (_direction) {

    case SEQ_DIR_FORWARD:
        return (_currentStep + 1) % _stepCount;

    case SEQ_DIR_REVERSE:
        return (_currentStep - 1 + _stepCount) % _stepCount;

    case SEQ_DIR_BOUNCE: {
        int next = _currentStep + _bounceDir;
        if (next >= _stepCount) return (_stepCount - 2 >= 0) ? _stepCount - 2 : 0;
        if (next < 0)           return (1 < _stepCount)      ? 1               : 0;
        return next;
    }

    case SEQ_DIR_RANDOM:
        // Random target is unknowable in advance; use the next linear step
        // as a slide target approximation (slide + random is an edge case).
        return (_currentStep + 1) % _stepCount;

    default:
        return (_currentStep + 1) % _stepCount;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ccToUnipolar — CC 0-127 → 0.0 … 1.0
//                Marked static; no division guard needed (127 is a constant).
// ─────────────────────────────────────────────────────────────────────────────

float StepSequencer::ccToUnipolar(uint8_t cc) {
    return static_cast<float>(cc) * (1.0f / 127.0f);  // multiply is cheaper than divide
}

// ─────────────────────────────────────────────────────────────────────────────
// Parameter setters
// ─────────────────────────────────────────────────────────────────────────────

void StepSequencer::setEnabled(bool on) {
    if (!on && _enabled) {
        // Disabling while running — start ramp-down so output fades cleanly.
        _ramping        = true;
        _rampValue      = 1.0f;
        _lastGateOutput = _output;   // freeze current output as ramp reference
    }
    _enabled  = on;
    if (!on) _gateOpen = false;
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
    _bounceDir = 1;   // always reset bounce direction on mode change
}

void StepSequencer::setDepth(float d) {
    _depth = constrain(d, -1.0f, 1.0f);
}

void StepSequencer::setRate(float hz) {
    _rateHz = constrain(hz, 0.05f, 50.0f);
    // Only recalculate duration in free-running mode; sync mode duration
    // is owned by updateFromBPMClock().
    if (_timingMode == TIMING_FREE) recalcDuration();
}

void StepSequencer::setTimingMode(TimingMode mode) {
    _timingMode = mode;
    // Restore free-rate duration when returning to free mode.
    // When switching TO a sync mode, the caller (SynthEngine) is responsible
    // for calling updateFromBPMClock() immediately after to apply BPM duration.
    if (mode == TIMING_FREE) recalcDuration();
}

void StepSequencer::updateFromBPMClock(const BPMClockManager& clock) {
    // No-op in free mode — duration is set by recalcDuration() / setRate().
    if (_timingMode == TIMING_FREE) return;
    const float ms = clock.getTimeForMode(_timingMode);
    if (ms > 0.0f) _stepDurationMs = ms;
}

void StepSequencer::setRetrigger(bool on) {
    _retrigger = on;
}

void StepSequencer::reset() {
    _currentStep    = 0;
    _phaseMs        = 0.0f;
    _bounceDir      = 1;
    _gateOpen       = false;
    _ramping        = false;
    _rampValue      = 0.0f;
    _lastGateOutput = 0.0f;
    _output         = 0.0f;
}

// ─────────────────────────────────────────────────────────────────────────────
// recalcDuration — convert _rateHz to milliseconds per step.
//                  Only meaningful in TIMING_FREE mode.
// ─────────────────────────────────────────────────────────────────────────────

void StepSequencer::recalcDuration() {
    _stepDurationMs = (_rateHz > 0.0f) ? (1000.0f / _rateHz) : 1000.0f;
}