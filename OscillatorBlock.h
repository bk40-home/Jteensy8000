#pragma once
// =============================================================================
// OscillatorBlock.h — JT-8000 oscillator with fully hardware-driven pitch
//
// ARCHITECTURE (Delivery 1 rewrite):
//   ALL pitch modulation now flows through the AudioSynthWaveformModulated
//   frequency modulation (FM) input. The software pitch calculation loop
//   (OscillatorBlock::update()) has been removed entirely.
//
//   The oscillator's base frequency is set ONCE on noteOn() via .frequency().
//   All pitch offsets (coarse, fine, detune, pitch bend, glide) are encoded
//   as DC amplitudes scaled by FM_SEMITONE_SCALE and summed in a pre-mixer
//   before entering the main FM mixer.
//
// FM MIXER SLOT ALLOCATION (4 channels on _frequencyModMixer):
//   Slot 0: _pitchPreMixer output — combined static pitch offsets + glide
//   Slot 1: LFO1 pitch modulation (wired by SynthEngine)
//   Slot 2: LFO2 pitch modulation (wired by SynthEngine)
//   Slot 3: Pitch envelope (wired by SynthEngine)
//
// PRE-MIXER SLOT ALLOCATION (4 channels on _pitchPreMixer):
//   Slot 0: _pitchOffsetDc — coarse + fine + detune combined
//   Slot 1: _pitchBendDc   — pitch bend from wheel
//   Slot 2: _glideDc       — portamento slide (ramped in update())
//   Slot 3: (spare for future external pitch CV)
//
// SCALING:
//   _mainOsc.frequencyModulation(FM_OCTAVE_RANGE) where FM_OCTAVE_RANGE = 10.
//   A ±1.0 signal on the FM input shifts pitch by ±10 octaves.
//   To shift by S semitones: amplitude = S / (FM_OCTAVE_RANGE × 12) = S / 120.
//   This is FM_SEMITONE_SCALE = 1/120 ≈ 0.00833.
//
// CPU SAVINGS:
//   - No powf() per voice per frame (was in update())
//   - No frequency() call per voice per frame
//   - Glide is the only source that needs periodic update (DC ramp)
//   - Dirty flag on glide means inactive voices cost zero CPU
// =============================================================================

#include <Audio.h>
#include "WaveForms.h"
#include "AKWF_All.h"
#include "AudioSynthSupersaw.h"
#include "DebugTrace.h"

// Must match the argument to _mainOsc.frequencyModulation() — defined once here.
static constexpr float FM_OCTAVE_RANGE    = 10.0f;
static constexpr float FM_SEMITONE_SCALE  = 1.0f / (FM_OCTAVE_RANGE * 12.0f);

class OscillatorBlock {
public:
    // =========================================================================
    // LIFECYCLE
    // =========================================================================

    /**
     * @brief Constructor with optional supersaw capability.
     * @param enableSupersaw true for OSC1 (supersaw capable), false for OSC2.
     */
    OscillatorBlock(bool enableSupersaw = false);

    /**
     * @brief Periodic update — only needed for glide ramp.
     * Called from VoiceBlock::update(). Returns immediately if no glide active.
     * No powf(), no frequency() calls — just DC amplitude ramping.
     */
    void update();

    /**
     * @brief Trigger note with frequency and velocity.
     * Sets base frequency via .frequency() ONCE. Resets glide target.
     * @param frequency Target frequency in Hz.
     * @param velocity  Normalised velocity 0.0–1.0.
     */
    void noteOn(float frequency, float velocity);

    /**
     * @brief Silence oscillators (amplitude → 0).
     */
    void noteOff();

    // =========================================================================
    // WAVEFORM & AMPLITUDE CONTROL
    // =========================================================================

    void setWaveformType(int type);
    void setAmplitude(float amplitude);

    // =========================================================================
    // PITCH CONTROL — all routed through FM pre-mixer as DC values
    // =========================================================================

    /** Coarse pitch offset in semitones (e.g. -24, -12, 0, +12, +24). */
    void setPitchOffset(float semitones);

    /** Fine tune in cents (-100 to +100). */
    void setFineTune(float cents);

    /** Detune spread in Hz (additive, converted to semitones at base freq). */
    void setDetune(float hertz);

    /** Pitch bend from wheel in semitones (±range). Updated per-frame by SynthEngine. */
    void setPitchBend(float semitones);

    /** External static pitch DC offset (e.g. from FREQ_DC CC). Pre-scaled by caller. */
    void setExternalPitchDc(float fmScaledAmplitude);

    // =========================================================================
    // GLIDE (PORTAMENTO) — ramped DC in pre-mixer slot 2
    // =========================================================================

    void setGlideEnabled(bool enabled);
    void setGlideTime(float milliseconds);

    // =========================================================================
    // SUPERSAW CONTROL (OSC1 only)
    // =========================================================================

    void setSupersawDetune(float amount);
    void setSupersawMix(float mix);

    // =========================================================================
    // DC MODULATION SOURCES (shape/frequency for external control)
    // =========================================================================

    void setShapeDcAmp(float amplitude);

    // =========================================================================
    // ARBITRARY WAVEFORM SELECTION
    // =========================================================================

    void setArbBank(ArbBank bank);
    void setArbTableIndex(uint16_t index);
    ArbBank  getArbBank()      const { return _arbBank; }
    uint16_t getArbTableIndex() const { return _arbIndex; }

    // =========================================================================
    // FEEDBACK OSCILLATION (JP-8000 STYLE)
    // =========================================================================

    void  setFeedbackAmount(float amount);
    void  setFeedbackMix(float mix);
    bool  getFeedbackEnabled() const { return _feedbackEnabled; }
    float getFeedbackAmount()  const { return _feedbackGain; }
    float getFeedbackMix()     const { return _feedbackMixLevel; }

    // =========================================================================
    // PARAMETER GETTERS
    // =========================================================================

    int   getWaveform()       const { return _currentType; }
    float getPitchOffset()    const { return _pitchOffsetSemitones; }
    float getFineTune()       const { return _fineTuneCents; }
    float getDetune()         const { return _detuneHz; }
    float getSupersawDetune() const { return _supersawDetune; }
    float getSupersawMix()    const { return _supersawMix; }
    bool  getGlideEnabled()   const { return _glideEnabled; }
    float getGlideTime()      const { return _glideTimeMs; }
    float getShapeDcAmp()     const { return _shapeDcAmp; }
    float getExternalPitchDc() const { return _externalPitchDcAmp; }

    // =========================================================================
    // AUDIO OUTPUTS & MODULATION MIXERS
    // =========================================================================

    /** Final oscillator output (after output mixer with feedback path). */
    AudioStream& output();

    /** FM mixer — external sources (LFO, pitch env) connect to slots 1-3. */
    AudioMixer4& frequencyModMixer();

    /** Shape/PWM modulation mixer. */
    AudioMixer4& shapeModMixer();

private:
    // =========================================================================
    // AUDIO COMPONENTS — PITCH MODULATION CHAIN
    // =========================================================================

    // Pre-mixer: combines static pitch offsets before the main FM mixer.
    //   Slot 0: coarse + fine + detune (combined DC)
    //   Slot 1: pitch bend DC
    //   Slot 2: glide DC (ramped in update())
    //   Slot 3: spare
    AudioSynthWaveformDc  _pitchOffsetDc;    // Coarse + fine + detune combined
    AudioSynthWaveformDc  _pitchBendDc;      // Pitch bend from wheel
    AudioSynthWaveformDc  _glideDc;          // Portamento slide (ramped)
    AudioSynthWaveformDc  _externalPitchDc;  // External static pitch offset (FREQ_DC CC)
    AudioMixer4           _pitchPreMixer;    // Sums slots 0-3 → FM mixer slot 0
    AudioConnection*      _patchOffsetToPre; // _pitchOffsetDc → pre slot 0
    AudioConnection*      _patchBendToPre;   // _pitchBendDc → pre slot 1
    AudioConnection*      _patchGlideToPre;  // _glideDc → pre slot 2
    AudioConnection*      _patchExtToPre;    // _externalPitchDc → pre slot 3

    // Main FM mixer: pre-mixer + LFO1 + LFO2 + pitch envelope → osc FM input.
    AudioSynthWaveformDc  _shapeDc;
    AudioMixer4           _frequencyModMixer;
    AudioMixer4           _shapeModMixer;
    AudioConnection*      _patchPreToFM;     // _pitchPreMixer → FM mixer slot 0
    AudioConnection*      _patchShapeDc;     // _shapeDc → shape mixer slot 0
    AudioConnection*      _patchFMToOsc;     // FM mixer → osc FM input
    AudioConnection*      _patchShapeToOsc;  // Shape mixer → osc shape input

    // =========================================================================
    // AUDIO COMPONENTS — MAIN OSCILLATOR PATH
    // =========================================================================

    AudioSynthWaveformModulated _mainOsc;
    AudioSynthSupersaw*         _supersaw;   // nullptr if disabled (OSC2)
    AudioMixer4                 _outputMix;
    AudioConnection*            _patchMainOsc;   // _mainOsc → output mixer
    AudioConnection*            _patchSupersaw;  // Conditional: only if supersaw

    // =========================================================================
    // FEEDBACK COMB NETWORK (JP-8000 SIMULATION)
    // =========================================================================

    bool  _feedbackEnabled   = false;
    float _feedbackGain      = 0.6f;
    float _feedbackMixLevel  = 0.9f;
    static constexpr float FEEDBACK_DELAY_MS = 5.0f;

    AudioMixer4        _combMixer;
    AudioEffectDelay   _combDelay;
    AudioConnection*   _patchMainToComb;
    AudioConnection*   _patchSupersawToComb;
    AudioConnection*   _patchDelayToComb;
    AudioConnection*   _patchCombToDelay;
    AudioConnection*   _patchDelayToOut;

    // =========================================================================
    // OSCILLATOR STATE
    // =========================================================================

    bool  _supersawEnabled;
    int   _currentType          = 1;       // WAVEFORM_SAWTOOTH
    float _baseFreq             = 440.0f;  // Set once on noteOn()
    float _lastVelocity         = 1.0f;

    // Pitch parameters (stored for getters and DC recalculation)
    float _pitchOffsetSemitones = 0.0f;    // Coarse pitch (-24 to +24)
    float _fineTuneCents        = 0.0f;    // Fine tune (-100 to +100)
    float _detuneHz             = 0.0f;    // Detune in Hz
    float _pitchBendSemitones   = 0.0f;    // Current pitch bend

    // Supersaw
    float _supersawDetune = 0.0f;
    float _supersawMix    = 0.5f;

    // Glide state
    bool  _glideEnabled  = false;
    float _glideTimeMs   = 0.0f;
    float _glideRate     = 0.0f;           // Interpolation rate per sample
    float _glideTargetHz = 440.0f;         // Target frequency for glide
    float _glideCurrentHz = 440.0f;        // Current glide position
    bool  _glideActive   = false;          // True while sliding

    // Shape DC
    float _shapeDcAmp = 0.0f;

    // External pitch DC (from FREQ_DC CC)
    float _externalPitchDcAmp = 0.0f;

    // Arbitrary waveforms
    ArbBank  _arbBank  = ArbBank::BwBlended;
    uint16_t _arbIndex = 0;

    // =========================================================================
    // PRIVATE HELPERS
    // =========================================================================

    /** Load arbitrary waveform table into _mainOsc. */
    void _applyArbWave();

    /** Recalculate the combined pitch offset DC amplitude.
     *  Called when coarse, fine, or detune changes.
     *  Combines all three into a single FM-scaled DC value on _pitchOffsetDc. */
    void _updatePitchOffsetDc();

    /** Recalculate the glide DC amplitude from current glide position.
     *  The glide DC represents the DIFFERENCE between glide position and
     *  the note's base frequency, in FM-scaled semitones. */
    void _updateGlideDc();
};
