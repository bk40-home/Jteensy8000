// =============================================================================
// PlateReverb.cpp — see PlateReverb.h for provenance, deviations, CPU notes.
// =============================================================================
// DSP ported verbatim from v1 AudioEffectPlateReverbJT.cpp.  Line-level fidelity
// is deliberate: these mappings, tap positions and coefficient curves are
// validated character, not code to "improve" (CLAUDE.md rule 11 spirit).
// =============================================================================
#include "core/dsp/PlateReverb.h"

namespace JT {

// -----------------------------------------------------------------------------
// Static tables (verbatim from v1 — hexefx wavetables.c origin).
// Placed in this single TU so the 37+257 floats aren't duplicated per include.
// JT_FLASH_DATA keeps them out of DTCM on Teensy (harmless no-op on host).
// -----------------------------------------------------------------------------

// Pitch shifter: exact 12-TET ratios, index = semitones + 12 (-12..+24).
const float PlateReverb::PitchShifter::kSemitoneRatios[37] JT_FLASH_DATA = {
    0.500000f, 0.529732f, 0.561231f, 0.594604f, 0.629961f, 0.667420f, 0.707107f, 0.749154f,
    0.793701f, 0.840896f, 0.890899f, 0.943874f, 1.000000f, 1.059463f, 1.122462f, 1.189207f,
    1.259921f, 1.334840f, 1.414214f, 1.498307f, 1.587401f, 1.681793f, 1.781797f, 1.887749f,
    2.000000f, 2.118926f, 2.244924f, 2.378414f, 2.519842f, 2.669680f, 2.828427f, 2.996614f,
    3.174802f, 3.363586f, 3.563595f, 3.775497f, 4.000000f
};

// Pitch shifter: 257-entry raised-cosine crossfade window; [256]=1.0 over-read.
const float PlateReverb::PitchShifter::kFadeTable[257] JT_FLASH_DATA = {
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

// -----------------------------------------------------------------------------
// Tuning constants (verbatim from v1).
// -----------------------------------------------------------------------------
static constexpr float    IDIFF_GAIN_0 = 0.75f;
static constexpr float    IDIFF_GAIN_1 = 0.75f;
static constexpr float    IDIFF_GAIN_2 = 0.625f;
static constexpr float    IDIFF_GAIN_3 = 0.625f;
static constexpr float    TANK_APF_GAIN = 0.60f;

// Output tap positions (samples behind write head) — decorrelated stereo.
static constexpr uint32_t TAP_L0 =  266, TAP_L1 = 2974, TAP_L2 = 1913;
static constexpr uint32_t TAP_R0 =  353, TAP_R1 = 3627, TAP_R2 = 1228;

// 3-tap sum can exceed 1.0; empirical normalisation to stay below full-scale.
static constexpr float    kWetScale = 0.3f;

// -----------------------------------------------------------------------------
// begin / assignBuffers — carve the caller pool, apply v1 GlobalFX ctor state.
// -----------------------------------------------------------------------------
void PlateReverb::begin(float* pool)
{
    _pool = pool;
    memset(_diffuserBuf, 0, sizeof(_diffuserBuf));
    if (!_pool) return;                     // inert; processBlock bails on null

    memset(_pool, 0, kPoolFloats * sizeof(float));
    assignBuffers();

    // Input diffuser gains (Dattorro).  (diffusion() below overwrites these to
    // the shared coeff — kept explicit so the object is valid pre-diffusion.)
    _inputDiffuser[0].gain = IDIFF_GAIN_0;
    _inputDiffuser[1].gain = IDIFF_GAIN_1;
    _inputDiffuser[2].gain = IDIFF_GAIN_2;
    _inputDiffuser[3].gain = IDIFF_GAIN_3;
    _tankAPF[0].gain = TANK_APF_GAIN;
    _tankAPF[1].gain = TANK_APF_GAIN;

    for (uint8_t i = 0; i < 2; ++i) {
        _tankLPF[i].clear();  _tankLPF[i].coeff = _hiDampCoeff;
        _tankHPF[i].clear();  _tankHPF[i].coeff = _loDampCoeff;
        _masterLPF[i].clear(); _masterLPF[i].coeff = 0.0f;   // bypass at boot
        _masterHPF[i].clear(); _masterHPF[i].coeff = 0.0f;
    }

    // Pitch shifters: unity + mix off => zero CPU until enabled.
    _pitchL.setPitch(1.0f);     _pitchL.setMix(0.0f);
    _pitchR.setPitch(1.0f);     _pitchR.setMix(0.0f);
    _pitchShimL.setPitch(2.0f); _pitchShimL.setMix(0.0f);   // default +12 st
    _pitchShimR.setPitch(2.0f); _pitchShimR.setMix(0.0f);

    // v1 GlobalFX ctor one-shots (GlobalFX.cpp:31-56): diffusion 0.65, no
    // reverb pitch, shimmer octave up (but mix 0), no freeze bleed.  size/damp
    // stay at their member defaults until SynthCore applies the patch.
    // Fold diffusion() inline (it also sets the shared allpass gains):
    _diffusionCoeff = 0.005f + 0.65f * (0.65f - 0.005f);   // == 0.42425
    for (uint8_t i = 0; i < 4; ++i) _inputDiffuser[i].gain = _diffusionCoeff;
    _tankAPF[0].gain = _diffusionCoeff;
    _tankAPF[1].gain = _diffusionCoeff;

    updateModRate();
}

void PlateReverb::assignBuffers()
{
    // Input diffusers -> DTCM member buffer (contiguous).
    const uint32_t idiffLens[4] = { kIdiffLen0, kIdiffLen1, kIdiffLen2, kIdiffLen3 };
    float* dtcm = _diffuserBuf;
    for (uint8_t i = 0; i < 4; ++i) {
        _inputDiffuser[i].dl.buf      = dtcm;
        _inputDiffuser[i].dl.len      = idiffLens[i];
        _inputDiffuser[i].dl.writeIdx = 0;
        dtcm += idiffLens[i];
    }

    // Everything else -> caller pool (PSRAM on Teensy).
    float* p = _pool;
    _predelay.buf = p;       _predelay.len = kPredelayMax;  _predelay.writeIdx = 0; p += kPredelayMax;
    _tankAPF[0].dl.buf = p;  _tankAPF[0].dl.len = kTankApfLen0; _tankAPF[0].dl.writeIdx = 0; p += kTankApfLen0;
    _tankAPF[1].dl.buf = p;  _tankAPF[1].dl.len = kTankApfLen1; _tankAPF[1].dl.writeIdx = 0; p += kTankApfLen1;
    _tankDelay[0].buf = p;   _tankDelay[0].len = kTankDlyLen0;  _tankDelay[0].writeIdx = 0;  p += kTankDlyLen0;
    _tankDelay[1].buf = p;   _tankDelay[1].len = kTankDlyLen1;  _tankDelay[1].writeIdx = 0;  p += kTankDlyLen1;
    _pitchL.assign(p);      p += PitchShifter::BUF_SIZE;
    _pitchR.assign(p);      p += PitchShifter::BUF_SIZE;
    _pitchShimL.assign(p);  p += PitchShifter::BUF_SIZE;
    _pitchShimR.assign(p);  p += PitchShifter::BUF_SIZE;
}

// -----------------------------------------------------------------------------
// Parameter setters — identical mappings to v1 (spec §1.3).
// -----------------------------------------------------------------------------
static inline float clamp01(float n) { return n < 0.0f ? 0.0f : (n > 1.0f ? 1.0f : n); }

// -----------------------------------------------------------------------------
// DEVIATION FROM v1 (flagged — CLAUDE.md rule 2): filter mappings are now
// specified in HERTZ, not in raw one-pole coefficient.
//
// WHY.  Both OnePole_LP and OnePole_HP use the pole form
//         y[n] = (1-a)*x[n] + a*y[n-1]          (HP output = x - y)
// whose -3 dB corner is                a = exp(-2*pi*fc/fs).
// That relation is violently non-linear, so v1's "a = 0.1 + 0.85*n" style
// mappings were not linear in anything perceptual, and — for the two HIGH-PASS
// controls — ran BACKWARDS: a small `a` is a HIGH corner, so turning lo-damp /
// hi-pass UP moved the corner DOWN.  v1's minimum non-zero lo-damp setting was
// a ~12 kHz high-pass sitting inside the tank feedback loop, which strips the
// tank of essentially all its energy.  That is why every damping control had to
// be left at zero to get a usable tail.
//
// Fix: map `n` exponentially across a musically chosen frequency span (equal
// octaves per unit of n), then convert to a coefficient once, in the setter.
// expf/powf here are fine — setters are control-rate, never per-sample.
// -----------------------------------------------------------------------------

// One-pole coefficient for a -3 dB corner at fcHz.  Returns 0 (== transparent
// for LP, and below the HP's own bypass guard) once fc approaches Nyquist.
static inline float onePoleCoeff(float fcHz)
{
    if (fcHz >= kSampleRate * 0.45f) return 0.0f;
    return expf(-6.28318531f * fcHz / kSampleRate);
}

// Exponential (constant octaves-per-unit) sweep of n across [lo, hi].
static inline float expSweep(float n, float loHz, float hiHz)
{
    return loHz * powf(hiHz / loHz, n);
}

// --- filter sweep endpoints (tune these; they are the only "voicing" knobs) --
static constexpr float kTankLpFcMax = 18000.0f;  // hiDamp  n->0 : open plate
static constexpr float kTankLpFcMin =   800.0f;  // hiDamp  n->1 : dark felt
static constexpr float kTankHpFcMin =    20.0f;  // loDamp  n->0 : full weight
static constexpr float kTankHpFcMax =   600.0f;  // loDamp  n->1 : thin/ambient
static constexpr float kMastLpFcMax = 20000.0f;  // lowpass n->0 : off
static constexpr float kMastLpFcMin =   500.0f;  // lowpass n->1
static constexpr float kMastHpFcMin =    20.0f;  // hipass  n->0 : off
static constexpr float kMastHpFcMax =  1000.0f;  // hipass  n->1

// Shimmer ceiling.  The shimmer pitch shifter sits INSIDE the tank loop, so its
// mix compounds on every round trip (~281 ms here): a static mix of m becomes an
// unbounded stack of +12 st copies as the tail recirculates.  The musically
// useful window in v1 measured roughly 0.02..0.05 — i.e. CC 1..3 — with the rest
// of the control range unusable.  Cap + square-law taper spreads that window
// across the whole control instead.
static constexpr float kShimmerMaxMix = 0.20f;

// NOT CHANGED — but measured, for the record.  The tank round trip here is
// (1800+3720+2656+4217)/fs = 281 ms and the per-round-trip loop gain is decay^2,
// so this mapping yields:
//     n=0.25 -> 0.68 s    n=0.5 -> 1.17 s   n=0.75 -> 2.6 s
//     n=0.90 -> 6.9 s     n=0.95 -> 14 s    n=1.00 -> ~1940 s (decay 0.9995)
// i.e. everything from 3 s upward is crammed into the last 10 % of travel, and
// n=1 is effectively self-oscillation — risky, since the MODULATED tank allpass
// is not exactly unity gain.  If that ever bothers you, map T60 directly:
//     const float t60   = 0.25f * powf(80.0f, n);          // 0.25 s .. 20 s
//     _decay = powf(10.0f, -1.5f * kTankRoundTripSec / t60);
//     if (_decay > 0.98f) _decay = 0.98f;                  // leave inf to freeze()
// Left alone for now: this curve is the validated v1 character (rule 11).
void PlateReverb::setSize(float n)
{
    n = clamp01(n);
    // decay = 0.1 + 0.8995 * (0.5n + 0.5n^2).  0.1 (n=0) .. 1.0 (n=1, inf hold).
    const float shaped = 0.5f * n + 0.5f * n * n;
    _decay = 0.1f + 0.8995f * shaped;
}

// In-tank HF damping.  n=0 -> filter fully bypassed.  n>0 sweeps the loop LP
// from 18 kHz down to 800 Hz, ~4.5 octaves spread evenly across the control.
// Because the LP is inside the feedback loop, its effect is cumulative: the
// per-pass corner is what is set here, the audible tail brightness falls faster.
void PlateReverb::setHiDamp(float n)
{
    n = clamp01(n);
    _hiDampCoeff = (n > 0.001f)
                 ? onePoleCoeff(expSweep(n, kTankLpFcMax, kTankLpFcMin))
                 : 0.0f;
    _tankLPF[0].coeff = _hiDampCoeff;
    _tankLPF[1].coeff = _hiDampCoeff;
}

// In-tank LF damping.  n=0 -> bypassed.  n>0 sweeps the loop HP from 20 Hz
// (inaudible) up to 600 Hz (thin, ambient).  NOTE the coefficient span this
// produces is ~0.918..0.997 — i.e. the extreme top of the coefficient range.
// v1 swept 0.1..0.9, which is the wrong end and the wrong direction entirely.
void PlateReverb::setLoDamp(float n)
{
    n = clamp01(n);
    _loDampCoeff = (n > 0.001f)
                 ? onePoleCoeff(expSweep(n, kTankHpFcMin, kTankHpFcMax))
                 : 0.0f;
    _tankHPF[0].coeff = _loDampCoeff;
    _tankHPF[1].coeff = _loDampCoeff;
}

// Post-tank master LP (wet only).  20 kHz (transparent) -> 500 Hz.
void PlateReverb::setLowpass(float n)
{
    n = clamp01(n);
    _masterLpCoeff = (n > 0.001f)
                   ? onePoleCoeff(expSweep(n, kMastLpFcMax, kMastLpFcMin))
                   : 0.0f;
    _masterLPF[0].coeff = _masterLpCoeff;
    _masterLPF[1].coeff = _masterLpCoeff;
}

// Post-tank master HP (wet only).  20 Hz (transparent) -> 1 kHz.
void PlateReverb::setHipass(float n)
{
    n = clamp01(n);
    _masterHpCoeff = (n > 0.001f)
                   ? onePoleCoeff(expSweep(n, kMastHpFcMin, kMastHpFcMax))
                   : 0.0f;
    _masterHPF[0].coeff = _masterHpCoeff;
    _masterHPF[1].coeff = _masterHpCoeff;
}

void PlateReverb::setShimmer(float n)
{
    if (_frozen) return;                      // freeze() owns the mix while frozen
    n = clamp01(n);

    // DEVIATION FROM v1 (flagged).  v1 used `2n - n^2`, which EXPANDS small
    // values (n=0.024 -> 0.047) and reaches full 1.0 mix at the top.  With the
    // shifter in the feedback loop that made the entire usable range live below
    // CC 4.  Square-law taper into a 0.20 ceiling instead:
    //     n=0.25 -> 0.013   n=0.5 -> 0.050   n=0.75 -> 0.113   n=1.0 -> 0.200
    // so v1's sweet spot now sits at mid-travel and the top is a deliberate
    // over-the-top extreme rather than a wall of octaves.
    const float m = kShimmerMaxMix * n * n;

    _shimmerMix = m;
    _pitchShimL.setMix(m);
    _pitchShimR.setMix(m);
}

void PlateReverb::setFreeze(bool on)
{
    if (_frozen == on) return;
    _frozen = on;

    if (on) {
        // Save the tail-shaping params, then force infinite bright hold.
        _savedDecay       = _decay;
        _savedHiDampCoeff = _hiDampCoeff;
        _savedLoDampCoeff = _loDampCoeff;
        _savedShimmerMix  = _shimmerMix;

        _decay = 1.0f;
        _hiDampCoeff = 0.0f;  _loDampCoeff = 0.0f;
        _tankLPF[0].coeff = 0.0f; _tankLPF[1].coeff = 0.0f;
        _tankHPF[0].coeff = 0.0f; _tankHPF[1].coeff = 0.0f;
        _pitchShimL.setMix(0.0f); _pitchShimR.setMix(0.0f);  // no runaway shimmer
    } else {
        _decay       = _savedDecay;
        _hiDampCoeff = _savedHiDampCoeff;
        _loDampCoeff = _savedLoDampCoeff;
        _tankLPF[0].coeff = _hiDampCoeff; _tankLPF[1].coeff = _hiDampCoeff;
        _tankHPF[0].coeff = _loDampCoeff; _tankHPF[1].coeff = _loDampCoeff;
        _pitchShimL.setMix(_savedShimmerMix);
        _pitchShimR.setMix(_savedShimmerMix);
    }
}

// -----------------------------------------------------------------------------
// processBlock — v1 update() per-sample chain, float-native, dry/wet blended
// with `mix` (spec §1.4 + Decision #5).  Caller ensures we're not bypassed.
// -----------------------------------------------------------------------------
void PlateReverb::processBlock(float* left, float* right, size_t n, float mix)
{
    if (!_pool) return;                       // inert if no memory attached

    // Block-constant caches (v1 caches per block to avoid per-sample member
    // loads).  Freeze forces decay=1 and mutes input except a small bleed.
    const float decay     = _frozen ? 1.0f : _decay;
    const float inputGain = _frozen ? _freezeBleedGain : 1.0f;
    const bool  doMasterLP = (_masterLpCoeff > 0.001f);
    const bool  doMasterHP = (_masterHpCoeff > 0.001f);
    const float modDepthSmp = _modDepth;
    const uint32_t predelaySmp = _predelaySamples;

    for (size_t i = 0; i < n; ++i) {
        const float inL = left[i];
        const float inR = right[i];
        const float monoIn = (inL + inR) * 0.5f * inputGain;

        // Pre-delay (skipped entirely at 0 — the v2 default).
        float predelayed;
        if (predelaySmp > 0) {
            _predelay.write(monoIn);
            predelayed = _predelay.read(predelaySmp);
        } else {
            predelayed = monoIn;
        }

        // Input diffuser chain (DTCM).
        float diffused = predelayed;
        for (uint8_t d = 0; d < 4; ++d) diffused = _inputDiffuser[d].process(diffused);

        // Reverb-tail pitch shift (zero cost when mix 0 — default).
        const float diffusedL = _pitchL.process(diffused);
        const float diffusedR = _pitchR.process(diffused);

        const float lfo = triangleLFO();

        // Cross-feedback from previous sample's FILTERED tank outputs — the
        // damping filters sit INSIDE the loop, which is what makes hi/lo damp
        // audible.  decay scales the recirculation (size control).
        const float crossFB0 = _tank1fb * decay;
        const float crossFB1 = _tank0fb * decay;

        // Tank half 0.
        float tank0 = _pitchShimR.process(crossFB0 + diffusedL);
        tank0 = _tankAPF[0].processModulated(tank0, lfo * modDepthSmp);
        _tankDelay[0].write(tank0);
        _tank0fb = _tankHPF[0].process(
                   _tankLPF[0].process(
                   _tankDelay[0].read(_tankDelay[0].len - 1)));

        // Tank half 1 (inverted LFO for stereo decorrelation).
        float tank1 = _pitchShimL.process(crossFB1 + diffusedR);
        tank1 = _tankAPF[1].processModulated(tank1, -lfo * modDepthSmp);
        _tankDelay[1].write(tank1);
        _tank1fb = _tankHPF[1].process(
                   _tankLPF[1].process(
                   _tankDelay[1].read(_tankDelay[1].len - 1)));

        // Decorrelated output taps (subtractive cross tap widens the image).
        float wetL = _tankDelay[0].read(TAP_L0)
                   + _tankDelay[0].read(TAP_L1)
                   - _tankDelay[1].read(TAP_L2);
        float wetR = _tankDelay[1].read(TAP_R0)
                   + _tankDelay[1].read(TAP_R1)
                   - _tankDelay[0].read(TAP_R2);
        wetL *= kWetScale;
        wetR *= kWetScale;

        // Master output EQ (wet only; skipped per-block at bypass coeffs).
        if (doMasterLP) { wetL = _masterLPF[0].process(wetL); wetR = _masterLPF[1].process(wetR); }
        if (doMasterHP) { wetL = _masterHPF[0].process(wetL); wetR = _masterHPF[1].process(wetR); }

        // Dry/wet blend (Decision #5): tank is 100 % wet, blend against the
        // untouched dry input here.  mix==0 never reaches this loop (SynthCore
        // auto-bypasses), so no special-case needed.
        float outL = inL + mix * wetL;
        float outR = inR + mix * wetR;

        // Soft clip — free unless we actually overshoot (v1 x/(1+|x|)).
        if (outL > 1.0f || outL < -1.0f) outL = outL / (1.0f + fabsf(outL));
        if (outR > 1.0f || outR < -1.0f) outR = outR / (1.0f + fabsf(outR));

        left[i]  = outL;
        right[i] = outR;
    }
}

} // namespace JT