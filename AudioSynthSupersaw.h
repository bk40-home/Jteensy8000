// AudioSynthSupersaw.h
// =============================================================================
// JP-8000-style supersaw oscillator — 7 detuned sawtooth voices summed together.
//
// AUDIO INPUTS (2 inputs, same convention as AudioSynthWaveformJT):
//   Input 0 — Frequency modulation (FM / pitch mod)
//             Signal range: ±1.0 (int16 full-scale = ±32767)
//             Effect: multiplies all 7 voice phase increments by 2^(in * FM_OCTAVE_RANGE)
//             At FM_OCTAVE_RANGE=10 a ±1.0 signal shifts pitch ±10 octaves.
//             Patch a DC offset or LFO here using the same FM mixer as the
//             main oscillator — consistent scaling across both OSC paths.
//
//   Input 1 — Phase modulation
//             Signal range: ±1.0 (int16 full-scale = ±32767)
//             Effect: adds a per-sample phase offset to all 7 voices.
//             phaseMod(degrees) sets the maximum swing in degrees (0..360).
//             At 180° a ±full-scale input shifts phase by ±half-cycle.
//
// CONNECT NOTHING: both inputs are optional — receiveReadOnly() returns nullptr
//   when disconnected, which the update() guards against at zero extra cost.
//
// DESIGN NOTE ON FM SCALING:
//   The per-block float approach is used rather than the int32 polynomial trick
//   in Synth_Waveform.cpp because the supersaw already works in float for its
//   phase accumulators.  powf() is called ONCE per block (not per sample) so
//   the cost is negligible (~7 CPU cycles amortised over 128 samples).
// =============================================================================

#pragma once

#include <Arduino.h>
#include "AudioStream.h"

#define SUPERSAW_VOICES 7

class AudioSynthSupersaw : public AudioStream {
public:
    AudioSynthSupersaw();

    // -------------------------------------------------------------------------
    // Core parameters
    // -------------------------------------------------------------------------

    // Base frequency in Hz — set once on noteOn via OscillatorBlock.
    // Pitch modulation (FM input 0) shifts from this base without recomputing.
    void setFrequency(float freq);

    void setAmplitude(float amp);
    void setDetune(float amount);   // 0.0 (unison) .. 1.0 (wide)
    void setMix(float mix);         // 0.0 (centre only) .. 1.0 (all sides)
    void setOutputGain(float gain);

    // -------------------------------------------------------------------------
    // Modulation range setters
    // -------------------------------------------------------------------------

    // Set FM depth in octaves — must match FM_OCTAVE_RANGE used by the main
    // oscillator so both OSC paths respond identically to the pitch mixer.
    // Default: 10.0 (±10 octaves at full signal, matching AudioSynthWaveformJT).
    void frequencyModulation(float octaves);

    // Set phase modulation range in degrees.
    // Default: 180.0° — full-scale input = ±half-cycle phase shift.
    void phaseModulation(float degrees);

    // -------------------------------------------------------------------------
    // Quality options
    // -------------------------------------------------------------------------

    // 2× oversampling — reduces aliasing at cost of ~2× CPU. Off by default.
    void setOversample(bool enable);

    // PolyBLEP band-limited saws — better alias rejection than oversample for
    // most use cases, lower CPU cost. Off by default.
    void setBandLimited(bool enable);

    // -------------------------------------------------------------------------
    // Mix gain compensation
    // -------------------------------------------------------------------------

    // When enabled, output is boosted as mix increases to compensate for the
    // natural level drop when detuned voices partially cancel each other.
    void setMixCompensation(bool enable);
    void setCompensationMaxGain(float maxGain);

    // -------------------------------------------------------------------------
    // Playback control
    // -------------------------------------------------------------------------
    void noteOn();  // Reset phases to initial offsets for consistent attack

    // AudioStream interface
    virtual void update(void) override;

private:
    // Audio input queue — 2 slots for FM and phase mod inputs
    audio_block_t* inputQueueArray[2];

    // Base oscillator parameters
    float freq;
    float detuneAmt;
    float mixAmt;
    float amp;
    float outputGain;

    // 7-voice state
    float phases[SUPERSAW_VOICES];    // Current phase [0..1)
    float phaseInc[SUPERSAW_VOICES];  // Phase increment per sample at base freq
    float gains[SUPERSAW_VOICES];     // Per-voice amplitude

    // High-pass filter state (removes DC from summed oscillators)
    float hpfPrevIn;
    float hpfPrevOut;
    float hpfAlpha;

    // Modulation range — stored so update() can scale incoming signals
    float fmOctaveRange;       // Octaves per ±1.0 FM input.  Default: 10.0
    float phaseModRange;       // Phase range in normalised units [0..1].
                               // Derived from degrees: range = degrees/360.0.
                               // Default: 0.5 (180°)

    // Quality flags
    bool oversample2x;
    bool usePolyBLEP;

    // Mix compensation
    bool  mixCompensationEnabled;
    float compensationMaxGain;

    // Internal helpers
    float detuneCurve(float x);
    void  calculateIncrements();
    void  calculateGains();
    void  calculateHPF();
};
