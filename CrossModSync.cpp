// =============================================================================
// CrossModSync.cpp — JT-8000 Cross Modulation & Oscillator Hard Sync
// =============================================================================
//
// See CrossModSync.h for architecture, signal flow, and design rationale.
//
// KEY IMPLEMENTATION NOTES:
//   - The update() loop processes 128 samples.  For each sample:
//     1. Read FM modulation inputs (if connected).
//     2. Compute phase increment for master (OSC2) and slave (OSC1).
//     3. Advance master phase.  Check for wrap (sync trigger).
//     4. If sync enabled and master wrapped: reset slave phase to 0.
//     5. If cross-mod active: inject master's raw sample into slave's
//        phase increment via the FM exp2 path.
//     6. Advance slave phase.
//     7. Generate waveform samples for both oscillators.
//     8. Write to output blocks (ch0 = slave, ch1 = master).
//
//   - The FM exp2 approximation is duplicated from Synth_Waveform.cpp to
//     avoid function call overhead in the inner loop.  Both use the Laurent
//     de Soras fast approximation (musicdsp.org #106).
//
//   - Shape modulation (PWM) is supported via inputs [2] and [3].
//     When no shape input is connected, the default pulse width is used.
//
//   - Band-limited waveforms (PolyBLEP) are NOT supported in sync mode.
//     The phase discontinuity from hard sync makes PolyBLEP correction
//     unreliable at the reset point.  Standard waveforms produce the
//     expected raw sync tearing — this is musically desirable.
//
// =============================================================================

#include "CrossModSync.h"
#include <utility/dspinst.h>   // multiply_32x32_rshift32, signed_saturate_rshift

// Sine lookup — defined in the Audio library's waveforms.c
extern "C" {
extern const int16_t AudioWaveformSine[257];
}

#if JT_OPT_OSC_SYNC

// =============================================================================
// CONSTRUCTOR
// =============================================================================

AudioSynthOscSync::AudioSynthOscSync()
    : AudioStream(4, inputQueueArray)
{
    // Default FM range matches OscillatorBlock — ±10 octaves at full signal.
    _modulationFactor = (uint32_t)(FM_OCTAVE_RANGE * 4096.0f);
}

// =============================================================================
// PARAMETER SETTERS
// =============================================================================

void AudioSynthOscSync::setSlaveFrequency(float freq) {
    if (freq < 0.0f)                          freq = 0.0f;
    else if (freq > AUDIO_SAMPLE_RATE_EXACT / 2.0f) freq = AUDIO_SAMPLE_RATE_EXACT / 2.0f;
    _slavePhaseInc = freq * (4294967296.0f / AUDIO_SAMPLE_RATE_EXACT);
    if (_slavePhaseInc > 0x7FFE0000u) _slavePhaseInc = 0x7FFE0000u;
}

void AudioSynthOscSync::setMasterFrequency(float freq) {
    if (freq < 0.0f)                          freq = 0.0f;
    else if (freq > AUDIO_SAMPLE_RATE_EXACT / 2.0f) freq = AUDIO_SAMPLE_RATE_EXACT / 2.0f;
    _masterPhaseInc = freq * (4294967296.0f / AUDIO_SAMPLE_RATE_EXACT);
    if (_masterPhaseInc > 0x7FFE0000u) _masterPhaseInc = 0x7FFE0000u;
}

void AudioSynthOscSync::setSlaveAmplitude(float amp) {
    if (amp < 0.0f) amp = 0.0f;
    else if (amp > 1.0f) amp = 1.0f;
    _slaveMagnitude = amp * 65536.0f;
}

void AudioSynthOscSync::setMasterAmplitude(float amp) {
    if (amp < 0.0f) amp = 0.0f;
    else if (amp > 1.0f) amp = 1.0f;
    _masterMagnitude = amp * 65536.0f;
}

void AudioSynthOscSync::setSlaveWaveform(short type) {
    _slaveWaveform = type;
}

void AudioSynthOscSync::setMasterWaveform(short type) {
    _masterWaveform = type;
}

void AudioSynthOscSync::frequencyModulation(float octaves) {
    if (octaves > 12.0f)     octaves = 12.0f;
    else if (octaves < 0.1f) octaves = 0.1f;
    _modulationFactor = octaves * 4096.0f;
}

void AudioSynthOscSync::setSlavePulseWidth(float pw) {
    if (pw < 0.0f) pw = 0.0f;
    else if (pw > 1.0f) pw = 1.0f;
    _slavePulseWidth = pw * 4294967296.0f;
}

void AudioSynthOscSync::setMasterPulseWidth(float pw) {
    if (pw < 0.0f) pw = 0.0f;
    else if (pw > 1.0f) pw = 1.0f;
    _masterPulseWidth = pw * 4294967296.0f;
}

// =============================================================================
// FM EXP2 — Fast integer exponential for frequency modulation
// =============================================================================
// Identical to the Laurent de Soras approximation in Synth_Waveform.cpp.
// Input:  n = sample_value × modulation_factor (scaled octave count).
// Output: unsigned scale factor for phase increment (16.16 fixed point).
//
// The multiplication phaseInc × scale is done in 64-bit to avoid overflow,
// then right-shifted by 16 to get the modulated phase increment.
// =============================================================================

inline uint32_t AudioSynthOscSync::fmExp2(int32_t n) {
    // Extract integer octave part (4 bits) and fractional part (27 bits)
    int32_t ipart = n >> 27;
    n &= 0x7FFFFFF;

    // Fast exp2 approximation (Laurent de Soras, musicdsp.org #106)
    n = (n + 134217728) << 3;
    n = multiply_32x32_rshift32_rounded(n, n);
    n = multiply_32x32_rshift32_rounded(n, 715827883) << 3;
    n = n + 715827882;

    // Apply integer octave shift
    return (uint32_t)n >> (14 - ipart);
}

// =============================================================================
// WAVEFORM SAMPLE GENERATION — one sample at a time
// =============================================================================
// Processes a single sample for the given waveform type.  Called twice per
// loop iteration (once for slave, once for master).
//
// This duplicates the waveform cases from Synth_Waveform.cpp but operates
// on single samples rather than blocks.  The compiler inlines this into the
// update() loop, keeping phase variables in registers.
//
// Band-limited waveforms are NOT supported — they require continuous phase
// tracking that hard sync disrupts.  Using standard waveforms produces the
// expected raw sync harmonics.
// =============================================================================

inline int16_t AudioSynthOscSync::generateSample(
    uint8_t  waveform,
    uint32_t phase,
    int32_t  magnitude,
    uint32_t pulseWidth,
    const int16_t* arbdata,
    int16_t& sampleHold,
    uint32_t priorPhase)
{
    // Silent — skip lookup entirely
    if (magnitude == 0) return 0;

    uint32_t index, index2, scale;
    int32_t  val1, val2;

    switch (waveform) {

    case WAVEFORM_SINE: {
        index = phase >> 24;
        val1  = AudioWaveformSine[index];
        val2  = AudioWaveformSine[index + 1];
        scale = (phase >> 8) & 0xFFFF;
        val2 *= scale;
        val1 *= 0x10000 - scale;
        return (int16_t)multiply_32x32_rshift32(val1 + val2, magnitude);
    }

    case WAVEFORM_SAWTOOTH: {
        // Phase 0..FFFFFFFF maps linearly to −magnitude..+magnitude
        return (int16_t)signed_multiply_32x16t(magnitude, phase);
    }

    case WAVEFORM_SAWTOOTH_REVERSE: {
        // Reversed: phase 0..FFFFFFFF maps to +magnitude..−magnitude
        return (int16_t)signed_multiply_32x16t(magnitude, 0xFFFFFFFFu - phase);
    }

    case WAVEFORM_SQUARE: {
        // 50% duty cycle — high for first half, low for second half
        return (phase & 0x80000000u) ?
            (int16_t)(magnitude >> 16) :
            (int16_t)(-(magnitude >> 16));
    }

    case WAVEFORM_PULSE: {
        // Variable duty cycle — pulseWidth determines threshold
        return (phase < pulseWidth) ?
            (int16_t)(magnitude >> 16) :
            (int16_t)(-(magnitude >> 16));
    }

    case WAVEFORM_TRIANGLE: {
        uint32_t phtop = phase >> 30;
        if (phtop == 1 || phtop == 2) {
            return (int16_t)(((0xFFFF - (phase >> 15)) * magnitude) >> 16);
        } else {
            return (int16_t)((((int32_t)phase >> 15) * magnitude) >> 16);
        }
    }

    case WAVEFORM_ARBITRARY: {
        if (!arbdata) return 0;
        index  = phase >> 24;
        index2 = index + 1;
        if (index2 >= 256) index2 = 0;
        val1  = arbdata[index];
        val2  = arbdata[index2];
        scale = (phase >> 8) & 0xFFFF;
        val2 *= scale;
        val1 *= 0x10000 - scale;
        return (int16_t)multiply_32x32_rshift32(val1 + val2, magnitude);
    }

    case WAVEFORM_SAMPLE_HOLD: {
        // Latch new random value on phase wrap
        if (phase < priorPhase) {
            sampleHold = random(magnitude) - (magnitude >> 1);
        }
        return sampleHold;
    }

    default:
        return 0;
    }
}


// =============================================================================
// UPDATE — Main audio processing loop (128 samples per call)
// =============================================================================
//
// This is the heart of the sync engine.  Called every audio interrupt (~2.9 ms
// at 44.1 kHz / 128 samples).
//
// SIGNAL FLOW PER SAMPLE:
//   1. Read FM mod inputs for both oscillators (if connected).
//   2. Read shape mod inputs for both oscillators (if connected).
//   3. Compute master (OSC2) modulated phase increment.
//   4. Advance master phase.  Detect wrap for sync trigger.
//   5. Compute slave (OSC1) modulated phase increment.
//      - If cross-mod active: add master's current sample to slave's FM.
//   6. If sync triggered: reset slave phase to zero.
//   7. Advance slave phase.
//   8. Generate waveform samples for both.
//   9. Write to output blocks.
//
// =============================================================================

void AudioSynthOscSync::update(void)
{
    // ── Silence check: skip everything if both oscillators are muted ────────
    if (_slaveMagnitude == 0 && _masterMagnitude == 0) {
        return;  // No output blocks allocated — downstream receives silence
    }

    // ── Allocate output blocks ──────────────────────────────────────────────
    audio_block_t* slaveOut  = allocate();
    if (!slaveOut) return;

    audio_block_t* masterOut = allocate();
    if (!masterOut) {
        release(slaveOut);
        return;
    }

    // ── Receive optional modulation inputs ──────────────────────────────────
    // receiveReadOnly() returns nullptr when nothing is connected — zero cost.
    audio_block_t* slaveFM    = receiveReadOnly(0);  // OSC1 FM mod
    audio_block_t* masterFM   = receiveReadOnly(1);  // OSC2 FM mod
    audio_block_t* slaveShape = receiveReadOnly(2);  // OSC1 shape/PWM
    audio_block_t* masterShape = receiveReadOnly(3); // OSC2 shape/PWM

    // ── Cache member variables in locals for register optimisation ───────────
    uint32_t slavePh    = _slavePhase;
    uint32_t masterPh   = _masterPhase;
    const uint32_t slaveInc   = _slavePhaseInc;
    const uint32_t masterInc  = _masterPhaseInc;
    const int32_t  slaveMag   = _slaveMagnitude;
    const int32_t  masterMag  = _masterMagnitude;
    const uint32_t modFactor  = _modulationFactor;
    const bool     syncOn     = _syncEnabled;
    const float    xmodDepth  = _crossModDepth;
    const uint8_t  slaveWf    = _slaveWaveform;
    const uint8_t  masterWf   = _masterWaveform;
    const int16_t* slaveArb   = _slaveArbData;
    const int16_t* masterArb  = _masterArbData;

    uint32_t slavePW  = _slavePulseWidth;
    uint32_t masterPW = _masterPulseWidth;

    int16_t slaveSH   = _slaveSampleHold;
    int16_t masterSH  = _masterSampleHold;

    // ── Scale factor for cross-mod FM injection ─────────────────────────────
    // Cross-mod works by treating the master's raw output sample as an FM signal.
    // The sample is int16 (±32767).  We need to scale it the same way the FM
    // input path does: sample × modulation_factor, then run through fmExp2().
    //
    // xmodDepth is already in FM-mixer units (octaves / FM_OCTAVE_RANGE).
    // We pre-multiply by modulation_factor so the per-sample multiply is cheap.
    // The result feeds directly into fmExp2() alongside the normal FM input.
    const bool     xmodActive = (xmodDepth > 0.0001f);
    // Cross-mod gain as a fixed-point multiplier matching the FM path.
    // xmodDepth is 0..CROSS_MOD_FULL_SCALE (e.g. 0..0.2 for ±2 octaves).
    // Multiply by modFactor to bring into the same integer domain as FM input.
    const int32_t  xmodScale  = xmodActive ?
        (int32_t)(xmodDepth * (float)modFactor) : 0;

    // ── Main sample loop ────────────────────────────────────────────────────
    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {

        // ---- Master (OSC2): advance phase ----
        uint32_t masterPrior = masterPh;
        uint32_t mInc = masterInc;

        // Apply FM modulation to master if connected
        if (masterFM) {
            int32_t n = (int32_t)masterFM->data[i] * (int32_t)modFactor;
            uint32_t sc = fmExp2(n);
            uint64_t phstep = (uint64_t)masterInc * sc;
            uint32_t phstep_msw = phstep >> 32;
            mInc = (phstep_msw < 0x7FFE) ? (uint32_t)(phstep >> 16) : 0x7FFE0000u;
        }

        masterPh += mInc;

        // ---- Sync detection: did master wrap? ----
        // A wrap occurred if the new phase is LESS than the previous phase.
        // This is the sync trigger point — exactly at this sample.
        bool syncTrigger = syncOn && (masterPh < masterPrior);

        // ---- Generate master sample (needed for cross-mod before slave) ----
        int16_t masterSample = generateSample(
            masterWf, masterPh, masterMag,
            masterShape ? ((masterShape->data[i] + 0x8000) & 0xFFFF) << 16 : masterPW,
            masterArb, masterSH, masterPrior
        );

        // ---- Slave (OSC1): compute modulated phase increment ----
        uint32_t slavePrior = slavePh;
        uint32_t sInc = slaveInc;

        // Combine slave FM input and cross-mod into one FM value
        if (slaveFM || xmodActive) {
            int32_t fmTotal = 0;

            // Normal FM from the slave's frequency mod mixer
            if (slaveFM) {
                fmTotal = (int32_t)slaveFM->data[i] * (int32_t)modFactor;
            }

            // Cross-mod: master's audio sample modulates slave's pitch
            if (xmodActive) {
                // masterSample is int16 (±32767).  Scale by xmodScale to
                // bring into the same domain as FM input × modFactor.
                fmTotal += (int32_t)masterSample * xmodScale;
            }

            uint32_t sc = fmExp2(fmTotal);
            uint64_t phstep = (uint64_t)slaveInc * sc;
            uint32_t phstep_msw = phstep >> 32;
            sInc = (phstep_msw < 0x7FFE) ? (uint32_t)(phstep >> 16) : 0x7FFE0000u;
        }

        // ---- Apply sync reset BEFORE advancing slave phase ----
        // Resetting before the increment means the slave starts its new cycle
        // from phase 0 + sInc, which is the correct musical behaviour.
        if (syncTrigger) {
            slavePh = 0;
            slavePrior = 0;
        }

        slavePh += sInc;

        // ---- Generate slave sample ----
        int16_t slaveSample = generateSample(
            slaveWf, slavePh, slaveMag,
            slaveShape ? ((slaveShape->data[i] + 0x8000) & 0xFFFF) << 16 : slavePW,
            slaveArb, slaveSH, slavePrior
        );

        // ---- Write outputs ----
        slaveOut->data[i]  = slaveSample;
        masterOut->data[i] = masterSample;
    }

    // ── Store phase state back to members ────────────────────────────────────
    _slavePhase      = slavePh;
    _masterPhase     = masterPh;
    _slaveSampleHold = slaveSH;
    _masterSampleHold = masterSH;

    // ── Release modulation inputs ───────────────────────────────────────────
    if (slaveFM)     release(slaveFM);
    if (masterFM)    release(masterFM);
    if (slaveShape)  release(slaveShape);
    if (masterShape) release(masterShape);

    // ── Transmit output blocks ──────────────────────────────────────────────
    transmit(slaveOut, 0);   // Output 0 = OSC1 (slave)
    transmit(masterOut, 1);  // Output 1 = OSC2 (master)
    release(slaveOut);
    release(masterOut);
}

#endif // JT_OPT_OSC_SYNC
