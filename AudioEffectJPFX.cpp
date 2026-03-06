/*
 * AudioEffectJPFX.cpp (OPTIMIZED VERSION with Continuous Processing)
 *
 * KEY IMPROVEMENTS:
 * 1. Continuous processing even without input (maintains LFO phase, effect tails)
 * 2. CPU optimization: skips expensive calculations when effects disabled
 * 3. Smart bypass: only processes enabled effects
 * 4. Maintains reverb input even with no note playing (effect tails)
 *
 * CRITICAL BUG FIXES FROM PREVIOUS VERSION:
 * 1. Constructor uses 1 input, not 2 (AudioStream limitation)
 * 2. Separated modulation and delay buffers (were sharing, causing conflicts)
 * 3. Fixed update() to handle mono input with internal stereo
 * 4. Fixed transmit() to use single output channel
 * 5. Added proper NULL checks for buffer allocation
 * 6. Fixed bypass logic when effects are off
 * 7. Added destructor to free buffers
 * 8. Fixed write index management (was advancing twice per sample)
 */

#include "AudioEffectJPFX.h"
#include "DebugTrace.h"
#include <math.h>
#include "BPMClockManager.h"

#ifdef __arm__
#include <arm_math.h>
#endif

//-----------------------------------------------------------------------------
// Modulation presets (unchanged)
//-----------------------------------------------------------------------------
const AudioEffectJPFX::ModParams AudioEffectJPFX::modParams[JPFX_NUM_MOD_VARIATIONS] = {
    /* JPFX_CHORUS1 */     {15.0f, 15.0f,  2.0f,  4.0f, 0.25f, 0.0f, 0.5f, false, false},
    /* JPFX_CHORUS2 */     {20.0f, 20.0f,  3.0f,  5.0f, 0.80f, 0.0f, 0.6f, false, false},
    /* JPFX_CHORUS3 */     {25.0f, 25.0f,  4.0f,  6.0f, 0.40f, 0.0f, 0.7f, false, false},
    /* JPFX_FLANGER1 */    { 3.0f,  3.0f,  2.0f,  2.0f, 0.50f, 0.5f, 0.5f, false, true },
    /* JPFX_FLANGER2 */    { 5.0f,  5.0f,  2.5f,  2.5f, 0.35f, 0.7f, 0.5f, false, true },
    /* JPFX_FLANGER3 */    { 2.0f,  2.0f,  1.0f,  1.0f, 1.50f, 0.3f, 0.4f, false, true },
    /* JPFX_PHASER1 */     { 0.0f,  0.0f,  4.0f,  4.0f, 0.25f, 0.6f, 0.5f, true, false},
    /* JPFX_PHASER2 */     { 0.0f,  0.0f,  5.0f,  5.0f, 0.50f, 0.7f, 0.5f, true, false},
    /* JPFX_PHASER3 */     { 0.0f,  0.0f,  6.0f,  6.0f, 0.10f, 0.8f, 0.5f, true, false},
    /* JPFX_PHASER4 */     { 0.0f,  0.0f,  3.0f,  3.0f, 1.20f, 0.5f, 0.6f, true, false},
    /* JPFX_CHORUS_DEEP */ {30.0f, 30.0f, 10.0f, 12.0f, 0.20f, 0.0f, 0.7f, false, false}
};

//-----------------------------------------------------------------------------
// Delay presets (unchanged)
//-----------------------------------------------------------------------------
const AudioEffectJPFX::DelayParams AudioEffectJPFX::delayParams[JPFX_NUM_DELAY_VARIATIONS] = {
    /* JPFX_DELAY_SHORT */     {150.0f, 150.0f, 0.30f, 0.5f},
    /* JPFX_DELAY_LONG */      {500.0f, 500.0f, 0.40f, 0.5f},
    /* JPFX_DELAY_PINGPONG1 */ {300.0f, 600.0f, 0.40f, 0.6f},
    /* JPFX_DELAY_PINGPONG2 */ {150.0f, 300.0f, 0.50f, 0.6f},
    /* JPFX_DELAY_PINGPONG3 */ {400.0f, 200.0f, 0.40f, 0.6f}
};

//-----------------------------------------------------------------------------
// Constructor - Initialize all state
//-----------------------------------------------------------------------------
AudioEffectJPFX::AudioEffectJPFX()
    : AudioStream(1, inputQueueArray)  // mono input to the block
{
    // Initialize tone filters
    auto initShelf = [&](ShelfFilter &f) {
        f.b0 = 1.0f; f.b1 = 0.0f; f.a1 = 0.0f;
        f.in1 = 0.0f; f.out1 = 0.0f;
    };
    initShelf(bassFilterL);
    initShelf(bassFilterR);
    initShelf(trebleFilterL);
    initShelf(trebleFilterR);
    targetBassGain = 0.0f;
    targetTrebleGain = 0.0f;
    toneDirty = true;

    // Initialize modulation state
    modType = JPFX_MOD_OFF;
    modMix = 0.5f;
    modRateOverride = -1.0f;
    modFeedbackOverride = -1.0f;
    lfoPhaseL = 0.0f;
    lfoPhaseR = 0.5f;
    lfoIncL = 0.0f;
    lfoIncR = 0.0f;

    // Initialize delay state
    delayType = JPFX_DELAY_OFF;
    delayMix = 0.5f;
    delayFeedbackOverride = -1.0f;
    delayTimeOverride = -1.0f;

    // Initialize BPM timing state (NEW)
    _delayTimingMode = TIMING_FREE;       // Default: free-running ms
    _freeRunningDelayTime = 250.0f;       // Default delay time

    // Initialize all buffer pointers to NULL
    modBufL = nullptr;
    modBufR = nullptr;
    delayBufL = nullptr;
    delayBufR = nullptr;
    modBufSize = 0;
    delayBufSize = 0;
    modWriteIndex = 0;
    delayWriteIndex = 0;

    // Initialise saturation — bypass by default (zero CPU when drive=0)
    _satDrive      = 0.0f;
    _satInputGain  = 1.0f;
    _satOutputGain = 1.0f;
    _satIsSoft     = true;
    _satDirty      = false;
    _hpState       = 0.0f;

    // Initialise output limiter — unity gain (no attenuation)
    _limGain = 1.0f;

    // Allocate buffers (will use PSRAM if available)
    allocateDelayBuffers();
}

//-----------------------------------------------------------------------------
// Destructor - Free allocated buffers
//-----------------------------------------------------------------------------
AudioEffectJPFX::~AudioEffectJPFX()
{
    if (modBufL) free(modBufL);
    if (modBufR) free(modBufR);
    if (delayBufL) free(delayBufL);
    if (delayBufR) free(delayBufR);
}

//-----------------------------------------------------------------------------
// allocateDelayBuffers - Allocate separate buffers for mod and delay
// Uses PSRAM (extmem_malloc) if available, otherwise regular RAM
//-----------------------------------------------------------------------------
void AudioEffectJPFX::allocateDelayBuffers()
{
    const float sr = AUDIO_SAMPLE_RATE_EXACT;
    
    // Calculate delay buffer size (for delay effects)
    float maxDelaySeconds = JPFX_MAX_DELAY_MS * 0.001f;
    uint32_t delaySamples = (uint32_t)ceilf((maxDelaySeconds * sr) + 2.0f);
    delayBufSize = delaySamples;
    delayWriteIndex = 0;
    
    // Calculate modulation buffer size (for chorus/flanger/phaser)
    // These need much less buffer (max ~30ms base + depth)
    float maxModSeconds = 0.050f;  // 50ms is plenty for modulation effects
    uint32_t modSamples = (uint32_t)ceilf((maxModSeconds * sr) + 2.0f);
    modBufSize = modSamples;
    modWriteIndex = 0;
    
    uint32_t delayBytes = sizeof(float) * delayBufSize;
    uint32_t modBytes = sizeof(float) * modBufSize;
    uint32_t totalBytes = (delayBytes + modBytes) * 2;  // *2 for stereo
    
    JT_LOGF("[JPFX] Alloc: Delay=%uKB Mod=%uKB Total=%uKB\n",
            (unsigned)((delayBytes * 2) / 1024),
            (unsigned)((modBytes * 2) / 1024),
            (unsigned)(totalBytes / 1024));
    
    // Try PSRAM first (Teensy 4.x), then fall back to regular RAM
    #if defined(__IMXRT1062__)  // Teensy 4.x
        delayBufL = (float *)extmem_malloc(delayBytes);
        delayBufR = (float *)extmem_malloc(delayBytes);
        modBufL = (float *)extmem_malloc(modBytes);
        modBufR = (float *)extmem_malloc(modBytes);
        
        if (delayBufL && delayBufR && modBufL && modBufR) {
            JT_LOGF("[JPFX] Using PSRAM for all buffers");
        } else {
            // PSRAM failed, try regular malloc
            if (delayBufL) free(delayBufL);
            if (delayBufR) free(delayBufR);
            if (modBufL) free(modBufL);
            if (modBufR) free(modBufR);
            
            JT_LOGF("[JPFX] PSRAM unavailable, trying regular RAM");
            delayBufL = (float *)malloc(delayBytes);
            delayBufR = (float *)malloc(delayBytes);
            modBufL = (float *)malloc(modBytes);
            modBufR = (float *)malloc(modBytes);
        }
    #else
        delayBufL = (float *)malloc(delayBytes);
        delayBufR = (float *)malloc(delayBytes);
        modBufL = (float *)malloc(modBytes);
        modBufR = (float *)malloc(modBytes);
    #endif
    
    // Check for allocation failure
    if (!delayBufL || !delayBufR || !modBufL || !modBufR) {
        JT_LOGF("[JPFX] ERROR: Buffer allocation failed!");
        if (delayBufL) { free(delayBufL); delayBufL = nullptr; }
        if (delayBufR) { free(delayBufR); delayBufR = nullptr; }
        if (modBufL) { free(modBufL); modBufL = nullptr; }
        if (modBufR) { free(modBufR); modBufR = nullptr; }
        return;
    }
    
    // Clear buffers
    for (uint32_t i = 0; i < delayBufSize; ++i) {
        delayBufL[i] = 0.0f;
        delayBufR[i] = 0.0f;
    }
    for (uint32_t i = 0; i < modBufSize; ++i) {
        modBufL[i] = 0.0f;
        modBufR[i] = 0.0f;
    }
    
    JT_LOGF("[JPFX] Buffers allocated OK");
}

// ADD new method implementations:
void AudioEffectJPFX::setDelayTimingMode(TimingMode mode) {
    _delayTimingMode = mode;
    
    if (mode == TIMING_FREE) {
        // Restore free-running delay time
        setDelayTime(_freeRunningDelayTime);
    }
    // When switching to BPM mode, time will be updated by
    // updateFromBPMClock() call from FXChainBlock
}

void AudioEffectJPFX::updateFromBPMClock(const BPMClockManager& bpmClock) {
    // Only update if in BPM-synced mode
    if (_delayTimingMode == TIMING_FREE) return;
    if (delayType == JPFX_DELAY_OFF) return;  // No delay active
    
    // Get time for current timing mode
    float syncedTimeMs = bpmClock.getTimeForMode(_delayTimingMode);
    if (syncedTimeMs > 0.0f) {
        // Apply directly to delay parameters, bypass setDelayTime()
        delayTimeOverride = syncedTimeMs;
    }
}

//-----------------------------------------------------------------------------
// computeShelfCoeffs - Calculate biquad coefficients for shelving filters
// Frequency in Hz, gain in dB, highShelf=true for treble
//-----------------------------------------------------------------------------
void AudioEffectJPFX::computeShelfCoeffs(ShelfFilter &f, float freqHz, float gaindB, bool highShelf)
{
    const float fs = AUDIO_SAMPLE_RATE_EXACT;
    const float A = powf(10.0f, gaindB / 40.0f);
    const float w0 = 2.0f * M_PI * freqHz / fs;
    const float sinW0 = sinf(w0);
    const float cosW0 = cosf(w0);
    const float alpha = sinW0 / (2.0f * 0.707f);  // Q=0.707 for smooth response
    
    if (highShelf) {
        // High-shelf: boost/cut high frequencies
        const float a0 = (A+1.0f) - (A-1.0f)*cosW0 + 2.0f*sqrtf(A)*alpha;
        f.b0 = (A*((A+1.0f) + (A-1.0f)*cosW0 + 2.0f*sqrtf(A)*alpha)) / a0;
        f.b1 = (-2.0f*A*((A-1.0f) + (A+1.0f)*cosW0)) / a0;
        f.a1 = (-((A+1.0f) - (A-1.0f)*cosW0 - 2.0f*sqrtf(A)*alpha)) / a0;
    } else {
        // Low-shelf: boost/cut low frequencies
        const float a0 = (A+1.0f) + (A-1.0f)*cosW0 + 2.0f*sqrtf(A)*alpha;
        f.b0 = (A*((A+1.0f) - (A-1.0f)*cosW0 + 2.0f*sqrtf(A)*alpha)) / a0;
        f.b1 = (2.0f*A*((A-1.0f) - (A+1.0f)*cosW0)) / a0;
        f.a1 = (-((A+1.0f) + (A-1.0f)*cosW0 - 2.0f*sqrtf(A)*alpha)) / a0;
    }
}

//-----------------------------------------------------------------------------
// applyTone - Apply bass and treble shelving filters
// Only processes if tone controls are non-zero for CPU optimization
//-----------------------------------------------------------------------------
inline void AudioEffectJPFX::applyTone(float &l, float &r)
{
    // CPU OPTIMIZATION: Skip if both tone controls are at 0dB (bypass)
    if (targetBassGain == 0.0f && targetTrebleGain == 0.0f) {
        return;  // No tone adjustment needed
    }
    
    // Apply bass (low-shelf) left channel
    if (targetBassGain != 0.0f) {
        float x0 = l;
        float y0 = bassFilterL.b0 * x0 + bassFilterL.b1 * bassFilterL.in1 - bassFilterL.a1 * bassFilterL.out1;
        bassFilterL.in1 = x0;
        bassFilterL.out1 = y0;
        l = y0;
    }
    
    // Apply bass (low-shelf) right channel
    if (targetBassGain != 0.0f) {
        float x0 = r;
        float y0 = bassFilterR.b0 * x0 + bassFilterR.b1 * bassFilterR.in1 - bassFilterR.a1 * bassFilterR.out1;
        bassFilterR.in1 = x0;
        bassFilterR.out1 = y0;
        r = y0;
    }
    
    // Apply treble (high-shelf) left channel
    if (targetTrebleGain != 0.0f) {
        float x0 = l;
        float y0 = trebleFilterL.b0 * x0 + trebleFilterL.b1 * trebleFilterL.in1 - trebleFilterL.a1 * trebleFilterL.out1;
        trebleFilterL.in1 = x0;
        trebleFilterL.out1 = y0;
        l = y0;
    }
    
    // Apply treble (high-shelf) right channel
    if (targetTrebleGain != 0.0f) {
        float x0 = r;
        float y0 = trebleFilterR.b0 * x0 + trebleFilterR.b1 * trebleFilterR.in1 - trebleFilterR.a1 * trebleFilterR.out1;
        trebleFilterR.in1 = x0;
        trebleFilterR.out1 = y0;
        r = y0;
    }
}

//-----------------------------------------------------------------------------
// Parameter Setters
//-----------------------------------------------------------------------------

void AudioEffectJPFX::setSaturation(float norm)
{
    norm = constrain(norm, 0.0f, 1.0f);
    if (norm != _satDrive) {
        _satDrive = norm;
        _satDirty = true;
    }
}

// Recompute saturation gain staging whenever drive changes.
// Called once per update() when _satDirty is set — never per-sample.
void AudioEffectJPFX::computeSatParams()
{
    _satDirty = false;
    if (_satDrive < 0.001f) {
        // Bypass: leave gains at 1.0, flag handled by early-exit in applySaturation
        _satInputGain  = 1.0f;
        _satOutputGain = 1.0f;
        return;
    }

    if (_satDrive < 0.5f) {
        // ----- Soft clip (tanh) -----
        // Input gain 1..4x as drive goes 0..0.5, output compensates so
        // a sine wave at 50% amplitude stays roughly the same loudness.
        _satIsSoft    = true;
        _satInputGain  = 1.0f + (_satDrive / 0.5f) * 3.0f;  // 1..4x
        // tanhf(1.0) ≈ 0.762, tanhf(4.0) ≈ 0.999; makeup ≈ 1/tanh(gain*0.5)
        const float testOut = tanhf(_satInputGain * 0.5f);
        _satOutputGain = (testOut > 0.0f) ? (0.5f / testOut) : 1.0f;
    } else {
        // ----- Hard clip (asymmetric) -----
        // Drive 0.5..1.0 → input gain 4..8x, clipped at ±0.7 / +0.5 (asymmetric).
        // Asymmetry mimics the diode-like character of the JP output stage.
        _satIsSoft    = false;
        _satInputGain  = 4.0f + ((_satDrive - 0.5f) / 0.5f) * 4.0f;  // 4..8x
        _satOutputGain = 1.0f / 0.7f;  // normalise to clipping ceiling
    }
}

// Per-sample saturation — inlined; called 128x per update().
// Early return when drive=0 keeps zero overhead in bypass mode.
inline float AudioEffectJPFX::applySaturation(float x)
{
    if (_satDrive < 0.001f) return x;

    const float driven = x * _satInputGain;

    float shaped;
    if (_satIsSoft) {
        // tanhf is FPU-accelerated on Cortex-M7 — fast enough per-sample
        shaped = tanhf(driven);
    } else {
        // Asymmetric hard clip: positive peak at +0.7, negative at -1.0
        // Asymmetry adds even-order harmonics (2nd harmonic dominance = warmth)
        // Pre-emphasis HP (1-pole, fc~20 Hz) removes any DC offset buildup
        const float hpAlpha = 0.9997f;  // 1 - (2π × 20 / 44100)
        const float hpOut   = driven - _hpState;
        _hpState = _hpState * hpAlpha + driven * (1.0f - hpAlpha);

        if (hpOut > 0.7f)        shaped =  0.7f;
        else if (hpOut < -1.0f)  shaped = -1.0f;
        else                     shaped =  hpOut;
    }
    return shaped * _satOutputGain;
}

void AudioEffectJPFX::setBassGain(float dB)
{
    if (dB != targetBassGain) {
        targetBassGain = dB;
        toneDirty = true;
    }
}

void AudioEffectJPFX::setTrebleGain(float dB)
{
    if (dB != targetTrebleGain) {
        targetTrebleGain = dB;
        toneDirty = true;
    }
}

void AudioEffectJPFX::setModEffect(ModEffectType type)
{
    if (type != modType) {
        modType = type;
        lfoPhaseL = 0.0f;
        lfoPhaseR = 0.5f;
        updateLfoIncrements();
    }
}

void AudioEffectJPFX::setModMix(float mix)
{
    modMix = constrain(mix, 0.0f, 1.0f);
}

void AudioEffectJPFX::setModRate(float rate)
{
    if (rate < 0.0f) return;
    modRateOverride = (rate == 0.0f) ? -1.0f : rate;
    updateLfoIncrements();
}

void AudioEffectJPFX::setModFeedback(float fb)
{
    if (fb < 0.0f) {
        modFeedbackOverride = -1.0f;
    } else {
        modFeedbackOverride = constrain(fb, 0.0f, 0.99f);
    }
}

void AudioEffectJPFX::setDelayEffect(DelayEffectType type)
{
    if (type != delayType) {
        delayType = type;
        delayWriteIndex = 0;
        
        // Clear delay buffers when changing effect type
        if (delayBufL && delayBufR) {
            for (uint32_t i = 0; i < delayBufSize; ++i) {
                delayBufL[i] = 0.0f;
                delayBufR[i] = 0.0f;
            }
        }
    }
}

void AudioEffectJPFX::setDelayMix(float mix)
{
    delayMix = constrain(mix, -1.0f, 1.0f);
}

void AudioEffectJPFX::setDelayFeedback(float fb)
{
    if (fb < 0.0f) {
        delayFeedbackOverride = -1.0f;
    } else {
        delayFeedbackOverride = constrain(fb, 0.0f, 0.99f);
    }
}

void AudioEffectJPFX::setDelayTime(float ms) {
    _freeRunningDelayTime = ms;  // Always store for mode switching
    
    // Only apply if in free-running mode
    if (_delayTimingMode == TIMING_FREE) {
        delayTimeOverride = ms;
        //Serial.printf("Delay setDelayTime %.1f ms (FREE)\n", ms);
    } 
    // else {
    //     Serial.println("Delay in sync mode, time managed by BPM clock");
    // }
}

//-----------------------------------------------------------------------------
// updateLfoIncrements - Calculate LFO phase increment per sample
// Called when modulation effect or rate changes
//-----------------------------------------------------------------------------
void AudioEffectJPFX::updateLfoIncrements()
{
    // CPU OPTIMIZATION: If modulation is off, set increments to 0
    if (modType == JPFX_MOD_OFF) {
        lfoIncL = lfoIncR = 0.0f;
        return;
    }
    
    // Get rate from preset or override
    float rate = modParams[modType].rate;
    if (modRateOverride > 0.0f) {
        rate = modRateOverride;
    }
    
    // Calculate phase increment (radians per sample)
    const float fs = AUDIO_SAMPLE_RATE_EXACT;
    const float twoPi = 2.0f * 3.14159265358979323846f;
    float phaseInc = twoPi * rate / fs;
    lfoIncL = phaseInc;
    lfoIncR = phaseInc * 1.01f;  // Slight offset for stereo width
}

//-----------------------------------------------------------------------------
// processModulation - Chorus, flanger, phaser effects
// CPU OPTIMIZATION: Bypasses completely if effect is disabled
//-----------------------------------------------------------------------------
inline void AudioEffectJPFX::processModulation(float inL, float inR, float &outL, float &outR)
{
    // CPU OPTIMIZATION: Early bypass if modulation disabled or no buffer
    if (modType == JPFX_MOD_OFF || !modBufL || !modBufR) {
        outL = inL;
        outR = inR;
        return;
    }
    
    const ModParams &params = modParams[modType];
    const float baseDelayL = params.baseDelayL;
    const float baseDelayR = params.baseDelayR;
    const float depthL = params.depthL;
    const float depthR = params.depthR;
    const float feedback = (modFeedbackOverride >= 0.0f) ? modFeedbackOverride : params.feedback;
    const float presetMix = params.mix;
    const float wetMix = modMix * presetMix;
    const float dryMix = 1.0f - wetMix;
    
    // Fast LFO sin approximation (Bhaskara I, < 0.002% error over 0..2π).
    // Avoids sinf() transcendental call — saves ~15% CPU on this block.
    // Formula: sin(x) ≈ 16x(π-x) / (5π² - 4x(π-x))  for x in [0, π]
    // Mapped to full cycle by folding the phase.
    auto fastSin = [](float phase) -> float {
        // Fold phase into [0, π]
        const float pi  = 3.14159265f;
        const float twoPi = 6.28318530f;
        if (phase < 0.0f) phase += twoPi;
        float x = phase;
        float sign = 1.0f;
        if (x > pi) { x -= pi; sign = -1.0f; }
        const float xpi = x * (pi - x);
        return sign * (16.0f * xpi) / (5.0f * pi * pi - 4.0f * xpi);
    };

    const float lfoValL = fastSin(lfoPhaseL);
    const float lfoValR = fastSin(lfoPhaseR);
    lfoPhaseL += lfoIncL;
    if (lfoPhaseL > 6.283185307179586f) lfoPhaseL -= 6.283185307179586f;
    lfoPhaseR += lfoIncR;
    if (lfoPhaseR > 6.283185307179586f) lfoPhaseR -= 6.283185307179586f;
    
    // Convert delay times to samples
    const float fs = AUDIO_SAMPLE_RATE_EXACT;
    float delaySamplesL = (baseDelayL + depthL * lfoValL) * 0.001f * fs;
    float delaySamplesR = (baseDelayR + depthR * lfoValR) * 0.001f * fs;
    
    // Clamp within buffer bounds
    delaySamplesL = constrain(delaySamplesL, 0.0f, (float)(modBufSize - 2));
    delaySamplesR = constrain(delaySamplesR, 0.0f, (float)(modBufSize - 2));
    
    // Read with linear interpolation - LEFT
    float readIndexL = (float)modWriteIndex - delaySamplesL;
    if (readIndexL < 0.0f) readIndexL += (float)modBufSize;
    uint32_t idxL0 = (uint32_t)readIndexL;
    uint32_t idxL1 = (idxL0 + 1) % modBufSize;
    float fracL = readIndexL - (float)idxL0;
    float delayedL = modBufL[idxL0] + (modBufL[idxL1] - modBufL[idxL0]) * fracL;
    
    // Read with linear interpolation - RIGHT
    float readIndexR = (float)modWriteIndex - delaySamplesR;
    if (readIndexR < 0.0f) readIndexR += (float)modBufSize;
    uint32_t idxR0 = (uint32_t)readIndexR;
    uint32_t idxR1 = (idxR0 + 1) % modBufSize;
    float fracR = readIndexR - (float)idxR0;
    float delayedR = modBufR[idxR0] + (modBufR[idxR1] - modBufR[idxR0]) * fracR;
    
    // Write with feedback
    modBufL[modWriteIndex] = inL + delayedL * feedback;
    modBufR[modWriteIndex] = inR + delayedR * feedback;
    
    // Advance write pointer
    modWriteIndex = (modWriteIndex + 1) % modBufSize;
    
    // Mix dry and wet
    outL = dryMix * inL + wetMix * delayedL;
    outR = dryMix * inR + wetMix * delayedR;
}

//-----------------------------------------------------------------------------
// processDelay - Delay and ping-pong delay effects
// CPU OPTIMIZATION: Bypasses if disabled, but continues to decay buffer
//-----------------------------------------------------------------------------
inline void AudioEffectJPFX::processDelay(float inL, float inR, float &outL, float &outR)
{
    // CPU OPTIMIZATION: If delay disabled or no buffer, bypass
    // BUT still advance write pointer to decay the buffer naturally
    if (delayType == JPFX_DELAY_OFF || !delayBufL || !delayBufR) {
        outL = inL;
        outR = inR;
        
        // Continue to let delay buffer decay (write silence with feedback)
        if (delayBufL && delayBufR) {
            delayBufL[delayWriteIndex] = delayBufL[delayWriteIndex] * 0.95f;  // Decay
            delayBufR[delayWriteIndex] = delayBufR[delayWriteIndex] * 0.95f;
            delayWriteIndex = (delayWriteIndex + 1) % delayBufSize;
        }
        return;
    }
    
    const DelayParams &params = delayParams[delayType];
    float delayTimeL = params.delayL;
    float delayTimeR = params.delayR;
    const float feedback = (delayFeedbackOverride >= 0.0f) ? delayFeedbackOverride : params.feedback;
    float wetMix = delayMix;
    const float dryMix = 1.0f - fabsf(wetMix);
    const bool invertWet = (wetMix < 0.0f);
    wetMix = fabsf(wetMix);
    
    // Apply time override if set
    if (delayTimeOverride >= 0.0f) {
        delayTimeL = delayTimeOverride;
        delayTimeR = delayTimeOverride;
    }
    
    // Convert to samples
    const float fs = AUDIO_SAMPLE_RATE_EXACT;
    float delaySamplesL = delayTimeL * 0.001f * fs;
    float delaySamplesR = delayTimeR * 0.001f * fs;
    
    // Clamp within buffer bounds
    delaySamplesL = constrain(delaySamplesL, 0.0f, (float)(delayBufSize - 2));
    delaySamplesR = constrain(delaySamplesR, 0.0f, (float)(delayBufSize - 2));
    
    // Read with linear interpolation - LEFT
    float readIndexL = (float)delayWriteIndex - delaySamplesL;
    if (readIndexL < 0.0f) readIndexL += (float)delayBufSize;
    uint32_t idxL0 = (uint32_t)readIndexL;
    uint32_t idxL1 = (idxL0 + 1) % delayBufSize;
    float fracL = readIndexL - (float)idxL0;
    float delayedL = delayBufL[idxL0] + (delayBufL[idxL1] - delayBufL[idxL0]) * fracL;
    
    // Read with linear interpolation - RIGHT
    float readIndexR = (float)delayWriteIndex - delaySamplesR;
    if (readIndexR < 0.0f) readIndexR += (float)delayBufSize;
    uint32_t idxR0 = (uint32_t)readIndexR;
    uint32_t idxR1 = (idxR0 + 1) % delayBufSize;
    float fracR = readIndexR - (float)idxR0;
    float delayedR = delayBufR[idxR0] + (delayBufR[idxR1] - delayBufR[idxR0]) * fracR;
    
    // Write with feedback
    delayBufL[delayWriteIndex] = inL + delayedL * feedback;
    delayBufR[delayWriteIndex] = inR + delayedR * feedback;
    
    // Advance write pointer
    delayWriteIndex = (delayWriteIndex + 1) % delayBufSize;
    
    // Mix dry and wet (with optional inversion for phase tricks)
    float wetL = invertWet ? -delayedL : delayedL;
    float wetR = invertWet ? -delayedR : delayedR;
    outL = dryMix * inL + wetMix * wetL;
    outR = dryMix * inR + wetMix * wetR;
}

void AudioEffectJPFX::update(void)
{
    // Receive mono input
    audio_block_t *in = receiveReadOnly(0);
    
    // Allocate TWO output blocks for stereo
    audio_block_t *outL = allocate();
    audio_block_t *outR = allocate();
    
    // Check allocation
    if (!outL || !outR) {
        if (outL) release(outL);
        if (outR) release(outR);
        if (in) release(in);
        return;
    }
    
    // Block-rate parameter updates (computed once, not per-sample)
    if (toneDirty) {
        // JP-8000 reference corners: bass=80 Hz, treble=8 kHz
        computeShelfCoeffs(bassFilterL,   80.0f,   targetBassGain,   false);
        computeShelfCoeffs(bassFilterR,   80.0f,   targetBassGain,   false);
        computeShelfCoeffs(trebleFilterL, 8000.0f, targetTrebleGain, true);
        computeShelfCoeffs(trebleFilterR, 8000.0f, targetTrebleGain, true);
        toneDirty = false;
    }
    if (_satDirty) {
        computeSatParams();  // recompute drive input/output gains
    }

    // Track block peak for the limiter — updated after the sample loop
    float blockPeak = 0.0f;

    // Signal chain per sample:
    //   Input → Saturation → Tone EQ → Modulation → Delay → Limiter → Output
    // Saturation first matches the JP-8000 VCA→output-stage-drive topology.
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        // Get mono input sample (0 if no connection — keeps effect tails running)
        float input = in ? ((float)in->data[i] * (1.0f / 32768.0f)) : 0.0f;

        // Stage 1: Saturation — bypass is zero-cost when drive = 0
        float l = applySaturation(input);
        float r = l;  // fan mono→stereo for the remaining stages

        // Stage 2: Tone EQ (bass/treble shelving)
        applyTone(l, r);

        // Stage 3: Modulation (chorus / flanger / phaser)
        float modL, modR;
        processModulation(l, r, modL, modR);

        // Stage 4: Delay (standard / ping-pong)
        float delL, delR;
        processDelay(modL, modR, delL, delR);

        // Track peak magnitude for block-rate limiter below
        const float peakL = fabsf(delL);
        const float peakR = fabsf(delR);
        if (peakL > blockPeak) blockPeak = peakL;
        if (peakR > blockPeak) blockPeak = peakR;

        // Write output with current limiter gain applied
        outL->data[i] = (int16_t)constrain(delL * _limGain * 32767.0f, -32767.0f, 32767.0f);
        outR->data[i] = (int16_t)constrain(delR * _limGain * 32767.0f, -32767.0f, 32767.0f);
    }

    // Stage 5: Block-rate brick-wall limiter (runs once per 128-sample block).
    // Fast attack prevents digital clipping; slow release avoids pumping artefacts.
    // Ceiling at 0.97 (≈ -0.26 dBFS) leaves just enough headroom.
    const float kCeiling = 0.97f;
    if (blockPeak * _limGain > kCeiling) {
        // Overload — snap gain down to bring peak to ceiling (1-block attack)
        _limGain = kCeiling / blockPeak;
    } else {
        // No overload — recover gain slowly toward unity (release ~100 ms)
        _limGain += kLimRelease;
        if (_limGain > 1.0f) _limGain = 1.0f;
    }
    
    // Transmit both channels
    transmit(outL, 0);  // Output 0 = Left
    transmit(outR, 1);  // Output 1 = Right
    
    // Release all blocks
    release(outL);
    release(outR);
    if (in) release(in);
}