// =============================================================================
// OscillatorBlock.h — JT-8000 oscillator with hardware-driven pitch modulation
//
// ARCHITECTURE:
//   ALL pitch modulation flows through AudioSynthWaveformJT's FM input.
//   The base frequency is set ONCE on noteOn() via .frequency().
//   All pitch offsets (coarse, fine, detune, pitch bend, external DC) are
//   combined in software into a single AudioSynthWaveformDc (_combinedPitchDc)
//   feeding FM mixer slot 0.  No AudioMixer4 pre-stage is needed because all
//   these sources are constant-per-block DC values — summing them in software
//   is free compared to running a full AudioMixer4 in the ISR.
//
// FM MIXER SLOT ALLOCATION (_frequencyModMixer / AudioMixer4):
//   Slot 0 : Pitch DC pre-mixer output  — coarse + fine + detune + bend +
//             external (+ cross-mod audio when JT_OPT_CROSS_MOD enabled)
//   Slot 1 : LFO1 pitch output  (wired by SynthEngine)
//   Slot 2 : LFO2 pitch output  (wired by SynthEngine)
//   Slot 3 : Pitch envelope     (wired by SynthEngine)
//
// CROSS MODULATION (JT_OPT_CROSS_MOD):
//   A two-slot AudioMixer4 (_crossModPreMixer) sits between _combinedPitchDc
//   and _frequencyModMixer slot 0:
//
//     _combinedPitchDc ──► [_crossModPreMixer slot 0, gain 1.0]
//     OSC2 audio       ──► [_crossModPreMixer slot 1, gain = depth]
//                                     │
//                                     ▼
//                         _frequencyModMixer slot 0
//
//   This keeps all four _frequencyModMixer slots free for their existing
//   assignments (LFO1 / LFO2 / pitch env) — no slot conflicts.
//   When depth == 0.0 the pre-mixer slot 1 gain is zero; the pre-mixer
//   just passes the pitch DC through at unity with no audio cost beyond
//   the unconditional AudioMixer4::update() call (one block per ISR cycle).
//
//   VoiceBlock wires _osc2.output() → _osc1.crossModPreMixerRef() slot 1
//   in the constructor.  setCrossModGain() controls the depth.
//
//   When JT_OPT_CROSS_MOD is NOT defined, the pre-mixer is absent and
//   _combinedPitchDc connects directly to _frequencyModMixer slot 0 as
//   before — no code path change in that build.
//
// FM SCALING:
//   _mainOsc.frequencyModulation(FM_OCTAVE_RANGE) where FM_OCTAVE_RANGE = 10.
//   ±1.0 on the FM input shifts pitch by ±10 octaves.
//   To shift by S semitones: amplitude = S / (FM_OCTAVE_RANGE × 12) = S / 120.
//   FM_SEMITONE_SCALE = 1/120 ≈ 0.00833.
//
// PITCH UNITS:
//   ALL pitch parameters entering this class are in SEMITONES.
//   The Hz→semitone conversion for detune is done at the CC edge
//   (SynthEngine::handleControlChange), NOT inside OscillatorBlock.
//   This eliminates the frequency-dependent conversion that previously
//   lived in _updateStaticPitchFm() and removes a class of pitch-drift bugs.
//
// GLIDE:
//   Implemented entirely via direct _mainOsc.frequency() / supersaw.setFrequency()
//   calls in update().  The combined DC does not change during glide — the base
//   frequency slides while LFO/bend/envelope modulate on top.
//
// CPU SAVINGS vs. previous four-DC-source pre-mixer design:
//   - 3 fewer AudioSynthWaveformDc objects per OscillatorBlock  (×16 = 48 fewer)
//   - 1 fewer AudioMixer4 per OscillatorBlock                   (×16 = 16 fewer)
//   - No powf() anywhere in this file
//   - No frequency-dependent conversion in _updateStaticPitchFm()
//   - Glide is the only periodic work; dirty flag makes idle voices free
//   - Cross-mod pre-mixer (JT_OPT_CROSS_MOD): +1 AudioMixer4 on OSC1 only
//     (8 total, ~20 µs/block); zero cost when depth == 0.
//
// Synth_Waveform.h (local JT fork) is included below — do NOT also include
// the library <synth_waveform.h>.  The local copy's include guard matches the
// library guard so only one definition is compiled.
// =============================================================================

#pragma once

#include "Audio.h"
#include "WaveForms.h"
#include "AKWF_All.h"
#include "AudioSynthSupersaw.h"
#include "AudioSynthWaveformJT.h" // JT modulated oscillator — own guard, no ordering dependency
#include "JT8000_OptFlags.h"
#include "DebugTrace.h"

// FM range constant — must match the argument to _mainOsc.frequencyModulation().
// Defined here (not in .cpp) so SynthEngine can reference it when computing
// LFO / pitch-envelope gain values for the FM mixer.
static constexpr float FM_OCTAVE_RANGE   = 10.0f;
static constexpr float FM_SEMITONE_SCALE = 1.0f / (FM_OCTAVE_RANGE * 12.0f);  // ≈ 0.00833

class OscillatorBlock {
public:
    // =========================================================================
    // LIFECYCLE
    // =========================================================================

    /**
     * @brief Construct oscillator.
     * @param enableSupersaw  true for OSC1 (has supersaw), false for OSC2.
     *                        Allocating supersaw on OSC2 wastes ~10 audio objects
     *                        even when not in use, so OSC2 always passes false.
     */
    OscillatorBlock(bool enableSupersaw = false);

    /**
     * @brief Periodic update — only needed for glide ramp.
     *
     * Called from VoiceBlock::update().  Returns immediately when no glide is
     * active, costing zero CPU for static (non-gliding) notes.
     * No powf(), no .frequency() calls during idle — all pitch is hardware-routed.
     */
    void update();

    /**
     * @brief Start a note.
     * @param frequency  Target pitch in Hz.
     * @param velocity   Normalised velocity 0.0–1.0 (do NOT divide by 127 again).
     *
     * Sets the base frequency ONCE via .frequency().  All subsequent pitch
     * shifts come from the FM mixer (DC sources + LFO + pitch env).
     */
    void noteOn(float frequency, float velocity);

    /** Gate the oscillator off (amplitude → 0).  FX tails continue independently. */
    void noteOff();

    // =========================================================================
    // WAVEFORM & AMPLITUDE
    // =========================================================================

    void setWaveformType(int type);
    void setAmplitude(float amplitude);

    // =========================================================================
    // PITCH CONTROL — all routes combine into _combinedPitchDc (software sum)
    //
    // ALL pitch parameters are in SEMITONES.  The caller (SynthEngine CC handler)
    // is responsible for any Hz→semitone conversion at the MIDI edge.
    // =========================================================================

    /** Coarse pitch offset in semitones (e.g. −24, 0, +12, +24). */
    void setPitchOffset(float semitones);

    /** Fine tune in cents (−100 to +100). */
    void setFineTune(float cents);

    /**
     * @brief Detune offset in SEMITONES.
     *
     * For unison spread: already in semitones from _applyUnisonDetune().
     * For CC detune: SynthEngine converts the CC Hz value to semitones
     * before calling this.  No frequency-dependent conversion here.
     */
    void setDetune(float semitones);

    /**
     * @brief Pitch-wheel bend in semitones (±range).
     *
     * Updated per wheel event by SynthEngine.  Stored as a software float;
     * the combined DC amplitude is recalculated immediately — no audio object
     * for bend alone needed.
     */
    void setPitchBend(float semitones);

    /**
     * @brief External static pitch offset (e.g. FREQ_DC CC).
     * @param fmScaledAmplitude  Already in FM-mixer units (semitones × FM_SEMITONE_SCALE).
     *                           Caller is responsible for the scaling.
     */
    void setExternalPitchDc(float fmScaledAmplitude);

    /**
     * @brief Set step sequencer pitch offset (FM-scaled).
     *        Folds into _combinedPitchDc alongside coarse/fine/detune/bend/external.
     *        Caller must pre-scale: semitones × FM_SEMITONE_SCALE.
     */
    void setSeqPitchOffset(float fmScaledOffset);

    // =========================================================================
    // CROSS MODULATION — depth control (OSC1 only, JT_OPT_CROSS_MOD)
    // =========================================================================

#if JT_OPT_CROSS_MOD
    /**
     * @brief Set the cross-mod FM injection gain on the pitch DC pre-mixer.
     *
     * This is the gain applied to OSC2's audio at slot 1 of _crossModPreMixer.
     * Use crossModDepthFromCC() (CrossModSync.h) to derive this value from a CC.
     *
     * @param gain  0.0 = no modulation.
     *              CROSS_MOD_FULL_SCALE = maximum depth (±JT_CROSS_MOD_MAX_OCTAVES).
     *
     * Only meaningful on OSC1; calling this on OSC2 has no effect (pre-mixer
     * slot 1 is never wired on OSC2).
     */
    void setCrossModGain(float gain);
#endif

    // =========================================================================
    // GLIDE (PORTAMENTO)
    // =========================================================================

    void setGlideEnabled(bool enabled);
    void setGlideTime(float milliseconds);

    // =========================================================================
    // SUPERSAW CONTROL (OSC1 only)
    // =========================================================================

    void setSupersawDetune(float amount);
    void setSupersawMix(float mix);

    // =========================================================================
    // SHAPE / PWM DC
    // =========================================================================

    void setShapeDcAmp(float amplitude);

    // =========================================================================
    // ARBITRARY WAVEFORM
    // =========================================================================

    void setArbBank(ArbBank bank);
    void setArbTableIndex(uint16_t index);
    ArbBank  getArbBank()       const { return _arbBank; }
    uint16_t getArbTableIndex() const { return _arbIndex; }

    // =========================================================================
    // FEEDBACK OSCILLATION (JP-8000 comb-delay simulation)
    // =========================================================================

    void  setFeedbackAmount(float amount);
    void  setFeedbackMix(float mix);
    bool  getFeedbackEnabled() const { return _feedbackEnabled; }
    float getFeedbackAmount()  const { return _feedbackGain; }
    float getFeedbackMix()     const { return _feedbackMixLevel; }

    // =========================================================================
    // PARAMETER GETTERS — all pitch values in semitones
    // =========================================================================

    int   getWaveform()        const { return _currentType; }
    float getPitchOffset()     const { return _pitchOffsetSemitones; }
    float getFineTune()        const { return _fineTuneCents; }
    float getDetune()          const { return _detuneSemitones; }
    float getSupersawDetune()  const { return _supersawDetune; }
    float getSupersawMix()     const { return _supersawMix; }
    bool  getGlideEnabled()    const { return _glideEnabled; }
    float getGlideTime()       const { return _glideTimeMs; }
    float getShapeDcAmp()      const { return _shapeDcAmp; }
    float getExternalPitchDc() const { return _externalPitchDcAmp; }
    float getBaseFreq()        const { return _baseFreq; }

    // =========================================================================
    // AUDIO OUTPUT & MOD MIXER ACCESS
    // =========================================================================

    /** Final oscillator output (after output mixer + optional feedback path). */
    AudioStream& output();

    /**
     * @brief FM (pitch) modulation mixer.
     *
     * Slot 0 carries the pitch DC pre-mixer output (see header notes).
     * SynthEngine wires LFO1 → slot 1, LFO2 → slot 2, pitch env → slot 3.
     * Do NOT wire into slot 0 from outside — it is driven internally.
     */
    AudioMixer4& frequencyModMixer();

    /** Shape / PWM modulation mixer.  SynthEngine wires LFO shape into slot 1+. */
    AudioMixer4& shapeModMixer();

#if JT_OPT_CROSS_MOD
    /**
     * @brief Cross-mod pre-mixer reference (OSC1 only).
     *
     * VoiceBlock wires OSC2 output → slot 1 of this mixer.
     * Slot 0 is driven internally by _combinedPitchDc.
     * Slot 1 gain is controlled by setCrossModGain().
     * Slots 2 and 3 are unused.
     *
     * Only call on OSC1 (enableSupersaw = true instance).  Calling on OSC2
     * returns a valid reference but slot 1 is never driven with audio.
     */
    AudioMixer4& crossModPreMixerRef();
#endif

private:
    // =========================================================================
    // AUDIO OBJECTS — PITCH MODULATION CHAIN
    //
    // DESIGN NOTE: four DC sources + an AudioMixer4 pre-stage were previously
    // used to combine pitch offset, bend, glide and external DC before the main
    // FM mixer.  Because all four are constant-per-block DC values, summing them
    // as floating-point scalars in software is free (no ISR overhead), and the
    // result is written to a single AudioSynthWaveformDc.  This removes 4 audio
    // objects (3 DC + 1 mixer) per instance — 64 fewer objects in the graph with
    // 8 voices × 2 oscillators each, saving roughly 10–14 % CPU.
    // =========================================================================

    // Single DC source — holds the software sum of:
    //   coarse pitch offset + fine tune + detune (all in semitones, FM-scaled)
    //   + pitch bend (FM-scaled)
    //   + external pitch DC (FM-scaled, pre-scaled by caller)
    // Updated on any change to those sources via _updateCombinedPitchDc().
    AudioSynthWaveformDc  _combinedPitchDc;

    // Shape / PWM DC (independent of pitch, no need to consolidate with above)
    AudioSynthWaveformDc  _shapeDc;

#if JT_OPT_CROSS_MOD
    // Cross-mod pre-mixer — sits between _combinedPitchDc and FM mixer slot 0.
    //   Slot 0: _combinedPitchDc (gain 1.0, always)
    //   Slot 1: OSC2 audio (gain = cross-mod depth, wired by VoiceBlock)
    //   Slots 2-3: unused
    // Output feeds _frequencyModMixer slot 0.
    AudioMixer4           _crossModPreMixer;
    AudioConnection*      _patchPitchDcToCrossMod;  // _combinedPitchDc → pre-mixer slot 0
    AudioConnection*      _patchCrossModToFM;        // pre-mixer output → FM mixer slot 0
#else
    // Without cross-mod, pitch DC connects directly to FM mixer slot 0.
    // _patchPitchDcToFM is the original name used in the rest of the .cpp.
#endif

    // FM (pitch) mixer — slot 0 driven by pitch DC (or pre-mixer); slots 1-3 by SynthEngine
    AudioMixer4           _frequencyModMixer;

    // Shape / PWM mixer — slot 0 driven by _shapeDc; slots 1-3 by SynthEngine
    AudioMixer4           _shapeModMixer;

    // Static AudioConnection objects owned by this block
#if !JT_OPT_CROSS_MOD
    AudioConnection*      _patchPitchDcToFM;     // _combinedPitchDc → FM mixer slot 0
#endif
    AudioConnection*      _patchShapeDcToShape;  // _shapeDc → shape mixer slot 0
    AudioConnection*      _patchFMToOsc;          // FM mixer → main osc FM input (input 0)
    AudioConnection*      _patchShapeToOsc;       // Shape mixer → main osc shape input (input 1)

    // =========================================================================
    // AUDIO OBJECTS — MAIN OSCILLATOR PATH
    // =========================================================================

    AudioSynthWaveformJT _mainOsc;
    AudioSynthSupersaw*         _supersaw;       // nullptr when OSC2 (saves ~10 objects)
    AudioMixer4                 _outputMix;
    AudioConnection*            _patchMainOsc;   // _mainOsc → output mixer slot 0
    AudioConnection*            _patchSupersaw;  // _supersaw → output mixer slot 1 (or nullptr)

    // =========================================================================
    // AUDIO OBJECTS — FEEDBACK COMB NETWORK (JP-8000 simulation)
    // =========================================================================

    bool  _feedbackEnabled  = false;
    float _feedbackGain     = 0.6f;
    float _feedbackMixLevel = 0.9f;
    static constexpr float FEEDBACK_DELAY_MS = 5.0f;

    AudioMixer4        _combMixer;
    AudioEffectDelay   _combDelay;
    AudioConnection*   _patchMainToComb;      // _mainOsc → comb mixer slot 0
    AudioConnection*   _patchSupersawToComb;  // _supersaw → comb mixer slot 1 (or nullptr)
    AudioConnection*   _patchDelayToComb;     // _combDelay → comb mixer slot 2
    AudioConnection*   _patchCombToDelay;     // _combMixer → _combDelay
    AudioConnection*   _patchDelayToOut;      // _combDelay → output mixer slot 2

    // Supersaw modulation — FM and shape mixer outputs feed the supersaw so both
    // OSC paths respond identically to LFO/envelope modulation.
    AudioConnection*   _patchFMToSupersaw;     // FM mixer → supersaw FM input (or nullptr)
    AudioConnection*   _patchShapeToSupersaw;  // Shape mixer → supersaw PM input (or nullptr)

    // =========================================================================
    // OSCILLATOR STATE
    // =========================================================================

    bool  _supersawEnabled;
    int   _currentType      = 1;       // WAVEFORM_SAWTOOTH default
    float _baseFreq         = 440.0f;  // Set once on noteOn(), read during glide
    float _lastVelocity     = 1.0f;

    // Pitch components — ALL stored as semitones for consistent units
    float _pitchOffsetSemitones = 0.0f;   // Coarse (e.g. −24..+24)
    float _fineTuneCents        = 0.0f;   // Fine tune (−100..+100 cents)
    float _detuneSemitones      = 0.0f;   // Detune spread (semitones, converted at CC edge)
    float _pitchBendSemitones   = 0.0f;   // Current wheel position

    // Pre-computed FM-scaled contributions (updated by individual setters)
    float _staticPitchFm      = 0.0f;   // offset + fine + detune combined, FM-scaled
    float _pitchBendFm        = 0.0f;   // bend, FM-scaled
    float _externalPitchDcAmp = 0.0f;   // external DC, already FM-scaled
    float _seqPitchOffset     = 0.0f;   // step sequencer pitch, already FM-scaled

    // Supersaw parameters
    float _supersawDetune = 0.0f;
    float _supersawMix    = 0.5f;

    // Glide state
    bool  _glideEnabled   = false;
    float _glideTimeMs    = 0.0f;
    float _glideRate      = 0.0f;         // Fraction of remaining distance per audio-block
    float _glideTargetHz  = 440.0f;
    float _glideCurrentHz = 440.0f;
    bool  _glideActive    = false;

    // Shape DC (not related to pitch)
    float _shapeDcAmp = 0.0f;

    // Arbitrary waveform selection
    ArbBank  _arbBank  = ArbBank::BwBlended;
    uint16_t _arbIndex = 0;

    // =========================================================================
    // PRIVATE HELPERS
    // =========================================================================

    /** Load the selected arbitrary waveform table into _mainOsc. */
    void _applyArbWave();

    /**
     * @brief Recalculate the combined static pitch DC amplitude.
     *
     * Combines coarse offset (semitones), fine tune (cents→semitones) and
     * detune (semitones) into a single FM-scaled scalar in _staticPitchFm.
     * Then calls _updateCombinedPitchDc() to push the final sum to the
     * audio graph.
     *
     * No frequency-dependent conversion — all inputs are already semitones.
     * Call after any change to coarse, fine or detune.
     */
    void _updateStaticPitchFm();

    /**
     * @brief Push the software sum of all pitch contributions to _combinedPitchDc.
     *
     * Combined = _staticPitchFm + _pitchBendFm + _externalPitchDcAmp + _seqPitchOffset.
     * Call after any individual component changes.
     * Clamped to ±48 semitones × FM_SEMITONE_SCALE to prevent runaway.
     */
    void _updateCombinedPitchDc();
};
