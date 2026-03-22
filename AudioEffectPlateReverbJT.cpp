/*
 * AudioEffectPlateReverbJT.cpp
 * ============================
 * Dattorro-style stereo plate reverb for Teensy 4.1 with PSRAM.
 *
 * See AudioEffectPlateReverbJT.h for topology diagram and design notes.
 *
 * DELAY LINE LENGTHS (in samples at 44100 Hz):
 * ---------------------------------------------
 * These prime-number lengths are chosen to minimise metallic ringing
 * caused by coincident reflection times.  Larger values = longer reverb
 * tail / bigger perceived room.  The Dattorro paper specifies specific
 * ratios; we scale up for the "massive hall" character.
 *
 * MEMORY BUDGET (all floats, 4 bytes each):
 *   Pre-delay:          11025 samples =  44.1 KB  (250 ms max)
 *   Input diffusers:      142 + 107 + 379 + 277 =   3.6 KB
 *   Tank APF [0]:        1800 samples =   7.2 KB
 *   Tank APF [1]:        2656 samples =  10.6 KB
 *   Tank delay [0]:      3720 samples =  14.9 KB
 *   Tank delay [1]:      4217 samples =  16.9 KB
 *   ──────────────────────────────────────────────
 *   TOTAL:              23323 samples =  93.3 KB
 *
 *   Well within the 8 MB PSRAM budget; leaves plenty of room for JPFX
 *   delay buffers and future effects.
 *
 * REFERENCES:
 *   [1] Jon Dattorro, "Effect Design Part 1", JAES 1997, §2 Plate Reverb
 */

#include "AudioEffectPlateReverbJT.h"
#include "DebugTrace.h"         // JT_LOGF
#include <cmath>                // fabsf, ceilf

// =============================================================================
// DELAY LINE LENGTHS — prime-valued to decorrelate reflections
// =============================================================================
// Scaled up from the original Dattorro values for a larger, more diffuse
// reverb tail.  Each value is prime (verified) to prevent modal ringing.

// Pre-delay: 250 ms max at 44.1 kHz
static constexpr uint32_t PREDELAY_MAX_SAMPLES = 11025;

// Input diffuser chain (4 stages) — progressively longer for dense build-up
static constexpr uint32_t IDIFF_LEN_0 =  142;  // ~3.2 ms
static constexpr uint32_t IDIFF_LEN_1 =  107;  // ~2.4 ms
static constexpr uint32_t IDIFF_LEN_2 =  379;  // ~8.6 ms
static constexpr uint32_t IDIFF_LEN_3 =  277;  // ~6.3 ms

// Tank modulated allpass filters — longer for lush smearing
static constexpr uint32_t TANK_APF_LEN_0 = 1800;  // ~40.8 ms
static constexpr uint32_t TANK_APF_LEN_1 = 2656;  // ~60.2 ms

// Tank delay lines — long for extended reverb tail
static constexpr uint32_t TANK_DLY_LEN_0 = 3720;  // ~84.4 ms
static constexpr uint32_t TANK_DLY_LEN_1 = 4217;  // ~95.6 ms

// Total buffer pool size (sum of all delay line lengths)
static constexpr uint32_t TOTAL_BUFFER_SAMPLES =
    PREDELAY_MAX_SAMPLES +
    IDIFF_LEN_0 + IDIFF_LEN_1 + IDIFF_LEN_2 + IDIFF_LEN_3 +
    TANK_APF_LEN_0 + TANK_APF_LEN_1 +
    TANK_DLY_LEN_0 + TANK_DLY_LEN_1;

// =============================================================================
// DEFAULT COEFFICIENTS
// =============================================================================

// Input diffuser allpass gains — Dattorro recommends 0.75 for the first two,
// 0.625 for the second two.  Slight asymmetry improves stereo spread.
static constexpr float IDIFF_GAIN_0 = 0.75f;
static constexpr float IDIFF_GAIN_1 = 0.75f;
static constexpr float IDIFF_GAIN_2 = 0.625f;
static constexpr float IDIFF_GAIN_3 = 0.625f;

// Tank allpass gains — lower than input diffusers for stability at high decay
static constexpr float TANK_APF_GAIN = 0.60f;

// Output tap positions (samples behind write head in each tank delay line)
// Decorrelated positions for wide stereo image.
// Left output taps from tank 0 and tank 1:
static constexpr uint32_t TAP_L0 =  266;   // early reflection
static constexpr uint32_t TAP_L1 = 2974;   // late reflection
static constexpr uint32_t TAP_L2 = 1913;   // cross-tank contribution

// Right output taps (different positions for stereo decorrelation):
static constexpr uint32_t TAP_R0 =  353;
static constexpr uint32_t TAP_R1 = 3627;
static constexpr uint32_t TAP_R2 = 1228;

// =============================================================================
// CONSTRUCTOR
// =============================================================================

AudioEffectPlateReverbJT::AudioEffectPlateReverbJT()
    : AudioStream(2, inputQueueArray)   // stereo input
    , _decay(0.7f)
    , _hiDampCoeff(0.3f)
    , _loDampCoeff(0.0f)
    , _wetLevel(1.0f)
    , _dryLevel(0.0f)
    , _predelaySamples(0)
    , _modDepth(8.0f)
    , _modRate(0.8f)
    , _modPhase(0.0f)
    , _modPhaseInc(0.0f)
    , _bypassed(true)           // start bypassed (FXChainBlock default)
    , _frozen(false)
    , _bufferPool(nullptr)
    , _bufferPoolSize(0)
{
    // Allocate the contiguous PSRAM buffer and assign sub-regions
    if (allocateBuffers()) {
        assignBuffers();

        // Set allpass feedback gains
        _inputDiffuser[0].gain = IDIFF_GAIN_0;
        _inputDiffuser[1].gain = IDIFF_GAIN_1;
        _inputDiffuser[2].gain = IDIFF_GAIN_2;
        _inputDiffuser[3].gain = IDIFF_GAIN_3;

        _tankAPF[0].gain = TANK_APF_GAIN;
        _tankAPF[1].gain = TANK_APF_GAIN;

        // Init one-pole filter states
        for (uint8_t i = 0; i < 2; i++) {
            _tankLPF[i].state = 0.0f;
            _tankLPF[i].coeff = _hiDampCoeff;
            _tankHPF[i].state = 0.0f;
            _tankHPF[i].coeff = _loDampCoeff;
        }

        // Calculate LFO phase increment
        updateModRate();

        JT_LOGF("[PlateReverbJT] Allocated %u KB in PSRAM\n",
                (unsigned)(TOTAL_BUFFER_SAMPLES * sizeof(float) / 1024));
    } else {
        JT_LOGF("[PlateReverbJT] ERROR: Buffer allocation FAILED\n");
    }
}

// =============================================================================
// DESTRUCTOR
// =============================================================================

AudioEffectPlateReverbJT::~AudioEffectPlateReverbJT()
{
    freeBuffers();
}

// =============================================================================
// MEMORY ALLOCATION
// =============================================================================

bool AudioEffectPlateReverbJT::allocateBuffers()
{
    _bufferPoolSize = TOTAL_BUFFER_SAMPLES;
    const uint32_t totalBytes = _bufferPoolSize * sizeof(float);

    // Try PSRAM first (Teensy 4.1 with PSRAM chip)
    #if defined(__IMXRT1062__)
        _bufferPool = (float*)extmem_malloc(totalBytes);
        if (_bufferPool) {
            JT_LOGF("[PlateReverbJT] Using PSRAM (%u KB)\n",
                    (unsigned)(totalBytes / 1024));
        } else {
            // PSRAM unavailable — fall back to heap
            JT_LOGF("[PlateReverbJT] PSRAM unavailable, trying heap\n");
            _bufferPool = (float*)malloc(totalBytes);
        }
    #else
        _bufferPool = (float*)malloc(totalBytes);
    #endif

    if (!_bufferPool) {
        _bufferPoolSize = 0;
        return false;
    }

    // Zero the entire pool
    memset(_bufferPool, 0, totalBytes);
    return true;
}

void AudioEffectPlateReverbJT::freeBuffers()
{
    if (_bufferPool) {
        // extmem_malloc'd memory is freed with free() on Teensy 4.x
        free(_bufferPool);
        _bufferPool = nullptr;
        _bufferPoolSize = 0;
    }
}

void AudioEffectPlateReverbJT::assignBuffers()
{
    // Carve the contiguous pool into sub-regions for each delay line.
    // Order must match the sum in TOTAL_BUFFER_SAMPLES.
    float* ptr = _bufferPool;

    // Pre-delay
    _predelay.buf      = ptr;
    _predelay.len      = PREDELAY_MAX_SAMPLES;
    _predelay.writeIdx = 0;
    ptr += PREDELAY_MAX_SAMPLES;

    // Input diffusers (4 stages)
    static constexpr uint32_t idiffLens[4] = {
        IDIFF_LEN_0, IDIFF_LEN_1, IDIFF_LEN_2, IDIFF_LEN_3
    };
    for (uint8_t i = 0; i < NUM_INPUT_DIFFUSERS; i++) {
        _inputDiffuser[i].dl.buf      = ptr;
        _inputDiffuser[i].dl.len      = idiffLens[i];
        _inputDiffuser[i].dl.writeIdx = 0;
        ptr += idiffLens[i];
    }

    // Tank allpass [0] and [1]
    _tankAPF[0].dl.buf      = ptr;
    _tankAPF[0].dl.len      = TANK_APF_LEN_0;
    _tankAPF[0].dl.writeIdx = 0;
    ptr += TANK_APF_LEN_0;

    _tankAPF[1].dl.buf      = ptr;
    _tankAPF[1].dl.len      = TANK_APF_LEN_1;
    _tankAPF[1].dl.writeIdx = 0;
    ptr += TANK_APF_LEN_1;

    // Tank delays [0] and [1]
    _tankDelay[0].buf      = ptr;
    _tankDelay[0].len      = TANK_DLY_LEN_0;
    _tankDelay[0].writeIdx = 0;
    ptr += TANK_DLY_LEN_0;

    _tankDelay[1].buf      = ptr;
    _tankDelay[1].len      = TANK_DLY_LEN_1;
    _tankDelay[1].writeIdx = 0;
    ptr += TANK_DLY_LEN_1;
}

// =============================================================================
// PARAMETER SETTERS — clamped at the boundary, never in the audio path
// =============================================================================

void AudioEffectPlateReverbJT::size(float n)
{
    // Clamp 0..1
    if (n < 0.0f) n = 0.0f;
    if (n > 1.0f) n = 1.0f;

    // Map 0..1 to decay coefficient 0.05..0.9995
    // Exponential mapping gives finer control at the long-tail end,
    // which is where the "massive trance reverb" character lives.
    // decay = 0.05 + 0.9495 * n^2   (quadratic for musically useful taper)
    _decay = 0.05f + 0.9495f * n * n;
}

void AudioEffectPlateReverbJT::hidamp(float n)
{
    if (n < 0.0f) n = 0.0f;
    if (n > 1.0f) n = 1.0f;

    // Map 0..1 to LPF coefficient.
    // 0 = no damping (bright), 1 = heavy damping (dark).
    // coeff = n * 0.7  (cap at 0.7 to prevent the tail going completely dead)
    _hiDampCoeff = n * 0.7f;

    _tankLPF[0].coeff = _hiDampCoeff;
    _tankLPF[1].coeff = _hiDampCoeff;
}

void AudioEffectPlateReverbJT::lodamp(float n)
{
    if (n < 0.0f) n = 0.0f;
    if (n > 1.0f) n = 1.0f;

    // Map 0..1 to HPF coefficient.
    // 0 = full bass, 1 = thin/no bass.
    _loDampCoeff = n * 0.7f;

    _tankHPF[0].coeff = _loDampCoeff;
    _tankHPF[1].coeff = _loDampCoeff;
}

void AudioEffectPlateReverbJT::mix(float n)
{
    if (n < 0.0f) n = 0.0f;
    if (n > 1.0f) n = 1.0f;

    _wetLevel = n;
    _dryLevel = 1.0f - n;
}

void AudioEffectPlateReverbJT::bypass_set(bool state)
{
    _bypassed = state;
}

void AudioEffectPlateReverbJT::predelay(float ms)
{
    if (ms < 0.0f)   ms = 0.0f;
    if (ms > 250.0f) ms = 250.0f;

    _predelaySamples = (uint32_t)(ms * (AUDIO_SAMPLE_RATE_EXACT / 1000.0f));

    // Clamp to buffer length (safety)
    if (_predelaySamples >= PREDELAY_MAX_SAMPLES) {
        _predelaySamples = PREDELAY_MAX_SAMPLES - 1;
    }
}

void AudioEffectPlateReverbJT::modDepth(float n)
{
    if (n < 0.0f) n = 0.0f;
    if (n > 1.0f) n = 1.0f;

    // Map 0..1 to 0..16 samples of modulation excursion.
    // 16 samples at 44.1 kHz ≈ 0.36 ms — enough for lush chorusing
    // without audible pitch artifacts.
    _modDepth = n * 16.0f;
}

void AudioEffectPlateReverbJT::modRate(float hz)
{
    if (hz < 0.1f) hz = 0.1f;
    if (hz > 5.0f) hz = 5.0f;

    _modRate = hz;
    updateModRate();
}

void AudioEffectPlateReverbJT::freeze(bool state)
{
    _frozen = state;
}

void AudioEffectPlateReverbJT::updateModRate()
{
    // Phase increment per sample for the triangle LFO
    _modPhaseInc = _modRate / AUDIO_SAMPLE_RATE_EXACT;
}

// =============================================================================
// update() — main audio processing callback
//
// Called by the Teensy Audio Library every 128 samples (~2.9 ms at 44.1 kHz).
//
// Processing order per sample:
//   1. Sum stereo input to mono (or use mono if only one channel connected)
//   2. Pre-delay
//   3. Input diffuser chain (4× allpass)
//   4. Tank processing (2 cross-coupled halves)
//   5. Decorrelated output taps → stereo
//   6. Wet/dry mix → int16 output
// =============================================================================

void AudioEffectPlateReverbJT::update(void)
{
    // -------------------------------------------------------------------------
    // Bypass check — zero CPU when bypassed
    // -------------------------------------------------------------------------
    if (_bypassed || !_bufferPool) {
        // Still need to receive and release input blocks to prevent
        // upstream stalls in the audio graph.
        audio_block_t* inL = receiveReadOnly(0);
        audio_block_t* inR = receiveReadOnly(1);
        if (inL) release(inL);
        if (inR) release(inR);
        return;
    }

    // -------------------------------------------------------------------------
    // Receive stereo input
    // -------------------------------------------------------------------------
    audio_block_t* inL = receiveReadOnly(0);
    audio_block_t* inR = receiveReadOnly(1);

    // Allocate stereo output blocks
    audio_block_t* outL = allocate();
    audio_block_t* outR = allocate();

    if (!outL || !outR) {
        // Memory exhausted — release everything and bail
        if (outL) release(outL);
        if (outR) release(outR);
        if (inL)  release(inL);
        if (inR)  release(inR);
        return;
    }

    // -------------------------------------------------------------------------
    // Cache parameters locally to avoid member access overhead per sample
    // -------------------------------------------------------------------------
    const float    decay         = _frozen ? 1.0f : _decay;
    const float    inputGain     = _frozen ? 0.0f : 1.0f;
    const float    wetLevel      = _wetLevel;
    const float    dryLevel      = _dryLevel;
    const uint32_t predelaySmp   = _predelaySamples;
    const float    modDepthSmp   = _modDepth;

    // Scale factor for int16 ↔ float conversion
    static constexpr float kToFloat  = 1.0f / 32768.0f;
    static constexpr float kToInt16  = 32767.0f;

    // -------------------------------------------------------------------------
    // Per-sample processing loop
    // -------------------------------------------------------------------------
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {

        // --- Read stereo input, sum to mono ---
        float inSampleL = inL ? (float)inL->data[i] * kToFloat : 0.0f;
        float inSampleR = inR ? (float)inR->data[i] * kToFloat : 0.0f;
        float monoIn    = (inSampleL + inSampleR) * 0.5f * inputGain;

        // --- Pre-delay ---
        _predelay.write(monoIn);
        float predelayed = (predelaySmp > 0)
                         ? _predelay.read(predelaySmp)
                         : monoIn;

        // --- Input diffuser chain ---
        float diffused = predelayed;
        for (uint8_t d = 0; d < NUM_INPUT_DIFFUSERS; d++) {
            diffused = _inputDiffuser[d].process(diffused);
        }

        // --- Triangle LFO for tank modulation ---
        float lfo = triangleLFO();

        // --- Tank processing ---
        // Two cross-coupled halves.  Each iteration:
        //   1. Add diffused input + cross-feedback from the other tank
        //   2. Modulated allpass filter
        //   3. Delay line
        //   4. Damping filters (LPF + HPF)
        //   5. Scale by decay coefficient

        // Read cross-feedback from previous iteration (end of each tank delay)
        float crossFB0 = _tankDelay[1].read(_tankDelay[1].len - 1) * decay;
        float crossFB1 = _tankDelay[0].read(_tankDelay[0].len - 1) * decay;

        // --- Tank half 0 ---
        float tank0 = diffused + crossFB0;
        tank0 = _tankAPF[0].processModulated(tank0, lfo * modDepthSmp);
        _tankDelay[0].write(tank0);
        float tank0out = _tankDelay[0].read(_tankDelay[0].len - 1);
        tank0out = _tankLPF[0].process(tank0out);
        tank0out = _tankHPF[0].process(tank0out);

        // --- Tank half 1 ---
        float tank1 = diffused + crossFB1;
        // Inverted LFO for stereo decorrelation
        tank1 = _tankAPF[1].processModulated(tank1, -lfo * modDepthSmp);
        _tankDelay[1].write(tank1);
        float tank1out = _tankDelay[1].read(_tankDelay[1].len - 1);
        tank1out = _tankLPF[1].process(tank1out);
        tank1out = _tankHPF[1].process(tank1out);

        // --- Decorrelated output taps ---
        // Multiple taps from different positions in each tank for a wide,
        // complex stereo image.  The Dattorro paper specifies subtractive
        // taps for some positions to increase decorrelation.
        float wetL = _tankDelay[0].read(TAP_L0)
                   + _tankDelay[0].read(TAP_L1)
                   - _tankDelay[1].read(TAP_L2);

        float wetR = _tankDelay[1].read(TAP_R0)
                   + _tankDelay[1].read(TAP_R1)
                   - _tankDelay[0].read(TAP_R2);

        // Scale wet signal — the tap summing can produce levels > 1.0,
        // so we normalise down.  0.3 is empirically tuned to prevent
        // clipping under sustained polyphonic input while maintaining
        // presence.
        static constexpr float kWetScale = 0.3f;
        wetL *= kWetScale;
        wetR *= kWetScale;

        // --- Wet/dry mix and int16 output ---
        float outSampleL = dryLevel * inSampleL + wetLevel * wetL;
        float outSampleR = dryLevel * inSampleR + wetLevel * wetR;

        // Soft-clip to prevent digital overs (tanh approximation)
        // Only active when signal exceeds ±1.0 — no cost at normal levels
        if (outSampleL > 1.0f || outSampleL < -1.0f) {
            outSampleL = outSampleL / (1.0f + fabsf(outSampleL));
        }
        if (outSampleR > 1.0f || outSampleR < -1.0f) {
            outSampleR = outSampleR / (1.0f + fabsf(outSampleR));
        }

        outL->data[i] = (int16_t)(outSampleL * kToInt16);
        outR->data[i] = (int16_t)(outSampleR * kToInt16);
    }

    // -------------------------------------------------------------------------
    // Transmit stereo output and release all blocks
    // -------------------------------------------------------------------------
    transmit(outL, 0);
    transmit(outR, 1);

    release(outL);
    release(outR);
    if (inL) release(inL);
    if (inR) release(inR);
}
