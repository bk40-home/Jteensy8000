/*
 * AudioEffectPlateReverbJT.h
 * ==========================
 * JT-8000 stereo plate reverb — Dattorro topology with extended decay,
 * shimmer, reverb pitch shifting, master output EQ, and enhanced freeze.
 *
 * Copyright (c) 2021 Piotr Zapart (hexefx) — original Dattorro plate algorithm
 * Copyright (c) 2024 Kris Bishop       — JT-8000 implementation, extensions
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
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * DESIGN GOALS:
 *   - Massive, lush trance/Eventide-style reverb tails (up to ~30 s decay)
 *   - All delay memory allocated from PSRAM (falls back to heap if absent)
 *   - Zero heap allocation in the audio path
 *   - Shimmer + reverb pitch shifting for ethereal ambient textures
 *   - Master output LP/HP EQ for independent tonal shaping
 *   - Enhanced freeze: saves/restores parameters, supports bleed-in
 *   - Zero CPU cost for any inactive feature (early-return guards)
 *
 * TOPOLOGY (Dattorro "Effect Design Part 1", 1997 — extended):
 *
 *   Input ──► Pre-delay ──► Input diffusers (4× allpass)
 *                                    │
 *                            pitchL / pitchR  ◄── reverb pitch shift
 *                                    │
 *                        ┌───────────┴───────────┐
 *                        ▼                       ▼
 *                  Tank Left                Tank Right
 *           pitchShimR(fb+in) →         pitchShimL(fb+in) →
 *           APF → delay → flt → decay   APF → delay → flt → decay
 *                   │    ╳ cross-feedback        │
 *                   └───────────────────────────┘
 *                                    │
 *                   Decorrelated taps ──► masterLPF ──► masterHPF
 *                                    │
 *                             Wet/dry mix ──► int16 output
 *
 * CPU NOTES:
 *   - bypass_set(true): update() returns immediately — zero CPU
 *   - All tank filtering: first-order (1 multiply + 1 state per filter)
 *   - PitchShifter::process(): early-return when mix==0 or ratio==unity
 *   - Master LP/HP filters: skipped when coefficients are at bypass values
 *   - Diffusion: zero-cost parameter — just changes allpass gain coefficients
 *   - Freeze: zero-cost — parameter cache swap only
 *   - Triangle LFO: 4 muls + 1 comparison, no sin/cos
 *
 * MEMORY (PSRAM) BUDGET:
 *   Existing:  23,323 floats =  93.3 KB
 *   + 4× PitchShifter buffers (4096 each): 16,384 floats = 65.5 KB
 *   New total: 39,707 floats = 158.8 KB
 *   Well within 8 MB PSRAM budget.
 *
 * REFERENCES:
 *   [1] Jon Dattorro, "Effect Design Part 1: Reverberator and Other
 *       Filters", J. Audio Eng. Soc., 1997
 *   [2] Piotr Zapart (hexefx) — AudioEffectPlateReverb_F32 (MIT licence),
 *       pitch shifter and shimmer placement
 *   [3] Adam Szabó, "How to Emulate the Super Saw", KTH 2010
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
    // PUBLIC API — original parameters (unchanged)
    // =========================================================================

    // Room size / decay time.  0.0 = tight room, 1.0 = infinite hold.
    // Internally maps to decay coefficient (0.05 .. 0.9995).
    void size(float n);

    // High-frequency damping in the tank.  0.0 = bright, 1.0 = very dark.
    // Controls a first-order LPF inside each tank loop.
    void hidamp(float n);

    // Low-frequency damping in the tank.  0.0 = full bass, 1.0 = thin.
    // Controls a first-order HPF inside each tank loop.
    void lodamp(float n);

    // Wet/dry mix.  0.0 = fully dry, 1.0 = fully wet.
    // NOTE: FXChainBlock always sets mix=1.0 and uses an external mixer.
    void mix(float n);

    // Hard bypass — update() returns immediately when true (zero CPU).
    void bypass_set(bool state);

    // =========================================================================
    // EXTENDED API — original JT controls
    // =========================================================================

    // Pre-delay in milliseconds.  Range 0..250 ms.
    // Adds space between dry signal and reverb onset.
    void predelay(float ms);

    // Modulation depth (tank allpass chorusing).  0.0 = off, 1.0 = full.
    // Maps to 0..16 samples of delay modulation excursion.
    void modDepth(float n);

    // Modulation rate.  Range 0.1..5.0 Hz.
    void modRate(float hz);

    // =========================================================================
    // EXTENDED API — new features ported from hexefx F32 plate reverb
    // =========================================================================

    // Output lowpass filter.  0.0 = no filtering (bright), 1.0 = heavy cut.
    // Applied after the tank on the wet signal only.  Zero CPU when at 0.
    void lowpass(float n);

    // Output highpass filter.  0.0 = no filtering (full bass), 1.0 = heavy cut.
    // Applied after the tank on the wet signal only.  Zero CPU when at 0.
    void hipass(float n);

    // Input diffusion.  0.0 = sparse / discrete echoes, 1.0 = dense wash.
    // Maps to allpass feedback coefficient range 0.005..0.65.
    // Zero-cost at any value — just changes the allpass gain.
    void diffusion(float n);

    // Freeze / infinite hold.  When true:
    //   - decay set to 1.0 (infinite), input muted (except bleed-in)
    //   - current size/hidamp/lodamp/shimmer saved for restore on unfreeze
    //   - shimmer disabled (prevents runaway pitch escalation)
    // On unfreeze: all saved parameters restored automatically.
    void freeze(bool state);

    // Freeze bleed-in.  How much input signal leaks through during freeze.
    // Range 0.0 (total mute) to 1.0 (mapped to 0.0..0.1 to avoid oscillation).
    void freezeBleedIn(float b);

    // Shimmer amount.  0.0 = off, 1.0 = full shimmer in tank feedback loop.
    // Automatically disabled during freeze to prevent runaway.
    // Zero CPU when mix == 0 (early-return in PitchShifter::process).
    void shimmer(float s);

    // Shimmer pitch in semitones.  Range -12 to +12.
    // Uses the 9-entry musical interval table: -12, -7, -5, -3, 0, +3, +5, +7, +12.
    void shimmerPitchSemitones(int8_t semitones);

    // Shimmer pitch from normalised CC value (0.0..1.0 → semitone table index).
    void shimmerPitchNormalized(float value);

    // Reverb tail pitch shift in semitones.  Range -12 to +24.
    // Applied after the input diffuser chain, before the tank.
    void pitchSemitones(int8_t semitones);

    // Reverb tail pitch from normalised CC value (0.0..1.0 → semitone table index).
    void pitchNormalized(float value);

    // Reverb pitch shifter wet/dry blend.  0.0 = natural pitch, 1.0 = fully shifted.
    void pitchMix(float s);

    // AudioStream interface (2 inputs, 2 outputs, int16 blocks)
    virtual void update(void);

private:
    // =========================================================================
    // DELAY LINE — simple circular buffer with integer read/write
    // =========================================================================

    struct DelayLine {
        float*   buf;       // pointer into PSRAM pool
        uint32_t len;       // buffer length in samples
        uint32_t writeIdx;  // current write position (advances each sample)

        // Write one sample and advance write pointer (wraps at len)
        inline void write(float sample) {
            buf[writeIdx] = sample;
            if (++writeIdx >= len) writeIdx = 0;
        }

        // Read at a fixed tap offset (samples behind the write head)
        inline float read(uint32_t delaySamples) const {
            // Conditional subtract avoids the % operator (no division on M7)
            uint32_t idx = (writeIdx >= delaySamples)
                         ? writeIdx - delaySamples
                         : writeIdx + len - delaySamples;
            return buf[idx];
        }

        // Linear interpolation read for modulated allpass taps
        inline float readInterp(float delaySamples) const {
            uint32_t intPart  = (uint32_t)delaySamples;
            float    fracPart = delaySamples - (float)intPart;
            float    s0       = read(intPart);
            float    s1       = read(intPart + 1);
            return s0 + fracPart * (s1 - s0);
        }

        // Zero buffer and reset write index (called at init)
        void clear() {
            if (buf) memset(buf, 0, len * sizeof(float));
            writeIdx = 0;
        }
    };

    // =========================================================================
    // ALLPASS FILTER — first-order allpass delay with feedback
    // =========================================================================

    struct Allpass {
        DelayLine dl;
        float     gain;     // feedback/feedforward coefficient

        // Fixed-delay allpass: y = -g*x + z^-N + g*y_prev
        inline float process(float input) {
            float delayed = dl.read(dl.len - 1);
            float output  = -gain * input + delayed;
            dl.write(input + gain * output);
            return output;
        }

        // Modulated-delay allpass: for tank chorusing / shimmer LFO
        // delaySmp is clamped to valid buffer bounds to prevent out-of-range reads
        inline float processModulated(float input, float modSamples) {
            float delaySmp = (float)(dl.len - 1) + modSamples;
            // Clamp to 1..len-1 (never zero, never past end)
            if (delaySmp < 1.0f)                delaySmp = 1.0f;
            if (delaySmp > (float)(dl.len - 1)) delaySmp = (float)(dl.len - 1);

            float delayed = dl.readInterp(delaySmp);
            float output  = -gain * input + delayed;
            dl.write(input + gain * output);
            return output;
        }

        void clear() { dl.clear(); }
    };

    // =========================================================================
    // ONE-POLE FILTERS — minimal CPU for tank damping and master EQ
    // =========================================================================

    // First-order lowpass: y[n] = (1-a)*x[n] + a*y[n-1]
    // coeff == 0 → transparent (no filtering)
    struct OnePole_LP {
        float state = 0.0f;
        float coeff = 0.0f;

        inline float process(float input) {
            state = input + coeff * (state - input);
            return state;
        }

        void clear() { state = 0.0f; }
    };

    // First-order highpass: y[n] = x[n] - LP(x[n])
    // coeff == 0 → transparent bypass (returns input unchanged).
    // CRITICAL: without the bypass guard, coeff=0 makes state=input every
    // sample → output = input-input = SILENCE. Guard is mandatory.
    struct OnePole_HP {
        float state = 0.0f;
        float coeff = 0.0f;

        inline float process(float input) {
            if (coeff < 0.001f) return input;   // bypass guard — prevents silence
            state = input + coeff * (state - input);
            return input - state;
        }

        void clear() { state = 0.0f; }
    };

    // =========================================================================
    // PITCH SHIFTER — Doppler delay-line pitch shifter
    //
    // Ported from AudioBasicPitch (hexefx, Piotr Zapart, MIT licence).
    //
    // Two read pointers 180° apart in a circular buffer with smooth crossfade
    // windowing to hide the splice discontinuity — standard granular pitch
    // shifting technique.
    //
    // CPU cost:
    //   - mix == 0 OR ratio == unity → early-return, zero DSP work
    //   - Otherwise: ~20 arithmetic ops per sample (two interpolated reads +
    //     crossfade blend + one LP filter sample)
    //
    // Buffer memory: 4096 floats (16 KB) per instance, from PSRAM pool.
    // =========================================================================

    struct PitchShifter {
        // Circular buffer pointer (assigned from PSRAM pool, never owned here)
        float*   buf    = nullptr;

        // Fixed-point read address (16.16 format — top BUF_BITS are integer index,
        // lower bits are fractional position within the 4096-sample buffer)
        uint32_t readAddr  = 0;

        // Integer write address (advances one sample at a time)
        uint16_t writeAddr = 0;

        // Pitch ratio encoded as fixed-point phase increment per sample.
        // Unity = pitchDelta0 (readAddr advances at the same rate as writeAddr).
        // 2.0× = 2 * pitchDelta0 (reads twice as fast → octave up).
        uint32_t readAdder = 0;

        // Wet/dry blend for the pitch shifter output.
        // 0.0 = pass-through (process() returns input unchanged without DSP work).
        float mix = 0.0f;

        // Output one-pole lowpass state — softens aliasing artefacts from the
        // granular splicing.  Coefficient is baked in as a constant.
        float lpState = 0.0f;

        // ─── Buffer sizing constants ─────────────────────────────────────────
        static constexpr uint32_t BUF_BITS = 12;
        static constexpr uint32_t BUF_SIZE = 1u << BUF_BITS;           // 4096
        static constexpr uint32_t BUF_MASK = BUF_SIZE - 1u;
        // Fractional mask: lower (32 - BUF_BITS) bits of readAddr
        static constexpr uint32_t FRAC_BITS = 32u - BUF_BITS;
        static constexpr uint32_t FRAC_MASK = (1u << FRAC_BITS) - 1u;
        // Unity pitch step: readAddr advances BUF_SIZE steps per sample
        static constexpr uint32_t DELTA_0   = 1u << FRAC_BITS;

        // Output LP coefficient.  ~0.7 gives a gentle ~6 kHz rolloff at 44.1 kHz,
        // smoothing granular splicing artefacts without losing air.
        // Equivalent to the lp_gain = 0.26 setting in the F32 original.
        static constexpr float LP_COEFF = 0.26f;

        // ─── Semitone ratio table ─────────────────────────────────────────────
        // -12 to +24 semitones (37 entries).  Index = semitones + 12.
        // Values from hexefx wavetables.c (music_intevals[]).
        static const float kSemitoneRatios[37];

        // ─── Crossfade window ─────────────────────────────────────────────────
        // 257-entry raised-cosine window (0 → 1 → 0) used to smoothly blend
        // the two 180°-offset read pointers.  Ported from hexefx wavetables.c
        // (AudioWaveformFader_f32[]).  Extra entry [256] = 1.0 avoids a bounds check.
        static const float kFadeTable[257];

        // ─── Methods ─────────────────────────────────────────────────────────

        // Assign buffer from PSRAM pool (called by AudioEffectPlateReverbJT::assignBuffers)
        void assign(float* poolPtr) {
            buf = poolPtr;
            clear();
        }

        // Set pitch as a linear ratio (0.5 = octave down, 1.0 = no change, 2.0 = octave up)
        void setPitch(float ratio) {
            readAdder = (uint32_t)((float)DELTA_0 * ratio);
        }

        // Set pitch by semitone offset (-12..+24)
        void setPitchSemitones(int8_t semitones) {
            semitones = constrain(semitones, -12, +24);
            setPitch(kSemitoneRatios[semitones + 12]);
        }

        // Set wet/dry blend.  0.0 → process() is a no-op (zero CPU).
        void setMix(float m) {
            mix = constrain(m, 0.0f, 1.0f);
        }

        // Zero buffer and reset all pointers
        void clear() {
            if (buf) memset(buf, 0, BUF_SIZE * sizeof(float));
            readAddr  = 0;
            writeAddr = 0;
            readAdder = DELTA_0;  // unity pitch on reset
            lpState   = 0.0f;
        }

        // Process one sample.  Returns pitch-shifted output blended with input.
        // Early-returns immediately (no DSP work) when mix == 0 or pitch == unity.
        inline float process(float input) {
            // Write new sample into circular buffer before anything else
            buf[writeAddr] = input;

            // Update read pointer (fixed-point, wraps naturally on 32-bit overflow)
            readAddr += readAdder;

            // ── Early return: bypass ───────────────────────────────────────────
            // No work done when mix is zero OR when pitch is unity (readAdder == DELTA_0)
            if (mix == 0.0f || readAdder == DELTA_0) {
                writeAddr = (writeAddr + 1u) & BUF_MASK;
                return input;
            }

            // ── Primary read pointer (interpolated) ───────────────────────────
            // Top BUF_BITS of readAddr are the integer buffer index
            uint32_t idx1    = (readAddr >> FRAC_BITS) & BUF_MASK;
            float    kFrac1  = (float)(readAddr & FRAC_MASK) * (1.0f / (float)FRAC_MASK);
            float    sMain   = buf[idx1] * (1.0f - kFrac1)
                             + buf[(idx1 + 1u) & BUF_MASK] * kFrac1;

            // ── Secondary read pointer (180° offset = half buffer away) ────────
            uint32_t readAddr2 = readAddr + 0x80000000u;
            uint32_t idx2    = (readAddr2 >> FRAC_BITS) & BUF_MASK;
            float    kFrac2  = (float)(readAddr2 & FRAC_MASK) * (1.0f / (float)FRAC_MASK);
            float    sHalf   = buf[idx2] * (1.0f - kFrac2)
                             + buf[(idx2 + 1u) & BUF_MASK] * kFrac2;

            // ── Crossfade between the two read pointers ───────────────────────
            // Distance between write and read pointers determines blend weight.
            // As the read pointer "laps" the write pointer, the crossfade creates
            // a smooth transition rather than a click at the splice point.
            uint32_t distAcc  = readAddr - ((uint32_t)writeAddr << FRAC_BITS);
            // Top 9 bits encode position in fade-in (0..255) / fade-out (256..511)
            uint32_t fadeIdx  = (distAcc >> (32u - 9u)) & 0x1FFu;
            float    fadeFrac = (float)(distAcc & ((1u << 23u) - 1u))
                              * (1.0f / (float)((1u << 23u) - 1u));

            // Interpolate within the 256-entry half of the fade table
            uint32_t tblIdx = fadeIdx & 0xFFu;
            float    xf0    = kFadeTable[tblIdx];
            float    xf1    = kFadeTable[tblIdx + 1u];  // [256] = 1.0, safe overread
            float    blend  = xf0 * (1.0f - fadeFrac) + xf1 * fadeFrac;

            // Invert curve for the fade-out half (indices 256..511)
            if (fadeIdx > 0xFFu) blend = 1.0f - blend;

            float pitched = sMain * blend + sHalf * (1.0f - blend);

            // ── Output lowpass — soften granular artefacts ────────────────────
            lpState += LP_COEFF * (pitched - lpState);
            pitched = lpState;

            // Advance write pointer
            writeAddr = (writeAddr + 1u) & BUF_MASK;

            // ── Wet/dry mix ───────────────────────────────────────────────────
            return pitched * mix + input * (1.0f - mix);
        }
    };

    // =========================================================================
    // TOPOLOGY COMPONENTS
    // =========================================================================

    // Pre-delay line (mono, before input diffusers)
    DelayLine _predelay;

    // Input diffuser chain — 4 series allpass filters.
    // Smears the dry impulse into a dense texture before entering the tank.
    static constexpr uint8_t NUM_INPUT_DIFFUSERS = 4;
    Allpass _inputDiffuser[NUM_INPUT_DIFFUSERS];

    // Tank: two mirrored halves with cross-feedback.
    // Each half: modulated APF → delay → LPF → HPF → decay → cross to other.
    Allpass     _tankAPF[2];    // modulated allpass, one per tank half
    DelayLine   _tankDelay[2];  // long delay line, one per tank half
    OnePole_LP  _tankLPF[2];   // HF damping inside each tank loop
    OnePole_HP  _tankHPF[2];   // LF damping inside each tank loop

    // Reverb pitch shifters (applied after input diffusers, before tank)
    // Zero CPU when pitchMix == 0 (early-return in PitchShifter::process).
    PitchShifter _pitchL;       // left-channel reverb pitch shift
    PitchShifter _pitchR;       // right-channel reverb pitch shift

    // Shimmer pitch shifters (placed inside the tank feedback loop)
    // Zero CPU when shimmerMix == 0.
    PitchShifter _pitchShimL;   // shimmer, tank half 0 path
    PitchShifter _pitchShimR;   // shimmer, tank half 1 path

    // Master output EQ (applied to wet signal after tap summing)
    OnePole_LP _masterLPF[2];   // output darkening filter, one per stereo channel
    OnePole_HP _masterHPF[2];   // output thinning filter, one per stereo channel

    // =========================================================================
    // PARAMETERS — cached, updated via setters, read in update()
    // =========================================================================

    // Tank feedback coefficient.  Mapped from size(): 0.05 .. 0.9995.
    float _decay;

    // Filtered tank outputs carried between samples for cross-feedback.
    // The damping filters (LPF/HPF) must sit inside the feedback path — their
    // output feeds the OTHER tank half on the next sample, not the raw delay end.
    // Initialised to 0.0 in the constructor; persists across update() calls.
    float _tank0fb;   // filtered output of tank half 0 → feeds crossFB1
    float _tank1fb;   // filtered output of tank half 1 → feeds crossFB0

    // LPF / HPF coefficients for the tank damping filters.
    float _hiDampCoeff;         // hidamp() → 0..0.7
    float _loDampCoeff;         // lodamp() → 0..0.7

    // Wet/dry mix levels.
    float _wetLevel;
    float _dryLevel;

    // Pre-delay in samples.  0 = bypassed (no delay).
    uint32_t _predelaySamples;

    // Modulation depth in samples (0..16) and rate in Hz.
    float _modDepth;
    float _modRate;

    // Triangle LFO state for tank allpass modulation.
    float _modPhase;            // current phase 0..1
    float _modPhaseInc;         // phase increment per sample = modRate / sampleRate

    // Hard bypass flag.  When true, update() returns immediately.
    bool _bypassed;

    // Freeze state.
    bool _frozen;

    // Diffusion coefficient (allpass feedback gain shared across input diffusers
    // and tank allpass filters).  Set by diffusion().
    float _diffusionCoeff;

    // ── Master output filter coefficients ────────────────────────────────────
    // 0.0 = bypass (no filtering).  Applied per-sample only when non-zero.
    float _masterLpCoeff;       // lowpass() → 0..0.9
    float _masterHpCoeff;       // hipass()  → 0..0.9

    // ── Shimmer state ─────────────────────────────────────────────────────────
    float   _shimmerMix;        // current shimmer wet amount (after quadratic curve)
    int8_t  _shimmerSemitones;  // current shimmer pitch semitone offset
    float   _pitchMixAmount;    // reverb pitch blend (pitchMix())
    int8_t  _pitchSemitones;    // current reverb pitch semitone offset

    // ── Freeze save/restore slots ─────────────────────────────────────────────
    // Values are saved when freeze(true) is called and restored on freeze(false).
    float   _savedDecay;
    float   _savedHiDampCoeff;
    float   _savedLoDampCoeff;
    float   _savedShimmerMix;

    // Input gain applied during freeze bleed-in (0.0 by default).
    // Range internally mapped to 0.0..0.1 to prevent oscillation.
    float   _freezeBleedGain;

    // Semitone table used by shimmerPitchNormalized() and pitchNormalized().
    // 9 musically useful intervals: octave down through octave up.
    static const int8_t kShimmerSemitoneTable[9];

    // =========================================================================
    // MEMORY MANAGEMENT
    // =========================================================================

    // Single contiguous PSRAM allocation; all delay and pitch-shifter buffers
    // are carved from this one block.  One allocation = one PSRAM heap entry.
    float*   _bufferPool;
    uint32_t _bufferPoolSize;   // total floats allocated

    // AudioStream input queue (2 inputs: L and R)
    audio_block_t* inputQueueArray[2];

    // =========================================================================
    // PRIVATE HELPERS
    // =========================================================================

    // Allocate the master pool from PSRAM (fallback: heap)
    bool allocateBuffers();

    // Free the master pool
    void freeBuffers();

    // Carve the pool into sub-regions and assign to each delay line
    void assignBuffers();

    // Recalculate _modPhaseInc from _modRate
    void updateModRate();

    // Triangle LFO — advances phase and returns bipolar value (-1 .. +1).
    // Much cheaper than sinf(): 4 multiplies + 1 compare + no lookup table.
    inline float triangleLFO() {
        _modPhase += _modPhaseInc;
        if (_modPhase >= 1.0f) _modPhase -= 1.0f;
        // map phase 0..1 to triangle -1..+1:  4*|phase - 0.5| - 1
        float t = _modPhase - 0.5f;
        if (t < 0.0f) t = -t;
        return 4.0f * t - 1.0f;
    }
};

// End of AudioEffectPlateReverbJT.h
