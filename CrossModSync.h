#pragma once
// =============================================================================
// CrossModSync.h — JT-8000 Cross Modulation & Oscillator Hard Sync
// =============================================================================
//
// TWO INDEPENDENT FEATURES IN ONE FILE:
//
//   1. CROSS MODULATION (audio-rate FM: OSC2 → OSC1 pitch)
//      Implemented as an AudioMixer4 pre-stage injected before OSC1's FM mixer
//      slot 0.  When depth is 0.0, the mixer passes the static pitch DC through
//      at unity — effectively free beyond the unconditional update() call.
//
//   2. OSCILLATOR HARD SYNC (OSC1 slave, OSC2 master)
//      Sample-accurate sync requires both oscillators' phase accumulators to be
//      coupled within the same update() call.  AudioSynthOscSync is a single
//      AudioStream that contains BOTH oscillator cores internally, generating
//      two output channels (ch0 = OSC1/slave, ch1 = OSC2/master).
//
//      When sync is DISABLED, VoiceBlock uses the existing separate
//      OscillatorBlock path (no CPU change).
//      When sync is ENABLED, VoiceBlock swaps audio connections to route through
//      AudioSynthOscSync instead.
//
// COMPILE FLAGS (JT8000_OptFlags.h):
//   JT_OPT_CROSS_MOD         — master enable for cross-modulation
//   JT_OPT_OSC_SYNC          — master enable for hard sync
//   JT_CROSS_MOD_CURVE       — 0 = linear, 1 = exponential depth curve
//
// CPU COST:
//   Cross-mod:  +1 AudioMixer4 per voice (8 total). ~20 µs/block total.
//               When depth=0.0 and no audio connected, null block passthrough.
//   Osc sync:   AudioSynthOscSync replaces two AudioSynthWaveformJT updates
//               when active.  Net cost is similar — two phase accumulators +
//               waveform lookups, but in one update() call with sync check.
//               When sync is OFF, the class is not in the audio graph at all.
//
// =============================================================================

#include "Audio.h"
#include "JT8000_OptFlags.h"
#include "OscillatorBlock.h"    // FM_OCTAVE_RANGE, FM_SEMITONE_SCALE

// Forward declarations
class VoiceBlock;

// =============================================================================
// PART 1: CROSS MODULATION — AudioMixer4 pre-stage for OSC1 FM input
// =============================================================================
//
// SIGNAL FLOW (when cross-mod is active):
//
//   _combinedPitchDc ──► [CrossModPreMixer] slot 0 (gain 1.0, always)
//   OSC2 output      ──► [CrossModPreMixer] slot 1 (gain = depth × scale)
//                              │
//                              ▼
//                     OSC1 _frequencyModMixer slot 0
//                              │
//                              ▼
//                     (LFO1/LFO2/PitchEnv on slots 1-3 as before)
//                              │
//                              ▼
//                     _mainOsc FM input
//
// The CrossModPreMixer replaces the direct _combinedPitchDc → FM mixer
// connection on OSC1 only.  OSC2 is unaffected.
//
// DEPTH SCALING:
//   OSC2 output is int16 audio (±32767).  The FM mixer interprets ±1.0 as
//   ±FM_OCTAVE_RANGE octaves.  Raw OSC2 at unity gain would produce ±10
//   octave modulation — far too aggressive.
//
//   Cross-mod depth CC maps 0–127 to a scaled gain on slot 1:
//     Linear:      depth = cc_to_norm(cc) × CROSS_MOD_MAX_OCTAVES / FM_OCTAVE_RANGE
//     Exponential: depth = (e^(cc_to_norm(cc) × k) - 1) / (e^k - 1) × max_scale
//
//   CROSS_MOD_MAX_OCTAVES defaults to 2.0 (±2 octave swing at full depth).
//   Adjustable via compile flag or runtime setter.
//
// IMPORTANT:
//   The cross-mod signal is OSC2's AUDIO output, not its phase.  This is
//   frequency modulation (not phase modulation).  It flows through the
//   existing FM path in AudioSynthWaveformJT which uses the integer exp2
//   approximation — no additional powf() per sample.
// =============================================================================

#if JT_OPT_CROSS_MOD || JT_OPT_OSC_SYNC

// Maximum cross-mod swing in octaves at CC 127 (full depth).
// ±2 octaves is musically useful without being completely chaotic.
// Increase for more extreme FM sounds.
#ifndef JT_CROSS_MOD_MAX_OCTAVES
#define JT_CROSS_MOD_MAX_OCTAVES  2.0f
#endif

// Pre-computed scale factor: maps full-scale audio (±1.0 after int16 normalisation)
// to ±CROSS_MOD_MAX_OCTAVES in the FM mixer's octave space.
// The FM mixer interprets ±1.0 input as ±FM_OCTAVE_RANGE octaves.
// To get ±N octaves from a ±1.0 signal: gain = N / FM_OCTAVE_RANGE.
static constexpr float CROSS_MOD_FULL_SCALE =
    JT_CROSS_MOD_MAX_OCTAVES / FM_OCTAVE_RANGE;

/// Convert CC 0–127 to cross-mod mixer gain using the selected curve.
/// Returns 0.0 at CC 0 (no modulation) and CROSS_MOD_FULL_SCALE at CC 127.
inline float crossModDepthFromCC(uint8_t cc) {
    if (cc == 0) return 0.0f;
    const float norm = (float)cc / 127.0f;

#if JT_CROSS_MOD_CURVE == 0
    // --- Linear curve ---
    // Simple and predictable.  Most of the musical action is in the first
    // quarter of the knob range; the upper range gets aggressive fast.
    return norm * CROSS_MOD_FULL_SCALE;

#elif JT_CROSS_MOD_CURVE == 1
    // --- Exponential curve ---
    // More resolution at low depths where subtle FM timbres live.
    // k controls the curve steepness (higher = more exponential).
    static constexpr float k = 4.0f;
    static constexpr float denom = expf(k) - 1.0f;  // computed once
    const float shaped = (expf(norm * k) - 1.0f) / denom;
    return shaped * CROSS_MOD_FULL_SCALE;

#else
    #error "JT_CROSS_MOD_CURVE must be 0 (linear) or 1 (exponential)"
#endif
}

#endif // JT_OPT_CROSS_MOD || JT_OPT_OSC_SYNC


// =============================================================================
// PART 2: OSCILLATOR HARD SYNC — Sample-accurate coupled dual oscillator
// =============================================================================
//
// AudioSynthOscSync is a 2-output AudioStream.
//   Output 0 = OSC1 (slave) — phase resets when master wraps.
//   Output 1 = OSC2 (master) — runs freely.
//
// AUDIO INPUTS (optional):
//   Input 0 = OSC1 FM modulation signal  (from OSC1's _frequencyModMixer)
//   Input 1 = OSC2 FM modulation signal  (from OSC2's _frequencyModMixer)
//   Input 2 = OSC1 shape/PWM signal      (from OSC1's _shapeModMixer)
//   Input 3 = OSC2 shape/PWM signal      (from OSC2's _shapeModMixer)
//
// SYNC BEHAVIOUR:
//   On every sample, the master (OSC2) phase accumulator is advanced.
//   If the master phase wraps (current < previous), the slave (OSC1) phase
//   accumulator is reset to zero AT THAT EXACT SAMPLE.  The slave's waveform
//   output for that sample is computed from the reset phase, producing the
//   characteristic hard-sync harmonic tearing.
//
// CROSS MODULATION + SYNC:
//   When both features are active simultaneously, OSC2's output feeds into
//   OSC1's FM path.  The sync engine handles this internally — the cross-mod
//   signal is the master's (OSC2) raw sample value, injected into the slave's
//   phase increment calculation on a per-sample basis.
//
// WAVEFORM SUPPORT:
//   Both oscillators support the same waveform types as AudioSynthWaveformJT:
//   sine, saw, square, triangle, pulse (with shape mod), arbitrary, sample&hold,
//   and all band-limited variants.
//
// WHAT THIS CLASS DOES NOT DO:
//   - Supersaw.  When sync is active, supersaw is not available on OSC1.
//     The supersaw requires 7 independent phase accumulators; syncing all 7
//     would produce a very different (and arguably useless) sound.
//     VoiceBlock should force OSC1 to a standard waveform when sync is enabled.
//   - Feedback comb.  The comb network is downstream of the oscillator output
//     and continues to work normally on the sync outputs.
//   - Glide.  Glide changes the base frequency via .setFrequency() and works
//     identically — the sync engine reads the current phase_increment.
//
// CPU COST:
//   Two phase accumulators + two waveform lookups + one phase-wrap comparison
//   per sample.  Essentially the same as running two AudioSynthWaveformJT
//   instances, plus ~1 cycle per sample for the sync check.
//
// DESIGN NOTE:
//   This class duplicates some waveform generation code from Synth_Waveform.cpp.
//   This is intentional — extracting the inner loop into a shared function would
//   add function call overhead per sample (128 calls/block) and prevent the
//   compiler from keeping phase accumulators in registers across the loop.
//   The FM exp2 approximation is also inlined for the same reason.
//
// =============================================================================

#if JT_OPT_OSC_SYNC

class AudioSynthOscSync : public AudioStream {
public:
    AudioSynthOscSync();

    // =========================================================================
    // OSCILLATOR PARAMETERS — mirror AudioSynthWaveformJT interface
    // =========================================================================

    /// Set OSC1 (slave) base frequency in Hz.
    void setSlaveFrequency(float freq);

    /// Set OSC2 (master) base frequency in Hz.
    void setMasterFrequency(float freq);

    /// Set OSC1 (slave) amplitude 0.0–1.0.
    void setSlaveAmplitude(float amp);

    /// Set OSC2 (master) amplitude 0.0–1.0.
    void setMasterAmplitude(float amp);

    /// Set OSC1 (slave) waveform type (WAVEFORM_SINE, WAVEFORM_SAWTOOTH, etc.)
    void setSlaveWaveform(short type);

    /// Set OSC2 (master) waveform type.
    void setMasterWaveform(short type);

    /// Set FM modulation range in octaves (applied to both oscillators).
    /// Must match FM_OCTAVE_RANGE for consistent behaviour with the rest
    /// of the pitch modulation chain.
    void frequencyModulation(float octaves);

    // =========================================================================
    // SYNC CONTROL
    // =========================================================================

    /// Enable or disable hard sync.  When disabled, both oscillators run
    /// independently (no phase reset).  The outputs are still valid —
    /// VoiceBlock can keep this in the graph even when sync is off,
    /// though switching to separate OscillatorBlocks saves the 4-input
    /// overhead when sync is not needed.
    void setSyncEnabled(bool enabled) { _syncEnabled = enabled; }
    bool getSyncEnabled() const       { return _syncEnabled; }

    // =========================================================================
    // CROSS MODULATION (integrated — OSC2 output → OSC1 FM, per-sample)
    // =========================================================================

    /// Set cross-mod depth.  This is the gain applied to the master's raw
    /// sample output before it modulates the slave's phase increment.
    /// Range: 0.0 (off) to CROSS_MOD_FULL_SCALE (full depth).
    /// Use crossModDepthFromCC() to convert from CC value.
    void setCrossModDepth(float depth) { _crossModDepth = depth; }
    float getCrossModDepth() const     { return _crossModDepth; }

    // =========================================================================
    // SHAPE / PULSE WIDTH
    // =========================================================================

    /// Set OSC1 pulse width for WAVEFORM_PULSE (0.0–1.0).
    void setSlavePulseWidth(float pw);

    /// Set OSC2 pulse width for WAVEFORM_PULSE (0.0–1.0).
    void setMasterPulseWidth(float pw);

    // =========================================================================
    // ARBITRARY WAVEFORM
    // =========================================================================

    /// Set OSC1 arbitrary waveform table.
    void setSlaveArbData(const int16_t* data) { _slaveArbData = data; }

    /// Set OSC2 arbitrary waveform table.
    void setMasterArbData(const int16_t* data) { _masterArbData = data; }

    // =========================================================================
    // AudioStream interface
    // =========================================================================

    virtual void update(void) override;

private:
    // Audio input queue — 4 slots:
    //   [0] = OSC1 FM mod, [1] = OSC2 FM mod,
    //   [2] = OSC1 shape mod, [3] = OSC2 shape mod
    audio_block_t* inputQueueArray[4];

    // ── Slave (OSC1) state ──────────────────────────────────────────────────
    uint32_t _slavePhase      = 0;        // Phase accumulator
    uint32_t _slavePhaseInc   = 0;        // Base phase increment (from frequency)
    int32_t  _slaveMagnitude  = 0;        // Amplitude (0–65536)
    uint8_t  _slaveWaveform   = WAVEFORM_SAWTOOTH;
    uint32_t _slavePulseWidth = 0x40000000u;  // 50% default
    const int16_t* _slaveArbData = nullptr;
    int16_t  _slaveSampleHold = 0;        // Held value for S&H waveform

    // ── Master (OSC2) state ─────────────────────────────────────────────────
    uint32_t _masterPhase     = 0;
    uint32_t _masterPhaseInc  = 0;
    int32_t  _masterMagnitude = 0;
    uint8_t  _masterWaveform  = WAVEFORM_SAWTOOTH;
    uint32_t _masterPulseWidth = 0x40000000u;
    const int16_t* _masterArbData = nullptr;
    int16_t  _masterSampleHold = 0;

    // ── Modulation ──────────────────────────────────────────────────────────
    uint32_t _modulationFactor = 32768;   // FM scaling (octaves × 4096)
    bool     _syncEnabled      = false;
    float    _crossModDepth    = 0.0f;    // 0.0 = off

    // ── Internal helpers ────────────────────────────────────────────────────

    /// Generate one sample for the given waveform type at the given phase.
    /// This is the inner waveform lookup — inlined for performance.
    /// shape is the per-sample pulse width (from shape modulation or default).
    static inline int16_t generateSample(
        uint8_t waveform,
        uint32_t phase,
        int32_t magnitude,
        uint32_t pulseWidth,
        const int16_t* arbdata,
        int16_t& sampleHold,
        uint32_t priorPhase
    );

    /// Fast integer exp2 approximation for FM — identical to Synth_Waveform.cpp.
    /// Input: scaled octave count (n = sample × modulation_factor).
    /// Output: phase increment scale factor (unsigned, 16.16 fixed point).
    static inline uint32_t fmExp2(int32_t n);
};

#endif // JT_OPT_OSC_SYNC
