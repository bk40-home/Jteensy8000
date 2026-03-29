// AudioSynthSupersaw.h
// =============================================================================
// JP-8000-style Super Saw oscillator — 7 detuned sawtooth voices summed.
//
// Emulation based on Adam Szabó's analysis ("How to Emulate the Super Saw",
// KTH 2010).  Four properties define the sound:
//
//   1. DETUNE — Non-linear 11th-order polynomial maps CC 0..127 to the actual
//      frequency spread.  The curve is very gradual below 50%, allowing fine
//      string/pad sounds, then rises steeply above 90% for extreme spreads.
//      Seven oscillators: 3 below centre, centre, 3 above.  Frequency offsets
//      are ratios relative to the centre oscillator (Table 1, Szabó p.9).
//
//   2. MIX — Centre oscillator: linear fade  y = -0.55366x + 0.99785
//            Side oscillators:  parabolic     y = -0.73764x² + 1.2841x + 0.044372
//      (Szabó p.15).  Centre never fully mutes; sides peak around x ≈ 0.87.
//
//   3. PHASE — Random on every noteOn().  Each trigger assigns a new random
//      phase (0.0–1.0) to all 7 oscillators, creating organic variation
//      between notes (Szabó §3.4, p.20-22).
//
//   4. SHAPE — Naive (aliased) sawtooths through a pitch-tracked 1-pole HPF.
//      The aliasing adds "airiness"; the HPF removes sub-fundamental rumble.
//      Optional PolyBLEP mode for band-limited output when preferred.
//
// AUDIO INPUTS (2 inputs, same convention as AudioSynthWaveformJT):
//   Input 0 — Frequency modulation (FM / pitch mod)
//             Signal range: ±1.0 (int16 full-scale = ±32767)
//             Effect: multiplies all 7 voice phase increments by
//             2^(in × fmOctaveRange).  Default range: ±10 octaves.
//
//   Input 1 — Phase modulation
//             Signal range: ±1.0 (int16 full-scale = ±32767)
//             Effect: adds a per-sample phase offset to all 7 voices.
//             phaseModulation(degrees) sets the maximum swing (0–360°).
//
// Both inputs are optional — nullptr from receiveReadOnly() costs zero.
// =============================================================================

#pragma once

#include <Arduino.h>
#include "AudioStream.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define SUPERSAW_VOICES     7           // JP-8000: 3 below + centre + 3 above
#define SUPERSAW_CENTRE_IDX 3           // Index of the centre oscillator

class AudioSynthSupersaw : public AudioStream {
public:
    AudioSynthSupersaw();

    // -------------------------------------------------------------------------
    // Core parameters — all setters are safe to call from any context
    // -------------------------------------------------------------------------

    /// Set the base frequency in Hz.  Called once per noteOn by OscillatorBlock.
    /// FM input 0 shifts pitch from this base without recomputing.
    void setFrequency(float freqHz);

    /// Master amplitude (0.0–1.0).  Multiplied into every voice gain.
    void setAmplitude(float amp);

    /// Detune amount: 0.0 = unison, 1.0 = maximum spread.
    /// Internally mapped through the Szabó 11th-order polynomial LUT.
    void setDetune(float amount);

    /// Mix amount: 0.0 = centre oscillator only, 1.0 = max side level.
    /// Uses Szabó's measured centre (linear) + side (parabolic) curves.
    void setMix(float mix);

    /// Post-mix output gain (0.0–2.0).  Applied after HPF, before int16 clip.
    void setOutputGain(float gain);

    // -------------------------------------------------------------------------
    // Modulation range setters
    // -------------------------------------------------------------------------

    /// FM depth in octaves — must match FM_OCTAVE_RANGE used by the main
    /// oscillator so both paths respond identically to the pitch mixer.
    /// Default: 10.0.  Clamped to [0.1, 12.0].
    void frequencyModulation(float octaves);

    /// Phase modulation range in degrees.  Default: 180° (half-cycle swing).
    /// Stored internally as normalised fraction of a full cycle.
    void phaseModulation(float degrees);

    // -------------------------------------------------------------------------
    // Quality options
    // -------------------------------------------------------------------------

    /// 2× oversampling — halves aliasing at ~2× CPU.  Off by default.
    void setOversample(bool enable);

    /// PolyBLEP band-limited saws — better alias rejection than oversampling
    /// for most use cases, at lower CPU cost.  Off by default.
    /// NOTE: Szabó found that the JP-8000 uses naive (aliased) saws — the
    /// aliasing noise contributes "airiness".  Enable PolyBLEP only if you
    /// prefer a cleaner sound.
    void setBandLimited(bool enable);

    // -------------------------------------------------------------------------
    // Mix gain compensation
    // -------------------------------------------------------------------------

    /// When enabled, output is boosted as mix increases to compensate for
    /// partial cancellation between detuned voices.
    void setMixCompensation(bool enable);
    void setCompensationMaxGain(float maxGain);

    // -------------------------------------------------------------------------
    // Playback control
    // -------------------------------------------------------------------------

    /// Randomise all 7 oscillator phases.  Call on every note trigger.
    /// Per Szabó §3.4: each noteOn must produce a different waveform shape
    /// by assigning random phases — this is what makes the Super Saw organic.
    void noteOn();

    /// AudioStream interface — generates one block of 128 int16 samples.
    virtual void update(void) override;

private:
    // Audio input queue — 2 slots: FM (input 0) and phase mod (input 1)
    audio_block_t* inputQueueArray[2];

    // -------------------------------------------------------------------------
    // Oscillator state
    // -------------------------------------------------------------------------
    float _freq;                                // Base frequency (Hz)
    float _detuneAmt;                           // Raw detune CC [0.0, 1.0]
    float _mixAmt;                              // Raw mix CC [0.0, 1.0]
    float _amp;                                 // Master amplitude [0.0, 1.0]
    float _outputGain;                          // Post-HPF gain [0.0, 2.0]

    // Per-voice state
    float _phase[SUPERSAW_VOICES];              // Current phase [0.0, 1.0)
    float _phaseInc[SUPERSAW_VOICES];           // Phase increment per sample
    float _gain[SUPERSAW_VOICES];               // Per-voice amplitude

    // -------------------------------------------------------------------------
    // High-pass filter state (1-pole, pitch-tracked)
    // -------------------------------------------------------------------------
    float _hpfPrevIn;                           // Previous input sample
    float _hpfPrevOut;                          // Previous output sample
    float _hpfAlpha;                            // Filter coefficient

    // -------------------------------------------------------------------------
    // Modulation range
    // -------------------------------------------------------------------------
    float _fmOctaveRange;                       // Octaves per ±1.0 FM input
    float _phaseModRange;                       // Phase range [0.0, 1.0] cycles

    // -------------------------------------------------------------------------
    // Quality flags
    // -------------------------------------------------------------------------
    bool _oversample2x;
    bool _usePolyBLEP;

    // -------------------------------------------------------------------------
    // Mix compensation
    // -------------------------------------------------------------------------
    bool  _mixCompensationEnabled;
    float _compensationMaxGain;

    // -------------------------------------------------------------------------
    // Internal helpers — called by setters, never from ISR directly
    // -------------------------------------------------------------------------

    /// Look up the Szabó non-linear detune curve.
    /// Input: x in [0.0, 1.0] (normalised CC).
    /// Output: detune depth in [0.0, 1.0] to multiply against kFreqOffsetsMax.
    float _detuneCurve(float x);

    /// Recalculate per-voice phase increments from _freq and _detuneAmt.
    void _calculateIncrements();

    /// Recalculate per-voice gains from _mixAmt, _amp, and Szabó mix curves.
    void _calculateGains();

    /// Recalculate the pitch-tracked HPF coefficient from _freq.
    void _calculateHPF();
};
