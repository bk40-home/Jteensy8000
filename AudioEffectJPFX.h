/*
 * AudioEffectJPFX.h — JP-8000 Effects Engine (Stereo Output, BPM Timing)
 *
 * Implements the effects section of the Roland JP-8000 synthesizer
 * for the JT-8000 project.
 *
 * SIGNAL CHAIN (per sample):
 *   Mono input → Saturation → Tone EQ → Modulation → Delay → Limiter → Stereo out
 *
 * ARCHITECTURE:
 *   - 1 mono input, 2 stereo outputs
 *   - Separate circular buffers for modulation and delay effects
 *   - Continuous processing maintains LFO phase and effect tails after note-off
 *   - Block-rate parameter caching avoids redundant per-sample computation
 *   - Smart bypass: disabled stages cost zero CPU
 *
 * REFACTORED (Delivery 2 prep):
 *
 *   TONE CONTROL — replaced broken biquad implementation with crossover EQ.
 *     Previous code computed 2nd-order biquad coefficients (RBJ Cookbook)
 *     but applied them through a 1st-order filter struct (3 coefficients,
 *     1 state variable per section).  This mismatch meant the filter was
 *     never truly flat at 0 dB — it always coloured the signal, even at
 *     the midpoint.  Additionally, the bypass check used exact float
 *     equality (== 0.0f), so CC value 64 mapping to ~0.094 dB never
 *     triggered bypass, and the broken filter ran unconditionally.
 *     New design uses a two-band crossover (one-pole LP at 200 Hz and
 *     3 kHz) that is mathematically guaranteed to be transparent at unity
 *     gain: output = input + (gain-1)*component, so when gain=1 the
 *     addition is exactly zero.  Cheaper per-sample, simpler code.
 *
 *   DELAY PRESETS — updated to match JP-8000 manual (SoS review confirms:
 *     "three panning delays plus short and long mono delays").
 *     Old: SHORT, LONG, PINGPONG1/2/3
 *     New: MONO_SHORT, MONO_LONG, PAN_LR, PAN_RL, PAN_STEREO
 *     Same count (5), same CC mapping, different stereo behaviour.
 *
 *   SUPER CHORUS — 3-tap BBD ensemble chorus replacing CHORUS_DEEP.
 *     The JP-8000's chorus is central to the "divine" supersaw sound.
 *     Single-tap chorus sounds thin.  The 3-tap technique (120° LFO
 *     phase offsets) recreates the classic Roland ensemble effect
 *     (JC-120, Boss CE-5).  Only the Super Chorus preset uses 3 taps;
 *     all other presets remain single-tap (no CPU increase).
 *
 *   DEEP FLANGER — replaces FLANGER3 with parameters tuned to match the
 *     SoS-praised "Deep Flanger, as good as any I've heard."
 *
 *   CHORUS PRESET TUNING — base delays reduced from 15-25 ms (doubling
 *     territory) to 5-12 ms (proper chorus range).  Preset mix set to
 *     1.0 so the user Level knob is the sole wet/dry control, matching
 *     the JP-8000's single chorus Level pot.
 *
 *   DEAD CODE REMOVED — isPhaser/isFlanger flags were stored in ModParams
 *     but never read by processModulation().  True phaser requires
 *     cascaded all-pass filters (future work, documented below).
 *
 *   CPU — memset for buffer clears, triangle LFO for chorus (cheaper than
 *     sine), precomputed ms-to-samples factor.
 *
 * KNOWN LIMITATIONS:
 *   - Phaser presets (PHASER1-4) use delay-line modulation, not cascaded
 *     all-pass stages.  The effect approximates phaser-like sweeping but
 *     lacks the notch/peak comb structure of a true phaser.  Implementing
 *     proper all-pass cascade is a future enhancement.
 *
 * BREAKING CHANGES (enum renames — numeric values unchanged):
 *   Mod:   JPFX_FLANGER3    → JPFX_DEEP_FLANGER  (position 5)
 *          JPFX_CHORUS_DEEP → JPFX_SUPER_CHORUS   (position 10)
 *   Delay: JPFX_DELAY_SHORT    → JPFX_DELAY_MONO_SHORT  (position 0)
 *          JPFX_DELAY_LONG     → JPFX_DELAY_MONO_LONG   (position 1)
 *          JPFX_DELAY_PINGPONG1 → JPFX_DELAY_PAN_LR     (position 2)
 *          JPFX_DELAY_PINGPONG2 → JPFX_DELAY_PAN_RL     (position 3)
 *          JPFX_DELAY_PINGPONG3 → JPFX_DELAY_PAN_STEREO (position 4)
 *   FXChainBlock name tables need updating to match.
 */

#pragma once

#include <Arduino.h>
#include "AudioStream.h"
#include "BPMClockManager.h"

// ---------------------------------------------------------------------------
// Buffer sizing constants
// ---------------------------------------------------------------------------

// Maximum delay time in milliseconds.  The JP-8000 delay extends to 1250 ms;
// we keep a small safety margin.  This determines the delay buffer allocation.
static constexpr float JPFX_MAX_DELAY_MS = 1500.0f;

static constexpr float JPFX_MIN_DELAY_SAMP = 1.0f;

// Maximum modulation delay in milliseconds.  Chorus needs ~20 ms max
// (base + depth); 50 ms provides headroom for future presets.
static constexpr float JPFX_MAX_MOD_MS = 50.0f;

// Preset table sizes
static constexpr uint8_t JPFX_NUM_MOD_VARIATIONS   = 11;
static constexpr uint8_t JPFX_NUM_DELAY_VARIATIONS  = 5;


class AudioEffectJPFX : public AudioStream {
public:

    // -----------------------------------------------------------------------
    // Modulation effect type selector
    // Numeric values 0-10 map directly to CC dispatch — do not reorder.
    // -----------------------------------------------------------------------
    enum ModEffectType : int8_t {
        JPFX_MOD_OFF       = -1,
        JPFX_CHORUS1       =  0,   // Light chorus, fast rate
        JPFX_CHORUS2       =  1,   // Medium chorus, warm sweep
        JPFX_CHORUS3       =  2,   // Wide chorus, slow and lush
        JPFX_FLANGER1      =  3,   // Gentle flanger
        JPFX_FLANGER2      =  4,   // Metallic flanger, high feedback
        JPFX_DEEP_FLANGER  =  5,   // Deep flanger (was FLANGER3)
        JPFX_PHASER1       =  6,   // Slow phaser sweep
        JPFX_PHASER2       =  7,   // Medium phaser
        JPFX_PHASER3       =  8,   // Fast phaser
        JPFX_PHASER4       =  9,   // Wide phaser
        JPFX_SUPER_CHORUS  = 10    // 3-tap BBD ensemble (was CHORUS_DEEP)
    };

    // -----------------------------------------------------------------------
    // Delay effect type selector
    // Numeric values 0-4 map directly to CC dispatch — do not reorder.
    // JP-8000 manual: "three panning delays plus short and long mono delays"
    // -----------------------------------------------------------------------
    enum DelayEffectType : int8_t {
        JPFX_DELAY_OFF        = -1,
        JPFX_DELAY_MONO_SHORT =  0,   // Short flutter echo, mono
        JPFX_DELAY_MONO_LONG  =  1,   // Long echo, mono
        JPFX_DELAY_PAN_LR     =  2,   // Panning delay, left → right
        JPFX_DELAY_PAN_RL     =  3,   // Panning delay, right → left
        JPFX_DELAY_PAN_STEREO =  4    // Wide stereo ping-pong
    };

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------
    AudioEffectJPFX();
    ~AudioEffectJPFX();

    // AudioStream processing callback — called every 128 samples (~2.9 ms)
    virtual void update(void) override;

    // -----------------------------------------------------------------------
    // Tone control interface — two-band crossover EQ
    // -----------------------------------------------------------------------
    void setBassGain(float dB);     // ±12 dB, 0 = flat
    void setTrebleGain(float dB);   // ±12 dB, 0 = flat

    // -----------------------------------------------------------------------
    // Saturation / drive interface
    // -----------------------------------------------------------------------
    // norm: 0.0         = bypass (zero CPU)
    //       0.001..0.499 = soft clip (tanh waveshaper, warm harmonics)
    //       0.5..1.0     = hard clip (asymmetric, JP-8000 output stage)
    void  setSaturation(float norm);
    float getSaturation() const { return _satDrive; }

    // -----------------------------------------------------------------------
    // Modulation effect interface
    // -----------------------------------------------------------------------
    void setModEffect(ModEffectType type);
    void setModMix(float mix);      // 0.0..1.0 (Level knob)
    void setModRate(float rateHz);  // Hz, 0 = use preset
    void setModFeedback(float fb);  // 0.0..0.99, -1 = use preset

    // -----------------------------------------------------------------------
    // Delay effect interface
    // -----------------------------------------------------------------------
    void setDelayEffect(DelayEffectType type);
    void setDelayMix(float mix);        // -1.0..1.0 (negative inverts phase)
    void setDelayFeedback(float fb);    // 0.0..0.99, -1 = use preset
    void setDelayTime(float ms);        // ms override, preserves preset L/R ratio

    // -----------------------------------------------------------------------
    // BPM timing interface
    // -----------------------------------------------------------------------
    void setDelayTimingMode(TimingMode mode);
    TimingMode getDelayTimingMode() const { return _delayTimingMode; }
    void updateFromBPMClock(const BPMClockManager& bpmClock);

private:

    // AudioStream input queue (1 mono input)
    audio_block_t *inputQueueArray[1];

    // -----------------------------------------------------------------------
    // Tone control — crossover EQ internals
    //
    // Two one-pole LP filters split the signal into three bands:
    //   bass  = LP(200 Hz)
    //   mid   = input - bass - treble  (implicit, never computed)
    //   treble = input - LP(3 kHz)
    //
    // Output = input + (bassGain - 1) * bass + (trebleGain - 1) * treble
    //
    // When both gains are 1.0 (unity), both delta terms are zero and the
    // output equals the input exactly — guaranteed transparent midpoint.
    // -----------------------------------------------------------------------
    float _toneXoverBassAlpha;      // One-pole LP coefficient at ~200 Hz
    float _toneXoverTrebAlpha;      // One-pole LP coefficient at ~3 kHz
    float _toneBassLpStateL;        // Bass crossover LP state, left channel
    float _toneBassLpStateR;        // Bass crossover LP state, right channel
    float _toneTrebLpStateL;        // Treble crossover LP state, left channel
    float _toneTrebLpStateR;        // Treble crossover LP state, right channel
    float _toneBassGainDelta;       // (linear bass gain - 1.0)
    float _toneTrebGainDelta;       // (linear treble gain - 1.0)
    float _targetBassGainDb;        // Stored bass gain in dB (for readback)
    float _targetTrebleGainDb;      // Stored treble gain in dB (for readback)
    bool  _toneActive;              // false = both bands at unity (bypass)
    bool  _toneDirty;               // true = gains changed, recompute needed

    void computeToneParams();
    inline void applyTone(float &sampleL, float &sampleR);

    // -----------------------------------------------------------------------
    // Saturation / drive internals (unchanged from previous version)
    // -----------------------------------------------------------------------
    float _satDrive;                // Normalised 0..1 from setSaturation()
    float _satInputGain;            // Pre-clip gain (1..8×)
    float _satOutputGain;           // Post-clip makeup gain
    bool  _satIsSoft;               // true = tanh, false = asymmetric hard clip
    bool  _satDirty;                // Recompute gains on next update()
    float _hpState;                 // Hard-clip DC blocking HP filter state

    void  computeSatParams();
    inline float applySaturation(float sample);

    // -----------------------------------------------------------------------
    // Output brick-wall limiter internals
    // Block-rate peak follower prevents digital clipping.
    // Attack: ~1 block (catches transients), Release: ~100 ms
    // -----------------------------------------------------------------------
    float _limGain;
    static constexpr float kLimAttack  = 0.001f;
    static constexpr float kLimRelease = 0.029f;

    // -----------------------------------------------------------------------
    // Modulation effect preset parameters
    //
    // Each preset defines the delay times, LFO rate, feedback, mix, and
    // the LFO waveform/tap-count configuration.
    //
    // tapCount: 1 = standard single-tap chorus/flanger
    //           3 = BBD ensemble (Super Chorus) — three delay taps at 120°
    //               LFO phase offsets, averaged.  Recreates the classic
    //               Roland JC-120 / Boss CE-5 ensemble character.
    //
    // useTriangleLfo: true = triangle waveform (chorus/ensemble — linear
    //                   delay sweep matches BBD bucket-passing characteristic)
    //                 false = sine waveform (flanger/phaser — smoother
    //                   modulation avoids audible stepping at short delays)
    // -----------------------------------------------------------------------
    struct ModParams {
        float baseDelayMsL;         // Base delay, left channel (ms)
        float baseDelayMsR;         // Base delay, right channel (ms)
        float depthMsL;             // Modulation depth, left (ms)
        float depthMsR;             // Modulation depth, right (ms)
        float rateHz;               // LFO rate (Hz)
        float feedback;             // Feedback coefficient (0.0..0.99)
        float mix;                  // Preset wet level (1.0 = user Level controls)
        uint8_t tapCount;           // 1 = standard, 3 = BBD ensemble
        bool useTriangleLfo;        // true = triangle, false = sine
    };

    static const ModParams modPresets[JPFX_NUM_MOD_VARIATIONS];

    ModEffectType _modType;
    float _modMix;                  // User Level knob (0..1)
    float _modRateOverride;         // -1 = use preset
    float _modFeedbackOverride;     // -1 = use preset
    float _lfoPhaseL;               // Left LFO phase (radians)
    float _lfoPhaseR;               // Right LFO phase (radians)
    float _lfoIncL;                 // Left LFO phase increment per sample
    float _lfoIncR;                 // Right LFO phase increment per sample

    void updateLfoIncrements();
    inline void processModulation(float inL, float inR,
                                  float &outL, float &outR);

    // -----------------------------------------------------------------------
    // Delay effect preset parameters
    //
    // JP-8000 delay types from the manual:
    //   - Short mono delay (flutter echo, ~80 ms)
    //   - Long mono delay (~500 ms)
    //   - Three panning delays with offset L/R times
    // -----------------------------------------------------------------------
    struct DelayParams {
        float delayMsL;             // Left channel delay time (ms)
        float delayMsR;             // Right channel delay time (ms)
        float feedback;             // Feedback coefficient (0.0..0.99)
        float mix;                  // Preset wet level
    };

    static const DelayParams delayPresets[JPFX_NUM_DELAY_VARIATIONS];

    DelayEffectType _delayType;
    float _delayMix;                // User delay Level (-1..1, negative inverts)
    float _delayFeedbackOverride;   // -1 = use preset
    float _delayTimeOverrideL;      // -1 = use preset L time (ms)
    float _delayTimeOverrideR;      // -1 = use preset R time (ms)

    // BPM timing state
    TimingMode _delayTimingMode;
    float _freeRunningDelayTime;

    // Circular delay buffers (separate for modulation and delay effects)
    float *_modBufL,   *_modBufR;
    float *_delayBufL, *_delayBufR;
    uint32_t _modBufSize,   _delayBufSize;
    uint32_t _modWriteIdx,  _delayWriteIdx;

    void allocateDelayBuffers();

    // -----------------------------------------------------------------------
    // Delay wet mute counter — click-free preset transitions
    //
    // On preset change, the wet signal is muted for one full delay-length
    // period while the write pointer overwrites stale buffer content with
    // fresh audio.  This replaces the expensive memset of ~517KB PSRAM
    // that was causing ISR overruns and voice drops (~500 µs per clear).
    //
    // Decremented per sample in processDelay().  While > 0 the buffer is
    // written to (building fresh content) but the wet output is zeroed.
    // Once 0, every reachable read position contains post-change audio.
    // -----------------------------------------------------------------------
    uint32_t _delayMuteCounter;

    // Block-constant delay cache — written by prepareDelay(), read by processDelay()
    float _delaySampLCached;        // Left delay time in samples
    float _delaySampRCached;        // Right delay time in samples
    float _delayFbCached;           // Feedback coefficient
    float _delayWetCached;          // Wet level (absolute)
    float _delayDryCached;          // 1 - wet
    bool  _delayInvertCached;       // Phase inversion flag

    void prepareDelay();
    inline void processDelay(float inL, float inR,
                             float &outL, float &outR);
};
