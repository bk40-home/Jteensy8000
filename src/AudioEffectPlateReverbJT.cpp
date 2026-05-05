/*
 * AudioEffectPlateReverbJT.cpp
 * ============================
 * Dattorro-style stereo plate reverb for Teensy 4.1 with PSRAM.
 * Extended with shimmer, reverb pitch shifting, master output EQ,
 * enhanced freeze, and diffusion control.
 *
 * Copyright (c) 2021 Piotr Zapart (hexefx) — original Dattorro plate algorithm
 * Copyright (c) 2024 Kris Bishop       — JT-8000 implementation, extensions
 *
 * See AudioEffectPlateReverbJT.h for topology diagram, design notes,
 * and full licence text.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * DELAY LINE LENGTHS (samples at 44.1 kHz):
 *
 *   Pre-delay:         11025 samples =  44.1 KB  (250 ms max)  → PSRAM
 *   Input diffusers:   142 + 107 + 379 + 277 =   3.6 KB        → DTCM
 *   Tank APF[0]:        1800 samples =   7.2 KB                → PSRAM
 *   Tank APF[1]:        2656 samples =  10.6 KB                → PSRAM
 *   Tank delay[0]:      3720 samples =  14.9 KB                → PSRAM
 *   Tank delay[1]:      4217 samples =  16.9 KB                → PSRAM
 *   PitchShifter ×4:  4×4096 samples =  65.5 KB                → PSRAM
 *   ──────────────────────────────────────────────
 *   PSRAM TOTAL:        39707 floats = 158.8 KB
 *   DTCM TOTAL:           905 floats =   3.6 KB
 *
 * CPU OPTIMISATIONS (vs previous version):
 *   1. Input diffusers moved from PSRAM → DTCM.
 *      Saves 8 random PSRAM accesses/sample (~4% CPU at 720 MHz).
 *   2. PitchShifter::process() skips PSRAM write when bypassed (mix==0).
 *      Saves 4 PSRAM writes/sample when shimmer + pitch are off (~2% CPU).
 *   3. Pre-delay bypassed entirely when set to 0 ms.
 *      Saves 2 PSRAM accesses/sample (~1% CPU).
 *   Total saving: ~7% CPU. Reverb floor ~8-12% depending on PSRAM speed.
 *
 * OUTPUT TAP POSITIONS (samples behind write head):
 *   Left output:  TAP_L0=266 (early), TAP_L1=2974 (late), TAP_L2=1913 (cross)
 *   Right output: TAP_R0=353,          TAP_R1=3627,         TAP_R2=1228
 *
 * These are empirically tuned for wide stereo decorrelation.
 *
 * REFERENCES:
 *   [1] Jon Dattorro, "Effect Design Part 1", JAES 1997, §2 Plate Reverb
 *   [2] Piotr Zapart (hexefx), AudioEffectPlateReverb_F32 (MIT licence)
 */

#include "AudioEffectPlateReverbJT.h"
#include "DebugTrace.h"
#include <cmath>

// =============================================================================
// STATIC LOOKUP TABLES
// =============================================================================

// ─── Semitone ratio table (9 musically useful shimmer intervals) ─────────────
// Used by shimmerPitchNormalized() and pitchNormalized().
// CC value 0..127 → index 0..8 via:  index = (uint8_t)(norm * 8.999f)
const int8_t AudioEffectPlateReverbJT::kShimmerSemitoneTable[9] = {
    -12, -7, -5, -3, 0, 3, 5, 7, 12
};

// ─── Pitch shifter: full semitone ratio table (-12 to +24) ──────────────────
// Index = semitones + 12.  Values: exact 12-TET ratios from hexefx wavetables.c.
const float AudioEffectPlateReverbJT::PitchShifter::kSemitoneRatios[37] = {
    // -12   -11      -10      -9       -8       -7       -6       -5
    0.500000f, 0.529732f, 0.561231f, 0.594604f, 0.629961f, 0.667420f, 0.707107f, 0.749154f,
    // -4    -3       -2       -1        0       +1       +2       +3
    0.793701f, 0.840896f, 0.890899f, 0.943874f, 1.000000f, 1.059463f, 1.122462f, 1.189207f,
    // +4    +5       +6       +7       +8       +9      +10      +11
    1.259921f, 1.334840f, 1.414214f, 1.498307f, 1.587401f, 1.681793f, 1.781797f, 1.887749f,
    // +12   +13      +14      +15      +16      +17     +18      +19
    2.000000f, 2.118926f, 2.244924f, 2.378414f, 2.519842f, 2.669680f, 2.828427f, 2.996614f,
    // +20   +21      +22      +23      +24
    3.174802f, 3.363586f, 3.563595f, 3.775497f, 4.000000f
};

// ─── Pitch shifter: raised-cosine crossfade window (257 entries) ─────────────
// Entry [i] = 0.5 * (1 - cos(π × i / 255)).  [256] = 1.0 (safe over-read).
// Used to blend the two 180°-offset read pointers of the pitch shifter.
// Source: hexefx wavetables.c AudioWaveformFader_f32[].
const float AudioEffectPlateReverbJT::PitchShifter::kFadeTable[257] = {
    0.000000f, 0.003075f, 0.006187f, 0.009336f, 0.012522f, 0.015745f, 0.019004f, 0.022300f,
    0.025633f, 0.029002f, 0.032407f, 0.035848f, 0.039324f, 0.042837f, 0.046384f, 0.049967f,
    0.053585f, 0.057238f, 0.060925f, 0.064647f, 0.068403f, 0.072193f, 0.076016f, 0.079873f,
    0.083764f, 0.087687f, 0.091643f, 0.095632f, 0.099653f, 0.103706f, 0.107791f, 0.111907f,
    0.116055f, 0.120233f, 0.124442f, 0.128681f, 0.132951f, 0.137250f, 0.141578f, 0.145936f,
    0.150322f, 0.154737f, 0.159180f, 0.163650f, 0.168148f, 0.172674f, 0.177226f, 0.181804f,
    0.186409f, 0.191039f, 0.195695f, 0.200375f, 0.205080f, 0.209810f, 0.214563f, 0.219340f,
    0.224139f, 0.228962f, 0.233806f, 0.238673f, 0.243561f, 0.248470f, 0.253400f, 0.258350f,
    0.263320f, 0.268309f, 0.273317f, 0.278343f, 0.283388f, 0.288450f, 0.293530f, 0.298626f,
    0.303738f, 0.308867f, 0.314011f, 0.319169f, 0.324342f, 0.329529f, 0.334730f, 0.339944f,
    0.345170f, 0.350408f, 0.355658f, 0.360920f, 0.366191f, 0.371473f, 0.376765f, 0.382066f,
    0.387375f, 0.392693f, 0.398018f, 0.403351f, 0.408690f, 0.414035f, 0.419386f, 0.424742f,
    0.430103f, 0.435468f, 0.440836f, 0.446208f, 0.451582f, 0.456958f, 0.462336f, 0.467714f,
    0.473093f, 0.478472f, 0.483851f, 0.489228f, 0.494604f, 0.499977f, 0.505348f, 0.510715f,
    0.516079f, 0.521438f, 0.526793f, 0.532142f, 0.537485f, 0.542821f, 0.548151f, 0.553473f,
    0.558787f, 0.564093f, 0.569389f, 0.574675f, 0.579952f, 0.585217f, 0.590471f, 0.595713f,
    0.600943f, 0.606160f, 0.611364f, 0.616553f, 0.621728f, 0.626888f, 0.632032f, 0.637160f,
    0.642271f, 0.647366f, 0.652444f, 0.657500f, 0.662539f, 0.667559f, 0.672559f, 0.677539f,
    0.682498f, 0.687435f, 0.692351f, 0.697244f, 0.702114f, 0.706960f, 0.711782f, 0.716580f,
    0.721353f, 0.726100f, 0.730822f, 0.735516f, 0.740184f, 0.744824f, 0.749436f, 0.754020f,
    0.758574f, 0.763099f, 0.767594f, 0.772058f, 0.776492f, 0.780894f, 0.785264f, 0.789602f,
    0.793907f, 0.798178f, 0.802416f, 0.806619f, 0.810788f, 0.814922f, 0.819020f, 0.823082f,
    0.827107f, 0.831096f, 0.835047f, 0.838960f, 0.842835f, 0.846672f, 0.850469f, 0.854227f,
    0.857945f, 0.861623f, 0.865260f, 0.868856f, 0.872411f, 0.875924f, 0.879394f, 0.882822f,
    0.886206f, 0.889548f, 0.892845f, 0.896099f, 0.899308f, 0.902472f, 0.905591f, 0.908664f,
    0.911692f, 0.914673f, 0.917608f, 0.920496f, 0.923336f, 0.926129f, 0.928875f, 0.931572f,
    0.934221f, 0.936821f, 0.939373f, 0.941875f, 0.944327f, 0.946730f, 0.949082f, 0.951384f,
    0.953636f, 0.955836f, 0.957986f, 0.960084f, 0.962131f, 0.964126f, 0.966069f, 0.967959f,
    0.969797f, 0.971583f, 0.973315f, 0.974995f, 0.976621f, 0.978194f, 0.979714f, 0.981179f,
    0.982591f, 0.983948f, 0.985252f, 0.986501f, 0.987695f, 0.988835f, 0.989920f, 0.990950f,
    0.991925f, 0.992845f, 0.993709f, 0.994519f, 0.995272f, 0.995971f, 0.996614f, 0.997201f,
    0.997732f, 0.998208f, 0.998628f, 0.998992f, 0.999300f, 0.999552f, 0.999748f, 1.000000f,
    1.000000f   // [256] — safe over-read guard entry
};

// =============================================================================
// DELAY LINE LENGTHS — prime-valued to decorrelate reflections
// =============================================================================
// Scaled up from Dattorro 1997 for a "massive hall" character.
// Pre-delay: 250 ms max at 44.1 kHz
static constexpr uint32_t PREDELAY_MAX_SAMPLES = 11025;

// Input diffuser lengths are declared in the header (IDIFF_LEN_0..3)
// because the DTCM buffer array size must be known at compile time.

// Tank modulated allpass filters
static constexpr uint32_t TANK_APF_LEN_0 = 1800;  // ~40.8 ms
static constexpr uint32_t TANK_APF_LEN_1 = 2656;  // ~60.2 ms

// Tank delay lines
static constexpr uint32_t TANK_DLY_LEN_0 = 3720;  // ~84.4 ms
static constexpr uint32_t TANK_DLY_LEN_1 = 4217;  // ~95.6 ms

// PitchShifter buffer size (4 instances × 4096 floats each).
static constexpr uint32_t PITCH_BUF_TOTAL = 4u * 4096u;  // 16384 floats = 65.5 KB

// PSRAM pool size — everything EXCEPT input diffusers (those are in DTCM).
static constexpr uint32_t PSRAM_BUFFER_SAMPLES =
    PREDELAY_MAX_SAMPLES +
    TANK_APF_LEN_0 + TANK_APF_LEN_1 +
    TANK_DLY_LEN_0 + TANK_DLY_LEN_1 +
    PITCH_BUF_TOTAL;

// =============================================================================
// DEFAULT COEFFICIENTS
// =============================================================================

// Input diffuser allpass gains (Dattorro 1997 §2)
static constexpr float IDIFF_GAIN_0 = 0.75f;
static constexpr float IDIFF_GAIN_1 = 0.75f;
static constexpr float IDIFF_GAIN_2 = 0.625f;
static constexpr float IDIFF_GAIN_3 = 0.625f;

// Tank allpass gains — lower than input diffusers for stability at high decay
static constexpr float TANK_APF_GAIN = 0.60f;

// Default diffusion coefficient used by diffusion() at its midpoint
static constexpr float DIFFUSION_DEFAULT = 0.65f;

// Output tap positions for decorrelated stereo (samples behind write head)
static constexpr uint32_t TAP_L0 =  266;   // early reflection, tank 0
static constexpr uint32_t TAP_L1 = 2974;   // late reflection,  tank 0
static constexpr uint32_t TAP_L2 = 1913;   // cross-tank contribution

static constexpr uint32_t TAP_R0 =  353;
static constexpr uint32_t TAP_R1 = 3627;
static constexpr uint32_t TAP_R2 = 1228;

// Wet signal normalisation factor — the 3-tap sum can exceed 1.0;
// this scale factor is tuned empirically to keep sustained polyphonic
// input below digital full-scale.
static constexpr float kWetScale = 0.3f;

// Int16 ↔ float conversion factors
static constexpr float kToFloat = 1.0f / 32768.0f;
static constexpr float kToInt16 = 32767.0f;

// =============================================================================
// CONSTRUCTOR
// =============================================================================

AudioEffectPlateReverbJT::AudioEffectPlateReverbJT()
    : AudioStream(2, inputQueueArray)
    , _decay(0.7f)
    , _tank0fb(0.0f)
    , _tank1fb(0.0f)
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
    , _diffusionCoeff(DIFFUSION_DEFAULT)
    , _masterLpCoeff(0.0f)
    , _masterHpCoeff(0.0f)
    , _shimmerMix(0.0f)
    , _shimmerSemitones(12)     // default shimmer: octave up
    , _pitchMixAmount(0.0f)
    , _pitchSemitones(0)
    , _savedDecay(0.7f)
    , _savedHiDampCoeff(0.3f)
    , _savedLoDampCoeff(0.0f)
    , _savedShimmerMix(0.0f)
    , _freezeBleedGain(0.0f)
    , _bufferPool(nullptr)
    , _bufferPoolSize(0)
{
    // Zero the DTCM diffuser buffer before assigning sub-regions
    memset(_diffuserBuf, 0, sizeof(_diffuserBuf));

    if (allocateBuffers()) {
        assignBuffers();

        // ── Input diffuser gains ──────────────────────────────────────────────
        _inputDiffuser[0].gain = IDIFF_GAIN_0;
        _inputDiffuser[1].gain = IDIFF_GAIN_1;
        _inputDiffuser[2].gain = IDIFF_GAIN_2;
        _inputDiffuser[3].gain = IDIFF_GAIN_3;

        // ── Tank allpass and damping filter init ──────────────────────────────
        _tankAPF[0].gain = TANK_APF_GAIN;
        _tankAPF[1].gain = TANK_APF_GAIN;

        for (uint8_t i = 0; i < 2; i++) {
            _tankLPF[i].state = 0.0f;
            _tankLPF[i].coeff = _hiDampCoeff;
            _tankHPF[i].state = 0.0f;
            _tankHPF[i].coeff = _loDampCoeff;
            _masterLPF[i].state = 0.0f;
            _masterLPF[i].coeff = 0.0f;    // bypass — no filtering at init
            _masterHPF[i].state = 0.0f;
            _masterHPF[i].coeff = 0.0f;
        }

        // ── Pitch shifter init ────────────────────────────────────────────────
        // Reverb pitch: unity pitch, mix off (no CPU until enabled)
        _pitchL.setPitch(1.0f);
        _pitchR.setPitch(1.0f);
        _pitchL.setMix(0.0f);
        _pitchR.setMix(0.0f);

        // Shimmer: default octave up, mix off (no CPU until enabled)
        _pitchShimL.setPitch(2.0f);
        _pitchShimR.setPitch(2.0f);
        _pitchShimL.setMix(0.0f);
        _pitchShimR.setMix(0.0f);

        // ── LFO ───────────────────────────────────────────────────────────────
        updateModRate();

        JT_LOGF("[PlateReverbJT] PSRAM: %u KB, DTCM: %u bytes (diffusers)\n",
                (unsigned)(PSRAM_BUFFER_SAMPLES * sizeof(float) / 1024),
                (unsigned)(IDIFF_TOTAL * sizeof(float)));
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
// PSRAM pool holds: predelay + tank APFs + tank delays + pitch shifters.
// Input diffusers are in the DTCM member array _diffuserBuf — no allocation.

bool AudioEffectPlateReverbJT::allocateBuffers()
{
    _bufferPoolSize = PSRAM_BUFFER_SAMPLES;
    const uint32_t totalBytes = _bufferPoolSize * sizeof(float);

    // Prefer PSRAM on Teensy 4.x — avoids heap fragmentation
#if defined(__IMXRT1062__)
    _bufferPool = (float*)extmem_malloc(totalBytes);
    if (_bufferPool) {
        JT_LOGF("[PlateReverbJT] Using PSRAM (%u KB)\n",
                (unsigned)(totalBytes / 1024));
    } else {
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

    // Zero the whole pool — IEEE 754 +0.0f = all zero bits, so memset is valid
    memset(_bufferPool, 0, totalBytes);
    return true;
}

void AudioEffectPlateReverbJT::freeBuffers()
{
    if (_bufferPool) {
        // extmem_malloc and malloc both use free() on Teensy 4.x
        free(_bufferPool);
        _bufferPool     = nullptr;
        _bufferPoolSize = 0;
    }
}

void AudioEffectPlateReverbJT::assignBuffers()
{
    // ── Input diffusers → DTCM (fast RAM) ─────────────────────────────────
    // Point each diffuser's delay line into the contiguous _diffuserBuf array.
    // These run at DTCM speed (~2ns/access) instead of PSRAM (~100-150ns).
    static constexpr uint32_t idiffLens[4] = {
        IDIFF_LEN_0, IDIFF_LEN_1, IDIFF_LEN_2, IDIFF_LEN_3
    };
    float* dtcmPtr = _diffuserBuf;
    for (uint8_t i = 0; i < NUM_INPUT_DIFFUSERS; i++) {
        _inputDiffuser[i].dl.buf      = dtcmPtr;
        _inputDiffuser[i].dl.len      = idiffLens[i];
        _inputDiffuser[i].dl.writeIdx = 0;
        dtcmPtr += idiffLens[i];
    }

    // ── Everything else → PSRAM pool ──────────────────────────────────────
    float* ptr = _bufferPool;

    // Pre-delay
    _predelay.buf      = ptr;
    _predelay.len      = PREDELAY_MAX_SAMPLES;
    _predelay.writeIdx = 0;
    ptr += PREDELAY_MAX_SAMPLES;

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

    // PitchShifter buffers (4 × 4096 floats)
    // Each assign() call zeroes the buffer and resets all pointers to unity pitch.
    _pitchL.assign(ptr);    ptr += PitchShifter::BUF_SIZE;
    _pitchR.assign(ptr);    ptr += PitchShifter::BUF_SIZE;
    _pitchShimL.assign(ptr); ptr += PitchShifter::BUF_SIZE;
    _pitchShimR.assign(ptr); ptr += PitchShifter::BUF_SIZE;
}

// =============================================================================
// PARAMETER SETTERS
// =============================================================================
// All setters: clamp at the boundary so the audio path never sees NaN/inf.
// Parameters that are read per-sample in update() are cached at the start
// of each block — no need for __disable_irq() guards because Teensy Audio
// Library callbacks are always in the same interrupt context.

void AudioEffectPlateReverbJT::size(float n)
{
    n = constrain(n, 0.0f, 1.0f);
    // Blend of linear and quadratic: decay = 0.1 + 0.8995 × (0.5n + 0.5n²)
    //
    // This spreads the musically useful range across the full knob:
    //   CC=0   → decay=0.10  (tight, short room)
    //   CC=64  → decay=0.44  (medium hall)
    //   CC=96  → decay=0.70  (large hall)
    //   CC=110 → decay=0.83  (very long tail)
    //   CC=127 → decay=1.00  (infinite hold)
    //
    // Old quadratic (n²) compressed most of the range into CC 100-127,
    // making the lower 80% of the knob near-inaudible.
    const float shaped = 0.5f * n + 0.5f * n * n;
    _decay = 0.1f + 0.8995f * shaped;
}

void AudioEffectPlateReverbJT::hidamp(float n)
{
    n = constrain(n, 0.0f, 1.0f);
    // Same compounding problem as lodamp: coeff=0.0075 at CC=1 gives a
    // 53 Hz LPF per circulation — instantly kills all high frequencies.
    // Apply the same offset mapping so CC=1 gives a gentle darkening:
    //   n=0   → coeff=0.0  (bypass → fully transparent, bright tail)
    //   n>0   → coeff starts at 0.1, scales to 0.95 at n=1
    //   CC=1  → -3dB ~750 Hz  (subtle darkening)
    //   CC=64 → -3dB ~3.5 kHz (warm, darker tail)
    //   CC=127→ -3dB ~6.7 kHz (very dark, damp room character)
    _hiDampCoeff = (n > 0.001f) ? (0.1f + 0.85f * n) : 0.0f;
    if (_hiDampCoeff > 0.95f) _hiDampCoeff = 0.95f;
    _tankLPF[0].coeff = _hiDampCoeff;
    _tankLPF[1].coeff = _hiDampCoeff;
}

void AudioEffectPlateReverbJT::lodamp(float n)
{
    n = constrain(n, 0.0f, 1.0f);
    // The tank HPF is inside the feedback loop — its effect compounds on every
    // circulation. A tiny coeff (e.g. 0.0075 at CC=1) cuts nearly everything
    // below 53 Hz per pass, killing the tail within milliseconds.
    //
    // Fix: map the useful range linearly from an audible starting point:
    //   n=0   → coeff=0.0  (bypass guard fires → fully transparent)
    //   n>0   → coeff jumps to 0.1 then scales to 0.9 at n=1
    //   CC=1  → -3dB ~750 Hz  (gentle bass trim)
    //   CC=64 → -3dB ~3.5 kHz (moderate bass cut)
    //   CC=127→ -3dB ~6.3 kHz (strong bass cut, thinning tail)
    _loDampCoeff = (n > 0.001f) ? (0.1f + 0.8f * n) : 0.0f;
    if (_loDampCoeff > 0.90f) _loDampCoeff = 0.90f;
    _tankHPF[0].coeff = _loDampCoeff;
    _tankHPF[1].coeff = _loDampCoeff;
}

void AudioEffectPlateReverbJT::mix(float n)
{
    n = constrain(n, 0.0f, 1.0f);
    _wetLevel = n;
    _dryLevel = 1.0f - n;
}

void AudioEffectPlateReverbJT::bypass_set(bool state)
{
    _bypassed = state;
}

void AudioEffectPlateReverbJT::predelay(float ms)
{
    ms = constrain(ms, 0.0f, 250.0f);
    _predelaySamples = (uint32_t)(ms * (AUDIO_SAMPLE_RATE_EXACT * 0.001f));
    // Safety clamp against buffer length
    if (_predelaySamples >= PREDELAY_MAX_SAMPLES) {
        _predelaySamples = PREDELAY_MAX_SAMPLES - 1;
    }
}

void AudioEffectPlateReverbJT::modDepth(float n)
{
    n = constrain(n, 0.0f, 1.0f);
    // 0..1 → 0..16 samples of modulation excursion (~0..0.36 ms at 44.1 kHz)
    _modDepth = n * 16.0f;
}

void AudioEffectPlateReverbJT::modRate(float hz)
{
    hz = constrain(hz, 0.1f, 5.0f);
    _modRate = hz;
    updateModRate();
}

void AudioEffectPlateReverbJT::lowpass(float n)
{
    n = constrain(n, 0.0f, 1.0f);
    // Map 0..1 → 0..0.97.
    // n=0 → coeff=0: OnePole_LP at coeff=0 passes input unchanged (transparent)
    // n=1 → coeff=0.97: heavy treble cut on the wet output
    _masterLpCoeff = n * 0.97f;
    _masterLPF[0].coeff = _masterLpCoeff;
    _masterLPF[1].coeff = _masterLpCoeff;
}

void AudioEffectPlateReverbJT::hipass(float n)
{
    n = constrain(n, 0.0f, 1.0f);
    // The OnePole_HP only produces audible output when coeff is above ~0.3.
    // Below that, the LP state tracks input so closely that output ≈ 0.
    // So we map the full 0..1 range into the useful coeff range 0..0.97:
    //   n=0   → coeff=0.0  (bypass guard fires → transparent, no filtering)
    //   n=0.1 → coeff=0.30 (first audible bass cut)
    //   n=0.5 → coeff=0.63 (moderate bass cut)
    //   n=1.0 → coeff=0.97 (heavy bass cut)
    // Uses a square-root curve so the lower half of the knob is more useful.
    _masterHpCoeff = (n > 0.001f) ? (0.3f + 0.67f * sqrtf(n)) : 0.0f;
    // Clamp to valid range — sqrtf(1.0) = 1.0, so max = 0.3 + 0.67 = 0.97
    if (_masterHpCoeff > 0.97f) _masterHpCoeff = 0.97f;
    _masterHPF[0].coeff = _masterHpCoeff;
    _masterHPF[1].coeff = _masterHpCoeff;
}

void AudioEffectPlateReverbJT::diffusion(float n)
{
    n = constrain(n, 0.0f, 1.0f);
    // Map 0..1 → 0.005..0.65 (matching F32 reference)
    // Low values: sparse, discrete echoes.  High values: dense wash.
    _diffusionCoeff = 0.005f + n * (0.65f - 0.005f);

    // Apply to input diffusers and tank allpass simultaneously
    for (uint8_t i = 0; i < NUM_INPUT_DIFFUSERS; i++) {
        _inputDiffuser[i].gain = _diffusionCoeff;
    }
    _tankAPF[0].gain = _diffusionCoeff;
    _tankAPF[1].gain = _diffusionCoeff;
}

void AudioEffectPlateReverbJT::freeze(bool state)
{
    if (_frozen == state) return;   // no-op if already in target state
    _frozen = state;

    if (state) {
        // ── Enter freeze ─────────────────────────────────────────────────────
        // Save current parameters so we can restore them on unfreeze
        _savedDecay       = _decay;
        _savedHiDampCoeff = _hiDampCoeff;
        _savedLoDampCoeff = _loDampCoeff;
        _savedShimmerMix  = _shimmerMix;

        // Infinite decay — tail holds forever
        _decay = 1.0f;

        // Full brightness: no damping in the frozen tail
        _hiDampCoeff = 0.0f;
        _loDampCoeff = 0.0f;
        _tankLPF[0].coeff = 0.0f;
        _tankLPF[1].coeff = 0.0f;
        _tankHPF[0].coeff = 0.0f;
        _tankHPF[1].coeff = 0.0f;

        // Disable shimmer during freeze — prevents runaway pitch escalation
        _pitchShimL.setMix(0.0f);
        _pitchShimR.setMix(0.0f);

    } else {
        // ── Exit freeze ──────────────────────────────────────────────────────
        // Restore saved parameters
        _decay       = _savedDecay;
        _hiDampCoeff = _savedHiDampCoeff;
        _loDampCoeff = _savedLoDampCoeff;

        // Restore tank filter coefficients
        _tankLPF[0].coeff = _hiDampCoeff;
        _tankLPF[1].coeff = _hiDampCoeff;
        _tankHPF[0].coeff = _loDampCoeff;
        _tankHPF[1].coeff = _loDampCoeff;

        // Re-enable shimmer at its pre-freeze level
        _pitchShimL.setMix(_savedShimmerMix);
        _pitchShimR.setMix(_savedShimmerMix);
    }
}

void AudioEffectPlateReverbJT::freezeBleedIn(float b)
{
    b = constrain(b, 0.0f, 1.0f);
    // Map 0..1 → 0..0.1 to keep bleed-in small enough to avoid oscillation
    _freezeBleedGain = b * 0.1f;
}

void AudioEffectPlateReverbJT::shimmer(float s)
{
    if (_frozen) return;   // no update while frozen — freeze() handles this
    s = constrain(s, 0.0f, 1.0f);
    // Quadratic curve for musically useful control taper
    s = 2.0f * s - s * s;
    _shimmerMix = s;
    _pitchShimL.setMix(s);
    _pitchShimR.setMix(s);
}

void AudioEffectPlateReverbJT::shimmerPitchSemitones(int8_t semitones)
{
    semitones = constrain(semitones, -12, +12);
    _shimmerSemitones = semitones;
    _pitchShimL.setPitchSemitones(semitones);
    _pitchShimR.setPitchSemitones(semitones);
}

void AudioEffectPlateReverbJT::shimmerPitchNormalized(float value)
{
    value = constrain(value, 0.0f, 1.0f);
    // Map 0..1 → 0..8 (9-entry semitone table)
    uint8_t idx = (uint8_t)(value * 8.999f);
    shimmerPitchSemitones(kShimmerSemitoneTable[idx]);
}

void AudioEffectPlateReverbJT::pitchSemitones(int8_t semitones)
{
    semitones = constrain(semitones, -12, +24);
    _pitchSemitones = semitones;
    _pitchL.setPitchSemitones(semitones);
    _pitchR.setPitchSemitones(semitones);
}

void AudioEffectPlateReverbJT::pitchNormalized(float value)
{
    value = constrain(value, 0.0f, 1.0f);
    uint8_t idx = (uint8_t)(value * 8.999f);
    pitchSemitones(kShimmerSemitoneTable[idx]);
}

void AudioEffectPlateReverbJT::pitchMix(float s)
{
    s = constrain(s, 0.0f, 1.0f);
    _pitchMixAmount = s;
    _pitchL.setMix(s);
    _pitchR.setMix(s);
}

// =============================================================================
// PRIVATE HELPERS
// =============================================================================

void AudioEffectPlateReverbJT::updateModRate()
{
    // Phase increment per sample: one full cycle (0..1) in 1/modRate seconds
    _modPhaseInc = _modRate / AUDIO_SAMPLE_RATE_EXACT;
}

// =============================================================================
// update() — main audio processing callback
//
// Called by the Teensy Audio Library every AUDIO_BLOCK_SAMPLES (128) samples
// (~2.9 ms at 44.1 kHz).
//
// Signal flow per sample:
//   1.  Sum stereo int16 input → mono float
//   2.  Apply input gain (1.0 normal / bleed-in during freeze / 0.0 muted)
//   3.  Pre-delay (skipped entirely when predelaySamples == 0)
//   4.  Input diffuser chain (4× allpass) — runs in DTCM, no PSRAM cost
//   5.  Reverb pitch shifters (pitchL / pitchR) — zero CPU when mix==0
//   6.  Tank half 0: shimmerR → APF(mod) → delay → LPF → HPF → × decay
//   7.  Tank half 1: shimmerL → APF(mod) → delay → LPF → HPF → × decay
//       (halves cross-feed into each other each sample)
//   8.  Decorrelated output taps (3 per channel)
//   9.  Master LP/HP output EQ — zero CPU when coefficients are 0
//  10.  Scale, wet/dry blend, soft-clip, → int16 output
// =============================================================================

void AudioEffectPlateReverbJT::update(void)
{
    // ── Bypass: zero CPU path ─────────────────────────────────────────────────
    // Still receive/release input blocks to prevent upstream graph stalls.
    if (_bypassed || !_bufferPool) {
        audio_block_t* inL = receiveReadOnly(0);
        audio_block_t* inR = receiveReadOnly(1);
        if (inL) release(inL);
        if (inR) release(inR);
        return;
    }

    // ── Receive stereo input ──────────────────────────────────────────────────
    audio_block_t* inL  = receiveReadOnly(0);
    audio_block_t* inR  = receiveReadOnly(1);
    audio_block_t* outL = allocate();
    audio_block_t* outR = allocate();

    if (!outL || !outR) {
        // Audio memory exhausted — release all blocks and bail gracefully
        if (outL) release(outL);
        if (outR) release(outR);
        if (inL)  release(inL);
        if (inR)  release(inR);
        return;
    }

    // ── Cache block-constant parameters ──────────────────────────────────────
    // Reading member variables per-sample is slower than a local variable
    // (extra pointer dereference + potential cache miss). Cache once per block.
    const float    decay         = _frozen ? 1.0f : _decay;
    const float    inputGain     = _frozen ? _freezeBleedGain : 1.0f;
    const float    wetLevel      = _wetLevel;
    const float    dryLevel      = _dryLevel;
    const uint32_t predelaySmp   = _predelaySamples;
    const float    modDepthSmp   = _modDepth;

    // Cache master EQ coefficients — skip entire filter when at bypass value.
    // Checking once per block avoids 128 branch evaluations per filter.
    const bool     doMasterLP    = (_masterLpCoeff > 0.001f);
    const bool     doMasterHP    = (_masterHpCoeff > 0.001f);

    // ── Per-sample processing loop ────────────────────────────────────────────
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {

        // ── Convert int16 input to float; sum to mono ─────────────────────────
        float inSampleL = inL ? (float)inL->data[i] * kToFloat : 0.0f;
        float inSampleR = inR ? (float)inR->data[i] * kToFloat : 0.0f;
        float monoIn    = (inSampleL + inSampleR) * 0.5f * inputGain;

        // ── Pre-delay ─────────────────────────────────────────────────────────
        // When predelay is 0, skip both the write and read to PSRAM entirely.
        // This saves 2 random PSRAM accesses per sample (~1% CPU).
        float predelayed;
        if (predelaySmp > 0) {
            _predelay.write(monoIn);
            predelayed = _predelay.read(predelaySmp);
        } else {
            predelayed = monoIn;
        }

        // ── Input diffuser chain (4× allpass) ─────────────────────────────────
        // These buffers are in DTCM — ~2ns per access instead of ~100-150ns
        // in PSRAM. This saves 8 PSRAM accesses/sample (~4% CPU).
        float diffused = predelayed;
        for (uint8_t d = 0; d < NUM_INPUT_DIFFUSERS; d++) {
            diffused = _inputDiffuser[d].process(diffused);
        }

        // ── Reverb pitch shifters ─────────────────────────────────────────────
        // Applied after input diffusers, affects the initial reverb character.
        // process() returns input immediately with ZERO PSRAM cost when mix==0.
        float diffusedL = _pitchL.process(diffused);
        float diffusedR = _pitchR.process(diffused);

        // ── Triangle LFO for tank allpass modulation ──────────────────────────
        // Creates the subtle chorusing that gives plate reverbs their life.
        float lfo = triangleLFO();

        // ── Read cross-feedback from previous sample's FILTERED tank outputs ─────
        // _tank0fb / _tank1fb hold the damped (LPF + HPF filtered) output from
        // the previous sample.  Using the filtered signal here is what makes
        // Hi Damp and Lo Damp audible — they sit inside the feedback loop.
        // Multiplying by decay scales the feedback level (size control).
        float crossFB0 = _tank1fb * decay;   // filtered tank1 → feeds tank0
        float crossFB1 = _tank0fb * decay;   // filtered tank0 → feeds tank1

        // ── Tank half 0 ───────────────────────────────────────────────────────
        // Shimmer processes (crossFB0 + diffusedL) before entering the APF.
        // Shimmer is placed INSIDE the feedback loop so the pitched signal
        // recirculates, creating the escalating shimmer effect.
        // process() returns input immediately with zero PSRAM cost when mix==0.
        float tank0 = _pitchShimR.process(crossFB0 + diffusedL);
        tank0 = _tankAPF[0].processModulated(tank0, lfo * modDepthSmp);
        _tankDelay[0].write(tank0);
        // Read from end of delay, apply damping filters, store for next sample's
        // cross-feedback.  Taps read separately below from intermediate positions.
        _tank0fb = _tankHPF[0].process(
                   _tankLPF[0].process(
                   _tankDelay[0].read(_tankDelay[0].len - 1)));

        // ── Tank half 1 ───────────────────────────────────────────────────────
        // Inverted LFO for stereo decorrelation (opposite phase modulation).
        float tank1 = _pitchShimL.process(crossFB1 + diffusedR);
        tank1 = _tankAPF[1].processModulated(tank1, -lfo * modDepthSmp);
        _tankDelay[1].write(tank1);
        _tank1fb = _tankHPF[1].process(
                   _tankLPF[1].process(
                   _tankDelay[1].read(_tankDelay[1].len - 1)));

        // ── Decorrelated output taps ──────────────────────────────────────────
        // Multiple read positions from each tank delay for a wide, complex
        // stereo image.  Subtractive taps increase channel decorrelation.
        float wetL = _tankDelay[0].read(TAP_L0)
                   + _tankDelay[0].read(TAP_L1)
                   - _tankDelay[1].read(TAP_L2);

        float wetR = _tankDelay[1].read(TAP_R0)
                   + _tankDelay[1].read(TAP_R1)
                   - _tankDelay[0].read(TAP_R2);

        // Normalise: 3-tap sum can exceed 1.0 under sustained input
        wetL *= kWetScale;
        wetR *= kWetScale;

        // ── Master output EQ ──────────────────────────────────────────────────
        // Applied to wet signal only.  Block-level flags skip the filter
        // entirely when coefficients are at bypass (0.0), saving per-sample
        // branch evaluations inside the OnePole process() methods.
        if (doMasterLP) {
            wetL = _masterLPF[0].process(wetL);
            wetR = _masterLPF[1].process(wetR);
        }
        if (doMasterHP) {
            wetL = _masterHPF[0].process(wetL);
            wetR = _masterHPF[1].process(wetR);
        }

        // ── Wet/dry mix ───────────────────────────────────────────────────────
        float outSampleL = dryLevel * inSampleL + wetLevel * wetL;
        float outSampleR = dryLevel * inSampleR + wetLevel * wetR;

        // ── Soft clip — protect against occasional overs ──────────────────────
        // Uses x/(1+|x|) approximation: free at normal levels (branch not taken),
        // smoothly limits without hard clipping when exceeded.
        if (outSampleL >  1.0f || outSampleL < -1.0f) {
            outSampleL = outSampleL / (1.0f + fabsf(outSampleL));
        }
        if (outSampleR >  1.0f || outSampleR < -1.0f) {
            outSampleR = outSampleR / (1.0f + fabsf(outSampleR));
        }

        // ── Convert back to int16 ─────────────────────────────────────────────
        outL->data[i] = (int16_t)(outSampleL * kToInt16);
        outR->data[i] = (int16_t)(outSampleR * kToInt16);
    }

    // ── Transmit and release blocks ───────────────────────────────────────────
    transmit(outL, 0);
    transmit(outR, 1);
    release(outL);
    release(outR);
    if (inL) release(inL);
    if (inR) release(inR);
}
