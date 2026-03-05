// OscillatorBlock.cpp — JT-8000 oscillator implementation
// =============================================================================
// See OscillatorBlock.h for architecture documentation.
//
// KEY DESIGN PRINCIPLES:
//   1. Base frequency is set ONCE via _mainOsc.frequency() on noteOn().
//   2. ALL pitch modulation flows through the FM input as DC signals.
//   3. The only periodic work is glide DC ramping (when active).
//   4. No powf() anywhere in this file — all math is addition/multiplication.
// =============================================================================

#include "OscillatorBlock.h"
#include "AKWF_All.h"

// =============================================================================
// CONSTRUCTOR — Build the dual signal path (normal + feedback)
// =============================================================================
OscillatorBlock::OscillatorBlock(bool enableSupersaw)
    : _supersaw(nullptr)
    , _supersawEnabled(enableSupersaw)
    , _baseFreq(440.0f)
{
    // =========================================================================
    // PITCH PRE-MIXER WIRING
    // Combines coarse/fine/detune (slot 0), bend (slot 1), glide (slot 2)
    // into a single signal feeding FM mixer slot 0.
    // =========================================================================
    _patchOffsetToPre = new AudioConnection(_pitchOffsetDc, 0, _pitchPreMixer, 0);
    _patchBendToPre   = new AudioConnection(_pitchBendDc,   0, _pitchPreMixer, 1);
    _patchGlideToPre  = new AudioConnection(_glideDc,       0, _pitchPreMixer, 2);
    _patchExtToPre    = new AudioConnection(_externalPitchDc, 0, _pitchPreMixer, 3);

    _pitchPreMixer.gain(0, 1.0f);  // Coarse + fine + detune
    _pitchPreMixer.gain(1, 1.0f);  // Pitch bend
    _pitchPreMixer.gain(2, 1.0f);  // Glide
    _pitchPreMixer.gain(3, 1.0f);  // External pitch DC

    // Initialise all pitch DCs to zero (no offset at startup)
    _pitchOffsetDc.amplitude(0.0f);
    _pitchBendDc.amplitude(0.0f);
    _glideDc.amplitude(0.0f);
    _externalPitchDc.amplitude(0.0f);

    // =========================================================================
    // MAIN FM MIXER WIRING
    // Slot 0 = pre-mixer output, slots 1-3 wired by SynthEngine (LFO1/2, PEnv)
    // =========================================================================
    _patchPreToFM    = new AudioConnection(_pitchPreMixer,    0, _frequencyModMixer, 0);
    _patchShapeDc    = new AudioConnection(_shapeDc,          0, _shapeModMixer, 0);
    _patchFMToOsc    = new AudioConnection(_frequencyModMixer, 0, _mainOsc, 0);
    _patchShapeToOsc = new AudioConnection(_shapeModMixer,    0, _mainOsc, 1);

    _frequencyModMixer.gain(0, 1.0f);  // Pre-mixer (pitch offsets)
    _frequencyModMixer.gain(1, 0.0f);  // LFO1 (set by SynthEngine)
    _frequencyModMixer.gain(2, 0.0f);  // LFO2 (set by SynthEngine)
    _frequencyModMixer.gain(3, 1.0f);  // Pitch envelope (set by SynthEngine)

    _shapeModMixer.gain(0, 1.0f);      // Shape DC
    _shapeModMixer.gain(1, 0.0f);      // LFO1 shape
    _shapeModMixer.gain(2, 0.0f);      // LFO2 shape
    _shapeModMixer.gain(3, 0.0f);      // Spare

    _shapeDc.amplitude(0.0f);

    // =========================================================================
    // MAIN OSCILLATOR SETUP
    // =========================================================================
    _mainOsc.begin(_currentType);
    _mainOsc.amplitude(1.0f);
    _mainOsc.frequencyModulation(FM_OCTAVE_RANGE);  // ±1.0 on FM = ±10 octaves
    _mainOsc.phaseModulation(175);

    // =========================================================================
    // OUTPUT MIXER — DUAL PATH
    //   Channel 0: Main oscillator (default ON)
    //   Channel 1: Supersaw oscillator (if enabled)
    //   Channel 2: Feedback comb output
    //   Channel 3: Unused
    // =========================================================================
    _patchMainOsc = new AudioConnection(_mainOsc, 0, _outputMix, 0);

    _outputMix.gain(0, 0.9f);   // Main osc output
    _outputMix.gain(1, 0.0f);   // Supersaw (off by default)
    _outputMix.gain(2, 0.0f);   // Feedback comb (off by default)
    _outputMix.gain(3, 0.0f);   // Unused

    // =========================================================================
    // FEEDBACK COMB NETWORK
    // =========================================================================
    _combMixer.gain(0, 1.0f);   // Main osc input
    _combMixer.gain(1, 0.0f);   // Supersaw input
    _combMixer.gain(2, 0.0f);   // Feedback path (off until enabled)
    _combMixer.gain(3, 0.0f);   // Unused

    _combDelay.delay(0, FEEDBACK_DELAY_MS);

    _patchMainToComb     = new AudioConnection(_mainOsc,   0, _combMixer, 0);
    _patchDelayToComb    = new AudioConnection(_combDelay, 0, _combMixer, 2);
    _patchCombToDelay    = new AudioConnection(_combMixer, 0, _combDelay, 0);
    _patchDelayToOut     = new AudioConnection(_combDelay, 0, _outputMix, 2);
    _patchSupersawToComb = nullptr;
    _patchSupersaw       = nullptr;

    // =========================================================================
    // SUPERSAW (conditional — OSC1 only)
    // =========================================================================
    if (_supersawEnabled) {
        _supersaw = new AudioSynthSupersaw();
        _patchSupersaw       = new AudioConnection(*_supersaw, 0, _outputMix, 1);
        _patchSupersawToComb = new AudioConnection(*_supersaw, 0, _combMixer, 1);

        _supersaw->setOversample(false);
        _supersaw->setMixCompensation(true);
        _supersaw->setCompensationMaxGain(1.5f);
        _supersaw->setBandLimited(false);
    }
}

// =============================================================================
// ARBITRARY WAVEFORM HELPERS
// =============================================================================

void OscillatorBlock::_applyArbWave() {
    uint16_t length = 0;
    const int16_t* table = akwf_get(_arbBank, _arbIndex, length);
    if (table && length > 0) {
        const float maxFreq = AUDIO_SAMPLE_RATE_EXACT / (float)length;
        _mainOsc.arbitraryWaveform(table, maxFreq);
    }
}

void OscillatorBlock::setArbBank(ArbBank bank) {
    _arbBank = bank;
    uint16_t count = akwf_bankCount(bank);
    if (count > 0 && _arbIndex >= count) {
        _arbIndex = count - 1;
    }
    if (_currentType == WAVEFORM_ARBITRARY) {
        _applyArbWave();
        _mainOsc.begin(WAVEFORM_ARBITRARY);
    }
}

void OscillatorBlock::setArbTableIndex(uint16_t index) {
    uint16_t count = akwf_bankCount(_arbBank);
    if (count == 0) { _arbIndex = 0; return; }
    if (index >= count) index = count - 1;
    _arbIndex = index;
    if (_currentType == WAVEFORM_ARBITRARY) {
        _applyArbWave();
        _mainOsc.begin(WAVEFORM_ARBITRARY);
    }
}

// =============================================================================
// WAVEFORM TYPE — Clean routing (independent of feedback state)
// =============================================================================

void OscillatorBlock::setWaveformType(int type) {
    _currentType = type;

    if (type == WAVEFORM_SUPERSAW) {
        if (_supersawEnabled && _supersaw) {
            // Route through supersaw, mute main
            _outputMix.gain(0, 0.0f);
            _outputMix.gain(1, 0.9f);
            if (_feedbackEnabled) {
                _combMixer.gain(0, 0.0f);
                _combMixer.gain(1, 1.0f);
            }
        } else {
            // Fallback to plain sawtooth (OSC2 has no supersaw)
            _mainOsc.begin(WAVEFORM_SAWTOOTH);
            _outputMix.gain(0, 0.7f);
            _outputMix.gain(1, 0.0f);
            if (_feedbackEnabled) {
                _combMixer.gain(0, 1.0f);
                _combMixer.gain(1, 0.0f);
            }
        }
    } else if (type == WAVEFORM_ARBITRARY) {
        _applyArbWave();
        _mainOsc.begin(WAVEFORM_ARBITRARY);
        _outputMix.gain(0, 0.7f);
        _outputMix.gain(1, 0.0f);
        if (_feedbackEnabled) {
            _combMixer.gain(0, 1.0f);
            _combMixer.gain(1, 0.0f);
        }
    } else {
        _mainOsc.begin((uint8_t)type);
        _outputMix.gain(0, 0.7f);
        _outputMix.gain(1, 0.0f);
        if (_feedbackEnabled) {
            _combMixer.gain(0, 1.0f);
            _combMixer.gain(1, 0.0f);
        }
    }
    // Note: _outputMix.gain(2) is managed by feedback state, not waveform type
}

// =============================================================================
// AMPLITUDE CONTROL
// =============================================================================

void OscillatorBlock::setAmplitude(float amplitude) {
    _mainOsc.amplitude(amplitude);
    if (_supersaw) _supersaw->setAmplitude(amplitude);
}

// =============================================================================
// PITCH CONTROL — All via DC sources into FM mixer
// =============================================================================

void OscillatorBlock::setPitchOffset(float semitones) {
    if (_pitchOffsetSemitones == semitones) return;  // Skip if unchanged
    _pitchOffsetSemitones = semitones;
    _updatePitchOffsetDc();
}

void OscillatorBlock::setFineTune(float cents) {
    if (_fineTuneCents == cents) return;
    _fineTuneCents = cents;
    _updatePitchOffsetDc();
}

void OscillatorBlock::setDetune(float hertz) {
    if (_detuneHz == hertz) return;
    _detuneHz = hertz;
    _updatePitchOffsetDc();
}

void OscillatorBlock::setPitchBend(float semitones) {
    if (_pitchBendSemitones == semitones) return;
    _pitchBendSemitones = semitones;
    _pitchBendDc.amplitude(semitones * FM_SEMITONE_SCALE);
}

void OscillatorBlock::setExternalPitchDc(float fmScaledAmplitude) {
    _externalPitchDcAmp = fmScaledAmplitude;
    _externalPitchDc.amplitude(fmScaledAmplitude);
}

void OscillatorBlock::_updatePitchOffsetDc() {
    // Combine coarse pitch, fine tune, and detune into one DC value.
    // Fine tune: cents → semitones (divide by 100)
    // Detune: Hz → approximate semitones at current base frequency
    //   detuneSemis ≈ detuneHz / (baseFreq × ln(2)/12) ≈ detuneHz × 12 / (baseFreq × 0.6931)
    //   For simplicity and CPU, use a fixed approximation at A440.
    //   At 440 Hz, 1 Hz ≈ 0.0394 semitones. Close enough for detune spread.
    const float fineSemitones  = _fineTuneCents / 100.0f;
    const float detuneApproxSemitones = (_baseFreq > 20.0f)
        ? (_detuneHz * 17.3123f / _baseFreq)  // 12/ln(2) ≈ 17.3123
        : 0.0f;

    const float totalSemitones = _pitchOffsetSemitones + fineSemitones + detuneApproxSemitones;

    // Clamp to safe range (±48 semitones = ±4 octaves)
    const float clamped = (totalSemitones > 48.0f) ? 48.0f :
                          (totalSemitones < -48.0f) ? -48.0f : totalSemitones;

    _pitchOffsetDc.amplitude(clamped * FM_SEMITONE_SCALE);
}

// =============================================================================
// NOTE ON/OFF — Set base frequency ONCE, configure glide
// =============================================================================

void OscillatorBlock::noteOn(float frequency, float velocity) {
    // Velocity is already normalised 0.0–1.0 by the sketch.
    // Do NOT divide by 127 here.
    const float amplitude = velocity;

    if (_glideEnabled && _glideTimeMs > 0.0f && _baseFreq > 20.0f) {
        // Start glide from current position toward new target
        _glideTargetHz  = frequency;
        _glideCurrentHz = _baseFreq;  // Slide from previous note
        _glideActive    = true;

        // Set the oscillator to the CURRENT glide position (will slide to target)
        AudioNoInterrupts();
        _mainOsc.frequency(_glideCurrentHz);
        if (_supersaw) _supersaw->setFrequency(_glideCurrentHz);
        AudioInterrupts();

        // Glide DC starts at 0 (we're at _glideCurrentHz which is _baseFreq)
        _glideDc.amplitude(0.0f);
        _baseFreq = _glideCurrentHz;
    } else {
        // No glide — jump directly to new frequency
        _baseFreq       = frequency;
        _glideTargetHz  = frequency;
        _glideCurrentHz = frequency;
        _glideActive    = false;
        _glideDc.amplitude(0.0f);

        AudioNoInterrupts();
        _mainOsc.frequency(frequency);
        if (_supersaw) _supersaw->setFrequency(frequency);
        AudioInterrupts();
    }

    // Set oscillator amplitude based on waveform type
    AudioNoInterrupts();
    if (_currentType == WAVEFORM_SUPERSAW && _supersaw) {
        _mainOsc.amplitude(0.0f);
        _supersaw->setAmplitude(amplitude);
    } else {
        _mainOsc.amplitude(amplitude);
        if (_supersaw) _supersaw->setAmplitude(0.0f);
    }
    AudioInterrupts();

    _lastVelocity = velocity;

    // Recalculate detune DC (it depends on base frequency for Hz→semitone conversion)
    _updatePitchOffsetDc();
}

void OscillatorBlock::noteOff() {
    _mainOsc.amplitude(0.0f);
    if (_supersaw) _supersaw->setAmplitude(0.0f);
}

// =============================================================================
// UPDATE — Only handles glide ramp (called from VoiceBlock::update)
// =============================================================================

void OscillatorBlock::update() {
    // Early exit if no glide active — zero CPU cost for static notes
    if (!_glideActive) return;
    //return;

    // Exponential glide toward target frequency
    const float delta = _glideTargetHz - _glideCurrentHz;

    if (fabsf(delta) < 0.1f) {
        // Close enough — snap to target and stop
        _glideCurrentHz = _glideTargetHz;
        _glideActive = false;

        // Update base frequency to final target and clear glide DC
        _baseFreq = _glideTargetHz;
        _mainOsc.frequency(_glideTargetHz);
        if (_supersaw) _supersaw->setFrequency(_glideTargetHz);
        _glideDc.amplitude(0.0f);

        // Recalculate detune (depends on base freq)
        _updatePitchOffsetDc();
        return;
    }

    // Slide toward target
    _glideCurrentHz += delta * _glideRate;

    // Update oscillator base frequency to current glide position
    _mainOsc.frequency(_glideCurrentHz);
    if (_supersaw) _supersaw->setFrequency(_glideCurrentHz);

    // The glide DC is not used during active glide — we're directly setting
    // frequency(). This avoids double-modulation artefacts. The FM mixer
    // still applies LFO/bend/envelope on top of the sliding base.
}

// =============================================================================
// GLIDE CONFIGURATION
// =============================================================================

void OscillatorBlock::setGlideEnabled(bool enabled) {
    _glideEnabled = enabled;
}

void OscillatorBlock::setGlideTime(float milliseconds) {
    _glideTimeMs = milliseconds;
    if (milliseconds > 0.0f) {
        // Rate = fraction of remaining distance to cover per sample
        const float samples = (milliseconds / 1000.0f) * AUDIO_SAMPLE_RATE_EXACT;
        _glideRate = 1.0f / samples;
    } else {
        _glideRate = 0.0f;
    }
}

// =============================================================================
// SUPERSAW CONTROL
// =============================================================================

void OscillatorBlock::setSupersawDetune(float amount) {
    _supersawDetune = amount;
    if (_supersaw) _supersaw->setDetune(amount);
}

void OscillatorBlock::setSupersawMix(float mix) {
    _supersawMix = mix;
    if (_supersaw) _supersaw->setMix(mix);
}

// =============================================================================
// SHAPE DC
// =============================================================================

void OscillatorBlock::setShapeDcAmp(float amplitude) {
    _shapeDcAmp = amplitude;
    _shapeDc.amplitude(amplitude);
}

// =============================================================================
// FEEDBACK OSCILLATION
// =============================================================================

void OscillatorBlock::setFeedbackAmount(float amount) {
    _feedbackGain = constrain(amount, 0.0f, 0.99f);
    _feedbackEnabled = (_feedbackGain > 0.0f);

    if (_feedbackEnabled) {
        // Route correct oscillator to comb input
        if (_currentType == WAVEFORM_SUPERSAW && _supersaw) {
            _combMixer.gain(0, 0.0f);
            _combMixer.gain(1, 1.0f);
        } else {
            _combMixer.gain(0, 1.0f);
            _combMixer.gain(1, 0.0f);
        }
        _combMixer.gain(2, _feedbackGain);
        _outputMix.gain(2, _feedbackMixLevel);
    } else {
        _combMixer.gain(2, 0.0f);
        _outputMix.gain(2, 0.0f);
    }
}

void OscillatorBlock::setFeedbackMix(float mix) {
    _feedbackMixLevel = constrain(mix, 0.0f, 1.0f);
    if (_feedbackEnabled) {
        _outputMix.gain(2, _feedbackMixLevel);
    }
}

// =============================================================================
// AUDIO OUTPUTS & MIXERS
// =============================================================================

AudioStream& OscillatorBlock::output()           { return _outputMix; }
AudioMixer4& OscillatorBlock::frequencyModMixer() { return _frequencyModMixer; }
AudioMixer4& OscillatorBlock::shapeModMixer()     { return _shapeModMixer; }
