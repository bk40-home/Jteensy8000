/* Audio Library for Teensy
 * Copyright (c) 2025, Kris Bishop, bishopkris40@hotmail.com
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
 */

#pragma once

// =============================================================================
// CrossModSync.h — JT-8000 Cross Modulation & Oscillator Hard Sync
// =============================================================================
//
// TWO FULLY INDEPENDENT FEATURES:
//
//   1. CROSS MODULATION  (JT_OPT_CROSS_MOD)
//      Audio-rate FM: OSC2's output is injected into OSC1's FM pitch path.
//      This is JP-8000 "OSC CROSS MOD" — works with any waveform combination.
//
//      Implementation (Option A — direct injection):
//        OSC2 output → OSC1 _frequencyModMixer slot 0 at a scaled gain.
//        The pitch DC already feeds slot 0 at gain 1.0.  AudioMixer4 sums
//        all inputs, so the OSC2 signal adds on top of the static pitch DC
//        without a dedicated pre-mixer object.  No AudioMixer4 is added.
//
//      When depth == 0.0:  OSC1 FM slot 0 gain for OSC2 = 0 → no modulation.
//      When depth  > 0.0:  OSC2 audio drives OSC1 pitch at a scaled gain.
//
//      DOES NOT REQUIRE SYNC.  Works when sync is off, on, or not compiled.
//
//   2. OSCILLATOR HARD SYNC  (JT_OPT_OSC_SYNC)
//      Sample-accurate phase reset: when OSC2 (master) phase wraps, OSC1
//      (slave) phase is reset to zero at that exact sample.
//      This is JP-8000 "OSC SYNC" — works with any waveform combination.
//
//      Implementation: AudioSynthOscSync is a single AudioStream containing
//      both oscillator cores.  VoiceBlock swaps audio connections to route
//      through this engine when sync is enabled.
//
//      When sync is OFF:  OscillatorBlock normal path is active.
//      When sync is ON:   AudioSynthOscSync replaces both oscillator outputs
//                         in the audio graph.
//
//      DOES NOT REQUIRE CROSS MOD.  The two features are independent.
//      Both features active simultaneously: sync + FM timbres, which is the
//      classic "JP-8000 sync sweep" sound.
//
// CROSS MOD WHEN SYNC IS ACTIVE:
//   When both features are enabled at the same time, the AudioSynthOscSync
//   engine handles cross-mod internally (per-sample, in the inner loop).
//   The external OSC1 FM slot 0 injection is not in the graph during sync
//   because OscillatorBlock outputs are disconnected.  Cross-mod is still
//   effective — it just routes through the sync engine instead.
//
// COMPILE FLAGS (JT8000_OptFlags.h):
//   JT_OPT_CROSS_MOD    — enable cross modulation (independent of sync)
//   JT_OPT_OSC_SYNC     — enable hard sync (independent of cross mod)
//   JT_CROSS_MOD_CURVE  — 0 = linear depth curve, 1 = exponential
//
// WAVEFORM SUPPORT:
//   Both features work with sine, saw, square, triangle, pulse, arbitrary,
//   and sample-and-hold.  PolyBLEP band-limited variants are NOT supported
//   in sync mode — PolyBLEP correction requires continuous phase tracking
//   which hard sync disrupts.  Raw sync harmonics are musically correct.
//   Cross-mod works fine with PolyBLEP waveforms since it only injects an
//   FM signal; it does not touch the phase accumulator.
//
// CPU COST:
//   Cross-mod:  Zero extra audio objects (Option A — direct injection).
//               One gain(1, ...) call on depth change.  When depth == 0 the
//               mixer slot is silent — no block is transmitted through it.
//   Osc sync:   AudioSynthOscSync runs two phase accumulators + waveform
//               lookups + one wrap comparison per sample.  Net cost is
//               equivalent to two separate AudioSynthWaveformJT instances.
//               When sync is OFF, the engine is not in the audio graph.
//
// =============================================================================

#include "Audio.h"
#include "JT8000_OptFlags.h"
#include "OscillatorBlock.h"    // FM_OCTAVE_RANGE, FM_SEMITONE_SCALE

// Forward declarations
class VoiceBlock;

// =============================================================================
// CROSS MODULATION — depth scaling helper
// =============================================================================
//
// crossModDepthFromCC() converts a CC 0–127 value to the gain that is applied
// to OSC2's audio output before it enters OSC1's FM mixer slot 0.
//
// SCALING RATIONALE:
//   OSC2 output is int16 audio (±32767, normalised to ±1.0 in the FM mixer).
//   The FM mixer interprets ±1.0 as ±FM_OCTAVE_RANGE octaves.
//   Raw OSC2 at unity gain would produce ±10 octave FM — far too extreme.
//
//   CROSS_MOD_MAX_OCTAVES (default 2.0) is the maximum swing at CC 127.
//   gain = depth_fraction × (MAX_OCTAVES / FM_OCTAVE_RANGE)
//
//   This function is guarded by JT_OPT_CROSS_MOD only — no dependency on
//   JT_OPT_OSC_SYNC.
//
// =============================================================================

#if JT_OPT_CROSS_MOD

// Maximum cross-mod FM swing in octaves at CC 127.
// ±2 octaves is musically useful; increase for more extreme FM.
#ifndef JT_CROSS_MOD_MAX_OCTAVES
#define JT_CROSS_MOD_MAX_OCTAVES  2.0f
#endif

// Pre-computed scale: maps ±1.0 audio (after int16 normalisation in the FM
// mixer) to ±CROSS_MOD_MAX_OCTAVES in the FM path's octave space.
// FM mixer interprets ±1.0 as ±FM_OCTAVE_RANGE octaves, so:
//   gain = MAX_OCTAVES / FM_OCTAVE_RANGE
static constexpr float CROSS_MOD_FULL_SCALE =
    JT_CROSS_MOD_MAX_OCTAVES / FM_OCTAVE_RANGE;

/// Convert CC 0–127 to the FM mixer gain for cross-mod.
/// Returns 0.0 at CC 0 (no modulation) and CROSS_MOD_FULL_SCALE at CC 127.
/// Apply this value directly to OSC1._frequencyModMixer.gain(1, result).
inline float crossModDepthFromCC(uint8_t cc) {
    if (cc == 0) return 0.0f;                           // Fast path — no mod
    const float norm = (float)cc / 127.0f;              // Normalise 0..1

#if JT_CROSS_MOD_CURVE == 0
    // ---- Linear ----
    // Predictable, most musical action in the lower half of the knob range.
    return norm * CROSS_MOD_FULL_SCALE;

#elif JT_CROSS_MOD_CURVE == 1
    // ---- Exponential ----
    // More resolution at low depths where subtle FM timbres live.
    // k controls curve steepness — higher = more exponential feel.
    static constexpr float k     = 4.0f;
    static constexpr float denom = expf(k) - 1.0f;     // Computed once at link time
    const float shaped = (expf(norm * k) - 1.0f) / denom;
    return shaped * CROSS_MOD_FULL_SCALE;

#else
    #error "JT_CROSS_MOD_CURVE must be 0 (linear) or 1 (exponential)"
#endif
}

#endif // JT_OPT_CROSS_MOD


// =============================================================================
// OSCILLATOR HARD SYNC — Sample-accurate coupled dual oscillator
// =============================================================================
//
// AudioSynthOscSync is a 2-output AudioStream.
//   Output 0 = OSC1 (slave)  — phase resets when master wraps.
//   Output 1 = OSC2 (master) — runs freely.
//
// AUDIO INPUTS (optional, connected by VoiceBlock when sync is enabled):
//   Input 0 = OSC1 FM modulation  (from OSC1's _frequencyModMixer)
//   Input 1 = OSC2 FM modulation  (from OSC2's _frequencyModMixer)
//   Input 2 = OSC1 shape/PWM      (from OSC1's _shapeModMixer)
//   Input 3 = OSC2 shape/PWM      (from OSC2's _shapeModMixer)
//
// SYNC BEHAVIOUR:
//   Master phase is advanced each sample.  On wrap (current < previous),
//   slave phase is reset to zero at that exact sample — sample-accurate sync.
//   The slave's output for that sample is computed from the reset phase,
//   producing the characteristic hard-sync tearing harmonics.
//
// CROSS MODULATION WHEN SYNC IS ACTIVE:
//   When _crossModDepth > 0, the master's raw sample value is injected into
//   the slave's FM calculation on a per-sample basis (inside the inner loop).
//   This is more accurate than the audio-graph approach because it bypasses
//   the one-block latency of an external AudioMixer4 connection.
//   VoiceBlock sets this depth from _crossModDepth when enabling sync.
//
// WAVEFORM SUPPORT:
//   sine, sawtooth, square, triangle, pulse (with shape mod), arbitrary,
//   sample-and-hold.  Band-limited (PolyBLEP) variants not supported in
//   sync mode — see header comment above.
//
// DESIGN NOTE — code duplication:
//   generateSample() duplicates waveform cases from Synth_Waveform.cpp.
//   This is intentional — extracting into a shared function would add 128
//   function call overheads per block and prevent the compiler keeping phase
//   accumulators in registers across the loop.  The FM exp2 approximation is
//   also inlined for the same reason.
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

    /// Enable or disable hard sync.
    /// When disabled, both oscillators run independently (no phase reset).
    /// Outputs are still valid — VoiceBlock disconnects the engine from the
    /// graph entirely when sync is off to save the 4-input overhead.
    void setSyncEnabled(bool enabled) { _syncEnabled = enabled; }
    bool getSyncEnabled() const       { return _syncEnabled; }

    // =========================================================================
    // CROSS MODULATION (integrated — OSC2 output → OSC1 FM, per-sample)
    // =========================================================================
    //
    // When the sync engine is active AND cross-mod depth > 0, the master's
    // raw sample is injected into the slave's FM path inside the inner loop.
    // This is independent of the external cross-mod injection in VoiceBlock —
    // when sync is on, the external OscillatorBlock outputs are not in the
    // graph so the internal path handles cross-mod automatically.
    //
    // VoiceBlock::setCrossModDepth() forwards the depth here when sync is on.

    /// Set cross-mod depth for in-engine injection.
    /// Range: 0.0 (off) → CROSS_MOD_FULL_SCALE (full depth at CC 127).
    /// Use crossModDepthFromCC() to convert from CC value.
    void  setCrossModDepth(float depth) { _crossModDepth = depth; }
    float getCrossModDepth() const      { return _crossModDepth; }

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

    /// Set OSC1 arbitrary waveform table pointer.
    void setSlaveArbData(const int16_t* data) { _slaveArbData = data; }

    /// Set OSC2 arbitrary waveform table pointer.
    void setMasterArbData(const int16_t* data) { _masterArbData = data; }

    // =========================================================================
    // AudioStream interface
    // =========================================================================

    virtual void update(void) override;

private:
    // Audio input queue — 4 slots:
    //   [0] = OSC1 FM mod, [1] = OSC2 FM mod,
    //   [2] = OSC1 shape,  [3] = OSC2 shape
    audio_block_t* inputQueueArray[4];

    // ── Slave (OSC1) state ──────────────────────────────────────────────────
    uint32_t _slavePhase      = 0;
    uint32_t _slavePhaseInc   = 0;
    int32_t  _slaveMagnitude  = 0;
    uint8_t  _slaveWaveform   = WAVEFORM_SAWTOOTH;
    uint32_t _slavePulseWidth = 0x40000000u;   // 50% default
    const int16_t* _slaveArbData = nullptr;
    int16_t  _slaveSampleHold = 0;

    // ── Master (OSC2) state ─────────────────────────────────────────────────
    uint32_t _masterPhase      = 0;
    uint32_t _masterPhaseInc   = 0;
    int32_t  _masterMagnitude  = 0;
    uint8_t  _masterWaveform   = WAVEFORM_SAWTOOTH;
    uint32_t _masterPulseWidth = 0x40000000u;
    const int16_t* _masterArbData = nullptr;
    int16_t  _masterSampleHold = 0;

    // ── Modulation ──────────────────────────────────────────────────────────
    uint32_t _modulationFactor = 32768;   // FM scaling (octaves × 4096)
    bool     _syncEnabled      = false;
    float    _crossModDepth    = 0.0f;

    // ── Internal helpers ────────────────────────────────────────────────────

    /// Generate one waveform sample.  Inlined into update() — compiler keeps
    /// phase accumulators in registers across the 128-sample loop.
    /// priorPhase is the phase value before this sample's increment, used
    /// for sample-and-hold wrap detection.
    static inline int16_t generateSample(
        uint8_t        waveform,
        uint32_t       phase,
        int32_t        magnitude,
        uint32_t       pulseWidth,
        const int16_t* arbdata,
        int16_t&       sampleHold,
        uint32_t       priorPhase
    );

    /// Fast integer exp2 approximation for FM — Laurent de Soras (musicdsp #106).
    /// Input:  n = sample × modulation_factor  (scaled octave count, int32).
    /// Output: phase increment scale factor (unsigned, 16.16 fixed point).
    /// Identical to the version in Synth_Waveform.cpp — duplicated to avoid
    /// function call overhead (128 calls/block) in the inner loop.
    static inline uint32_t fmExp2(int32_t n);
};

#endif // JT_OPT_OSC_SYNC
