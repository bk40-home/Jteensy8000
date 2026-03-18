// =============================================================================
// OscillatorBlock.cpp — JT-8000 oscillator implementation
//
// See OscillatorBlock.h for architecture and FM scaling documentation.
//
// KEY DESIGN PRINCIPLES:
//   1. Base frequency set ONCE via _mainOsc.frequency() on noteOn().
//   2. ALL pitch modulation (LFO, pitch env, bend, coarse, fine, detune)
//      flows through the AudioSynthWaveformJT FM input.
//   3. Pitch DC sources (offset + bend + external) are summed in software
//      and written to a single AudioSynthWaveformDc.  No AudioMixer4 pre-stage.
//   4. Glide slides the base frequency via direct .frequency() calls — the
//      combined DC does not change while gliding.
//   5. No powf() in this file — all arithmetic is addition / multiplication.
// =============================================================================

#include "OscillatorBlock.h"
#include "AKWF_All.h"

// =============================================================================
// CONSTRUCTOR — build audio graph for one oscillator
// =============================================================================

OscillatorBlock::OscillatorBlock(bool enableSupersaw)
    : _supersaw(nullptr)
    , _supersawEnabled(enableSupersaw)
    , _baseFreq(440.0f)
{
    // =========================================================================
    // COMBINED PITCH DC → FM MIXER
    //
    // A single DC source carries the software sum of:
    //   coarse pitch offset + fine tune + detune + pitch bend + external DC.
    // All are constant-per-block scalars, so software addition is identical
    // to a hardware AudioMixer4 pre-stage but costs no ISR time.
    // =========================================================================
    _combinedPitchDc.amplitude(0.0f);

    _patchPitchDcToFM    = new AudioConnection(_combinedPitchDc,    0, _frequencyModMixer, 0);
    _patchShapeDcToShape = new AudioConnection(_shapeDc,            0, _shapeModMixer,     0);

    // =========================================================================
    // FM MIXER SETUP
    // Slot 0 = combined pitch DC (all static offsets + bend + external).
    // Slots 1-3 wired by SynthEngine (LFO1 pitch, LFO2 pitch, pitch envelope).
    // =========================================================================
    _frequencyModMixer.gain(0, 1.0f);   // Combined pitch DC — unity passthrough
    _frequencyModMixer.gain(1, 0.0f);   // LFO1 (set by SynthEngine at runtime)
    _frequencyModMixer.gain(2, 0.0f);   // LFO2 (set by SynthEngine at runtime)
    _frequencyModMixer.gain(3, 1.0f);   // Pitch envelope (set by SynthEngine)

    // Shape mod mixer
    _shapeDc.amplitude(0.0f);
    _shapeModMixer.gain(0, 1.0f);   // Shape DC
    _shapeModMixer.gain(1, 0.0f);   // LFO1 shape (set by SynthEngine)
    _shapeModMixer.gain(2, 0.0f);   // LFO2 shape (set by SynthEngine)
    _shapeModMixer.gain(3, 0.0f);   // Spare

    // =========================================================================
    // MAIN OSCILLATOR
    //
    // frequencyModulation(N) enables the FM input and sets ±1.0 = ±N octaves.
    // MUST be called BEFORE any phaseModulation() call, which would switch the
    // oscillator to PM mode and silently ignore all FM pitch sources.
    // =========================================================================
    _mainOsc.begin(_currentType);
    _mainOsc.amplitude(1.0f);
    _mainOsc.frequencyModulation(FM_OCTAVE_RANGE);  // ±1.0 = ±10 octaves on FM input

    _patchFMToOsc    = new AudioConnection(_frequencyModMixer, 0, _mainOsc, 0);  // FM pitch
    _patchShapeToOsc = new AudioConnection(_shapeModMixer,     0, _mainOsc, 1);  // Shape / PWM

    // =========================================================================
    // OUTPUT MIXER
    //   Slot 0: Main oscillator
    //   Slot 1: Supersaw (OSC1 only; remains 0.0 until supersaw waveform selected)
    //   Slot 2: Feedback comb output
    //   Slot 3: Unused
    // =========================================================================
    _patchMainOsc = new AudioConnection(_mainOsc, 0, _outputMix, 0);
    _outputMix.gain(0, 0.9f);   // Main oscillator (default active)
    _outputMix.gain(1, 0.0f);   // Supersaw off by default
    _outputMix.gain(2, 0.0f);   // Feedback comb off by default
    _outputMix.gain(3, 0.0f);   // Unused

    // =========================================================================
    // FEEDBACK COMB NETWORK (JP-8000 comb delay)
    //   Comb mixer collects oscillator output + delayed feedback.
    //   The delay output is also tapped into the main output mixer for the
    //   filtered/coloured feedback signal.
    // =========================================================================
    _combMixer.gain(0, 1.0f);   // Main osc signal
    _combMixer.gain(1, 0.0f);   // Supersaw (routed when active)
    _combMixer.gain(2, 0.0f);   // Feedback return (off until enabled)
    _combMixer.gain(3, 0.0f);   // Unused

    _combDelay.delay(0, FEEDBACK_DELAY_MS);

    _patchMainToComb     = new AudioConnection(_mainOsc,   0, _combMixer, 0);
    _patchDelayToComb    = new AudioConnection(_combDelay, 0, _combMixer, 2);
    _patchCombToDelay    = new AudioConnection(_combMixer, 0, _combDelay, 0);
    _patchDelayToOut     = new AudioConnection(_combDelay, 0, _outputMix, 2);

    // Supersaw pointers null by default — assigned below if OSC1
    _patchSupersawToComb  = nullptr;
    _patchSupersaw        = nullptr;
    _patchFMToSupersaw    = nullptr;
    _patchShapeToSupersaw = nullptr;

    // =========================================================================
    // SUPERSAW (OSC1 only)
    //
    // If enabled, the supersaw gets the same FM mixer output as the main osc,
    // so both paths respond identically to LFO / pitch-envelope modulation.
    // OSC2 always passes enableSupersaw=false, keeping ~10 objects unallocated.
    // =========================================================================
    if (_supersawEnabled) {
        _supersaw = new AudioSynthSupersaw();

        _patchSupersaw       = new AudioConnection(*_supersaw, 0, _outputMix,  1);
        _patchSupersawToComb = new AudioConnection(*_supersaw, 0, _combMixer,  1);

        // Wire FM mixer → supersaw FM input (same source as _mainOsc FM input)
        _patchFMToSupersaw    = new AudioConnection(_frequencyModMixer, 0, *_supersaw, 0);
        // Wire shape mixer → supersaw phase-mod input
        _patchShapeToSupersaw = new AudioConnection(_shapeModMixer,     0, *_supersaw, 1);

        // Match FM range to main oscillator — ±1.0 = ±FM_OCTAVE_RANGE octaves
        _supersaw->frequencyModulation(FM_OCTAVE_RANGE);
        // Phase mod: 180° half-cycle swing at full-scale, matching AudioSynthWaveformJT
        _supersaw->phaseModulation(180.0f);

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
        // Nyquist limit: max frequency before aliasing = sampleRate / tableLength
        const float maxFreq = AUDIO_SAMPLE_RATE_EXACT / (float)length;
        _mainOsc.arbitraryWaveform(table, maxFreq);
    }
}

void OscillatorBlock::setArbBank(ArbBank bank) {
    _arbBank = bank;
    const uint16_t count = akwf_bankCount(bank);
    if (count > 0 && _arbIndex >= count) {
        _arbIndex = count - 1;
    }
    if (_currentType == WAVEFORM_ARBITRARY) {
        _applyArbWave();
        _mainOsc.begin(WAVEFORM_ARBITRARY);
    }
}

void OscillatorBlock::setArbTableIndex(uint16_t index) {
    const uint16_t count = akwf_bankCount(_arbBank);
    if (count == 0) { _arbIndex = 0; return; }
    if (index >= count) index = count - 1;
    _arbIndex = index;
    if (_currentType == WAVEFORM_ARBITRARY) {
        _applyArbWave();
        _mainOsc.begin(WAVEFORM_ARBITRARY);
    }
}

// =============================================================================
// WAVEFORM TYPE — routes output mixer gains, not the DC sources
// =============================================================================

void OscillatorBlock::setWaveformType(int type) {
    _currentType = type;

    if (type == WAVEFORM_SUPERSAW) {
        if (_supersawEnabled && _supersaw) {
            // Supersaw active: mute main osc, route supersaw to output
            _outputMix.gain(0, 0.0f);
            _outputMix.gain(1, 0.9f);
            if (_feedbackEnabled) {
                _combMixer.gain(0, 0.0f);
                _combMixer.gain(1, 1.0f);
            }
        } else {
            // OSC2 fallback — no supersaw object available, use plain sawtooth
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
    // NOTE: _outputMix.gain(2) (feedback comb) is managed by setFeedbackAmount(),
    // not by waveform type, so it is intentionally not touched here.
}

// =============================================================================
// AMPLITUDE CONTROL
// =============================================================================

void OscillatorBlock::setAmplitude(float amplitude) {
    _mainOsc.amplitude(amplitude);
    if (_supersaw) _supersaw->setAmplitude(amplitude);
}

// =============================================================================
// PITCH CONTROL — all write through _updateCombinedPitchDc()
// =============================================================================

void OscillatorBlock::setPitchOffset(float semitones) {
    if (_pitchOffsetSemitones == semitones) return;   // Skip if unchanged
    _pitchOffsetSemitones = semitones;
    _updateStaticPitchFm();                            // Recombines offset+fine+detune → DC
}

void OscillatorBlock::setFineTune(float cents) {
    if (_fineTuneCents == cents) return;
    _fineTuneCents = cents;
    _updateStaticPitchFm();
}

void OscillatorBlock::setDetune(float hertz) {
    if (_detuneHz == hertz) return;
    _detuneHz = hertz;
    _updateStaticPitchFm();
}

void OscillatorBlock::setPitchBend(float semitones) {
    if (_pitchBendSemitones == semitones) return;
    _pitchBendSemitones = semitones;
    _pitchBendFm = semitones * FM_SEMITONE_SCALE;
    _updateCombinedPitchDc();
}

void OscillatorBlock::setExternalPitchDc(float fmScaledAmplitude) {
    // Caller pre-scales into FM units — store and push to combined DC
    _externalPitchDcAmp = fmScaledAmplitude;
    _updateCombinedPitchDc();
}

void OscillatorBlock::setSeqPitchOffset(float fmScaledOffset) {
    _seqPitchOffset = fmScaledOffset;
    _updateCombinedPitchDc();
}

// -----------------------------------------------------------------------------
// _updateStaticPitchFm — recalculate the FM-scaled sum of coarse + fine + detune
//
// Called on any change to offset, fine tune, or detune, and on noteOn() (because
// detune Hz→semitone conversion is frequency-dependent and _baseFreq just changed).
// -----------------------------------------------------------------------------
void OscillatorBlock::_updateStaticPitchFm() {
    // Fine tune: cents → semitones
    const float fineSemitones = _fineTuneCents * 0.01f;

    // Detune Hz → approximate semitones at the current base frequency.
    // Formula: semitones = Hz × (12 / ln(2)) / baseFreq ≈ Hz × 17.3123 / baseFreq.
    // Guard against very low base frequencies to avoid division instability.
    const float detuneSemitones = (_baseFreq > 20.0f)
        ? (_detuneHz * 17.3123f / _baseFreq)   // 12/ln(2) ≈ 17.3123
        : 0.0f;

    _staticPitchFm = (_pitchOffsetSemitones + fineSemitones + detuneSemitones)
                     * FM_SEMITONE_SCALE;

    _updateCombinedPitchDc();
}

// -----------------------------------------------------------------------------
// _updateCombinedPitchDc — write the final summed DC to the audio graph.
//
// Sum = static offsets (coarse + fine + detune) + pitch bend + external DC.
// Clamped to ±(48 semitones × FM_SEMITONE_SCALE) = ±0.4 to prevent runaway.
// No audio processing here — just one .amplitude() call (safe to call anytime).
// -----------------------------------------------------------------------------
void OscillatorBlock::_updateCombinedPitchDc() {
    const float combined = _staticPitchFm + _pitchBendFm
                         + _externalPitchDcAmp + _seqPitchOffset;

    // ±48 semitones = ±4 octaves max shift from DC sources
    constexpr float kMaxFm = 48.0f * FM_SEMITONE_SCALE;  // ≈ 0.4
    const float clamped = (combined >  kMaxFm) ?  kMaxFm :
                          (combined < -kMaxFm) ? -kMaxFm : combined;

    _combinedPitchDc.amplitude(clamped);
}

// =============================================================================
// NOTE ON / OFF
// =============================================================================

void OscillatorBlock::noteOn(float frequency, float velocity) {
    // velocity is already 0.0–1.0 — do NOT divide by 127 again here.
    const float amplitude = velocity;

    if (_glideEnabled && _glideTimeMs > 0.0f && _baseFreq > 20.0f) {
        // --- Glide: slide from previous note toward new target ---
        _glideTargetHz  = frequency;
        _glideCurrentHz = _baseFreq;   // Slide starts from wherever we are now
        _glideActive    = true;

        // Start at the previous frequency; update() will ramp toward target
        AudioNoInterrupts();
        _mainOsc.frequency(_glideCurrentHz);
        if (_supersaw) _supersaw->setFrequency(_glideCurrentHz);
        AudioInterrupts();

        // _baseFreq stays at the CURRENT glide position for detune calculations
        // (it will be updated to the final target once glide completes).
        // Note: _combinedPitchDc does not need updating — it holds static offsets;
        // glide position changes are expressed via direct .frequency() calls.
    } else {
        // --- No glide: jump directly to new frequency ---
        _baseFreq       = frequency;
        _glideTargetHz  = frequency;
        _glideCurrentHz = frequency;
        _glideActive    = false;

        AudioNoInterrupts();
        _mainOsc.frequency(frequency);
        if (_supersaw) _supersaw->setFrequency(frequency);
        AudioInterrupts();
    }

    // Set amplitude per waveform type (supersaw / main osc are mutually exclusive)
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

    // Detune conversion depends on _baseFreq — recalculate after frequency change.
    // This only updates the static DC; bend and external remain unchanged.
    _updateStaticPitchFm();
}

void OscillatorBlock::noteOff() {
    _mainOsc.amplitude(0.0f);
    if (_supersaw) _supersaw->setAmplitude(0.0f);
}

// =============================================================================
// PERIODIC UPDATE — glide ramp only
//
// Called from VoiceBlock::update() every audio block (~2.9 ms at 44.1 kHz).
// Returns immediately when no glide active — zero CPU cost for static notes.
// Glide uses direct .frequency() calls, NOT the combined DC, so LFO / bend
// / pitch-env continue to modulate on top of the sliding base frequency.
// =============================================================================

void OscillatorBlock::update() {
    if (!_glideActive) return;

    const float delta = _glideTargetHz - _glideCurrentHz;

    if (fabsf(delta) < 0.1f) {
        // Close enough — snap to target, stop ramping
        _glideCurrentHz = _glideTargetHz;
        _glideActive    = false;
        _baseFreq       = _glideTargetHz;   // Update base so detune calc is correct

        AudioNoInterrupts();
        _mainOsc.frequency(_glideTargetHz);
        if (_supersaw) _supersaw->setFrequency(_glideTargetHz);
        AudioInterrupts();

        // Recalculate detune component now that _baseFreq has changed to final target
        _updateStaticPitchFm();
        return;
    }

    // Exponential (constant-time-ratio) slide toward target
    _glideCurrentHz += delta * _glideRate;

    AudioNoInterrupts();
    _mainOsc.frequency(_glideCurrentHz);
    if (_supersaw) _supersaw->setFrequency(_glideCurrentHz);
    AudioInterrupts();
    // Combined DC not touched — LFO/bend/env still modulate on top
}

// =============================================================================
// GLIDE CONFIGURATION
// =============================================================================

void OscillatorBlock::setGlideEnabled(bool enabled) {
    _glideEnabled = enabled;
    if (!enabled) _glideActive = false;   // Cancel any in-progress glide
}

void OscillatorBlock::setGlideTime(float milliseconds) {
    _glideTimeMs = milliseconds;
    if (milliseconds > 0.0f) {
        // Rate = fraction of remaining distance to cover per update() call.
        // update() is called once per audio block from VoiceBlock::update().
        // Using the per-sample count matches the JT4000 behaviour — changing this
        // would require re-calibrating all preset glide time values.
        // FUTURE: multiply by AUDIO_BLOCK_SAMPLES to correct the 128× implicit
        //         slowdown (block-rate call vs per-sample divisor), but only
        //         after all presets have been re-tuned.
        const float samples = (milliseconds / 1000.0f) * AUDIO_SAMPLE_RATE_EXACT;
        _glideRate = 1.0f / samples;
    } else {
        _glideRate   = 0.0f;
        _glideActive = false;
    }
}

// =============================================================================
// SUPERSAW CONTROL (OSC1 only)
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
// SHAPE / PWM DC
// =============================================================================

void OscillatorBlock::setShapeDcAmp(float amplitude) {
    _shapeDcAmp = amplitude;
    _shapeDc.amplitude(amplitude);
}

// =============================================================================
// FEEDBACK OSCILLATION (JP-8000 comb delay)
// =============================================================================

void OscillatorBlock::setFeedbackAmount(float amount) {
    _feedbackGain    = constrain(amount, 0.0f, 0.99f);
    _feedbackEnabled = (_feedbackGain > 0.0f);

    if (_feedbackEnabled) {
        // Route the active oscillator path into the comb mixer
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
        // Disable feedback — zero gain on feedback return path
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
// AUDIO OUTPUT ACCESSORS
// =============================================================================

AudioStream& OscillatorBlock::output()            { return _outputMix; }
AudioMixer4& OscillatorBlock::frequencyModMixer() { return _frequencyModMixer; }
AudioMixer4& OscillatorBlock::shapeModMixer()     { return _shapeModMixer; }
