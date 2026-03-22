/*
 * AudioEffectPlateReverbJT.h
 * ==========================
 * JT-8000 stereo plate reverb — Dattorro topology with extended decay.
 *
 * DESIGN GOALS:
 *   - Massive, lush trance/Eventide-style reverb tails (up to ~30 s decay)
 *   - All delay memory allocated from PSRAM (falls back to heap if absent)
 *   - Zero heap allocation in the audio path
 *   - Drop-in replacement for hexefx AudioEffectPlateReverb_i16:
 *     same public API so FXChainBlock needs only a header swap
 *   - Stereo in (2 inputs), stereo out (2 outputs)
 *
 * TOPOLOGY (Dattorro "Effect Design Part 1", 1997):
 *
 *   Input ──► Pre-delay ──► Input diffusers (4× allpass)
 *                                    │
 *                        ┌───────────┴───────────┐
 *                        ▼                       ▼
 *                   Tank Left                Tank Right
 *              (APF → delay → LPF      (APF → delay → LPF
 *               → HPF → decay FB)       → HPF → decay FB)
 *                   │    ╳                   │
 *                   └────┼───────────────────┘
 *                        │
 *                   Decorrelated taps ──► Stereo output
 *
 * CPU NOTES:
 *   - bypass_set(true) causes update() to return immediately (zero CPU)
 *   - All filtering is first-order (one multiply + one state var per filter)
 *   - Pre-delay uses simple integer indexing (no interpolation needed)
 *   - Tank allpass modulation uses a cheap triangle LFO
 *
 * MEMORY (PSRAM):
 *   Total buffer requirement ≈ 42 KB at 44.1 kHz (see .cpp for breakdown)
 *   Fits comfortably in the 8 MB PSRAM on Teensy 4.1
 *
 * REFERENCES:
 *   [1] Jon Dattorro, "Effect Design Part 1: Reverberator and Other
 *       Filters", J. Audio Eng. Soc., 1997
 *   [2] Sean Costello, "Valhalla Shimmer" design notes (public)
 *   [3] Piotr Zapart (hexefx) — original i16 plate reverb that inspired
 *       this implementation
 */

#pragma once

#include <Arduino.h>
#include "AudioStream.h"

// =============================================================================
// AudioEffectPlateReverbJT
// =============================================================================

class AudioEffectPlateReverbJT : public AudioStream {
public:
    AudioEffectPlateReverbJT();
    ~AudioEffectPlateReverbJT();

    // =========================================================================
    // PUBLIC API — matches hexefx AudioEffectPlateReverb_i16 for drop-in use
    // =========================================================================

    // Room size / decay time.  0.0 = tight room, 1.0 = infinite hold.
    // Internally maps to a decay coefficient (0.05 .. 0.9995).
    void size(float n);

    // High-frequency damping in the tank.  0.0 = bright, 1.0 = very dark.
    // Controls the cutoff of a first-order LPF inside each tank loop.
    void hidamp(float n);

    // Low-frequency damping in the tank.  0.0 = full bass, 1.0 = thin.
    // Controls the cutoff of a first-order HPF inside each tank loop.
    void lodamp(float n);

    // Wet/dry mix.  0.0 = fully dry, 1.0 = fully wet.
    // NOTE: In FXChainBlock, mix is always set to 1.0 and the external
    // mixer handles wet/dry.  Provided for standalone/testing use.
    void mix(float n);

    // Hard bypass — when true, update() returns immediately (zero CPU).
    // Called by FXChainBlock::updateReverbBypass().
    void bypass_set(bool state);

    // =========================================================================
    // EXTENDED API — new controls not in the hexefx original
    // =========================================================================

    // Pre-delay time in milliseconds.  0..250 ms.
    // Adds space between dry signal and reverb onset — essential for clarity
    // in dense trance mixes.
    void predelay(float ms);

    // Modulation depth.  0.0 = no modulation, 1.0 = full.
    // Controls the pitch wobble of the tank allpass filters.
    // Higher values create a lusher, more diffuse tail.
    void modDepth(float n);

    // Modulation rate in Hz.  0.1 .. 5.0 Hz.
    // Controls the speed of the internal triangle LFO.
    void modRate(float hz);

    // Freeze / infinite hold.  When true, decay = 1.0 and input is muted.
    // The current tail sustains indefinitely — great for ambient pads.
    void freeze(bool state);

    // AudioStream interface
    virtual void update(void);

private:
    // =========================================================================
    // DELAY LINE — simple circular buffer with integer read/write
    // =========================================================================

    struct DelayLine {
        float*   buf;       // buffer pointer (PSRAM or heap)
        uint32_t len;       // buffer length in samples
        uint32_t writeIdx;  // current write position

        // Write one sample and advance the write pointer
        inline void write(float sample) {
            buf[writeIdx] = sample;
            if (++writeIdx >= len) writeIdx = 0;
        }

        // Read from a fixed tap position (samples behind write head)
        inline float read(uint32_t delaySamples) const {
            // Branchless modular index using masking would require
            // power-of-two lengths.  We use a conditional subtract instead
            // since these run once per sample, not per-block.
            uint32_t idx = writeIdx >= delaySamples
                         ? writeIdx - delaySamples
                         : writeIdx + len - delaySamples;
            return buf[idx];
        }

        // Read with linear interpolation (for modulated allpass taps)
        inline float readInterp(float delaySamples) const {
            uint32_t intPart  = (uint32_t)delaySamples;
            float    fracPart = delaySamples - (float)intPart;

            float s0 = read(intPart);
            float s1 = read(intPart + 1);
            return s0 + fracPart * (s1 - s0);
        }

        // Zero the entire buffer (called at init and on parameter reset)
        void clear() {
            if (buf) memset(buf, 0, len * sizeof(float));
            writeIdx = 0;
        }
    };

    // =========================================================================
    // ALLPASS FILTER — single first-order allpass delay with feedback
    // =========================================================================

    struct Allpass {
        DelayLine dl;
        float     gain;     // feedback/feedforward coefficient

        // Process one sample through the allpass: y = -g*x + dl + g*y_prev
        inline float process(float input) {
            float delayed = dl.read(dl.len - 1);
            float output  = -gain * input + delayed;
            dl.write(input + gain * output);
            return output;
        }

        // Process with modulated delay (for tank allpass filters)
        inline float processModulated(float input, float modSamples) {
            // Clamp modulated delay to valid range
            float delaySmp = (float)(dl.len - 1) + modSamples;
            if (delaySmp < 1.0f)                   delaySmp = 1.0f;
            if (delaySmp > (float)(dl.len - 1))    delaySmp = (float)(dl.len - 1);

            float delayed = dl.readInterp(delaySmp);
            float output  = -gain * input + delayed;
            dl.write(input + gain * output);
            return output;
        }

        void clear() { dl.clear(); }
    };

    // =========================================================================
    // ONE-POLE FILTERS — minimal CPU for tank damping
    // =========================================================================

    // First-order lowpass: y[n] = (1-a)*x[n] + a*y[n-1]
    struct OnePole_LP {
        float state;
        float coeff;    // 0.0 = no filtering, approaching 1.0 = heavy cut

        inline float process(float input) {
            state = input + coeff * (state - input);
            return state;
        }

        void clear() { state = 0.0f; }
    };

    // First-order highpass: y[n] = x[n] - LP(x[n])
    struct OnePole_HP {
        float state;
        float coeff;    // 0.0 = no filtering, approaching 1.0 = heavy bass cut

        inline float process(float input) {
            state = input + coeff * (state - input);
            return input - state;
        }

        void clear() { state = 0.0f; }
    };

    // =========================================================================
    // TOPOLOGY COMPONENTS
    // =========================================================================

    // Pre-delay (mono, before diffusers)
    DelayLine _predelay;

    // Input diffuser chain — 4 series allpass filters
    // Smears the input impulse into a dense texture before entering the tank.
    static constexpr uint8_t NUM_INPUT_DIFFUSERS = 4;
    Allpass _inputDiffuser[NUM_INPUT_DIFFUSERS];

    // Tank: two mirrored halves with cross-feedback
    // Each half: modulated allpass → delay → LPF → HPF → decay gain → cross to other
    Allpass   _tankAPF[2];          // modulated allpass per tank half
    DelayLine _tankDelay[2];        // long delay per tank half
    OnePole_LP _tankLPF[2];        // high-frequency damping
    OnePole_HP _tankHPF[2];        // low-frequency damping

    // =========================================================================
    // PARAMETERS (cached, updated at block rate)
    // =========================================================================

    float _decay;               // tank feedback coefficient (0.05 .. 0.9995)
    float _hiDampCoeff;         // LPF coefficient for hidamp()
    float _loDampCoeff;         // HPF coefficient for lodamp()
    float _wetLevel;            // wet gain from mix()
    float _dryLevel;            // dry gain from mix()
    uint32_t _predelaySamples;  // pre-delay in samples

    float _modDepth;            // modulation depth in samples (0 .. 16)
    float _modRate;             // modulation rate in Hz
    float _modPhase;            // triangle LFO phase (0 .. 1)
    float _modPhaseInc;         // LFO phase increment per sample

    bool  _bypassed;            // true = skip all processing
    bool  _frozen;              // true = infinite decay, muted input

    // =========================================================================
    // MEMORY MANAGEMENT
    // =========================================================================

    // Master buffer — one contiguous PSRAM allocation, subdivided by init
    float* _bufferPool;
    uint32_t _bufferPoolSize;   // total floats allocated

    // Input queue for AudioStream (1 input for mono, but we accept 2)
    audio_block_t* inputQueueArray[2];

    // =========================================================================
    // PRIVATE HELPERS
    // =========================================================================

    // Allocate all delay buffers from PSRAM (or heap fallback)
    bool allocateBuffers();

    // Free the master buffer pool
    void freeBuffers();

    // Assign sub-regions of the pool to each delay line
    void assignBuffers();

    // Recalculate LFO phase increment from _modRate
    void updateModRate();

    // Triangle LFO: cheap, band-limited, bipolar (-1 .. +1)
    inline float triangleLFO() {
        // Advance phase
        _modPhase += _modPhaseInc;
        if (_modPhase >= 1.0f) _modPhase -= 1.0f;

        // Triangle wave from phase: 4*|phase - 0.5| - 1
        float t = _modPhase - 0.5f;
        if (t < 0.0f) t = -t;
        return 4.0f * t - 1.0f;
    }
};

// End of AudioEffectPlateReverbJT.h
