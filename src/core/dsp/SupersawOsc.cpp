// =============================================================================
// SupersawOsc.cpp — implementation (provenance and contracts in the header)
// =============================================================================
// The three tables below are BYTE-FOR-BYTE from v1 AudioSynthSupersaw.cpp:
// the detune LUT, the frequency-offset ratios, and the mix polynomials'
// coefficients.  Do not "clean up" the numbers — they ARE the JP-8000
// measurement (Szabo 2010) and every existing patch depends on them.
// (c) 2026 Kris Bishop - MIT licensed.
// =============================================================================

#include "core/dsp/SupersawOsc.h"

#include "core/AudioConfig.h"   // JT_FLASH_DATA

#include <cmath>   // fmaxf, fminf

#include "core/dsp/FastMath.h"

namespace JT {

namespace {

// -----------------------------------------------------------------------------
// Szabo Table 1 - per-voice frequency offsets at MAXIMUM detune.
// f_voice[i] = f_centre * (1 + kFreqOffsetsMax[i] * detuneDepth)
// -----------------------------------------------------------------------------
const float kFreqOffsetsMax[SupersawOsc::kVoices] JT_FLASH_DATA = {
    -0.11002313f,       // voice 1 - lowest
    -0.06288439f,       // voice 2
    -0.01952356f,       // voice 3
     0.0f,              // voice 4 - centre (unaffected by detune)
     0.01991221f,       // voice 5
     0.06216538f,       // voice 6
     0.10745242f        // voice 7 - highest
};

// -----------------------------------------------------------------------------
// Szabo 11th-order detune polynomial, pre-baked into a 256-entry LUT with
// linear interpolation (v1 measured the interp error at < 0.001 - inaudible).
// Very gradual below 50% (fine string/pad detunes), steep above 90%.
// 1 KB of read-only data; lives in flash on the Teensy.
// -----------------------------------------------------------------------------
constexpr int kLutSize = 256;
const float kDetuneLut[kLutSize] JT_FLASH_DATA = {
    0.000000000f, 0.005297449f, 0.006974790f, 0.008162972f, 0.008965444f,
    0.009471287f, 0.009756665f, 0.009886171f, 0.009914065f, 0.009885414f,
    0.009837144f, 0.009798996f, 0.009794413f, 0.009841335f, 0.009952939f,
    0.010138298f, 0.010402987f, 0.010749625f, 0.011178365f, 0.011687333f,
    0.012273021f, 0.012930635f, 0.013654403f, 0.014437846f, 0.015274017f,
    0.016155704f, 0.017075611f, 0.018026502f, 0.019001333f, 0.019993354f,
    0.020996193f, 0.022003927f, 0.023011128f, 0.024012902f, 0.025004911f,
    0.025983391f, 0.026945145f, 0.027887547f, 0.028808522f, 0.029706528f,
    0.030580532f, 0.031429980f, 0.032254764f, 0.033055186f, 0.033831920f,
    0.034585974f, 0.035318651f, 0.036031504f, 0.036726303f, 0.037404991f,
    0.038069647f, 0.038722449f, 0.039365639f, 0.040001489f, 0.040632269f,
    0.041260220f, 0.041887524f, 0.042516280f, 0.043148485f, 0.043786009f,
    0.044430583f, 0.045083783f, 0.045747014f, 0.046421509f, 0.047108312f,
    0.047808280f, 0.048522079f, 0.049250182f, 0.049992871f, 0.050750243f,
    0.051522211f, 0.052308515f, 0.053108728f, 0.053922266f, 0.054748403f,
    0.055586275f, 0.056434900f, 0.057293191f, 0.058159968f, 0.059033974f,
    0.059913892f, 0.060798357f, 0.061685976f, 0.062575341f, 0.063465046f,
    0.064353700f, 0.065239945f, 0.066122468f, 0.067000016f, 0.067871408f,
    0.068735549f, 0.069591441f, 0.070438194f, 0.071275033f, 0.072101312f,
    0.072916517f, 0.073720274f, 0.074512357f, 0.075292687f, 0.076061342f,
    0.076818552f, 0.077564706f, 0.078300348f, 0.079026177f, 0.079743043f,
    0.080451948f, 0.081154035f, 0.081850587f, 0.082543020f, 0.083232872f,
    0.083921800f, 0.084611568f, 0.085304038f, 0.086001157f, 0.086704951f,
    0.087417510f, 0.088140977f, 0.088877537f, 0.089629400f, 0.090398797f,
    0.091187959f, 0.091999107f, 0.092834443f, 0.093696130f, 0.094586290f,
    0.095506982f, 0.096460196f, 0.097447844f, 0.098471743f, 0.099533612f,
    0.100635058f, 0.101777571f, 0.102962517f, 0.104191126f, 0.105464493f,
    0.106783570f, 0.108149164f, 0.109561931f, 0.111022378f, 0.112530862f,
    0.114087589f, 0.115692617f, 0.117345858f, 0.119047083f, 0.120795926f,
    0.122591890f, 0.124434356f, 0.126322586f, 0.128255739f, 0.130232873f,
    0.132252963f, 0.134314907f, 0.136417539f, 0.138559643f, 0.140739965f,
    0.142957225f, 0.145210134f, 0.147497404f, 0.149817763f, 0.152169970f,
    0.154552825f, 0.156965186f, 0.159405980f, 0.161874214f, 0.164368988f,
    0.166889501f, 0.169435067f, 0.172005119f, 0.174599214f, 0.177217043f,
    0.179858430f, 0.182523338f, 0.185211866f, 0.187924249f, 0.190660854f,
    0.193422173f, 0.196208817f, 0.199021502f, 0.201861040f, 0.204728322f,
    0.207624298f, 0.210549963f, 0.213506329f, 0.216494404f, 0.219515161f,
    0.222569516f, 0.225658288f, 0.228782174f, 0.231941712f, 0.235137241f,
    0.238368873f, 0.241636445f, 0.244939492f, 0.248277199f, 0.251648370f,
    0.255051392f, 0.258484194f, 0.261944225f, 0.265428414f, 0.268933155f,
    0.272454278f, 0.275987040f, 0.279526115f, 0.283065593f, 0.286598989f,
    0.290119267f, 0.293618862f, 0.297089732f, 0.300523415f, 0.303911103f,
    0.307243736f, 0.310512120f, 0.313707059f, 0.316819521f, 0.319840823f,
    0.322762851f, 0.325578307f, 0.328281000f, 0.330866163f, 0.333330818f,
    0.335674187f, 0.337898145f, 0.340007725f, 0.342011687f, 0.343923134f,
    0.345760197f, 0.347546790f, 0.349313436f, 0.351098166f, 0.352947509f,
    0.354917562f, 0.357075156f, 0.359499128f, 0.362281685f, 0.365529892f,
    0.369367273f, 0.373935540f, 0.379396449f, 0.385933804f, 0.393755605f,
    0.403096353f, 0.414219517f, 0.427420181f, 0.443027869f, 0.461409562f,
    0.482972921f, 0.508169722f, 0.537499507f, 0.571513484f, 0.610818656f,
    0.656082224f, 0.708036247f, 0.767482592f, 0.835298180f, 0.912440542f,
    0.999953694f
};

// PolyBLEP residual - same maths as OscCore.cpp (see the note there about
// the deliberate duplication until OscSaw retires).
inline float polyBlep(float t, float dt)
{
    if (t < dt) {
        const float x = t / dt;
        return x + x - x * x - 1.0f;
    }
    if (t > 1.0f - dt) {
        const float x = (t - 1.0f) / dt;
        return x * x + x + x + 1.0f;
    }
    return 0.0f;
}

inline float clamp01(float v)
{
    return (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
}

} // namespace

// -----------------------------------------------------------------------------
// Construction - v1 constructor defaults, then the derived state.
// -----------------------------------------------------------------------------
SupersawOsc::SupersawOsc()
{
    noteOn();                 // "free-running" initial phases (Szabo 3.4)
    calculateIncrements();
    calculateGains();
    calculateHpf();
}

float SupersawOsc::nextRandom01()
{
    uint32_t x = _rng;        // xorshift32 - deterministic under seedNoise()
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    _rng = x;
    return (float)(x >> 8) * (1.0f / 16777216.0f);
}

void SupersawOsc::noteOn()
{
    for (int i = 0; i < kVoices; ++i)
        _phase[i] = nextRandom01();
}

// -----------------------------------------------------------------------------
// Control plane - every setter dirty-checks, and each recomputes ONLY the
// derived state it invalidates (frequency never touches gains; mix never
// touches increments).
// -----------------------------------------------------------------------------
void SupersawOsc::setFrequency(float hz)
{
    if (hz == _freq) return;
    _freq = hz;
    calculateIncrements();
    calculateHpf();           // the HPF tracks pitch - that IS the feature
}

void SupersawOsc::setDetune(float amount01)
{
    amount01 = clamp01(amount01);
    if (amount01 == _detuneAmt) return;
    _detuneAmt = amount01;
    calculateIncrements();
}

void SupersawOsc::setMix(float mix01)
{
    mix01 = clamp01(mix01);
    if (mix01 == _mixAmt) return;
    _mixAmt = mix01;
    calculateGains();
}

void SupersawOsc::setBandLimited(bool enable)
{
    _polyBlep = enable;
}

// -----------------------------------------------------------------------------
// Derived-state calculators - verbatim v1 maths.
// -----------------------------------------------------------------------------
float SupersawOsc::detuneCurve(float x) const
{
    if (x <= 0.0f) return 0.0f;                    // true unison at zero
    if (x >= 1.0f) return kDetuneLut[kLutSize - 1];

    const float idxFloat = x * (float)(kLutSize - 1);
    const int   idx0     = (int)idxFloat;
    const float frac     = idxFloat - (float)idx0;
    return kDetuneLut[idx0]
         + frac * (kDetuneLut[idx0 + 1] - kDetuneLut[idx0]);
}

void SupersawOsc::calculateIncrements()
{
    const float nyquist     = 0.5f * kSampleRate;
    const float detuneDepth = clamp01(detuneCurve(_detuneAmt));

    for (int i = 0; i < kVoices; ++i) {
        float voiceFreq = _freq * (1.0f + kFreqOffsetsMax[i] * detuneDepth);
        if (voiceFreq < 0.0f)    voiceFreq = 0.0f;
        if (voiceFreq > nyquist) voiceFreq = nyquist;
        _phaseInc[i] = voiceFreq / kSampleRate;
    }
}

void SupersawOsc::calculateGains()
{
    const float x = _mixAmt;

    // Centre (Szabo): y = -0.55366x + 0.99785 - linear, never fully mutes.
    const float centreGain = clamp01(-0.55366f * x + 0.99785f);

    // Sides (Szabo): y = -0.73764x^2 + 1.2841x + 0.044372 - parabolic,
    // near-silent at 0, peaking around x ~ 0.87; split across 6 voices.
    const float sideTotal = clamp01(-0.73764f * x * x + 1.2841f * x + 0.044372f);
    const float sideEach  = sideTotal / 6.0f;

    for (int i = 0; i < kVoices; ++i)
        _gain[i] = (i == kCentreIdx) ? centreGain : sideEach;
}

void SupersawOsc::calculateHpf()
{
    // 1-pole HPF tracking the fundamental: removes sub-fundamental rumble
    // and DC from the naive-saw sum while keeping the aliasing "air" above
    // it (Szabo 3.3).  alpha = RC / (RC + dt), RC = 1 / (2*pi*f).
    const float f  = fmaxf(_freq, 1.0f);
    const float rc = 1.0f / (6.2831853f * f);
    const float dt = 1.0f / kSampleRate;
    _hpfAlpha = rc / (rc + dt);
}

// -----------------------------------------------------------------------------
// Audio plane - the v1 inner loop, minus the int16 packing.
// -----------------------------------------------------------------------------
void SupersawOsc::render(float* out, size_t n,
                         const float* fmBuf, float fmOctaves,
                         const float* pmBuf)
{
    // Mix compensation, block-rate (v1: computed once per update()).
    // Detuned voices partially cancel; the boost restores perceived level
    // as mix rises.  ON with max 1.5 == the v1 OscillatorBlock setting.
    const float mixGain = 1.0f + _mixAmt * (kCompMaxGain - 1.0f);

    for (size_t s = 0; s < n; ++s) {
        // FM: ONE fastPow2 per sample shared by all 7 voices (v1 layout).
        float fmMult = 1.0f;
        if (fmBuf != nullptr)
            fmMult = FastMath::fastPow2(fmBuf[s] * fmOctaves);

        // Phase mod: additive read-time offset, never accumulated.
        float phOffset = 0.0f;
        if (pmBuf != nullptr)
            phOffset = pmBuf[s] * kPmRangeCycles;

        float sample = 0.0f;
        for (int i = 0; i < kVoices; ++i) {
            const float inc = _phaseInc[i] * fmMult;

            float ph = _phase[i] + phOffset;
            if      (ph >= 1.0f) ph -= 1.0f;
            else if (ph <  0.0f) ph += 1.0f;

            float v = 2.0f * ph - 1.0f;              // naive saw = the sound
            if (_polyBlep) v -= polyBlep(ph, inc);   // optional clean mode
            sample += v * _gain[i];

            _phase[i] += inc;
            if (_phase[i] >= 1.0f) _phase[i] -= 1.0f;
        }

        // Pitch-tracked HPF, then v1's exact clip -> gain -> clip staging
        // (the double clip shapes how hot mixes saturate - keep it).
        float hpOut = _hpfAlpha * (_hpfPrevOut + sample - _hpfPrevIn);
        _hpfPrevIn  = sample;
        _hpfPrevOut = hpOut;

        hpOut = fmaxf(-1.0f, fminf(1.0f, hpOut));
        float o = hpOut * kOutputGain * mixGain;
        out[s] = fmaxf(-1.0f, fminf(1.0f, o));
    }
}

} // namespace JT
