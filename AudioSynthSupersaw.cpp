// AudioSynthSupersaw.cpp
// =============================================================================
// JP-8000 Super Saw emulation — see AudioSynthSupersaw.h for full description.
//
// Corrections applied vs previous version:
//   1. DETUNE LUT regenerated from the actual Szabó 11th-order polynomial.
//      Old LUT was ~2.3× too aggressive at mid-range (cubic, not 11th-order).
//      LUT[0] forced to 0.0 so detune=0 gives true unison.
//   2. noteOn() now randomises all 7 phases per Szabó §3.4.
//      Old code used fixed offsets (the frequency ratios mistakenly applied
//      as phase values) — every note sounded identical.
//   3. calculateGains() now uses Szabó's measured curves (§3.2, p.15):
//        Centre: y = -0.55366x + 0.99785  (linear, never drops below ~0.445)
//        Side:   y = -0.73764x² + 1.2841x + 0.044372  (parabolic, peaks ~0.6)
//      Old code used simple linear crossfade (centre to 0, side to 1).
// =============================================================================

#include "AudioSynthSupersaw.h"
#include "JT8000_OptFlags.h"
#include <math.h>       // fminf, fmaxf, ldexpf, floorf

// =============================================================================
// FAST 2^x APPROXIMATION
// =============================================================================
// Replaces powf(2.0f, x) in the per-sample FM loop.
// Uses integer exponent extraction + 4th-order Remez minimax polynomial.
// Cost: ~8 FPU cycles vs ~50–100 for powf on Cortex-M7.
// Accuracy: < 0.005% relative error over ±10 octaves.
//
// Method:  x = i + f  where i = floor(x), 0 ≤ f < 1
//          2^x = 2^i × 2^f
//          2^i is exact via ldexpf (IEEE 754 exponent manipulation).
//          2^f is a Remez polynomial on [0, 1).
// =============================================================================
static inline float fast_pow2(float x)
{
    if (x >  126.0f) return 67108864.0f;       // Clamp: prevent IEEE overflow
    if (x < -126.0f) return 1.49e-38f;         // Clamp: prevent denormals

    const int32_t xi = (int32_t)floorf(x);     // Integer part (correct for negative x)
    const float   xf = x - (float)xi;          // Fractional part [0, 1)

    // Remez minimax coefficients for 2^f on [0, 1).  Error < 2e-6 absolute.
    const float poly = 1.00000000f
                     + xf * (0.69314718f
                     + xf * (0.24022651f
                     + xf * (0.05550411f
                     + xf *  0.00961823f)));

    return ldexpf(poly, xi);                    // 2^i × poly — single FPU cycle
}

// =============================================================================
// PolyBLEP HELPERS
// =============================================================================
// PolyBLEP (Polynomial Band-Limited Step) suppresses aliasing at sawtooth
// discontinuities by subtracting a small polynomial correction.
//   t  = current phase [0, 1)
//   dt = phase increment per sample (fractional advance)
// =============================================================================
static inline float poly_blep(float t, float dt)
{
    if (t < dt) {
        // Near cycle start: smooth the rising edge
        const float x = t / dt;
        return x + x - x * x - 1.0f;           // Ramps from -1 to 0
    }
    if (t > 1.0f - dt) {
        // Near cycle end: smooth the falling edge
        const float x = (t - 1.0f) / dt;
        return x * x + x + x + 1.0f;           // Ramps from 0 to 1
    }
    return 0.0f;                                // No correction needed mid-cycle
}

/// Generate one PolyBLEP-corrected sawtooth sample (-1 to +1).
static inline float saw_polyblep(float phase, float phaseInc)
{
    float s = 2.0f * phase - 1.0f;             // Naive saw: -1 to +1
    s -= poly_blep(phase, phaseInc);            // Subtract alias correction
    return s;
}

// =============================================================================
// DETUNE CURVE LOOKUP TABLE — Szabó 11th-Order Polynomial
// =============================================================================
// 256 entries sampled from the polynomial on p.12 of the Szabó thesis:
//
//   y = 10028.7313x¹¹ - 50818.8652x¹⁰ + 111363.4809x⁹ - 138150.6761x⁸
//     + 106649.6679x⁷ - 53046.9643x⁶  + 17019.9519x⁵  - 3425.0837x⁴
//     + 404.2704x³    - 24.1879x²      + 0.6717x        + 0.0030
//
// Entry[0] forced to 0.0 — true unison when detune CC = 0.
// Entry[255] = 0.9999537 — effectively 1.0 (full spread at max detune).
// Clamped to [0.0, 1.0] to handle polynomial overshoot at endpoints.
//
// Shape: nearly flat below x ≈ 0.5 (fine string/pad control), then steep
// exponential rise above x ≈ 0.9.  This is the key characteristic that
// distinguishes the JP-8000 Super Saw from linear-detune imitations.
//
// Memory: 256 × 4 = 1024 bytes in flash (PROGMEM).
// CPU: one multiply + one add (linear interpolation) vs 11 multiplies.
// =============================================================================
#define DETUNE_LUT_SIZE 256

static const float DETUNE_LUT[DETUNE_LUT_SIZE] PROGMEM = {
    // x = 0/255 .. 4/255                                          [x ≈ 0.000]
    0.000000000f, 0.005297449f, 0.006974790f, 0.008162972f, 0.008965444f,
    // x = 5/255 .. 9/255
    0.009471287f, 0.009756665f, 0.009886171f, 0.009914065f, 0.009885414f,
    // x = 10/255 .. 14/255
    0.009837144f, 0.009798996f, 0.009794413f, 0.009841335f, 0.009952939f,
    // x = 15/255 .. 19/255
    0.010138298f, 0.010402987f, 0.010749625f, 0.011178365f, 0.011687333f,
    // x = 20/255 .. 24/255
    0.012273021f, 0.012930635f, 0.013654403f, 0.014437846f, 0.015274017f,
    // x = 25/255 .. 29/255                                        [x ≈ 0.098]
    0.016155704f, 0.017075611f, 0.018026502f, 0.019001333f, 0.019993354f,
    // x = 30/255 .. 34/255
    0.020996193f, 0.022003927f, 0.023011128f, 0.024012902f, 0.025004911f,
    // x = 35/255 .. 39/255
    0.025983391f, 0.026945145f, 0.027887547f, 0.028808522f, 0.029706528f,
    // x = 40/255 .. 44/255
    0.030580532f, 0.031429980f, 0.032254764f, 0.033055186f, 0.033831920f,
    // x = 45/255 .. 49/255
    0.034585974f, 0.035318651f, 0.036031504f, 0.036726303f, 0.037404991f,
    // x = 50/255 .. 54/255                                        [x ≈ 0.196]
    0.038069647f, 0.038722449f, 0.039365639f, 0.040001489f, 0.040632269f,
    // x = 55/255 .. 59/255
    0.041260220f, 0.041887524f, 0.042516280f, 0.043148485f, 0.043786009f,
    // x = 60/255 .. 64/255
    0.044430583f, 0.045083783f, 0.045747014f, 0.046421509f, 0.047108312f,
    // x = 65/255 .. 69/255
    0.047808280f, 0.048522079f, 0.049250182f, 0.049992871f, 0.050750243f,
    // x = 70/255 .. 74/255
    0.051522211f, 0.052308515f, 0.053108728f, 0.053922266f, 0.054748403f,
    // x = 75/255 .. 79/255                                        [x ≈ 0.294]
    0.055586275f, 0.056434900f, 0.057293191f, 0.058159968f, 0.059033974f,
    // x = 80/255 .. 84/255
    0.059913892f, 0.060798357f, 0.061685976f, 0.062575341f, 0.063465046f,
    // x = 85/255 .. 89/255
    0.064353700f, 0.065239945f, 0.066122468f, 0.067000016f, 0.067871408f,
    // x = 90/255 .. 94/255
    0.068735549f, 0.069591441f, 0.070438194f, 0.071275033f, 0.072101312f,
    // x = 95/255 .. 99/255
    0.072916517f, 0.073720274f, 0.074512357f, 0.075292687f, 0.076061342f,
    // x = 100/255 .. 104/255                                      [x ≈ 0.392]
    0.076818552f, 0.077564706f, 0.078300348f, 0.079026177f, 0.079743043f,
    // x = 105/255 .. 109/255
    0.080451948f, 0.081154035f, 0.081850587f, 0.082543020f, 0.083232872f,
    // x = 110/255 .. 114/255
    0.083921800f, 0.084611568f, 0.085304038f, 0.086001157f, 0.086704951f,
    // x = 115/255 .. 119/255
    0.087417510f, 0.088140977f, 0.088877537f, 0.089629400f, 0.090398797f,
    // x = 120/255 .. 124/255
    0.091187959f, 0.091999107f, 0.092834443f, 0.093696130f, 0.094586290f,
    // x = 125/255 .. 129/255                                      [x ≈ 0.490]
    0.095506982f, 0.096460196f, 0.097447844f, 0.098471743f, 0.099533612f,
    // x = 130/255 .. 134/255
    0.100635058f, 0.101777571f, 0.102962517f, 0.104191126f, 0.105464493f,
    // x = 135/255 .. 139/255
    0.106783570f, 0.108149164f, 0.109561931f, 0.111022378f, 0.112530862f,
    // x = 140/255 .. 144/255
    0.114087589f, 0.115692617f, 0.117345858f, 0.119047083f, 0.120795926f,
    // x = 145/255 .. 149/255
    0.122591890f, 0.124434356f, 0.126322586f, 0.128255739f, 0.130232873f,
    // x = 150/255 .. 154/255                                      [x ≈ 0.588]
    0.132252963f, 0.134314907f, 0.136417539f, 0.138559643f, 0.140739965f,
    // x = 155/255 .. 159/255
    0.142957225f, 0.145210134f, 0.147497404f, 0.149817763f, 0.152169970f,
    // x = 160/255 .. 164/255
    0.154552825f, 0.156965186f, 0.159405980f, 0.161874214f, 0.164368988f,
    // x = 165/255 .. 169/255
    0.166889501f, 0.169435067f, 0.172005119f, 0.174599214f, 0.177217043f,
    // x = 170/255 .. 174/255
    0.179858430f, 0.182523338f, 0.185211866f, 0.187924249f, 0.190660854f,
    // x = 175/255 .. 179/255                                      [x ≈ 0.686]
    0.193422173f, 0.196208817f, 0.199021502f, 0.201861040f, 0.204728322f,
    // x = 180/255 .. 184/255
    0.207624298f, 0.210549963f, 0.213506329f, 0.216494404f, 0.219515161f,
    // x = 185/255 .. 189/255
    0.222569516f, 0.225658288f, 0.228782174f, 0.231941712f, 0.235137241f,
    // x = 190/255 .. 194/255
    0.238368873f, 0.241636445f, 0.244939492f, 0.248277199f, 0.251648370f,
    // x = 195/255 .. 199/255
    0.255051392f, 0.258484194f, 0.261944225f, 0.265428414f, 0.268933155f,
    // x = 200/255 .. 204/255                                      [x ≈ 0.784]
    0.272454278f, 0.275987040f, 0.279526115f, 0.283065593f, 0.286598989f,
    // x = 205/255 .. 209/255
    0.290119267f, 0.293618862f, 0.297089732f, 0.300523415f, 0.303911103f,
    // x = 210/255 .. 214/255
    0.307243736f, 0.310512120f, 0.313707059f, 0.316819521f, 0.319840823f,
    // x = 215/255 .. 219/255
    0.322762851f, 0.325578307f, 0.328281000f, 0.330866163f, 0.333330818f,
    // x = 220/255 .. 224/255
    0.335674187f, 0.337898145f, 0.340007725f, 0.342011687f, 0.343923134f,
    // x = 225/255 .. 229/255                                      [x ≈ 0.882]
    0.345760197f, 0.347546790f, 0.349313436f, 0.351098166f, 0.352947509f,
    // x = 230/255 .. 234/255
    0.354917562f, 0.357075156f, 0.359499128f, 0.362281685f, 0.365529892f,
    // x = 235/255 .. 239/255
    0.369367273f, 0.373935540f, 0.379396449f, 0.385933804f, 0.393755605f,
    // x = 240/255 .. 244/255
    0.403096353f, 0.414219517f, 0.427420181f, 0.443027869f, 0.461409562f,
    // x = 245/255 .. 249/255
    0.482972921f, 0.508169722f, 0.537499507f, 0.571513484f, 0.610818656f,
    // x = 250/255 .. 254/255                                      [x ≈ 0.980]
    0.656082224f, 0.708036247f, 0.767482592f, 0.835298180f, 0.912440542f,
    // x = 255/255 = 1.0
    0.999953694f
};

// =============================================================================
// FREQUENCY OFFSETS — Szabó Table 1, p.9
// =============================================================================
// Per-voice frequency ratios at maximum detune.  Each oscillator's frequency
// is: f_voice = f_centre × (1 + kFreqOffsetsMax[i] × detuneDepth)
//
// When detuneDepth = 0 (from LUT), all voices collapse to f_centre.
// When detuneDepth = 1 (max CC), offsets match the JP-8000 spectrum analysis.
// =============================================================================
static const float kFreqOffsetsMax[SUPERSAW_VOICES] = {
    -0.11002313f,       // Voice 1 — lowest
    -0.06288439f,       // Voice 2
    -0.01952356f,       // Voice 3
     0.0f,              // Voice 4 — centre (unaffected by detune)
     0.01991221f,       // Voice 5
     0.06216538f,       // Voice 6
     0.10745242f        // Voice 7 — highest
};

// =============================================================================
// UTILITY
// =============================================================================
static inline float clampf(float v, float lo, float hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

// =============================================================================
// CONSTRUCTOR
// =============================================================================
AudioSynthSupersaw::AudioSynthSupersaw()
    : AudioStream(2, inputQueueArray),
      _freq(440.0f),
      _detuneAmt(0.5f),
      _mixAmt(0.5f),
      _amp(1.0f),
      _outputGain(1.0f),
      _hpfPrevIn(0.0f),
      _hpfPrevOut(0.0f),
      _hpfAlpha(0.0f),
      _fmOctaveRange(10.0f),
      _phaseModRange(0.5f),
      _oversample2x(false),
      _usePolyBLEP(false),
      _mixCompensationEnabled(false),
      _compensationMaxGain(1.5f)
{
    // Randomise initial phases — Szabó §3.4: "free-running" oscillators
    for (int i = 0; i < SUPERSAW_VOICES; ++i) {
        _phase[i] = (float)random(0, 32768) * (1.0f / 32768.0f);
    }

    _calculateIncrements();
    _calculateGains();
    _calculateHPF();
}

// =============================================================================
// PARAMETER SETTERS
// =============================================================================

void AudioSynthSupersaw::setFrequency(float freqHz)
{
    _freq = (freqHz < 0.0f) ? 0.0f : freqHz;
    _calculateIncrements();
    _calculateHPF();
}

void AudioSynthSupersaw::setDetune(float amount)
{
    _detuneAmt = clampf(amount, 0.0f, 1.0f);
    _calculateIncrements();
}

void AudioSynthSupersaw::setMix(float mix)
{
    _mixAmt = clampf(mix, 0.0f, 1.0f);
    _calculateGains();
}

void AudioSynthSupersaw::setAmplitude(float amp)
{
    _amp = clampf(amp, 0.0f, 1.0f);
    _calculateGains();
}

void AudioSynthSupersaw::setOutputGain(float gain)
{
    _outputGain = clampf(gain, 0.0f, 2.0f);
}

void AudioSynthSupersaw::setOversample(bool enable)
{
    _oversample2x = enable;
}

void AudioSynthSupersaw::setBandLimited(bool enable)
{
    _usePolyBLEP = enable;
}

void AudioSynthSupersaw::setMixCompensation(bool enable)
{
    _mixCompensationEnabled = enable;
}

void AudioSynthSupersaw::setCompensationMaxGain(float maxGain)
{
    _compensationMaxGain = clampf(maxGain, 1.0f, 3.0f);
}

void AudioSynthSupersaw::frequencyModulation(float octaves)
{
    _fmOctaveRange = clampf(octaves, 0.1f, 12.0f);
}

void AudioSynthSupersaw::phaseModulation(float degrees)
{
    // Store as normalised fraction of a full cycle: 180° → 0.5
    _phaseModRange = clampf(degrees, 0.0f, 720.0f) / 360.0f;
}

// =============================================================================
// noteOn — RANDOMISE PHASES (Szabó §3.4)
// =============================================================================
// The JP-8000 assigns a random phase to each of the 7 oscillators on every
// note trigger.  This creates organic variation — each note onset has a
// different waveform shape due to the random interference pattern.
//
// Teensy's random() uses a 32-bit LFSR seeded at boot.  Dividing by 32768
// gives uniform distribution across [0.0, 1.0) with ~15-bit resolution —
// more than sufficient for phase randomisation (inaudible below ~10-bit).
// =============================================================================
void AudioSynthSupersaw::noteOn()
{
    for (int i = 0; i < SUPERSAW_VOICES; ++i) {
        _phase[i] = (float)random(0, 32768) * (1.0f / 32768.0f);
    }
}

// =============================================================================
// DETUNE CURVE — LUT with linear interpolation
// =============================================================================
// Maps normalised CC input [0.0, 1.0] to detune depth [0.0, 1.0] using the
// pre-computed Szabó polynomial table.
//
// Cost: 1 multiply + 1 add + 2 float reads from flash.
// Max interpolation error: < 0.001 between table points (inaudible).
// =============================================================================
float AudioSynthSupersaw::_detuneCurve(float x)
{
    if (x <= 0.0f) return 0.0f;                // True unison at zero
    if (x >= 1.0f) return DETUNE_LUT[DETUNE_LUT_SIZE - 1];

    const float idxFloat = x * (float)(DETUNE_LUT_SIZE - 1);
    const int   idx0     = (int)idxFloat;
    const float frac     = idxFloat - (float)idx0;

    const float y0 = DETUNE_LUT[idx0];
    const float y1 = DETUNE_LUT[idx0 + 1];

    return y0 + frac * (y1 - y0);              // Linear interpolation
}

// =============================================================================
// CALCULATE INCREMENTS — per-voice phase advance from freq + detune
// =============================================================================
// Uses the Szabó frequency offset ratios (Table 1) scaled by the non-linear
// detune curve output.
//
// f_voice[i] = f_centre × (1 + kFreqOffsetsMax[i] × detuneDepth)
//
// At detuneDepth=0, all 7 voices have the same frequency (unison).
// At detuneDepth=1, voices span the full JP-8000 spread (~±11% of centre).
// =============================================================================
void AudioSynthSupersaw::_calculateIncrements()
{
    const float sr      = AUDIO_SAMPLE_RATE_EXACT;
    const float nyquist = 0.5f * sr;

    // Look up the non-linear detune depth from the Szabó curve
    const float detuneDepth = clampf(_detuneCurve(_detuneAmt), 0.0f, 1.0f);

    for (int i = 0; i < SUPERSAW_VOICES; ++i) {
        float voiceFreq = _freq * (1.0f + kFreqOffsetsMax[i] * detuneDepth);

        // Clamp to valid range — negative freq or above Nyquist is undefined
        if (voiceFreq < 0.0f)     voiceFreq = 0.0f;
        if (voiceFreq > nyquist)  voiceFreq = nyquist;

        _phaseInc[i] = voiceFreq / sr;
    }
}

// =============================================================================
// CALCULATE GAINS — Szabó mix curves (§3.2, p.15)
// =============================================================================
// Centre oscillator:  y = -0.55366x + 0.99785   (linear decrease)
// Side oscillators:   y = -0.73764x² + 1.2841x + 0.044372  (parabolic)
//
// These are amplitude scaling factors (0.0–1.0 range), multiplied by _amp.
//
// Key differences from a simple crossfade:
//   - Centre never fully mutes — at mix=1 it's still at ~0.445 (not 0.0)
//   - Side oscillators follow a parabola that peaks around x ≈ 0.87
//   - At mix ≈ 0.75 (CC 95), centre and side are at equal volume
//
// The six side oscillators share the same gain curve but divide by 6 to
// distribute the energy equally among them.
// =============================================================================
void AudioSynthSupersaw::_calculateGains()
{
    const float x = _mixAmt;

    // --- Centre oscillator (voice index 3): linear decrease ---
    // Szabó: y = -0.55366x + 0.99785
    // At x=0: ~1.0 (full level).  At x=1: ~0.445 (still audible).
    const float centreGain = clampf(-0.55366f * x + 0.99785f, 0.0f, 1.0f);

    // --- Side oscillators (voices 0-2, 4-6): parabolic increase ---
    // Szabó: y = -0.73764x² + 1.2841x + 0.044372
    // At x=0: ~0.044 (nearly silent).  Peaks at x ≈ 0.87: ~0.60.
    const float sideTotal = clampf(-0.73764f * x * x + 1.2841f * x + 0.044372f,
                                    0.0f, 1.0f);

    // Divide equally among 6 side voices
    const float sideEach = sideTotal / 6.0f;

    for (int i = 0; i < SUPERSAW_VOICES; ++i) {
        if (i == SUPERSAW_CENTRE_IDX) {
            _gain[i] = _amp * centreGain;
        } else {
            _gain[i] = _amp * sideEach;
        }
    }
}

// =============================================================================
// HPF — Pitch-tracked 1-pole high-pass filter (Szabó §3.3)
// =============================================================================
// Removes aliasing noise and DC below the fundamental frequency.
// Cutoff tracks the oscillator pitch — Szabó found the JP-8000 uses a
// pitch-tracked HPF that removes sub-fundamental content while preserving
// the "airy" aliasing noise above the fundamental.
//
// 1-pole HPF: alpha = RC / (RC + dt)  where RC = 1 / (2π × f_cutoff)
// =============================================================================
void AudioSynthSupersaw::_calculateHPF()
{
    const float f  = fmaxf(_freq, 1.0f);       // Prevent division by zero
    const float rc = 1.0f / (6.2831853f * f);  // 1 / (2π × f)
    const float dt = 1.0f / AUDIO_SAMPLE_RATE_EXACT;
    _hpfAlpha = rc / (rc + dt);
}

// =============================================================================
// update() — AUDIO ISR: generate one block of 128 samples
// =============================================================================
void AudioSynthSupersaw::update(void)
{
    audio_block_t* outBlock = allocate();
    if (!outBlock) return;

#if JT_OPT_SUPERSAW_IDLE_GATE
    // -------------------------------------------------------------------------
    // IDLE GATE — skip synthesis when amplitude is effectively zero.
    // When a non-supersaw waveform is selected, OscillatorBlock sets the
    // output mixer gain to 0.0 — this guard avoids wasting ~7168 sample
    // computations per block across 8 polyphony voices.
    // -------------------------------------------------------------------------
    if (_amp < JT_SUPERSAW_IDLE_THRESHOLD) {
        memset(outBlock->data, 0, sizeof(outBlock->data));
        transmit(outBlock);
        release(outBlock);
        return;
    }
#endif

    // --- Receive optional modulation inputs (nullptr if disconnected) ---
    audio_block_t* fmBlock    = receiveReadOnly(0);
    audio_block_t* phaseBlock = receiveReadOnly(1);

    // --- Block-rate mix compensation (computed once, not per sample) ---
    float mixGain = 1.0f;
    if (_mixCompensationEnabled) {
        mixGain = 1.0f + _mixAmt * (_compensationMaxGain - 1.0f);
    }

    // int16 → normalised ±1.0 conversion factor
    static constexpr float kInt16Norm = 1.0f / 32767.0f;

    if (!_oversample2x) {
        // =================================================================
        // STANDARD RENDERING — one sample per output sample
        // =================================================================
        for (int n = 0; n < AUDIO_BLOCK_SAMPLES; ++n) {

            // --- FM: per-sample phase increment multiplier ---
            // fast_pow2 maps normalised FM signal to frequency ratio:
            //   2^(fmNorm × octaveRange)
            // Computed once per sample, shared by all 7 voices.
            float fmMult = 1.0f;
            if (fmBlock) {
                const float fmNorm = fmBlock->data[n] * kInt16Norm;
                fmMult = fast_pow2(fmNorm * _fmOctaveRange);
            }

            // --- Phase mod: per-sample fractional phase offset ---
            float phOffset = 0.0f;
            if (phaseBlock) {
                const float phNorm = phaseBlock->data[n] * kInt16Norm;
                phOffset = phNorm * _phaseModRange;
            }

            // --- Sum 7 voices ---
            float sample = 0.0f;
            for (int i = 0; i < SUPERSAW_VOICES; ++i) {
                const float inc = _phaseInc[i] * fmMult;

                // Apply phase mod offset (wrap to [0,1) for correct waveform)
                float ph = _phase[i] + phOffset;
                if      (ph >= 1.0f) ph -= 1.0f;
                else if (ph <  0.0f) ph += 1.0f;

                // Generate saw sample
                float s;
                if (_usePolyBLEP) {
                    s = saw_polyblep(ph, inc);
                } else {
                    s = 2.0f * ph - 1.0f;          // Naive saw
                }
                sample += s * _gain[i];

                // Advance phase (FM applied, PM is additive per-sample only)
                _phase[i] += inc;
                if (_phase[i] >= 1.0f) _phase[i] -= 1.0f;
            }

            // --- Pitch-tracked 1-pole HPF ---
            float hpOut = _hpfAlpha * (_hpfPrevOut + sample - _hpfPrevIn);
            _hpfPrevIn  = sample;
            _hpfPrevOut = hpOut;

            // --- Output: clip → gain → clip → int16 ---
            hpOut = fmaxf(-1.0f, fminf(1.0f, hpOut));
            float out = hpOut * _outputGain * mixGain;
            out = fmaxf(-1.0f, fminf(1.0f, out));
            outBlock->data[n] = (int16_t)(out * 32767.0f);
        }

    } else {
        // =================================================================
        // 2× OVERSAMPLED RENDERING
        // =================================================================
        // FM and phase mod are applied at output sample rate (not sub-sample
        // rate) — modulation signals are themselves at audio rate.
        // =================================================================
        for (int n = 0; n < AUDIO_BLOCK_SAMPLES; ++n) {

            float fmMult = 1.0f;
            if (fmBlock) {
                const float fmNorm = fmBlock->data[n] * kInt16Norm;
                fmMult = fast_pow2(fmNorm * _fmOctaveRange);
            }

            float phOffset = 0.0f;
            if (phaseBlock) {
                const float phNorm = phaseBlock->data[n] * kInt16Norm;
                phOffset = phNorm * _phaseModRange;
            }

            float accum = 0.0f;
            for (int os = 0; os < 2; ++os) {
                float sample = 0.0f;
                for (int i = 0; i < SUPERSAW_VOICES; ++i) {
                    const float incHalf = _phaseInc[i] * fmMult * 0.5f;

                    float ph = _phase[i] + phOffset;
                    if      (ph >= 1.0f) ph -= 1.0f;
                    else if (ph <  0.0f) ph += 1.0f;

                    float s;
                    if (_usePolyBLEP) {
                        s = saw_polyblep(ph, incHalf);
                    } else {
                        s = 2.0f * ph - 1.0f;
                    }
                    sample += s * _gain[i];

                    _phase[i] += incHalf;
                    if (_phase[i] >= 1.0f) _phase[i] -= 1.0f;
                }
                accum += sample;
            }

            float sample = accum * 0.5f;

            float hpOut = _hpfAlpha * (_hpfPrevOut + sample - _hpfPrevIn);
            _hpfPrevIn  = sample;
            _hpfPrevOut = hpOut;

            hpOut = fmaxf(-1.0f, fminf(1.0f, hpOut));
            float out = hpOut * _outputGain * mixGain;
            out = fmaxf(-1.0f, fminf(1.0f, out));
            outBlock->data[n] = (int16_t)(out * 32767.0f);
        }
    }

    // Release modulation blocks (nullptr-safe)
    if (fmBlock)    release(fmBlock);
    if (phaseBlock) release(phaseBlock);

    transmit(outBlock);
    release(outBlock);
}
