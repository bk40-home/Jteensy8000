/*
 * AudioEffectReverbJT.cpp
 * =======================
 * Multi-algorithm stereo reverb for Teensy 4.1 with PSRAM.
 * See AudioEffectReverbJT.h for topology diagrams and design notes.
 */

#include "AudioEffectReverbJT.h"
#include "DebugTrace.h"
#include <cmath>

// =============================================================================
// DELAY LINE LENGTHS (samples at 44100 Hz, all prime to decorrelate reflections)
// =============================================================================

// --- Pre-delay (shared, 500 ms max) ---
static constexpr uint32_t PREDELAY_MAX = 22051;

// --- Input diffusers (shared by all algorithms) ---
static constexpr uint32_t IDIFF_LEN[4] = { 142, 107, 379, 277 };
static constexpr float    IDIFF_GAIN[4] = { 0.75f, 0.75f, 0.625f, 0.625f };

// --- PLATE / SHIMMER tank ---
static constexpr uint32_t PLATE_APF_LEN[2]   = { 1801, 2657 };
static constexpr uint32_t PLATE_DLY_LEN[2]   = { 3721, 4219 };

// --- HALL FDN (8 lines, mutually prime) ---
static constexpr uint32_t FDN_DLY_LEN[8] = {
    1553, 1823, 2099, 2477,     // shorter group
    2803, 3251, 3607, 4001      // longer group
};

// --- SHIMMER pitch buffer (long enough for -24 semitones at 44.1k) ---
// At -24 semitones (2 octaves down) the read pointer moves at 0.25× speed,
// so we need 4× the cycle length.  4096 samples ≈ 93 ms — ample.
static constexpr uint32_t SHIMMER_BUF_LEN = 4096;

// --- SPRING (short allpass chain) ---
static constexpr uint32_t SPRING_APF_LEN[6] = { 53, 79, 127, 151, 199, 241 };
static constexpr uint32_t SPRING_DLY_LEN[2] = { 887, 1013 };

// --- CLOUD (dense diffusion network) ---
static constexpr uint32_t CLOUD_APF_LEN[8] = {
    631, 809, 1049, 1327, 1559, 1907, 2203, 2503
};
static constexpr uint32_t CLOUD_DLY_LEN[2] = { 5501, 6271 };

// --- Total pool size ---
static constexpr uint32_t TOTAL_POOL =
    PREDELAY_MAX +
    IDIFF_LEN[0] + IDIFF_LEN[1] + IDIFF_LEN[2] + IDIFF_LEN[3] +
    // Plate / shimmer
    PLATE_APF_LEN[0] + PLATE_APF_LEN[1] + PLATE_DLY_LEN[0] + PLATE_DLY_LEN[1] +
    // Hall FDN
    FDN_DLY_LEN[0] + FDN_DLY_LEN[1] + FDN_DLY_LEN[2] + FDN_DLY_LEN[3] +
    FDN_DLY_LEN[4] + FDN_DLY_LEN[5] + FDN_DLY_LEN[6] + FDN_DLY_LEN[7] +
    // Shimmer pitch buffer
    SHIMMER_BUF_LEN +
    // Spring
    SPRING_APF_LEN[0] + SPRING_APF_LEN[1] + SPRING_APF_LEN[2] +
    SPRING_APF_LEN[3] + SPRING_APF_LEN[4] + SPRING_APF_LEN[5] +
    SPRING_DLY_LEN[0] + SPRING_DLY_LEN[1] +
    // Cloud
    CLOUD_APF_LEN[0] + CLOUD_APF_LEN[1] + CLOUD_APF_LEN[2] + CLOUD_APF_LEN[3] +
    CLOUD_APF_LEN[4] + CLOUD_APF_LEN[5] + CLOUD_APF_LEN[6] + CLOUD_APF_LEN[7] +
    CLOUD_DLY_LEN[0] + CLOUD_DLY_LEN[1];

// --- Output tap positions (PLATE) ---
static constexpr uint32_t PLATE_TAP_L[3] = { 266, 2974, 1913 };
static constexpr uint32_t PLATE_TAP_R[3] = { 353, 3627, 1228 };

// =============================================================================
// CONSTRUCTOR
// =============================================================================

AudioEffectReverbJT::AudioEffectReverbJT()
    : AudioStream(2, inputQueueArray)
    , _type(ReverbType::PLATE)
    , _sizeParam(0.5f)
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
    , _bypassed(true)
    , _frozen(false)
    , _shimmerPhase(0.0f)
    , _shimmerPhaseInc(2.0f)    // +12 semitones = 2× read rate
    , _shimmerPitchSemi(12.0f)
    , _shimmerFeedbackMix(0.4f)
    , _bufferPool(nullptr)
    , _bufferPoolSize(0)
{
    // Zero FDN state
    for (uint8_t i = 0; i < FDN_SIZE; i++) _fdnState[i] = 0.0f;

    if (allocateBuffers()) {
        assignBuffers();
        updateModRate();

        JT_LOGF("[ReverbJT] Pool: %u KB in PSRAM (%u samples)\n",
                (unsigned)(TOTAL_POOL * sizeof(float) / 1024),
                (unsigned)TOTAL_POOL);
    } else {
        JT_LOGF("[ReverbJT] ERROR: Buffer allocation FAILED\n");
    }
}

AudioEffectReverbJT::~AudioEffectReverbJT()
{
    freeBuffers();
}

// =============================================================================
// MEMORY
// =============================================================================

bool AudioEffectReverbJT::allocateBuffers()
{
    _bufferPoolSize = TOTAL_POOL;
    const uint32_t totalBytes = _bufferPoolSize * sizeof(float);

    #if defined(__IMXRT1062__)
        _bufferPool = (float*)extmem_malloc(totalBytes);
        if (_bufferPool) {
            JT_LOGF("[ReverbJT] PSRAM OK (%u KB)\n", (unsigned)(totalBytes / 1024));
        } else {
            JT_LOGF("[ReverbJT] PSRAM fail, trying heap\n");
            _bufferPool = (float*)malloc(totalBytes);
        }
    #else
        _bufferPool = (float*)malloc(totalBytes);
    #endif

    if (!_bufferPool) {
        _bufferPoolSize = 0;
        return false;
    }

    memset(_bufferPool, 0, totalBytes);
    return true;
}

void AudioEffectReverbJT::freeBuffers()
{
    if (_bufferPool) {
        free(_bufferPool);
        _bufferPool = nullptr;
        _bufferPoolSize = 0;
    }
}

void AudioEffectReverbJT::assignBuffers()
{
    float* ptr = _bufferPool;

    // Macro-style lambdas to carve the pool — keeps assign code concise
    // without exposing private types in free functions.
    auto setDL = [](DelayLine& dl, float*& p, uint32_t len) {
        dl.buf = p; dl.len = len; dl.writeIdx = 0; p += len;
    };
    auto setAP = [&setDL](Allpass& ap, float*& p, uint32_t len, float g) {
        setDL(ap.dl, p, len); ap.gain = g;
    };

    // --- Shared: pre-delay ---
    setDL(_predelay, ptr, PREDELAY_MAX);

    // --- Shared: input diffusers ---
    for (uint8_t i = 0; i < NUM_INPUT_DIFFUSERS; i++) {
        setAP(_inputDiffuser[i], ptr, IDIFF_LEN[i], IDIFF_GAIN[i]);
    }

    // --- Plate / shimmer tank ---
    for (uint8_t i = 0; i < 2; i++) {
        setAP(_plateAPF[i], ptr, PLATE_APF_LEN[i], 0.60f);
    }
    for (uint8_t i = 0; i < 2; i++) {
        setDL(_plateDelay[i], ptr, PLATE_DLY_LEN[i]);
    }

    // --- Hall FDN ---
    for (uint8_t i = 0; i < FDN_SIZE; i++) {
        setDL(_fdnDelay[i], ptr, FDN_DLY_LEN[i]);
    }

    // --- Shimmer pitch buffer ---
    setDL(_shimmerBuf, ptr, SHIMMER_BUF_LEN);

    // --- Spring ---
    for (uint8_t i = 0; i < NUM_SPRING_APF; i++) {
        setAP(_springAPF[i], ptr, SPRING_APF_LEN[i], 0.65f);
    }
    for (uint8_t i = 0; i < 2; i++) {
        setDL(_springDelay[i], ptr, SPRING_DLY_LEN[i]);
    }

    // --- Cloud ---
    for (uint8_t i = 0; i < NUM_CLOUD_APF; i++) {
        setAP(_cloudAPF[i], ptr, CLOUD_APF_LEN[i], 0.55f);
    }
    for (uint8_t i = 0; i < 2; i++) {
        setDL(_cloudDelay[i], ptr, CLOUD_DLY_LEN[i]);
    }

    // --- Init all one-pole filters ---
    for (uint8_t i = 0; i < 2; i++) {
        _plateLPF[i].coeff = _hiDampCoeff;  _plateLPF[i].state = 0.0f;
        _plateHPF[i].coeff = _loDampCoeff;  _plateHPF[i].state = 0.0f;
        _springLPF[i].coeff = 0.4f;         _springLPF[i].state = 0.0f;
        _springHPF[i].coeff = 0.15f;        _springHPF[i].state = 0.0f;
        _cloudLPF[i].coeff = _hiDampCoeff;  _cloudLPF[i].state = 0.0f;
        _cloudHPF[i].coeff = _loDampCoeff;  _cloudHPF[i].state = 0.0f;
    }
    _springChirpLPF.coeff = 0.6f;
    _springChirpLPF.state = 0.0f;

    for (uint8_t i = 0; i < FDN_SIZE; i++) {
        _fdnLPF[i].coeff = _hiDampCoeff;  _fdnLPF[i].state = 0.0f;
        _fdnHPF[i].coeff = _loDampCoeff;  _fdnHPF[i].state = 0.0f;
    }
}

// =============================================================================
// PARAMETER SETTERS
// =============================================================================

void AudioEffectReverbJT::size(float n)
{
    if (n < 0.0f) n = 0.0f;
    if (n > 1.0f) n = 1.0f;
    _sizeParam = n;
    recalcDecay();
}

void AudioEffectReverbJT::recalcDecay()
{
    float n = _sizeParam;

    switch (_type) {
        case ReverbType::PLATE:
        case ReverbType::SHIMMER:
            // Quadratic: fine control at the long end
            _decay = 0.05f + 0.9495f * n * n;
            break;

        case ReverbType::HALL:
            // Even longer maximum — FDN supports stable decay near unity
            _decay = 0.10f + 0.8995f * n * n;
            break;

        case ReverbType::SPRING:
            // Shorter range — spring reverbs don't sustain as long
            _decay = 0.10f + 0.75f * n;
            break;

        case ReverbType::CLOUD:
            // Near-unity for pad-wash character
            _decay = 0.20f + 0.7995f * n * n;
            break;

        default:
            _decay = 0.5f;
            break;
    }
}

void AudioEffectReverbJT::hidamp(float n)
{
    if (n < 0.0f) n = 0.0f;
    if (n > 1.0f) n = 1.0f;
    _hiDampCoeff = n * 0.7f;

    // Apply to all algorithms' filters
    _plateLPF[0].coeff = _hiDampCoeff;
    _plateLPF[1].coeff = _hiDampCoeff;
    _cloudLPF[0].coeff = _hiDampCoeff;
    _cloudLPF[1].coeff = _hiDampCoeff;
    for (uint8_t i = 0; i < FDN_SIZE; i++) _fdnLPF[i].coeff = _hiDampCoeff;
    // Spring uses fixed damping — intentionally not updated
}

void AudioEffectReverbJT::lodamp(float n)
{
    if (n < 0.0f) n = 0.0f;
    if (n > 1.0f) n = 1.0f;
    _loDampCoeff = n * 0.7f;

    _plateHPF[0].coeff = _loDampCoeff;
    _plateHPF[1].coeff = _loDampCoeff;
    _cloudHPF[0].coeff = _loDampCoeff;
    _cloudHPF[1].coeff = _loDampCoeff;
    for (uint8_t i = 0; i < FDN_SIZE; i++) _fdnHPF[i].coeff = _loDampCoeff;
}

void AudioEffectReverbJT::mix(float n)
{
    if (n < 0.0f) n = 0.0f;
    if (n > 1.0f) n = 1.0f;
    _wetLevel = n;
    _dryLevel = 1.0f - n;
}

void AudioEffectReverbJT::bypass_set(bool state)  { _bypassed = state; }

void AudioEffectReverbJT::predelay(float ms)
{
    if (ms < 0.0f)   ms = 0.0f;
    if (ms > 500.0f) ms = 500.0f;
    _predelaySamples = (uint32_t)(ms * (AUDIO_SAMPLE_RATE_EXACT / 1000.0f));
    if (_predelaySamples >= PREDELAY_MAX) _predelaySamples = PREDELAY_MAX - 1;
}

void AudioEffectReverbJT::modDepth(float n)
{
    if (n < 0.0f) n = 0.0f;
    if (n > 1.0f) n = 1.0f;
    _modDepth = n * 16.0f;
}

void AudioEffectReverbJT::modRate(float hz)
{
    if (hz < 0.1f) hz = 0.1f;
    if (hz > 5.0f) hz = 5.0f;
    _modRate = hz;
    updateModRate();
}

void AudioEffectReverbJT::freeze(bool state) { _frozen = state; }

void AudioEffectReverbJT::shimmerPitch(float semitones)
{
    if (semitones < -24.0f) semitones = -24.0f;
    if (semitones >  24.0f) semitones =  24.0f;
    _shimmerPitchSemi = semitones;
    updateShimmerRate();
}

void AudioEffectReverbJT::shimmerMix(float n)
{
    if (n < 0.0f) n = 0.0f;
    if (n > 1.0f) n = 1.0f;
    _shimmerFeedbackMix = n;
}

void AudioEffectReverbJT::setType(ReverbType type)
{
    if (type >= ReverbType::COUNT) type = ReverbType::PLATE;
    if (type == _type) return;

    // Clear the destination algorithm's state to prevent stale audio
    clearAlgorithmState(type);

    _type = type;
    recalcDecay();  // each algorithm has its own decay mapping

    JT_LOGF("[ReverbJT] Type → %s\n", getTypeName());
}

const char* AudioEffectReverbJT::getTypeName() const
{
    return REVERB_TYPE_NAMES[(uint8_t)_type];
}

void AudioEffectReverbJT::updateModRate()
{
    _modPhaseInc = _modRate / AUDIO_SAMPLE_RATE_EXACT;
}

void AudioEffectReverbJT::updateShimmerRate()
{
    // Semitones to frequency ratio: ratio = 2^(semi/12)
    // Read pointer increment = ratio (>1 = higher pitch, <1 = lower)
    _shimmerPhaseInc = powf(2.0f, _shimmerPitchSemi / 12.0f);
}

void AudioEffectReverbJT::clearAlgorithmState(ReverbType type)
{
    switch (type) {
        case ReverbType::PLATE:
        case ReverbType::SHIMMER:
            for (uint8_t i = 0; i < 2; i++) {
                _plateAPF[i].clear();
                _plateDelay[i].clear();
                _plateLPF[i].clear();
                _plateHPF[i].clear();
            }
            _shimmerBuf.clear();
            _shimmerPhase = 0.0f;
            break;

        case ReverbType::HALL:
            for (uint8_t i = 0; i < FDN_SIZE; i++) {
                _fdnDelay[i].clear();
                _fdnLPF[i].clear();
                _fdnHPF[i].clear();
                _fdnState[i] = 0.0f;
            }
            break;

        case ReverbType::SPRING:
            for (uint8_t i = 0; i < NUM_SPRING_APF; i++) _springAPF[i].clear();
            for (uint8_t i = 0; i < 2; i++) {
                _springDelay[i].clear();
                _springLPF[i].clear();
                _springHPF[i].clear();
            }
            _springChirpLPF.clear();
            break;

        case ReverbType::CLOUD:
            for (uint8_t i = 0; i < NUM_CLOUD_APF; i++) _cloudAPF[i].clear();
            for (uint8_t i = 0; i < 2; i++) {
                _cloudDelay[i].clear();
                _cloudLPF[i].clear();
                _cloudHPF[i].clear();
            }
            break;

        default:
            break;
    }

    // Always clear shared input diffusers on type switch
    for (uint8_t i = 0; i < NUM_INPUT_DIFFUSERS; i++) {
        _inputDiffuser[i].clear();
    }
}

// =============================================================================
// update() — main audio processing callback
// =============================================================================

void AudioEffectReverbJT::update(void)
{
    // --- Bypass: zero CPU ---
    if (_bypassed || !_bufferPool) {
        audio_block_t* inL = receiveReadOnly(0);
        audio_block_t* inR = receiveReadOnly(1);
        if (inL) release(inL);
        if (inR) release(inR);
        return;
    }

    audio_block_t* inL = receiveReadOnly(0);
    audio_block_t* inR = receiveReadOnly(1);

    audio_block_t* outL = allocate();
    audio_block_t* outR = allocate();

    if (!outL || !outR) {
        if (outL) release(outL);
        if (outR) release(outR);
        if (inL)  release(inL);
        if (inR)  release(inR);
        return;
    }

    // Cache parameters
    // Freeze decay capped at 0.9999 — inaudibly close to infinite hold but
    // prevents numerical drift from accumulating into NaN/overflow over time.
    // At 44.1 kHz with 0.9999 decay, the tail loses 1 dB after ~22 seconds.
    const float    decay       = _frozen ? 0.9999f : _decay;
    const float    inputGain   = _frozen ? 0.0f : 1.0f;
    const float    wetLvl      = _wetLevel;
    const float    dryLvl      = _dryLevel;
    const uint32_t pdSmp       = _predelaySamples;
    const float    modDepthSmp = _modDepth;

    static constexpr float kToFloat = 1.0f / 32768.0f;
    static constexpr float kToInt16 = 32767.0f;

    // Per-sample loop
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {

        float inSmpL = inL ? (float)inL->data[i] * kToFloat : 0.0f;
        float inSmpR = inR ? (float)inR->data[i] * kToFloat : 0.0f;
        float monoIn = (inSmpL + inSmpR) * 0.5f * inputGain;

        // Pre-delay
        _predelay.write(monoIn);
        float pd = (pdSmp > 0) ? _predelay.read(pdSmp) : monoIn;

        // Input diffusers
        float diffused = pd;
        for (uint8_t d = 0; d < NUM_INPUT_DIFFUSERS; d++) {
            diffused = _inputDiffuser[d].process(diffused);
        }

        // LFO
        float lfo = triangleLFO();

        // Algorithm dispatch — the compiler will optimise the switch on a
        // loop-invariant variable, but even if it doesn't, a branch per
        // sample is negligible compared to the DSP work.
        switch (_type) {
            case ReverbType::PLATE:
                updatePlate(diffused, lfo, outL->data, outR->data,
                           inSmpL, inSmpR, wetLvl, dryLvl, i);
                break;
            case ReverbType::HALL:
                updateHall(diffused, lfo, outL->data, outR->data,
                          inSmpL, inSmpR, wetLvl, dryLvl, i);
                break;
            case ReverbType::SHIMMER:
                updateShimmer(diffused, lfo, outL->data, outR->data,
                             inSmpL, inSmpR, wetLvl, dryLvl, i);
                break;
            case ReverbType::SPRING:
                updateSpring(diffused, lfo, outL->data, outR->data,
                            inSmpL, inSmpR, wetLvl, dryLvl, i);
                break;
            case ReverbType::CLOUD:
                updateCloud(diffused, lfo, outL->data, outR->data,
                           inSmpL, inSmpR, wetLvl, dryLvl, i);
                break;
            default:
                outL->data[i] = (int16_t)(inSmpL * kToInt16);
                outR->data[i] = (int16_t)(inSmpR * kToInt16);
                break;
        }
    }

    transmit(outL, 0);
    transmit(outR, 1);

    release(outL);
    release(outR);
    if (inL) release(inL);
    if (inR) release(inR);
}

// =============================================================================
// PLATE — Dattorro topology (same as the original single-algorithm version)
// =============================================================================

void AudioEffectReverbJT::updatePlate(
    float diffused, float lfo, int16_t* outDataL, int16_t* outDataR,
    float inL, float inR, float wetLvl, float dryLvl, int i)
{
    const float decay = _frozen ? 0.9999f : _decay;
    static constexpr float kToInt16 = 32767.0f;
    static constexpr float kWet = 0.3f;

    // Read cross-feedback from the END of each delay, apply damping + decay.
    // Damping must be inside the feedback loop to have any audible effect —
    // it shapes what re-enters the tank, progressively darkening the tail.
    float crossFB0 = _plateDelay[1].read(_plateDelay[1].len - 1);
    crossFB0 = _plateLPF[0].process(crossFB0);
    crossFB0 = _plateHPF[0].process(crossFB0);
    crossFB0 *= decay;

    float crossFB1 = _plateDelay[0].read(_plateDelay[0].len - 1);
    crossFB1 = _plateLPF[1].process(crossFB1);
    crossFB1 = _plateHPF[1].process(crossFB1);
    crossFB1 *= decay;

    // Tank half 0: diffused input + damped cross-feedback → APF → delay
    float t0 = diffused + crossFB0;
    t0 = _plateAPF[0].processModulated(t0, lfo * _modDepth);
    _plateDelay[0].write(t0);

    // Tank half 1: diffused input + damped cross-feedback → APF → delay
    float t1 = diffused + crossFB1;
    t1 = _plateAPF[1].processModulated(t1, -lfo * _modDepth);
    _plateDelay[1].write(t1);

    // Output taps: read directly from delay lines (pre-damping = brighter)
    float wetL = (_plateDelay[0].read(PLATE_TAP_L[0])
                + _plateDelay[0].read(PLATE_TAP_L[1])
                - _plateDelay[1].read(PLATE_TAP_L[2])) * kWet;
    float wetR = (_plateDelay[1].read(PLATE_TAP_R[0])
                + _plateDelay[1].read(PLATE_TAP_R[1])
                - _plateDelay[0].read(PLATE_TAP_R[2])) * kWet;

    float oL = softClip(dryLvl * inL + wetLvl * wetL);
    float oR = softClip(dryLvl * inR + wetLvl * wetR);

    outDataL[i] = (int16_t)(oL * kToInt16);
    outDataR[i] = (int16_t)(oR * kToInt16);
}

// =============================================================================
// HALL — 8-line FDN with Hadamard-style mixing
// =============================================================================

void AudioEffectReverbJT::updateHall(
    float diffused, float lfo, int16_t* outDataL, int16_t* outDataR,
    float inL, float inR, float wetLvl, float dryLvl, int i)
{
    const float decay = _frozen ? 0.9999f : _decay;
    static constexpr float kToInt16 = 32767.0f;

    // Read from all 8 delay line outputs
    float dlOut[FDN_SIZE];
    for (uint8_t n = 0; n < FDN_SIZE; n++) {
        dlOut[n] = _fdnDelay[n].read(_fdnDelay[n].len - 1);
    }

    // Hadamard-style mixing matrix (normalised orthogonal, 8×8)
    // Using a simplified butterfly structure for CPU efficiency:
    //   Each output = sum of 4 positive and 4 negative taps, scaled by 1/√8
    // This provides maximal energy spreading between delay lines.
    static constexpr float kMixScale = 0.353553f;  // 1/sqrt(8)

    float mixed[FDN_SIZE];
    // Butterfly stage 1: pairs
    float a0 = dlOut[0] + dlOut[1]; float a1 = dlOut[0] - dlOut[1];
    float a2 = dlOut[2] + dlOut[3]; float a3 = dlOut[2] - dlOut[3];
    float a4 = dlOut[4] + dlOut[5]; float a5 = dlOut[4] - dlOut[5];
    float a6 = dlOut[6] + dlOut[7]; float a7 = dlOut[6] - dlOut[7];
    // Butterfly stage 2: quads
    float b0 = a0 + a2; float b1 = a1 + a3;
    float b2 = a0 - a2; float b3 = a1 - a3;
    float b4 = a4 + a6; float b5 = a5 + a7;
    float b6 = a4 - a6; float b7 = a5 - a7;
    // Butterfly stage 3: octets
    mixed[0] = (b0 + b4) * kMixScale;
    mixed[1] = (b1 + b5) * kMixScale;
    mixed[2] = (b2 + b6) * kMixScale;
    mixed[3] = (b3 + b7) * kMixScale;
    mixed[4] = (b0 - b4) * kMixScale;
    mixed[5] = (b1 - b5) * kMixScale;
    mixed[6] = (b2 - b6) * kMixScale;
    mixed[7] = (b3 - b7) * kMixScale;

    // Feed back into delay lines: input + mixed feedback × decay + damping
    for (uint8_t n = 0; n < FDN_SIZE; n++) {
        float fb = mixed[n] * decay;
        fb = _fdnLPF[n].process(fb);
        fb = _fdnHPF[n].process(fb);
        _fdnDelay[n].write(diffused + fb);
    }

    // Output taps: alternate lines for L/R stereo image
    // Add slight LFO modulation to the first line for width
    float wetL = (dlOut[0] + dlOut[2] + dlOut[4] + dlOut[6]) * 0.25f;
    float wetR = (dlOut[1] + dlOut[3] + dlOut[5] + dlOut[7]) * 0.25f;

    float oL = softClip(dryLvl * inL + wetLvl * wetL);
    float oR = softClip(dryLvl * inR + wetLvl * wetR);

    outDataL[i] = (int16_t)(oL * kToInt16);
    outDataR[i] = (int16_t)(oR * kToInt16);
}

// =============================================================================
// SHIMMER — Plate tank with octave-up pitch shift in feedback
// =============================================================================

void AudioEffectReverbJT::updateShimmer(
    float diffused, float lfo, int16_t* outDataL, int16_t* outDataR,
    float inL, float inR, float wetLvl, float dryLvl, int i)
{
    const float decay = _frozen ? 0.9999f : _decay;
    static constexpr float kToInt16 = 32767.0f;
    static constexpr float kWet = 0.3f;

    // --- Pitch shift the feedback from both tank halves ---
    // Write the average of both tank outputs into the shimmer buffer
    float tankAvg = (_plateDelay[0].read(_plateDelay[0].len - 1) +
                     _plateDelay[1].read(_plateDelay[1].len - 1)) * 0.5f;
    _shimmerBuf.write(tankAvg);

    // Read back at shifted rate using two overlapping grains
    // for click-free pitch shifting (dual-grain overlap-add)
    float halfBuf = (float)SHIMMER_BUF_LEN * 0.5f;

    // Grain A
    float grainA = _shimmerBuf.readCubic(_shimmerPhase);
    // Grain B (180° offset for overlap)
    float phase2 = _shimmerPhase + halfBuf;
    if (phase2 >= (float)SHIMMER_BUF_LEN) phase2 -= (float)SHIMMER_BUF_LEN;
    float grainB = _shimmerBuf.readCubic(phase2);

    // Crossfade envelope: triangular window based on grain position
    float envA = _shimmerPhase / halfBuf;
    if (envA > 1.0f) envA = 2.0f - envA;
    float envB = 1.0f - envA;

    float pitched = grainA * envA + grainB * envB;

    // Advance read phase
    _shimmerPhase += _shimmerPhaseInc;
    if (_shimmerPhase >= (float)SHIMMER_BUF_LEN) {
        _shimmerPhase -= (float)SHIMMER_BUF_LEN;
    }

    // --- Tank processing (plate topology with shimmer injection) ---
    // Damping inside the feedback loop — same fix as plate.
    float shimmerFB = pitched * _shimmerFeedbackMix;

    float crossFB0 = _plateDelay[1].read(_plateDelay[1].len - 1);
    crossFB0 = _plateLPF[0].process(crossFB0);
    crossFB0 = _plateHPF[0].process(crossFB0);
    crossFB0 *= decay;

    float crossFB1 = _plateDelay[0].read(_plateDelay[0].len - 1);
    crossFB1 = _plateLPF[1].process(crossFB1);
    crossFB1 = _plateHPF[1].process(crossFB1);
    crossFB1 *= decay;

    float t0 = diffused + crossFB0 + shimmerFB;
    t0 = _plateAPF[0].processModulated(t0, lfo * _modDepth);
    _plateDelay[0].write(t0);

    float t1 = diffused + crossFB1 + shimmerFB;
    t1 = _plateAPF[1].processModulated(t1, -lfo * _modDepth);
    _plateDelay[1].write(t1);

    float wetL = (_plateDelay[0].read(PLATE_TAP_L[0])
                + _plateDelay[0].read(PLATE_TAP_L[1])
                - _plateDelay[1].read(PLATE_TAP_L[2])) * kWet;
    float wetR = (_plateDelay[1].read(PLATE_TAP_R[0])
                + _plateDelay[1].read(PLATE_TAP_R[1])
                - _plateDelay[0].read(PLATE_TAP_R[2])) * kWet;

    float oL = softClip(dryLvl * inL + wetLvl * wetL);
    float oR = softClip(dryLvl * inR + wetLvl * wetR);

    outDataL[i] = (int16_t)(oL * kToInt16);
    outDataR[i] = (int16_t)(oR * kToInt16);
}

// =============================================================================
// SPRING — short allpass cascade with band-limited recirculation
// =============================================================================

void AudioEffectReverbJT::updateSpring(
    float diffused, float lfo, int16_t* outDataL, int16_t* outDataR,
    float inL, float inR, float wetLvl, float dryLvl, int i)
{
    const float decay = _frozen ? 0.999f : _decay;  // spring can't truly freeze
    static constexpr float kToInt16 = 32767.0f;

    // Read feedback from delay outputs
    float fbL = _springDelay[0].read(_springDelay[0].len - 1) * decay;
    float fbR = _springDelay[1].read(_springDelay[1].len - 1) * decay;

    // Band-limit feedback for the metallic "boing" character
    fbL = _springLPF[0].process(fbL);
    fbL = _springHPF[0].process(fbL);
    fbR = _springLPF[1].process(fbR);
    fbR = _springHPF[1].process(fbR);

    // "Chirp" filter — emphasises the spring's resonant drip sound
    float chirp = _springChirpLPF.process(diffused);

    // Run through cascaded allpass chain (creates the dispersive spring sound)
    float ap = chirp + (fbL + fbR) * 0.5f;
    for (uint8_t n = 0; n < NUM_SPRING_APF; n++) {
        // Slight LFO modulation on alternating stages for organic feel
        if (n & 1) {
            ap = _springAPF[n].processModulated(ap, lfo * _modDepth * 0.3f);
        } else {
            ap = _springAPF[n].process(ap);
        }
    }

    // Split to stereo with slight offset
    _springDelay[0].write(ap);
    _springDelay[1].write(-ap * 0.95f);  // inverted + scaled for stereo width

    float wetL = _springDelay[0].read(_springDelay[0].len / 3);
    float wetR = _springDelay[1].read(_springDelay[1].len / 3);

    // Springs have less overall level
    static constexpr float kSpringWet = 0.5f;
    wetL *= kSpringWet;
    wetR *= kSpringWet;

    float oL = softClip(dryLvl * inL + wetLvl * wetL);
    float oR = softClip(dryLvl * inR + wetLvl * wetR);

    outDataL[i] = (int16_t)(oL * kToInt16);
    outDataR[i] = (int16_t)(oR * kToInt16);
}

// =============================================================================
// CLOUD — dense parallel diffusion for ambient wash / frozen textures
// =============================================================================

void AudioEffectReverbJT::updateCloud(
    float diffused, float lfo, int16_t* outDataL, int16_t* outDataR,
    float inL, float inR, float wetLvl, float dryLvl, int i)
{
    const float decay = _frozen ? 0.9999f : _decay;
    static constexpr float kToInt16 = 32767.0f;

    // Read long delay feedback
    float fbL = _cloudDelay[0].read(_cloudDelay[0].len - 1) * decay;
    float fbR = _cloudDelay[1].read(_cloudDelay[1].len - 1) * decay;

    // Damp the feedback
    fbL = _cloudLPF[0].process(fbL);
    fbL = _cloudHPF[0].process(fbL);
    fbR = _cloudLPF[1].process(fbR);
    fbR = _cloudHPF[1].process(fbR);

    // Run through dense parallel allpass network
    // Split into two groups of 4 for L and R decorrelation
    float cloudL = diffused + fbL;
    float cloudR = diffused + fbR;

    // Left group: APF 0,1,2,3 in series
    for (uint8_t n = 0; n < 4; n++) {
        // Alternate modulated and static for complex density
        if (n & 1) {
            cloudL = _cloudAPF[n].processModulated(cloudL, lfo * _modDepth * 0.5f);
        } else {
            cloudL = _cloudAPF[n].process(cloudL);
        }
    }

    // Right group: APF 4,5,6,7 in series (inverted LFO)
    for (uint8_t n = 4; n < 8; n++) {
        if (n & 1) {
            cloudR = _cloudAPF[n].processModulated(cloudR, -lfo * _modDepth * 0.5f);
        } else {
            cloudR = _cloudAPF[n].process(cloudR);
        }
    }

    // Write to long delay lines
    _cloudDelay[0].write(cloudL);
    _cloudDelay[1].write(cloudR);

    // Output taps: multiple positions for diffuse stereo image
    float wetL = (_cloudDelay[0].read(_cloudDelay[0].len / 3)
                + _cloudDelay[0].read(_cloudDelay[0].len * 2 / 3)) * 0.35f;
    float wetR = (_cloudDelay[1].read(_cloudDelay[1].len / 3)
                + _cloudDelay[1].read(_cloudDelay[1].len * 2 / 3)) * 0.35f;

    float oL = softClip(dryLvl * inL + wetLvl * wetL);
    float oR = softClip(dryLvl * inR + wetLvl * wetR);

    outDataL[i] = (int16_t)(oL * kToInt16);
    outDataR[i] = (int16_t)(oR * kToInt16);
}
