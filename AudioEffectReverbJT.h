/*
 * AudioEffectReverbJT.h
 * =====================
 * JT-8000 multi-algorithm stereo reverb engine.
 *
 * REPLACES: AudioEffectPlateReverbJT (single plate algorithm)
 *
 * Five reverb algorithms in one AudioStream object, switchable at runtime
 * with zero audio-graph rewiring.  Each algorithm has its own delay-line
 * layout and topology but shares the same PSRAM buffer pool and the same
 * public API, so FXChainBlock needs only a type change — no setter changes.
 *
 * ALGORITHMS:
 *   0  PLATE     — Dattorro allpass plate (tight, metallic, classic)
 *   1  HALL      — 8-line FDN with Hadamard mixing (huge, even, cinematic)
 *   2  SHIMMER   — Plate tank + octave-up pitch shift in feedback (trance!)
 *   3  SPRING    — Short allpass chain with band-limited feedback (lo-fi, drip)
 *   4  CLOUD     — Granular-style frozen diffusion network (ambient, pad wash)
 *
 * TOPOLOGY OVERVIEW:
 *
 *   Input ──► Pre-delay ──► [Algorithm-specific network] ──► Stereo out
 *
 *   All algorithms share:
 *     - Mono summed input with pre-delay (0..500 ms)
 *     - Input diffuser chain (4× allpass, configurable per algorithm)
 *     - Tank damping (hi + lo one-pole filters)
 *     - Soft-clip output limiter
 *     - Triangle LFO modulation in the tank
 *
 * MEMORY (PSRAM):
 *   Total pool ≈ 480 KB — allocated once, partitioned per algorithm.
 *   Only the active algorithm's buffers contain live data; inactive
 *   regions are zeroed on algorithm switch to prevent stale tails.
 *
 * CPU:
 *   - bypass_set(true) → zero CPU (input blocks released, immediate return)
 *   - PLATE / SPRING ≈ 3-4% at 816 MHz
 *   - HALL ≈ 5-6% (8 delay lines + Hadamard matrix)
 *   - SHIMMER ≈ 6-8% (plate + pitch shift interpolation)
 *   - CLOUD ≈ 5-7% (dense diffusion + freeze logic)
 *
 * REFERENCES:
 *   [1] Dattorro, "Effect Design Part 1", JAES 1997 (plate topology)
 *   [2] Jot & Chaigne, "Digital Delay Networks", AES 1991 (FDN/hall)
 *   [3] Costello, Valhalla Shimmer design notes (shimmer concept)
 *   [4] Smith, "Physical Audio Signal Processing" (spring model)
 *   [5] Erbe-Verb / Clouds documentation (granular reverb concept)
 */

#pragma once

#include <Arduino.h>
#include "AudioStream.h"

// =============================================================================
// REVERB TYPE ENUM
// =============================================================================

enum class ReverbType : uint8_t {
    PLATE   = 0,    // Dattorro allpass plate
    HALL    = 1,    // 8-line FDN with Hadamard mixing
    SHIMMER = 2,    // Plate + octave-up pitch shift in feedback
    SPRING  = 3,    // Short allpass chain, band-limited feedback
    CLOUD   = 4,    // Granular-style frozen diffusion network
    COUNT   = 5
};

// Human-readable names (indexed by ReverbType)
static const char* const REVERB_TYPE_NAMES[(uint8_t)ReverbType::COUNT] = {
    "Plate",
    "Hall",
    "Shimmer",
    "Spring",
    "Cloud"
};

// =============================================================================
// AudioEffectReverbJT
// =============================================================================

class AudioEffectReverbJT : public AudioStream {
public:
    AudioEffectReverbJT();
    ~AudioEffectReverbJT();

    // =========================================================================
    // CORE API — identical to AudioEffectPlateReverbJT for FXChainBlock drop-in
    // =========================================================================

    void size(float n);             // 0..1  decay time
    void hidamp(float n);           // 0..1  high-frequency damping
    void lodamp(float n);           // 0..1  low-frequency damping
    void mix(float n);              // 0..1  wet/dry (normally 1.0, external mix)
    void bypass_set(bool state);    // true = zero CPU

    // =========================================================================
    // EXTENDED API — algorithm selection and extra controls
    // =========================================================================

    // Switch algorithm.  Clears inactive buffers to prevent stale tails.
    // Safe to call at any time — no audio graph rewiring.
    void setType(ReverbType type);
    ReverbType getType() const { return _type; }
    const char* getTypeName() const;

    // Pre-delay: 0..500 ms (shared across all algorithms)
    void predelay(float ms);

    // Modulation depth: 0..1 (tank allpass wobble)
    void modDepth(float n);

    // Modulation rate: 0.1..5.0 Hz (triangle LFO speed)
    void modRate(float hz);

    // Freeze: infinite hold, input muted (works with all algorithms,
    // but especially powerful with CLOUD)
    void freeze(bool state);

    // Shimmer-specific: pitch shift amount in semitones (default +12 = octave up)
    // Range: -24..+24.  Only affects SHIMMER algorithm.
    void shimmerPitch(float semitones);

    // Shimmer-specific: amount of pitched signal fed back into tank (0..1)
    void shimmerMix(float n);

    // AudioStream interface
    virtual void update(void);

private:
    // =========================================================================
    // DELAY LINE — circular buffer with integer and interpolated reads
    // =========================================================================

    struct DelayLine {
        float*   buf;
        uint32_t len;
        uint32_t writeIdx;

        inline void write(float sample) {
            buf[writeIdx] = sample;
            if (++writeIdx >= len) writeIdx = 0;
        }

        inline float read(uint32_t delaySamples) const {
            uint32_t idx = (writeIdx >= delaySamples)
                         ? writeIdx - delaySamples
                         : writeIdx + len - delaySamples;
            return buf[idx];
        }

        // Linear interpolation for modulated / pitch-shifted reads
        inline float readInterp(float delaySamples) const {
            uint32_t intPart  = (uint32_t)delaySamples;
            float    fracPart = delaySamples - (float)intPart;
            float s0 = read(intPart);
            float s1 = read(intPart + 1);
            return s0 + fracPart * (s1 - s0);
        }

        // Cubic Hermite interpolation for pitch shifting (shimmer)
        // Better quality than linear for large pitch ratios
        inline float readCubic(float delaySamples) const {
            uint32_t i = (uint32_t)delaySamples;
            float    f = delaySamples - (float)i;

            float y0 = read(i > 0 ? i - 1 : 0);
            float y1 = read(i);
            float y2 = read(i + 1);
            float y3 = read(i + 2);

            // Hermite basis functions
            float c0 = y1;
            float c1 = 0.5f * (y2 - y0);
            float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
            float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
            return ((c3 * f + c2) * f + c1) * f + c0;
        }

        void clear() {
            if (buf) memset(buf, 0, len * sizeof(float));
            writeIdx = 0;
        }
    };

    // =========================================================================
    // ALLPASS FILTER
    // =========================================================================

    struct Allpass {
        DelayLine dl;
        float     gain;

        inline float process(float input) {
            float delayed = dl.read(dl.len - 1);
            float output  = -gain * input + delayed;
            dl.write(input + gain * output);
            return output;
        }

        inline float processModulated(float input, float modSamples) {
            float delaySmp = (float)(dl.len - 1) + modSamples;
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
    // ONE-POLE FILTERS
    // =========================================================================

    struct OnePole_LP {
        float state = 0.0f;
        float coeff = 0.0f;
        inline float process(float input) {
            state = input + coeff * (state - input);
            return state;
        }
        void clear() { state = 0.0f; }
    };

    struct OnePole_HP {
        float state = 0.0f;
        float coeff = 0.0f;
        inline float process(float input) {
            state = input + coeff * (state - input);
            return input - state;
        }
        void clear() { state = 0.0f; }
    };

    // =========================================================================
    // SHARED COMPONENTS (all algorithms use these)
    // =========================================================================

    DelayLine _predelay;                                // pre-delay buffer

    static constexpr uint8_t NUM_INPUT_DIFFUSERS = 4;
    Allpass _inputDiffuser[NUM_INPUT_DIFFUSERS];        // input smearing

    // =========================================================================
    // PLATE components (also used as base for SHIMMER)
    // =========================================================================

    Allpass    _plateAPF[2];        // tank modulated allpass
    DelayLine  _plateDelay[2];      // tank delay lines
    OnePole_LP _plateLPF[2];        // tank hi-damp
    OnePole_HP _plateHPF[2];        // tank lo-damp

    // =========================================================================
    // HALL components — 8-line FDN (Feedback Delay Network)
    // =========================================================================

    static constexpr uint8_t FDN_SIZE = 8;
    DelayLine  _fdnDelay[FDN_SIZE];     // 8 delay lines, prime lengths
    OnePole_LP _fdnLPF[FDN_SIZE];       // per-line hi-damp
    OnePole_HP _fdnHPF[FDN_SIZE];       // per-line lo-damp
    float      _fdnState[FDN_SIZE];     // feedback state (previous output)

    // =========================================================================
    // SHIMMER components — pitch shifter in feedback path
    // =========================================================================

    // Re-uses _plateAPF / _plateDelay / _plateLPF / _plateHPF for the tank.
    // Additional: pitch-shift delay line for the feedback path.
    DelayLine _shimmerBuf;              // circular buffer for pitch reading
    float     _shimmerPhase;            // read-head phase (fractional samples)
    float     _shimmerPhaseInc;         // read rate (derived from semitone shift)
    float     _shimmerPitchSemi;        // pitch shift in semitones
    float     _shimmerFeedbackMix;      // how much shifted signal re-enters tank

    // =========================================================================
    // SPRING components — short allpass chain + band-limited feedback
    // =========================================================================

    static constexpr uint8_t NUM_SPRING_APF = 6;
    Allpass    _springAPF[NUM_SPRING_APF];  // cascaded allpass chain
    DelayLine  _springDelay[2];             // short stereo delay pair
    OnePole_LP _springLPF[2];               // band-limit the feedback
    OnePole_HP _springHPF[2];               // remove sub-bass rumble
    OnePole_LP _springChirpLPF;             // "drip" character filter

    // =========================================================================
    // CLOUD components — dense parallel diffusion + freeze
    // =========================================================================

    static constexpr uint8_t NUM_CLOUD_APF = 8;
    Allpass    _cloudAPF[NUM_CLOUD_APF];    // dense diffusion network
    DelayLine  _cloudDelay[2];              // long stereo delay for smearing
    OnePole_LP _cloudLPF[2];               // damping
    OnePole_HP _cloudHPF[2];

    // =========================================================================
    // PARAMETERS
    // =========================================================================

    ReverbType _type;

    float    _sizeParam;            // raw 0..1 from size() call
    float    _decay;                // computed decay coefficient
    float    _hiDampCoeff;
    float    _loDampCoeff;
    float    _wetLevel;
    float    _dryLevel;
    uint32_t _predelaySamples;

    float    _modDepth;             // in samples
    float    _modRate;              // Hz
    float    _modPhase;             // triangle LFO phase (0..1)
    float    _modPhaseInc;          // per-sample increment

    bool     _bypassed;
    bool     _frozen;

    // =========================================================================
    // MEMORY POOL
    // =========================================================================

    float*   _bufferPool;
    uint32_t _bufferPoolSize;       // total floats

    audio_block_t* inputQueueArray[2];

    // =========================================================================
    // PRIVATE METHODS
    // =========================================================================

    bool allocateBuffers();
    void freeBuffers();
    void assignBuffers();

    // Per-algorithm update routines (called from update())
    void updatePlate(float diffused, float lfo, int16_t* outL, int16_t* outR,
                     float inL, float inR, float wetLvl, float dryLvl, int i);
    void updateHall(float diffused, float lfo, int16_t* outL, int16_t* outR,
                    float inL, float inR, float wetLvl, float dryLvl, int i);
    void updateShimmer(float diffused, float lfo, int16_t* outL, int16_t* outR,
                       float inL, float inR, float wetLvl, float dryLvl, int i);
    void updateSpring(float diffused, float lfo, int16_t* outL, int16_t* outR,
                      float inL, float inR, float wetLvl, float dryLvl, int i);
    void updateCloud(float diffused, float lfo, int16_t* outL, int16_t* outR,
                     float inL, float inR, float wetLvl, float dryLvl, int i);

    // Recalculate shimmer read rate from semitone setting
    void updateShimmerRate();

    // Recalculate LFO phase increment
    void updateModRate();

    // Triangle LFO
    inline float triangleLFO() {
        _modPhase += _modPhaseInc;
        if (_modPhase >= 1.0f) _modPhase -= 1.0f;
        float t = _modPhase - 0.5f;
        if (t < 0.0f) t = -t;
        return 4.0f * t - 1.0f;
    }

    // Soft-clip a single sample (only active above ±1.0)
    static inline float softClip(float x) {
        if (x > 1.0f || x < -1.0f) {
            return x / (1.0f + fabsf(x));
        }
        return x;
    }

    // Recalculate _decay from _sizeParam for the current algorithm
    void recalcDecay();

    // Clear all delay lines for a specific algorithm (on type switch)
    void clearAlgorithmState(ReverbType type);
};
