/*
 * AudioEffectJPFX.cpp — JP-8000 Effects Engine Implementation
 *
 * See AudioEffectJPFX.h for full change log and architecture notes.
 *
 * SIGNAL CHAIN (per sample):
 *   Mono input → Saturation → Tone EQ → Modulation → Delay → Limiter → Stereo out
 *
 * PERFORMANCE SUMMARY:
 *   - Tone EQ:      ~6 multiplies per sample when active, zero when bypass
 *   - Saturation:    1 tanhf or branch per sample, zero when bypass
 *   - Modulation:    2 interpolated reads (standard) or 6 reads (Super Chorus)
 *   - Delay:         2 interpolated reads per sample
 *   - Limiter:       runs once per 128-sample block (negligible)
 *   - All parameter recomputation is block-rate or on-change, never per-sample
 */

#include "AudioEffectJPFX.h"
#include "DebugTrace.h"
#include <math.h>
#include <string.h>

#ifdef __arm__
#include <arm_math.h>
#endif

// ---------------------------------------------------------------------------
// Compile-time constants used throughout the DSP
// ---------------------------------------------------------------------------
static constexpr float kPi    = 3.14159265f;
static constexpr float kTwoPi = 6.28318530f;


// ===========================================================================
// Modulation presets — JP-8000 chorus / flanger / phaser variations
//
// FORMAT: { baseL, baseR, depthL, depthR, rate, fb, mix, taps, triLfo }
//
// CHORUS base delays: 5-12 ms (proper chorus range — previous 15-25 ms
//   values were in doubling/slapback territory and sounded thin).
//
// Mix field set to 1.0 for all presets.  The user's Level knob (modMix)
//   is the sole wet/dry control, matching the JP-8000's single Level pot.
//   Previous double-multiply (modMix * presetMix) over-attenuated the wet
//   signal, making chorus sound weak.
//
// Chorus presets use triangle LFO (linear delay sweep, BBD characteristic).
// Flanger/phaser presets use sine LFO (smoother at short delays).
//
// PHASER NOTE: These use delay-line modulation, not cascaded all-pass
//   stages.  The effect approximates a phaser sweep but lacks the true
//   notch/peak comb structure.  See header for documentation.
// ===========================================================================
const AudioEffectJPFX::ModParams AudioEffectJPFX::modPresets[JPFX_NUM_MOD_VARIATIONS] = {
    //                  baseL  baseR  dpthL dpthR  rate    fb   mix  taps tri
    /* CHORUS1       */ { 5.0f,  5.0f, 1.5f, 2.0f, 0.80f, 0.00f, 1.0f, 1, true  },
    /* CHORUS2       */ { 8.0f,  8.0f, 2.5f, 3.0f, 0.50f, 0.00f, 1.0f, 1, true  },
    /* CHORUS3       */ {12.0f, 12.0f, 3.5f, 4.5f, 0.30f, 0.00f, 1.0f, 1, true  },
    /* FLANGER1      */ { 1.5f,  1.5f, 1.5f, 1.5f, 0.30f, 0.60f, 1.0f, 1, false },
    /* FLANGER2      */ { 2.0f,  2.0f, 2.0f, 2.0f, 0.15f, 0.75f, 1.0f, 1, false },
    /* DEEP_FLANGER  */ { 3.0f,  3.0f, 3.0f, 3.0f, 0.08f, 0.85f, 1.0f, 1, false },
    /* PHASER1       */ { 1.0f,  1.0f, 4.0f, 4.0f, 0.25f, 0.60f, 1.0f, 1, false },
    /* PHASER2       */ { 1.0f,  1.0f, 5.0f, 5.0f, 0.50f, 0.70f, 1.0f, 1, false },
    /* PHASER3       */ { 1.0f,  1.0f, 6.0f, 6.0f, 0.10f, 0.80f, 1.0f, 1, false },
    /* PHASER4       */ { 1.0f,  1.0f, 3.0f, 3.0f, 1.20f, 0.50f, 1.0f, 1, false },
    /* SUPER_CHORUS  */ { 8.0f,  8.0f, 3.0f, 4.0f, 0.50f, 0.00f, 1.0f, 3, true  },
};

// ===========================================================================
// Delay presets — JP-8000 delay types
//
// FORMAT: { delayL, delayR, feedback, mix }
//
// JP-8000 manual (confirmed by SoS review):
//   "three panning delays plus short and long mono delays"
//   "delay time maximum 1250 ms"
//
// Mono presets use identical L/R times.
// Panning presets use offset L/R times for stereo imaging.
// ===========================================================================
const AudioEffectJPFX::DelayParams AudioEffectJPFX::delayPresets[JPFX_NUM_DELAY_VARIATIONS] = {
    //                      delayL   delayR    fb    mix
    /* MONO_SHORT  */     {  80.0f,   80.0f, 0.25f, 0.40f },
    /* MONO_LONG   */     { 500.0f,  500.0f, 0.35f, 0.40f },
    /* PAN_LR      */     { 120.0f,  250.0f, 0.35f, 0.50f },
    /* PAN_RL      */     { 250.0f,  120.0f, 0.35f, 0.50f },
    /* PAN_STEREO  */     { 200.0f,  400.0f, 0.40f, 0.50f },
};


// ===========================================================================
// Constructor
// ===========================================================================
AudioEffectJPFX::AudioEffectJPFX()
    : AudioStream(1, inputQueueArray)
{
    // --- Tone control ---
    // Compute crossover LP coefficients once (fixed, depends only on sample rate)
    const float fs = AUDIO_SAMPLE_RATE_EXACT;
    _toneXoverBassAlpha = kTwoPi * 200.0f  / (kTwoPi * 200.0f  + fs);
    _toneXoverTrebAlpha = kTwoPi * 3000.0f / (kTwoPi * 3000.0f + fs);

    // Zero all filter states
    _toneBassLpStateL = 0.0f;
    _toneBassLpStateR = 0.0f;
    _toneTrebLpStateL = 0.0f;
    _toneTrebLpStateR = 0.0f;
    _toneBassGainDelta = 0.0f;
    _toneTrebGainDelta = 0.0f;
    _targetBassGainDb   = 0.0f;
    _targetTrebleGainDb = 0.0f;
    _toneActive = false;
    _toneDirty  = false;

    // --- Modulation ---
    _modType             = JPFX_MOD_OFF;
    _modMix              = 0.5f;
    _modRateOverride     = -1.0f;
    _modFeedbackOverride = -1.0f;
    _lfoPhaseL = 0.0f;
    _lfoPhaseR = 0.5f;      // Stereo offset for spatial width
    _lfoIncL   = 0.0f;
    _lfoIncR   = 0.0f;

    // --- Delay ---
    _delayType             = JPFX_DELAY_OFF;
    _pendingDelayType      = JPFX_DELAY_OFF;    // No pending change
    _delayMix              = 0.5f;
    _delayFeedbackOverride = -1.0f;
    _delayTimeOverrideL    = -1.0f;     // -1 = use preset L time
    _delayTimeOverrideR    = -1.0f;     // -1 = use preset R time
    _delayTimingMode       = TIMING_FREE;
    _freeRunningDelayTime  = 250.0f;

    // --- Delay cache ---
    _delaySampLCached  = 0.0f;
    _delaySampRCached  = 0.0f;
    _delayFbCached     = 0.0f;
    _delayWetCached    = 0.0f;
    _delayDryCached    = 1.0f;
    _delayInvertCached = false;

    // --- Buffers ---
    _modBufL   = nullptr;
    _modBufR   = nullptr;
    _delayBufL = nullptr;
    _delayBufR = nullptr;
    _modBufSize   = 0;
    _delayBufSize = 0;
    _modWriteIdx  = 0;
    _delayWriteIdx = 0;

    // --- Saturation (bypass by default — zero CPU when drive=0) ---
    _satDrive      = 0.0f;
    _satInputGain  = 1.0f;
    _satOutputGain = 1.0f;
    _satIsSoft     = true;
    _satDirty      = false;
    _hpState       = 0.0f;

    // --- Output limiter (unity gain = no attenuation) ---
    _limGain = 1.0f;

    // Allocate circular buffers (PSRAM if available, then RAM)
    allocateDelayBuffers();
}


// ===========================================================================
// Destructor
// ===========================================================================
AudioEffectJPFX::~AudioEffectJPFX()
{
    if (_modBufL)   free(_modBufL);
    if (_modBufR)   free(_modBufR);
    if (_delayBufL) free(_delayBufL);
    if (_delayBufR) free(_delayBufR);
}


// ===========================================================================
// allocateDelayBuffers — separate buffers for modulation and delay
//
// Uses PSRAM (extmem_malloc) on Teensy 4.x if available, otherwise RAM.
// Buffer sizes are determined by the maximum delay/modulation times.
// ===========================================================================
void AudioEffectJPFX::allocateDelayBuffers()
{
    const float fs = AUDIO_SAMPLE_RATE_EXACT;

    // Delay buffer: sized for maximum delay time plus 2-sample interpolation guard
    _delayBufSize = (uint32_t)ceilf(JPFX_MAX_DELAY_MS * 0.001f * fs) + 2;
    _delayWriteIdx = 0;

    // Modulation buffer: sized for maximum modulation time
    _modBufSize = (uint32_t)ceilf(JPFX_MAX_MOD_MS * 0.001f * fs) + 2;
    _modWriteIdx = 0;

    const uint32_t delayBytes = sizeof(float) * _delayBufSize;
    const uint32_t modBytes   = sizeof(float) * _modBufSize;
    const uint32_t totalBytes = (delayBytes + modBytes) * 2;  // ×2 for stereo

    JT_LOGF("[JPFX] Alloc: Delay=%uKB Mod=%uKB Total=%uKB\n",
            (unsigned)((delayBytes * 2) / 1024),
            (unsigned)((modBytes * 2) / 1024),
            (unsigned)(totalBytes / 1024));

    // Try PSRAM first (Teensy 4.x), fall back to regular RAM
    #if defined(__IMXRT1062__)
        _delayBufL = (float *)extmem_malloc(delayBytes);
        _delayBufR = (float *)extmem_malloc(delayBytes);
        _modBufL   = (float *)extmem_malloc(modBytes);
        _modBufR   = (float *)extmem_malloc(modBytes);

        if (_delayBufL && _delayBufR && _modBufL && _modBufR) {
            JT_LOGF("[JPFX] Using PSRAM for all buffers");
        } else {
            // PSRAM failed — free any partial allocations and try regular RAM
            if (_delayBufL) free(_delayBufL);
            if (_delayBufR) free(_delayBufR);
            if (_modBufL)   free(_modBufL);
            if (_modBufR)   free(_modBufR);

            JT_LOGF("[JPFX] PSRAM unavailable, trying regular RAM");
            _delayBufL = (float *)malloc(delayBytes);
            _delayBufR = (float *)malloc(delayBytes);
            _modBufL   = (float *)malloc(modBytes);
            _modBufR   = (float *)malloc(modBytes);
        }
    #else
        _delayBufL = (float *)malloc(delayBytes);
        _delayBufR = (float *)malloc(delayBytes);
        _modBufL   = (float *)malloc(modBytes);
        _modBufR   = (float *)malloc(modBytes);
    #endif

    // Check for allocation failure
    if (!_delayBufL || !_delayBufR || !_modBufL || !_modBufR) {
        JT_LOGF("[JPFX] ERROR: Buffer allocation failed!");
        if (_delayBufL) { free(_delayBufL); _delayBufL = nullptr; }
        if (_delayBufR) { free(_delayBufR); _delayBufR = nullptr; }
        if (_modBufL)   { free(_modBufL);   _modBufL   = nullptr; }
        if (_modBufR)   { free(_modBufR);   _modBufR   = nullptr; }
        return;
    }

    // Clear buffers — memset to 0 is valid for IEEE 754 floats (+0.0f = all zero bits)
    // and is DMA-accelerated on Cortex-M7
    memset(_delayBufL, 0, delayBytes);
    memset(_delayBufR, 0, delayBytes);
    memset(_modBufL,   0, modBytes);
    memset(_modBufR,   0, modBytes);

    JT_LOGF("[JPFX] Buffers allocated OK");
}


// ===========================================================================
// BPM timing
// ===========================================================================

void AudioEffectJPFX::setDelayTimingMode(TimingMode mode)
{
    _delayTimingMode = mode;

    if (mode == TIMING_FREE) {
        // Restore free-running delay time
        setDelayTime(_freeRunningDelayTime);
    }
    // In BPM mode, time is updated by updateFromBPMClock() from FXChainBlock
}

// ===========================================================================
// updateFromBPMClock — BPM-synced delay time
//
// Same ratio-preserving approach as setDelayTime().  The synced time
// from the clock manager becomes the LEFT channel time; RIGHT is scaled
// by the preset's L/R ratio.
// ===========================================================================
void AudioEffectJPFX::updateFromBPMClock(const BPMClockManager& bpmClock)
{
    // Only update delay time when synced to BPM
    if (_delayTimingMode == TIMING_FREE) return;
    if (_delayType == JPFX_DELAY_OFF)    return;

    float syncedTimeMs = bpmClock.getTimeForMode(_delayTimingMode);
    if (syncedTimeMs <= 0.0f) return;

    // Compute L/R ratio from the active preset
    float ratio = 1.0f;
    const DelayParams &p = delayPresets[_delayType];
    if (p.delayMsL > 0.0f) {
        ratio = p.delayMsR / p.delayMsL;
    }

    _delayTimeOverrideL = syncedTimeMs;
    _delayTimeOverrideR = syncedTimeMs * ratio;
}


// ===========================================================================
// Tone control — crossover EQ
// ===========================================================================

// Recompute linear gains from dB values.  Called once per block when dirty.
void AudioEffectJPFX::computeToneParams()
{
    _toneDirty = false;

    // Convert dB to linear gain
    float bassLin  = powf(10.0f, _targetBassGainDb   / 20.0f);
    float trebLin  = powf(10.0f, _targetTrebleGainDb  / 20.0f);

    // Snap to unity when within ±0.1 dB — prevents residual filtering
    // from CC midpoint values that map to ~0.094 dB instead of exactly 0.0
    if (fabsf(_targetBassGainDb)   < 0.1f) bassLin = 1.0f;
    if (fabsf(_targetTrebleGainDb) < 0.1f) trebLin = 1.0f;

    // Store deltas: when delta is zero, the multiply-add is a no-op
    _toneBassGainDelta = bassLin - 1.0f;
    _toneTrebGainDelta = trebLin - 1.0f;

    // Active flag: bypass entire tone stage when both bands are at unity
    _toneActive = (fabsf(_toneBassGainDelta) > 0.0001f ||
                   fabsf(_toneTrebGainDelta) > 0.0001f);
}

// Per-sample tone EQ.  Splits the signal into bass and treble bands via
// one-pole LPs, applies gain delta to each, and sums back.  When both
// deltas are zero, the additions are exactly zero → transparent.
inline void AudioEffectJPFX::applyTone(float &sampleL, float &sampleR)
{
    if (!_toneActive) return;

    // Extract bass component via one-pole LP at ~200 Hz
    _toneBassLpStateL += _toneXoverBassAlpha * (sampleL - _toneBassLpStateL);
    _toneBassLpStateR += _toneXoverBassAlpha * (sampleR - _toneBassLpStateR);

    // Extract treble component = input minus one-pole LP at ~3 kHz
    _toneTrebLpStateL += _toneXoverTrebAlpha * (sampleL - _toneTrebLpStateL);
    _toneTrebLpStateR += _toneXoverTrebAlpha * (sampleR - _toneTrebLpStateR);
    const float trebleL = sampleL - _toneTrebLpStateL;
    const float trebleR = sampleR - _toneTrebLpStateR;

    // Apply: output = input + (bassGain-1)*bass + (trebleGain-1)*treble
    // At unity gain both delta terms are zero → output = input exactly.
    sampleL += _toneBassGainDelta * _toneBassLpStateL
             + _toneTrebGainDelta * trebleL;
    sampleR += _toneBassGainDelta * _toneBassLpStateR
             + _toneTrebGainDelta * trebleR;
}

void AudioEffectJPFX::setBassGain(float dB)
{
    dB = constrain(dB, -12.0f, 12.0f);
    if (dB != _targetBassGainDb) {
        _targetBassGainDb = dB;
        _toneDirty = true;
    }
}

void AudioEffectJPFX::setTrebleGain(float dB)
{
    dB = constrain(dB, -12.0f, 12.0f);
    if (dB != _targetTrebleGainDb) {
        _targetTrebleGainDb = dB;
        _toneDirty = true;
    }
}


// ===========================================================================
// Saturation / drive — unchanged from previous version
// ===========================================================================

void AudioEffectJPFX::setSaturation(float norm)
{
    norm = constrain(norm, 0.0f, 1.0f);
    if (norm != _satDrive) {
        _satDrive = norm;
        _satDirty = true;
    }
}

// Recompute saturation gain staging.  Called once per block when dirty.
void AudioEffectJPFX::computeSatParams()
{
    _satDirty = false;

    // Below threshold: bypass (gains stay at 1.0, applySaturation early-exits)
    if (_satDrive < 0.001f) {
        _satInputGain  = 1.0f;
        _satOutputGain = 1.0f;
        return;
    }

    if (_satDrive < 0.5f) {
        // Soft clip (tanh) — warm 2nd/3rd harmonics
        // Input gain: 1..4× as drive goes 0..0.5
        _satIsSoft     = true;
        _satInputGain  = 1.0f + (_satDrive / 0.5f) * 3.0f;
        // Makeup gain: compensate for tanh compression
        const float testOut = tanhf(_satInputGain * 0.5f);
        _satOutputGain = (testOut > 0.0f) ? (0.5f / testOut) : 1.0f;
    } else {
        // Hard clip (asymmetric) — even-order harmonics, JP output stage
        // Input gain: 4..8× as drive goes 0.5..1.0
        _satIsSoft     = false;
        _satInputGain  = 4.0f + ((_satDrive - 0.5f) / 0.5f) * 4.0f;
        _satOutputGain = 1.0f / 0.7f;   // normalise to positive clipping ceiling
    }
}

// Per-sample saturation — inlined.  Early return when drive=0 for zero bypass cost.
inline float AudioEffectJPFX::applySaturation(float sample)
{
    if (_satDrive < 0.001f) return sample;

    const float driven = sample * _satInputGain;
    float shaped;

    if (_satIsSoft) {
        // tanhf is FPU-accelerated on Cortex-M7
        shaped = tanhf(driven);
    } else {
        // Asymmetric hard clip: +0.7 positive, -1.0 negative
        // Pre-emphasis HP removes DC offset from asymmetry
        const float hpAlpha = 0.9997f;     // 1 - (2π × 20 / 44100)
        const float hpOut   = driven - _hpState;
        _hpState = _hpState * hpAlpha + driven * (1.0f - hpAlpha);

        if      (hpOut >  0.7f) shaped =  0.7f;
        else if (hpOut < -1.0f) shaped = -1.0f;
        else                    shaped =  hpOut;
    }
    return shaped * _satOutputGain;
}


// ===========================================================================
// Modulation parameter setters
// ===========================================================================

void AudioEffectJPFX::setModEffect(ModEffectType type)
{
    if (type != _modType) {
        _modType = type;
        _lfoPhaseL = 0.0f;
        _lfoPhaseR = 0.5f;
        updateLfoIncrements();
    }
}

void AudioEffectJPFX::setModMix(float mix)
{
    _modMix = constrain(mix, 0.0f, 1.0f);
}

void AudioEffectJPFX::setModRate(float rateHz)
{
    if (rateHz < 0.0f) return;
    _modRateOverride = (rateHz == 0.0f) ? -1.0f : rateHz;
    updateLfoIncrements();
}

void AudioEffectJPFX::setModFeedback(float fb)
{
    _modFeedbackOverride = (fb < 0.0f) ? -1.0f : constrain(fb, 0.0f, 0.99f);
}


// ===========================================================================
// Delay parameter setters
// ===========================================================================

// ---------------------------------------------------------------------------
// setDelayEffect — ISR-safe delay preset switching
//
// Writes the requested type to _pendingDelayType.  The actual type change,
// buffer clear, and write index reset happen inside update() at the start
// of the next audio block.  This guarantees the ISR never sees a half-
// cleared buffer or a write index that was reset mid-block.
//
// On Cortex-M7 a single aligned word write is atomic, so no mutex or
// interrupt disable is needed — the ISR will see either the old or the
// new value, never a torn read.
// ---------------------------------------------------------------------------
void AudioEffectJPFX::setDelayEffect(DelayEffectType type)
{
    // Reset time overrides so the new preset's native L/R times take effect.
    // The user's knob position will re-apply via setDelayTime() on next CC.
    _delayTimeOverrideL = -1.0f;
    _delayTimeOverrideR = -1.0f;

    // Signal the ISR to pick up the change at the next block boundary.
    // If type == current _delayType, _pendingDelayType will match and
    // update() will skip the transition (no redundant buffer clears).
    _pendingDelayType = type;
}

void AudioEffectJPFX::setDelayMix(float mix)
{
    _delayMix = constrain(mix, -1.0f, 1.0f);
}

void AudioEffectJPFX::setDelayFeedback(float fb)
{
    _delayFeedbackOverride = (fb < 0.0f) ? -1.0f : constrain(fb, 0.0f, 0.99f);
}

// ===========================================================================
// setDelayTime — user delay time knob (single CC value)
//
// Preserves the active preset's L/R ratio so panning presets stay stereo.
// The override value is treated as the LEFT channel time.  The RIGHT
// channel is scaled by (preset R / preset L) to maintain the same
// stereo offset.  For mono presets (L == R), the ratio is 1.0 and
// both channels get the same time — no special-casing needed.
//
// A value of 0.0 (or negative) disables the override and reverts to
// pure preset times.
// ===========================================================================
void AudioEffectJPFX::setDelayTime(float ms)
{
    _freeRunningDelayTime = ms;

    // Only apply when in free-running mode (BPM mode is managed by clock)
    if (_delayTimingMode != TIMING_FREE) return;

    // Disable override: revert to preset times
    if (ms <= 0.0f) {
        _delayTimeOverrideL = -1.0f;
        _delayTimeOverrideR = -1.0f;
        return;
    }

    // Compute L/R ratio from the active preset to preserve stereo spread.
    // Guard against divide-by-zero on mono presets (L == R → ratio = 1.0).
    float ratio = 1.0f;
    if (_delayType >= 0 && _delayType < JPFX_NUM_DELAY_VARIATIONS) {
        const DelayParams &p = delayPresets[_delayType];
        if (p.delayMsL > 0.0f) {
            ratio = p.delayMsR / p.delayMsL;
        }
    }

    _delayTimeOverrideL = ms;
    _delayTimeOverrideR = ms * ratio;
}


// ===========================================================================
// updateLfoIncrements — compute LFO phase increment per sample
//
// Called when modulation effect type or rate override changes.
// Right channel gets a 1% rate offset for natural stereo decorrelation.
// ===========================================================================
void AudioEffectJPFX::updateLfoIncrements()
{
    if (_modType == JPFX_MOD_OFF) {
        _lfoIncL = _lfoIncR = 0.0f;
        return;
    }

    // Use override rate if set, otherwise preset rate
    float rateHz = modPresets[_modType].rateHz;
    if (_modRateOverride > 0.0f) {
        rateHz = _modRateOverride;
    }

    // Phase increment = 2π × rate / sampleRate  (radians per sample)
    const float inc = kTwoPi * rateHz / AUDIO_SAMPLE_RATE_EXACT;
    _lfoIncL = inc;
    _lfoIncR = inc * 1.01f;    // Slight stereo offset
}


// ===========================================================================
// processModulation — chorus / flanger / phaser per-sample processing
//
// Standard presets (tapCount=1): single delay tap per channel.
// Super Chorus (tapCount=3): three delay taps at 120° LFO phase offsets,
//   averaged together.  Recreates the BBD ensemble effect (Roland JC-120
//   / Boss CE-5).  The 120° spacing ensures the three taps never align,
//   producing constant chorus density without amplitude pulsing.
//
// LFO waveform is selectable per preset:
//   Triangle: linear delay sweep (BBD bucket-passing characteristic).
//             Used by chorus presets.  Cheaper than sine (branch + fmadd).
//   Sine:     smoother modulation.  Used by flanger/phaser presets where
//             short delays make linear sweeps more audible as stepping.
//             Uses Bhaskara I fast approximation (< 0.002% error).
// ===========================================================================
inline void AudioEffectJPFX::processModulation(float inL, float inR,
                                                float &outL, float &outR)
{
    // Early bypass: zero CPU when modulation is off or buffer missing
    if (_modType == JPFX_MOD_OFF || !_modBufL || !_modBufR) {
        outL = inL;
        outR = inR;
        return;
    }

    const ModParams &p = modPresets[_modType];

    // Resolve feedback: user override or preset value
    const float feedback = (_modFeedbackOverride >= 0.0f)
                         ? _modFeedbackOverride
                         : p.feedback;

    // Wet/dry mix: user Level knob × preset mix (preset mix is 1.0 for all
    // current presets, so effectively the Level knob is the sole control)
    const float wetMix = _modMix * p.mix;
    const float dryMix = 1.0f - wetMix;

    // --- LFO function selection ---
    // Triangle LFO: phase ∈ [0, 2π) → output ∈ [-1, +1]
    // Linear ramp — cheaper than sine, smoother for chorus
    auto triangleLfo = [](float phase) -> float {
        const float norm = phase * (1.0f / kTwoPi);   // [0, 1)
        return (norm < 0.5f) ? (-1.0f + 4.0f * norm)
                             : ( 3.0f - 4.0f * norm);
    };

    // Bhaskara I fast sine: phase ∈ [0, 2π) → output ∈ [-1, +1]
    // < 0.002% error over full cycle, avoids transcendental sinf()
    static constexpr float k5PiSq = 5.0f * kPi * kPi;
    auto fastSin = [](float phase) -> float {
        float x    = phase;
        float sign = 1.0f;
        if (x > kPi) { x -= kPi; sign = -1.0f; }
        const float xpi = x * (kPi - x);
        return sign * (16.0f * xpi) / (k5PiSq - 4.0f * xpi);
    };

    // Precompute ms-to-samples factor (constant for the block)
    const float msToSamples = 0.001f * AUDIO_SAMPLE_RATE_EXACT;
    const float maxIdx      = (float)(_modBufSize - 2);

    // Compute primary LFO values using the selected waveform
    float lfoL, lfoR;
    if (p.useTriangleLfo) {
        lfoL = triangleLfo(_lfoPhaseL);
        lfoR = triangleLfo(_lfoPhaseR);
    } else {
        lfoL = fastSin(_lfoPhaseL);
        lfoR = fastSin(_lfoPhaseR);
    }

    // Advance LFO phases (must happen every sample to keep phase continuous)
    _lfoPhaseL += _lfoIncL;
    if (_lfoPhaseL >= kTwoPi) _lfoPhaseL -= kTwoPi;
    _lfoPhaseR += _lfoIncR;
    if (_lfoPhaseR >= kTwoPi) _lfoPhaseR -= kTwoPi;

    // --- Helper: interpolated circular buffer read ---
    auto readInterp = [](const float *buf, uint32_t bufSize,
                         uint32_t writeIdx, float delaySamples) -> float
    {
        float readIdx = (float)writeIdx - delaySamples;
        if (readIdx < 0.0f) readIdx += (float)bufSize;
        const uint32_t i0   = (uint32_t)readIdx;
        const uint32_t i1   = (i0 + 1) % bufSize;
        const float    frac = readIdx - (float)i0;
        return buf[i0] + (buf[i1] - buf[i0]) * frac;
    };

    float delayedL, delayedR;

    if (p.tapCount >= 3) {
        // ---- Super Chorus: 3-tap BBD ensemble ----
        // Three delay taps at 120° LFO phase offsets, averaged.
        // Slight depth variation per tap simulates BBD component tolerance.
        static constexpr float kOffset1 = kTwoPi / 3.0f;        // 120°
        static constexpr float kOffset2 = kTwoPi * 2.0f / 3.0f; // 240°

        // Compute additional LFO values for taps 1 and 2
        float phase1L = _lfoPhaseL + kOffset1;
        if (phase1L >= kTwoPi) phase1L -= kTwoPi;
        float phase2L = _lfoPhaseL + kOffset2;
        if (phase2L >= kTwoPi) phase2L -= kTwoPi;

        float phase1R = _lfoPhaseR + kOffset1;
        if (phase1R >= kTwoPi) phase1R -= kTwoPi;
        float phase2R = _lfoPhaseR + kOffset2;
        if (phase2R >= kTwoPi) phase2R -= kTwoPi;

        float lfo1L, lfo2L, lfo1R, lfo2R;
        if (p.useTriangleLfo) {
            lfo1L = triangleLfo(phase1L);
            lfo2L = triangleLfo(phase2L);
            lfo1R = triangleLfo(phase1R);
            lfo2R = triangleLfo(phase2R);
        } else {
            lfo1L = fastSin(phase1L);
            lfo2L = fastSin(phase2L);
            lfo1R = fastSin(phase1R);
            lfo2R = fastSin(phase2R);
        }

        // Compute delay times for each tap (slight depth scaling for organic feel)
        float dL0 = (p.baseDelayMsL + p.depthMsL * 1.00f * lfoL ) * msToSamples;
        float dL1 = (p.baseDelayMsL + p.depthMsL * 0.90f * lfo1L) * msToSamples;
        float dL2 = (p.baseDelayMsL + p.depthMsL * 1.10f * lfo2L) * msToSamples;
        float dR0 = (p.baseDelayMsR + p.depthMsR * 1.00f * lfoR ) * msToSamples;
        float dR1 = (p.baseDelayMsR + p.depthMsR * 0.90f * lfo1R) * msToSamples;
        float dR2 = (p.baseDelayMsR + p.depthMsR * 1.10f * lfo2R) * msToSamples;

        // Clamp all taps within buffer bounds
        dL0 = constrain(dL0, 0.0f, maxIdx);
        dL1 = constrain(dL1, 0.0f, maxIdx);
        dL2 = constrain(dL2, 0.0f, maxIdx);
        dR0 = constrain(dR0, 0.0f, maxIdx);
        dR1 = constrain(dR1, 0.0f, maxIdx);
        dR2 = constrain(dR2, 0.0f, maxIdx);

        // Read 3 taps per channel and average
        float tap0L = readInterp(_modBufL, _modBufSize, _modWriteIdx, dL0);
        float tap1L = readInterp(_modBufL, _modBufSize, _modWriteIdx, dL1);
        float tap2L = readInterp(_modBufL, _modBufSize, _modWriteIdx, dL2);
        delayedL = (tap0L + tap1L + tap2L) * (1.0f / 3.0f);

        float tap0R = readInterp(_modBufR, _modBufSize, _modWriteIdx, dR0);
        float tap1R = readInterp(_modBufR, _modBufSize, _modWriteIdx, dR1);
        float tap2R = readInterp(_modBufR, _modBufSize, _modWriteIdx, dR2);
        delayedR = (tap0R + tap1R + tap2R) * (1.0f / 3.0f);

    } else {
        // ---- Standard: single-tap chorus/flanger/phaser ----
        float dSampL = (p.baseDelayMsL + p.depthMsL * lfoL) * msToSamples;
        float dSampR = (p.baseDelayMsR + p.depthMsR * lfoR) * msToSamples;
        dSampL = constrain(dSampL, 0.0f, maxIdx);
        dSampR = constrain(dSampR, 0.0f, maxIdx);

        delayedL = readInterp(_modBufL, _modBufSize, _modWriteIdx, dSampL);
        delayedR = readInterp(_modBufR, _modBufSize, _modWriteIdx, dSampR);
    }

    // Write input + feedback into circular buffer, advance write pointer
    _modBufL[_modWriteIdx] = inL + delayedL * feedback;
    _modBufR[_modWriteIdx] = inR + delayedR * feedback;
    _modWriteIdx = (_modWriteIdx + 1) % _modBufSize;

    // Blend dry and wet
    outL = dryMix * inL + wetMix * delayedL;
    outR = dryMix * inR + wetMix * delayedR;
}


// ===========================================================================
// prepareDelay — compute block-constant delay parameters
//
// Called once per update() block.  Caches delay time (in samples), feedback,
// wet/dry levels, and phase inversion flag.  processDelay() reads these
// cached values rather than recomputing for each of the 128 samples.
//
// Delay times are clamped to a minimum of JPFX_MIN_DELAY_SAMP (1 sample)
// to prevent the interpolated read from landing on the write position.
// ===========================================================================
void AudioEffectJPFX::prepareDelay()
{
    if (_delayType == JPFX_DELAY_OFF || !_delayBufL || !_delayBufR) {
        _delaySampLCached  = 0.0f;
        _delaySampRCached  = 0.0f;
        _delayFbCached     = 0.0f;
        _delayWetCached    = 0.0f;
        _delayDryCached    = 1.0f;
        _delayInvertCached = false;
        return;
    }

    const DelayParams &p = delayPresets[_delayType];

    // Resolve delay times: user override preserves preset L/R ratio
    float timeMsL = (_delayTimeOverrideL >= 0.0f) ? _delayTimeOverrideL : p.delayMsL;
    float timeMsR = (_delayTimeOverrideR >= 0.0f) ? _delayTimeOverrideR : p.delayMsR;

    // Convert ms to samples, clamp within safe bounds.
    // Minimum 1 sample prevents reading the write position (stale data click).
    // Maximum leaves room for the 2-sample interpolation guard band.
    const float msToSamp = 0.001f * AUDIO_SAMPLE_RATE_EXACT;
    const float maxSamp  = (float)(_delayBufSize - 2);

    float dSampL = timeMsL * msToSamp;
    float dSampR = timeMsR * msToSamp;
    dSampL = constrain(dSampL, JPFX_MIN_DELAY_SAMP, maxSamp);
    dSampR = constrain(dSampR, JPFX_MIN_DELAY_SAMP, maxSamp);

    // Resolve wet/dry from user mix (negative = phase inversion)
    float wet = _delayMix;
    _delayInvertCached = (wet < 0.0f);
    wet = fabsf(wet);

    // Cache all block-constant values
    _delaySampLCached = dSampL;
    _delaySampRCached = dSampR;
    _delayFbCached    = (_delayFeedbackOverride >= 0.0f)
                      ? _delayFeedbackOverride
                      : p.feedback;
    _delayWetCached   = wet;
    _delayDryCached   = 1.0f - wet;
}


// ===========================================================================
// processDelay — delay / panning delay per-sample processing
//
// Uses block-constant cache from prepareDelay().  The interpolated read
// index advances by 1 per sample (matching the write pointer) so the
// delay tap stays sample-accurate.
//
// When delay is off, the write pointer still advances (maintaining sync)
// but no buffer reads or writes occur — the buffer is left to decay
// naturally when the effect is re-enabled.
// ===========================================================================
inline void AudioEffectJPFX::processDelay(float inL, float inR,
                                           float &outL, float &outR)
{
    // Bypass path: pass input through, no buffer operations.
    // Buffer content decays naturally (no explicit per-sample decay needed —
    // it just sits there; on re-enable, prepareDelay() will read valid data
    // because the write pointer has not been writing garbage).
    if (_delayType == JPFX_DELAY_OFF || !_delayBufL || !_delayBufR) {
        outL = inL;
        outR = inR;
        return;
    }

    // Read cached block-constant parameters
    const float feedback  = _delayFbCached;
    const float wetLevel  = _delayWetCached;
    const float dryLevel  = _delayDryCached;
    const bool  invertWet = _delayInvertCached;
    const float dSampL    = _delaySampLCached;
    const float dSampR    = _delaySampRCached;

    // Interpolated read — LEFT channel
    float readIdxL = (float)_delayWriteIdx - dSampL;
    if (readIdxL < 0.0f) readIdxL += (float)_delayBufSize;
    const uint32_t iL0    = (uint32_t)readIdxL;
    const uint32_t iL1    = (iL0 + 1) % _delayBufSize;
    const float    fracL  = readIdxL - (float)iL0;
    const float    delayL = _delayBufL[iL0] + (_delayBufL[iL1] - _delayBufL[iL0]) * fracL;

    // Interpolated read — RIGHT channel
    float readIdxR = (float)_delayWriteIdx - dSampR;
    if (readIdxR < 0.0f) readIdxR += (float)_delayBufSize;
    const uint32_t iR0    = (uint32_t)readIdxR;
    const uint32_t iR1    = (iR0 + 1) % _delayBufSize;
    const float    fracR  = readIdxR - (float)iR0;
    const float    delayR = _delayBufR[iR0] + (_delayBufR[iR1] - _delayBufR[iR0]) * fracR;

    // Write input + feedback into circular buffer, advance write pointer
    _delayBufL[_delayWriteIdx] = inL + delayL * feedback;
    _delayBufR[_delayWriteIdx] = inR + delayR * feedback;
    _delayWriteIdx = (_delayWriteIdx + 1) % _delayBufSize;

    // Blend dry and wet (with optional phase inversion for creative use)
    const float wetL = invertWet ? -delayL : delayL;
    const float wetR = invertWet ? -delayR : delayR;
    outL = dryLevel * inL + wetLevel * wetL;
    outR = dryLevel * inR + wetLevel * wetR;
}


// ===========================================================================
// update() — main AudioStream processing callback
//
// Called by the Teensy Audio Library every 128 samples (~2.9 ms at 44.1 kHz).
//
// Processing order:
//   0. ISR-safe delay preset transition (if pending)
//   1. Block-rate parameter updates (tone, saturation, delay cache)
//   2. Per-sample loop: Saturation → Tone → Modulation → Delay → Limiter
//   3. Block-rate limiter gain adjustment
//   4. Transmit stereo output, release all blocks
// ===========================================================================
void AudioEffectJPFX::update(void)
{
    // --- Stage 0: ISR-safe delay preset transition ---
    // setDelayEffect() writes _pendingDelayType from the main loop.
    // We pick it up here, inside the ISR, so the buffer clear and
    // write index reset are atomic with respect to audio processing.
    const DelayEffectType pending = _pendingDelayType;
    if (pending != _delayType) {
        _delayType = pending;
        _delayWriteIdx = 0;

        // Clear delay buffers to prevent stale audio bleed
        if (_delayBufL && _delayBufR) {
            memset(_delayBufL, 0, sizeof(float) * _delayBufSize);
            memset(_delayBufR, 0, sizeof(float) * _delayBufSize);
        }
    }

    // Receive mono input (may be nullptr when no audio is connected —
    // processing continues to maintain LFO phase and effect tails)
    audio_block_t *in = receiveReadOnly(0);

    // Allocate two output blocks for stereo
    audio_block_t *outBlockL = allocate();
    audio_block_t *outBlockR = allocate();

    if (!outBlockL || !outBlockR) {
        if (outBlockL) release(outBlockL);
        if (outBlockR) release(outBlockR);
        if (in) release(in);
        return;
    }

    // --- Block-rate parameter updates (once per block, not per-sample) ---
    if (_toneDirty) {
        computeToneParams();
    }
    if (_satDirty) {
        computeSatParams();
    }
    prepareDelay();

    // Track block peak magnitude for the limiter
    float blockPeak = 0.0f;

    // --- Per-sample processing loop ---
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {

        // Read mono input (0.0 when no connection — keeps effect tails alive)
        float sample = in ? ((float)in->data[i] * (1.0f / 32768.0f)) : 0.0f;

        // Stage 1: Saturation (bypass is zero-cost when drive = 0)
        float sampleL = applySaturation(sample);
        float sampleR = sampleL;     // Fan mono → stereo for remaining stages

        // Stage 2: Tone EQ (crossover bass/treble shelving)
        applyTone(sampleL, sampleR);

        // Stage 3: Modulation (chorus / flanger / phaser)
        float modL, modR;
        processModulation(sampleL, sampleR, modL, modR);

        // Stage 4: Delay (mono / panning)
        float delL, delR;
        processDelay(modL, modR, delL, delR);

        // Track peak for block-rate limiter
        const float peakL = fabsf(delL);
        const float peakR = fabsf(delR);
        if (peakL > blockPeak) blockPeak = peakL;
        if (peakR > blockPeak) blockPeak = peakR;

        // Apply current limiter gain and convert to int16.
        // Manual clamp avoids Arduino constrain() macro (multi-eval issue).
        float outF_L = delL * _limGain * 32767.0f;
        float outF_R = delR * _limGain * 32767.0f;
        if (outF_L >  32767.0f) outF_L =  32767.0f;
        if (outF_L < -32767.0f) outF_L = -32767.0f;
        if (outF_R >  32767.0f) outF_R =  32767.0f;
        if (outF_R < -32767.0f) outF_R = -32767.0f;
        outBlockL->data[i] = (int16_t)outF_L;
        outBlockR->data[i] = (int16_t)outF_R;
    }

    // --- Stage 5: Block-rate brick-wall limiter ---
    // Fast attack (1-block) prevents digital clipping.
    // Slow release (~100 ms) avoids pumping artefacts.
    // Ceiling at 0.97 (≈ -0.26 dBFS) leaves headroom for DAC reconstruction.
    static constexpr float kCeiling = 0.97f;

    if (blockPeak * _limGain > kCeiling) {
        // Overload — snap gain down to bring peak to ceiling
        _limGain = kCeiling / blockPeak;
    } else {
        // No overload — recover gain slowly toward unity
        _limGain += kLimRelease;
        if (_limGain > 1.0f) _limGain = 1.0f;
    }

    // Transmit stereo output
    transmit(outBlockL, 0);     // Channel 0 = Left
    transmit(outBlockR, 1);     // Channel 1 = Right

    // Release all audio blocks
    release(outBlockL);
    release(outBlockR);
    if (in) release(in);
}
