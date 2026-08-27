// =============================================================================
// StepSequencer.cpp — JT-8000 v2 step sequencer implementation
// =============================================================================
// Logic ported 1:1 from v1 StepSequencer.cpp (see docs/PHASE7_SEQUENCER_SPEC.md
// for the file:line diagnosis).  Substitutions only: Arduino random() -> house
// xorshift (D-2); Arduino constrain() -> local clamps; tempo-sync inert (D-1).
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#include "core/dsp/StepSequencer.h"

namespace JT {

namespace {
inline int   clampi(int v, int lo, int hi)     { return v < lo ? lo : (v > hi ? hi : v); }
inline float clampf(float v, float lo, float hi){ return v < lo ? lo : (v > hi ? hi : v); }
} // namespace

StepSequencer::StepSequencer()
{
    recalcDuration();
}

// ---------------------------------------------------------------------------
// tick — v1 StepSequencer.cpp:66-176, ported verbatim.
// ---------------------------------------------------------------------------
void StepSequencer::tick(float deltaMs)
{
    // Early exit (disabled or empty) — but finish an in-flight ramp so
    // disabling mid-note doesn't click (v1 :70-85).
    if (!_enabled || _stepCount < 1) {
        _auxOutput = 0.0f;   // never leave a stale aux mod latched when disabled
        if (_ramping) {
            _rampValue -= deltaMs / kGateRampMs;
            if (_rampValue <= 0.0f) { _rampValue = 0.0f; _ramping = false; }
            _output = _lastGateOutput * _rampValue;   // fade from frozen value
            return;
        }
        _output = 0.0f;
        _gateOpen = false;
        return;
    }

    // Aux lane defaults to 0 for this tick; the gate-open branch below fills
    // it.  On gate-close/ramp/rest the aux lane simply reads 0 — its
    // destinations (cutoff/pan/send/drive) are block-rate smoothed at their own
    // stage, so they need no amp-style anti-click ramp here (D-B2).
    _auxOutput = 0.0f;

    // Depth zero still advances position (stay in phase if depth raised later).
    const bool hasOutput = (_depth != 0.0f);

    _phaseMs += deltaMs;
    while (_phaseMs >= _stepDurationMs) {
        _phaseMs -= _stepDurationMs;
        advanceStep();
    }

    const float phaseFrac = (_stepDurationMs > 0.0f)
                          ? (_phaseMs / _stepDurationMs) : 0.0f;

    const bool wasOpen = _gateOpen;
    _gateOpen = (phaseFrac < _gateLength);

    if (_gateOpen) {
        _ramping = false;
        _rampValue = 1.0f;

        // --- AUX LANE (Stage B) ---
        // Computed here, while the gate is open, using the SAME step index,
        // phase and slide as the gate lane but the aux values/depth.  Done
        // BEFORE the gate-depth early-return below so a zero GATE depth never
        // silences a depthed aux lane (the two lanes are independent).  Skips
        // its own work when the aux lane is idle (dest None or depth 0).
        if (_auxDest != SeqAuxDest::None && _auxDepth != 0.0f) {
            const float auxCur = stepValueOf(_auxValues[_currentStep], _auxBipolar);
            float auxRaw;
            if (_slide <= 0.0f) {
                auxRaw = auxCur;
            } else {
                const float auxNext  = stepValueOf(_auxValues[nextStepIndex()], _auxBipolar);
                const float gateFrac = (_gateLength > 0.0f) ? (phaseFrac / _gateLength) : 0.0f;
                const float t = gateFrac * _slide;
                auxRaw = auxCur + t * (auxNext - auxCur);
            }
            _auxOutput = auxRaw * _auxDepth;          // unipolar × bipolar
        }

        if (!hasOutput) { _output = 0.0f; return; }

        const float currentVal = stepValueOf(_stepValues[_currentStep], _stepBipolar);
        float raw;
        if (_slide <= 0.0f) {
            raw = currentVal;                         // hold flat
        } else {
            // Slide toward the next step; slide 1.0 reaches it exactly at gate
            // close (v1 :124-138).
            const float nextVal  = stepValueOf(_stepValues[nextStepIndex()], _stepBipolar);
            const float gateFrac = (_gateLength > 0.0f) ? (phaseFrac / _gateLength) : 0.0f;
            const float t = gateFrac * _slide;
            raw = currentVal + t * (nextVal - currentVal);
        }

        _output = raw * _depth;                       // step value x depth
        _lastGateOutput = _output;                    // freeze for ramp (v2.1 FIX 1)
    } else {
        if (wasOpen && !_gateOpen) { _ramping = true; _rampValue = 1.0f; }

        if (_ramping) {
            _rampValue -= deltaMs / kGateRampMs;
            if (_rampValue <= 0.0f) {
                _rampValue = 0.0f; _ramping = false; _output = 0.0f;
            } else {
                _output = _lastGateOutput * _rampValue;   // linear fade
            }
        } else {
            _output = 0.0f;
        }
    }
}

// ---------------------------------------------------------------------------
// advanceStep — v1 :182-218, ported (RANDOM uses house xorshift, D-2).
// ---------------------------------------------------------------------------
void StepSequencer::advanceStep()
{
    switch (_direction) {
    case SeqDir::Forward:
        _currentStep = (_currentStep + 1) % _stepCount;
        break;
    case SeqDir::Reverse:
        _currentStep = (_currentStep - 1 + _stepCount) % _stepCount;
        break;
    case SeqDir::Bounce:
        _currentStep += _bounceDir;
        if (_currentStep >= _stepCount) {
            _currentStep = _stepCount - 2; _bounceDir = -1;
            if (_currentStep < 0) _currentStep = 0;
        } else if (_currentStep < 0) {
            _currentStep = 1; _bounceDir = 1;
            if (_currentStep >= _stepCount) _currentStep = 0;
        }
        break;
    case SeqDir::Random:
        if (_stepCount > 1) {
            int next;
            do { next = (int)(nextRand() % (uint32_t)_stepCount); }
            while (next == _currentStep);              // avoid immediate repeat
            _currentStep = next;
        }
        break;
    default:
        _currentStep = (_currentStep + 1) % _stepCount;
        break;
    }
}

// nextStepIndex — direction-aware preview for slide (v1 :226-250).
int StepSequencer::nextStepIndex() const
{
    switch (_direction) {
    case SeqDir::Forward: return (_currentStep + 1) % _stepCount;
    case SeqDir::Reverse: return (_currentStep - 1 + _stepCount) % _stepCount;
    case SeqDir::Bounce: {
        int next = _currentStep + _bounceDir;
        if (next >= _stepCount) return (_stepCount - 2 >= 0) ? _stepCount - 2 : 0;
        if (next < 0)           return (1 < _stepCount)      ? 1               : 0;
        return next;
    }
    case SeqDir::Random:  return (_currentStep + 1) % _stepCount; // slide+random edge case
    default:              return (_currentStep + 1) % _stepCount;
    }
}

// ---------------------------------------------------------------------------
// Setters — v1 :265-345, ported (constrain -> local clamps).
// ---------------------------------------------------------------------------
void StepSequencer::setEnabled(bool on)
{
    if (!on && _enabled) {
        // Disabling while running — start ramp-down so output fades cleanly.
        _ramping = true; _rampValue = 1.0f; _lastGateOutput = _output;
    }
    _enabled = on;
    if (!on) _gateOpen = false;
}

void StepSequencer::setStepCount(int count)
{
    _stepCount = clampi(count, 1, kMaxSteps);
    if (_currentStep >= _stepCount) _currentStep = 0;
}

void StepSequencer::setStepValue(int step, uint8_t cc)
{
    if (step < 0 || step >= kMaxSteps) return;
    _stepValues[step] = (uint8_t)clampi((int)cc, 0, 127);
}

void StepSequencer::setGateLength(float frac) { _gateLength = clampf(frac, 0.0f, 1.0f); }
void StepSequencer::setSlide(float frac)      { _slide      = clampf(frac, 0.0f, 1.0f); }

void StepSequencer::setDirection(SeqDir dir)
{
    _direction = (dir < SeqDir::Count) ? dir : SeqDir::Forward;
    _bounceDir = 1;                                   // reset bounce on change
}

void StepSequencer::setDestination(SeqDest dest)
{
    _destination = (dest < SeqDest::Count) ? dest : SeqDest::None;
}

void StepSequencer::setDepth(float d) { _depth = clampf(d, -1.0f, 1.0f); }

// ---- AUX LANE setters (Stage B), mirroring the gate-lane clamps -----------
void StepSequencer::setAuxStepValue(int step, uint8_t cc)
{
    if (step < 0 || step >= kMaxSteps) return;
    _auxValues[step] = (uint8_t)clampi((int)cc, 0, 127);
}

void StepSequencer::setAuxDepth(float d) { _auxDepth = clampf(d, -1.0f, 1.0f); }

void StepSequencer::setAuxDestination(SeqAuxDest dest)
{
    _auxDest = (dest < SeqAuxDest::Count) ? dest : SeqAuxDest::None;
}

// Bipolar interpretation is a READ-TIME decision, so flipping it takes effect
// on the next tick with no state to migrate and no click beyond the ordinary
// step-to-step change the gate ramp already covers.
void StepSequencer::setStepBipolar(bool on) { _stepBipolar = on; }
void StepSequencer::setAuxBipolar(bool on)  { _auxBipolar  = on; }

void StepSequencer::setRate(float hz)
{
    _rateHz = clampf(hz, kFreeHzMin, kFreeHzMax);
    if (_timingMode == TempoClock::kFree) recalcDuration();
}

void StepSequencer::setRetrigger(bool on) { _retrigger = on; }

void StepSequencer::reset()
{
    _currentStep = 0; _phaseMs = 0.0f; _bounceDir = 1;
    _gateOpen = false; _ramping = false; _rampValue = 0.0f;
    _lastGateOutput = 0.0f; _output = 0.0f;
    _auxOutput = 0.0f;                              // aux lane (Stage B)
}

// D-1: tempo-sync deferred.  Store the mode, but stay free-running — no ms
// accessor on TempoClock yet.  When it lands, restore the v1 behaviour here.
void StepSequencer::setTimingMode(int mode)
{
    _timingMode = mode;
    if (mode == TempoClock::kFree) recalcDuration();
    // else: intentionally free-running until getTimeForMode(ms) exists.
}

void StepSequencer::updateFromClock(const TempoClock& /*clock*/)
{
    // D-1: no-op.  TempoClock has freqForMode(Hz) but not getTimeForMode(ms);
    // wiring sync requires that accessor (a bounded follow-up).
}

void StepSequencer::recalcDuration()
{
    _stepDurationMs = (_rateHz > 0.0f) ? (1000.0f / _rateHz) : 1000.0f;
}

} // namespace JT
