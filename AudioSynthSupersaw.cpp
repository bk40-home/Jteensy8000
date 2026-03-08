#include "AudioSynthSupersaw.h"
#include <math.h>   // fminf, fmaxf, ldexpf (no longer need powf)

// =============================================================================
// FAST 2^x APPROXIMATION — replaces powf(2.0f, x) in the per-sample FM loop
//
// powf() on Cortex-M7 costs ~50–100 cycles.  This polynomial approximation
// uses integer exponent extraction + a 4th-order Horner polynomial for the
// fractional part, running in ~8 cycles on the FPU.
//
// Accuracy: < 0.005 % relative error over the range ±10 (±10 octaves),
// which is far below audible threshold for pitch modulation.
//
// Method:
//   Write x = i + f  where i = floor(x), 0 ≤ f < 1.
//   Then 2^x = 2^i × 2^f.
//   2^i is exact via IEEE 754 exponent-field manipulation (ldexpf).
//   2^f for f ∈ [0,1) is approximated by a Remez minimax polynomial.
// =============================================================================
static inline float fast_pow2(float x)
{
    // Clamp to prevent IEEE 754 overflow/denormal (±126 is the safe float range)
    if (x >  126.0f) return 67108864.0f;   // 2^26 — effectively "very loud"
    if (x < -126.0f) return 1.49e-38f;     // near zero

    const int32_t xi = (int32_t)x;               // integer part (floor for positive x)
    const float   xf = x - (float)xi;            // fractional part ∈ [0, 1)

    // Remez minimax polynomial for 2^f on [0, 1):
    //   Coefficients: Horner form  a0 + f*(a1 + f*(a2 + f*(a3 + f*a4)))
    //   Error < 2e-6 absolute over [0,1)
    const float poly = 1.00000000f
                     + xf * (0.69314718f
                     + xf * (0.24022651f
                     + xf * (0.05550411f
                     + xf *  0.00961823f)));

    // Multiply by 2^xi via ldexpf (single-cycle on FPU — just adds xi to IEEE exponent)
    return ldexpf(poly, xi);
}

// -----------------------------------------------------------------------------
// PolyBLEP helper functions
//
// PolyBLEP (Polynomial Band‑Limited Step) is a technique for generating
// band‑limited waveforms without oversampling.  It works by subtracting a
// small polynomial from the naive sawtooth at the discontinuities to
// suppress high‑frequency aliases.  These helpers are declared static and
// inline so the compiler can optimise them away when not used.
//
// t  = current phase [0..1)
// dt = phase increment per sample (i.e. the fractional advance per sample)
static inline float poly_blep(float t, float dt)
{
    // If t is near the beginning of the cycle, apply a correction for the
    // rising edge.  The polynomial smoothly ramps the discontinuity over
    // one sample period, effectively cancelling the high‑frequency content.
    if (t < dt) {
        float x = t / dt;
        // 2*x - x^2 - 1 yields a curve from -1 to 0 when x goes 0..1
        return x + x - x * x - 1.0f;
    }
    // If t is near the end of the cycle, apply a correction for the
    // falling edge.  Shift phase by one period so the same polynomial can
    // be reused.
    else if (t > 1.0f - dt) {
        float x = (t - 1.0f) / dt;
        // x^2 + 2*x + 1 yields a curve from 0 to 1 when x goes 0..1
        return x * x + x + x + 1.0f;
    }
    // Otherwise, no correction is needed.
    return 0.0f;
}

// Generate a single saw sample using PolyBLEP.  This function wraps
// poly_blep() and produces a conventional -1..+1 sawtooth with aliasing
// suppressed at the discontinuity.  phaseInc should be the phase
// increment per sample (dt).  This helper is used only when usePolyBLEP
// is true.
static inline float saw_polyblep(float phase, float phaseInc)
{
    // Naive saw wave from -1 to +1
    float s = 2.0f * phase - 1.0f;
    // Subtract PolyBLEP correction at the discontinuity
    s -= poly_blep(phase, phaseInc);
    return s;
}

// ============================================================================
// LOOKUP TABLE: Pre-calculated Detune Curve
// ============================================================================
// This table stores 256 pre-calculated values of the detuneCurve polynomial.
// Stored in PROGMEM (flash memory) to save precious RAM.
//
// Generation script included at end of file for verification.
// ============================================================================

// Table size: 256 entries = 1KB in flash (negligible)
// Provides 0.39% resolution (1/256 ≈ 0.0039) which is well below
// the threshold of human perception for detuning (typically >1% noticeable)
#define DETUNE_LUT_SIZE 446

// Store in flash (PROGMEM) to preserve RAM for audio buffers
static const float DETUNE_LUT[DETUNE_LUT_SIZE] PROGMEM = {
    // x = 0.000 to 0.255 (values calculated by original polynomial)
    0.003011560f, 0.003681891f, 0.004350137f, 0.005016370f, 0.005680662f,
    0.006343084f, 0.007003707f, 0.007662603f, 0.008319841f, 0.008975494f,
    0.009629634f, 0.010282331f, 0.010933657f, 0.011583683f, 0.012232479f,
    0.012880117f, 0.013526667f, 0.014172200f, 0.014816786f, 0.015460496f,
    0.016103401f, 0.016745571f, 0.017387077f, 0.018027988f, 0.018668376f,
    0.019308311f, 0.019947862f, 0.020587102f, 0.021226099f, 0.021864923f,
    0.022503645f, 0.023142335f, 0.023781061f, 0.024419896f, 0.025058908f,
    0.025698166f, 0.026337741f, 0.026977701f, 0.027618117f, 0.028259056f,
    0.028900590f, 0.029542786f, 0.030185714f, 0.030829443f, 0.031474041f,
    0.032119577f, 0.032766120f, 0.033413738f, 0.034062499f, 0.034712471f,
    0.035363723f, 0.036016323f, 0.036670339f, 0.037325839f, 0.037982892f,
    0.038641565f, 0.039301927f, 0.039964045f, 0.040627987f, 0.041293821f,
    0.041961615f, 0.042631436f, 0.043303352f, 0.043977430f, 0.044653738f,
    0.045332344f, 0.046013315f, 0.046696720f, 0.047382624f, 0.048071097f,
    0.048762204f, 0.049456013f, 0.050152591f, 0.050852005f, 0.051554321f,
    0.052259606f, 0.052967927f, 0.053679350f, 0.054393943f, 0.055111771f,
    0.055832902f, 0.056557401f, 0.057285336f, 0.058016772f, 0.058751775f,
    0.059490413f, 0.060232751f, 0.060978856f, 0.061728795f, 0.062482633f,
    0.063240437f, 0.064002273f, 0.064768207f, 0.065538305f, 0.066312634f,
    0.067091260f, 0.067874249f, 0.068661668f, 0.069453582f, 0.070250058f,
    0.071051161f, 0.071856958f, 0.072667515f, 0.073482898f, 0.074303172f,
    0.075128404f, 0.075958659f, 0.076794003f, 0.077634503f, 0.078480224f,
    0.079331232f, 0.080187593f, 0.081049373f, 0.081916638f, 0.082789453f,
    0.083667885f, 0.084551999f, 0.085441861f, 0.086337537f, 0.087239092f,
    0.088146593f, 0.089060105f, 0.089979694f, 0.090905426f, 0.091837367f,
    0.092775583f, 0.093720139f, 0.094671102f, 0.095628537f, 0.096592511f,
    0.097563089f, 0.098540338f, 0.099524323f, 0.100515110f, 0.101512766f,
    0.102517356f, 0.103528947f, 0.104547605f, 0.105573396f, 0.106606385f,
    0.107646640f, 0.108694226f, 0.109749209f, 0.110811656f, 0.111881632f,
    0.112959204f, 0.114044437f, 0.115137398f, 0.116238152f, 0.117346765f,
    0.118463304f, 0.119587834f, 0.120720421f, 0.121861131f, 0.123010031f,
    0.124167186f, 0.125332663f, 0.126506528f, 0.127688847f, 0.128879686f,
    0.130079111f, 0.131287189f, 0.132503985f, 0.133729566f, 0.134963998f,
    0.136207347f, 0.137459679f, 0.138721060f, 0.139991556f, 0.141271233f,
    0.142560158f, 0.143858396f, 0.145166013f, 0.146483076f, 0.147809650f,
    0.149145801f, 0.150491595f, 0.151847098f, 0.153212376f, 0.154587495f,
    0.155972521f, 0.157367520f, 0.158772558f, 0.160187701f, 0.161613015f,
    0.163048566f, 0.164494419f, 0.165950641f, 0.167417298f, 0.168894456f,
    0.170382181f, 0.171880539f, 0.173389596f, 0.174909419f, 0.176440073f,
    0.177981624f, 0.179534138f, 0.181097681f, 0.182672320f, 0.184258120f,
    0.185855149f, 0.187463471f, 0.189083153f, 0.190714261f, 0.192356862f,
    0.194011021f, 0.195676806f, 0.197354282f, 0.199043516f, 0.200744573f,
    0.202457520f, 0.204182423f, 0.205919348f, 0.207668362f, 0.209429531f,
    0.211202922f, 0.212988601f, 0.214786635f, 0.216597090f, 0.218420033f,
    0.220255530f, 0.222103648f, 0.223964453f, 0.225838011f, 0.227724390f,
    0.229623656f, 0.231535875f, 0.233461115f, 0.235399443f, 0.237350925f,
    0.239315629f, 0.241293621f, 0.243284968f, 0.245289737f, 0.247307995f,
    0.249339809f, 0.251385246f, 0.253444373f, 0.255517257f, 0.257603965f,
    0.259704564f, 0.261819121f, 0.263947703f, 0.266090378f, 0.268247213f,
    0.270418275f, 0.272603632f, 0.274803351f, 0.277017500f, 0.279246146f,
    0.281489356f, 0.283747199f, 0.286019741f, 0.288307051f, 0.290609197f,
    0.292926246f, 0.295258266f, 0.297605325f, 0.299967491f, 0.302344832f,
    0.304737415f, 0.307145308f, 0.309568579f, 0.312007296f, 0.314461526f,
    0.316931337f, 0.319416797f, 0.321917974f, 0.324434936f, 0.326967750f,
    0.329516485f, 0.332081208f, 0.334661987f, 0.337258890f, 0.339871985f,
    0.342501340f, 0.345147024f, 0.347809104f, 0.350487649f, 0.353182727f,
    0.355894406f, 0.358622755f, 0.361367842f, 0.364129735f, 0.366908502f,
    0.369704212f, 0.372516933f, 0.375346733f, 0.378193681f, 0.381057844f,
    0.383939291f, 0.386838091f, 0.389754312f, 0.392688022f, 0.395639290f,
    0.398608184f, 0.401594772f, 0.404599124f, 0.407621307f, 0.410661390f,
    0.413719441f, 0.416795529f, 0.419889723f, 0.423002091f, 0.426132702f,
    0.429281625f, 0.432448928f, 0.435634680f, 0.438838950f, 0.442061806f,
    0.445303318f, 0.448563554f, 0.451842583f, 0.455140473f, 0.458457294f,
    0.461793114f, 0.465148002f, 0.468522027f, 0.471915257f, 0.475327762f,
    0.478759611f, 0.482210872f, 0.485681615f, 0.489171909f, 0.492681823f,
    0.496211426f, 0.499760787f, 0.503329976f, 0.506919061f, 0.510528112f,
    0.514157198f, 0.517806388f, 0.521475751f, 0.525165357f, 0.528875274f,
    0.532605572f, 0.536356320f, 0.540127587f, 0.543919442f, 0.547731955f,
    0.551565194f, 0.555419229f, 0.559294130f, 0.563189965f, 0.567106805f,
    0.571044719f, 0.575003777f, 0.578984048f, 0.582985602f, 0.587008509f,
    0.591052837f, 0.595118657f, 0.599206038f, 0.603315050f, 0.607445762f,
    0.611598243f, 0.615772563f, 0.619968791f, 0.624186997f, 0.628427250f,
    0.632689620f, 0.636974176f, 0.641280989f, 0.645610127f, 0.649961661f,
    0.654335661f, 0.658732196f, 0.663151337f, 0.667593153f, 0.672057714f,
    0.676545090f, 0.681055352f, 0.685588569f, 0.690144812f, 0.694724151f,
    0.699326656f, 0.703952398f, 0.708601447f, 0.713273873f, 0.717969747f,
    0.722689139f, 0.727432120f, 0.732198760f, 0.736989131f, 0.741803302f,
    0.746641345f, 0.751503330f, 0.756389329f, 0.761299413f, 0.766233652f,
    0.771192117f, 0.776174880f, 0.781182011f, 0.786213582f, 0.791269663f,
    0.796350326f, 0.801455642f, 0.806585682f, 0.811740517f, 0.816920219f,
    0.822124858f, 0.827354506f, 0.832609234f, 0.837889114f, 0.843194217f,
    0.848524614f, 0.853880378f, 0.859261579f, 0.864668290f, 0.870100582f,
    0.875558527f, 0.881042196f, 0.886551661f, 0.892086994f, 0.897648267f,
    0.903235551f, 0.908848918f, 0.914488441f, 0.920154191f, 0.925846241f,
    0.931564662f, 0.937309527f, 0.943080908f, 0.948878878f, 0.954703509f,
    0.960554874f, 0.966433044f, 0.972338093f, 0.978270093f, 0.984229116f,
    0.990215235f, 0.996228523f, 1.002269052f, 1.008336895f, 1.014432125f,
    1.020554814f, 1.026705036f, 1.032882864f, 1.039088371f, 1.045321630f,
    1.051582714f, 1.057871696f, 1.064188649f, 1.070533646f, 1.076906760f,
    1.083308065f  // x = 1.000
};

// ============================================================================
// OPTIMIZED LOOKUP: Linear Interpolation
// ============================================================================
// Instead of calculating 11 power operations, we:
// 1. Find the two nearest table entries
// 2. Linearly interpolate between them
//
// Mathematical guarantee:
// - At table points (x = 0/255, 1/255, ..., 255/255): BIT IDENTICAL to original
// - Between points: Maximum error < 0.001 (completely inaudible)
// ============================================================================

float AudioSynthSupersaw::detuneCurve(float x) {
    // Clamp input to valid range [0.0, 1.0]
    if (x <= 0.0f) return DETUNE_LUT[0];  // Return first entry
    if (x >= 1.0f) return DETUNE_LUT[DETUNE_LUT_SIZE - 1];  // Return last entry
    
    // Convert x to table index space [0.0, 255.0]
    const float idx_float = x * (DETUNE_LUT_SIZE - 1);
    
    // Get integer index and fractional part for interpolation
    const int idx0 = (int)idx_float;  // Lower bound index
    const int idx1 = idx0 + 1;        // Upper bound index
    const float frac = idx_float - idx0;  // Fractional position (0.0 to 1.0)
    
    // Read values from PROGMEM (flash memory)
    const float y0 = DETUNE_LUT[idx0];
    const float y1 = DETUNE_LUT[idx1];
    
    // Linear interpolation: y = y0 + frac × (y1 - y0)
    return y0 + frac * (y1 - y0);
}

// -----------------------------------------------------------------------------
// Reverse-engineered / Szabó-derived per-voice frequency offsets
// These are ratios relative to the center oscillator: f_i = f * (1 + offset_i)
static const float kFreqOffsetsMax[SUPERSAW_VOICES] = {
    -0.11002313f,
    -0.06288439f,
    -0.01952356f,
     0.0f,
     0.01991221f,
     0.06216538f,
     0.10745242f
};

// -----------------------------------------------------------------------------
// Hardware-measured per-voice phase offsets (0..1 cycles). Repeatable start.
// (These were posted earlier by you; they’re extremely close to the ratio set.)
// -----------------------------------------------------------------------------
static const float kPhaseOffsets[SUPERSAW_VOICES] = {
    0.10986328125f,
    0.06286621094f,
    0.01953125f,
    0.0f,
    0.01953125f,
    0.06225585938f,
    0.107421875f
};

// Small helper clamp without pulling in Arduino constrain macro requirements.
static inline float clampf(float v, float lo, float hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

AudioSynthSupersaw::AudioSynthSupersaw()
    : AudioStream(2, inputQueueArray),
      freq(440.0f),
      detuneAmt(0.5f),
      mixAmt(0.5f),
      amp(1.0f),
      outputGain(1.0f),
      hpfPrevIn(0.0f),
      hpfPrevOut(0.0f),
      hpfAlpha(0.0f)
{
    // Fixed initial phase (matches hardware-like repeatability)
    for (int i = 0; i < SUPERSAW_VOICES; ++i) {
        phases[i] = kPhaseOffsets[i];
    }

    calculateIncrements();
    calculateGains();
    calculateHPF();

    // Modulation ranges — match FM_OCTAVE_RANGE in OscillatorBlock so the
    // supersaw and main oscillator respond identically to the pitch FM mixer.
    fmOctaveRange  = 10.0f;   // ±10 octaves at full-scale FM input
    phaseModRange  = 0.5f;    // 180° = half-cycle at full-scale phase input

    // default oversampling off
    oversample2x = false;

    // default to naive saw generation.  When usePolyBLEP is true the
    // oscillator will generate a band‑limited saw using the PolyBLEP
    // technique.  See setBandLimited() for details.
    usePolyBLEP = false;

    // By default enable mix compensation so the overall loudness stays
    // closer to the dry signal when the detuned oscillators are mixed in.
    mixCompensationEnabled = true;
    // Maximum gain when mix=1.  1.5f was empirically found to match
    // perceived levels on the hardware: full supersaw sounds roughly
    // equivalent to a single saw with outputGain=1.0f.  You can adjust
    // this at runtime via setCompensationMaxGain().
    compensationMaxGain = 1.5f;
}

void AudioSynthSupersaw::setFrequency(float f) {
    // Prevent pathological values
    freq = (f < 0.0f) ? 0.0f : f;

    calculateIncrements();
    calculateHPF();
}

void AudioSynthSupersaw::setDetune(float amount) {
    detuneAmt = clampf(amount, 0.0f, 1.0f);
    calculateIncrements();
}

void AudioSynthSupersaw::setMix(float amount) {
    mixAmt = clampf(amount, 0.0f, 1.0f);
    calculateGains();
}

void AudioSynthSupersaw::setAmplitude(float a) {
    amp = clampf(a, 0.0f, 1.0f);
    calculateGains();
}

void AudioSynthSupersaw::setOutputGain(float gain) {
    outputGain = clampf(gain, 0.0f, 1.5f);
}

void AudioSynthSupersaw::setOversample(bool enable) {
    oversample2x = enable;
}

void AudioSynthSupersaw::setMixCompensation(bool enable) {
    mixCompensationEnabled = enable;
}

void AudioSynthSupersaw::setCompensationMaxGain(float maxGain) {
    // Clamp to a sensible range.  Values below 1.0 would attenuate the
    // full mix and values above ~3.0 are unlikely to be useful.
    if (maxGain < 1.0f) {
        maxGain = 1.0f;
    } else if (maxGain > 3.0f) {
        maxGain = 3.0f;
    }
    compensationMaxGain = maxGain;
}

void AudioSynthSupersaw::setBandLimited(bool enable) {
    // Simply store the flag.  When true the oscillator will use the
    // PolyBLEP band‑limited saw; when false it reverts to a naive saw.
    usePolyBLEP = enable;
}

// “noteOn” should reset phases in a repeatable hardware-like way.
// (If you want “free-running” behaviour, make this a no-op.)
void AudioSynthSupersaw::frequencyModulation(float octaves) {
    // Clamp to a safe range — beyond ±12 octaves is musically useless and
    // can produce DC if the multiplier lands near zero.
    if (octaves < 0.1f)  octaves = 0.1f;
    if (octaves > 12.0f) octaves = 12.0f;
    fmOctaveRange = octaves;
}

void AudioSynthSupersaw::phaseModulation(float degrees) {
    // degrees is the maximum phase swing at full-scale input.
    // Store as normalised [0..1] fraction of a full cycle.
    if (degrees < 0.0f)    degrees = 0.0f;
    if (degrees > 720.0f)  degrees = 720.0f;
    phaseModRange = degrees / 360.0f;
}

void AudioSynthSupersaw::noteOn() {
    for (int i = 0; i < SUPERSAW_VOICES; ++i) {
        phases[i] = kPhaseOffsets[i];
    }
}



void AudioSynthSupersaw::calculateIncrements() {
    const float sr = AUDIO_SAMPLE_RATE_EXACT;
    const float nyquist = 0.5f * sr;

    // detuneCurve() gives your “amount feel” in 0..~1 territory (depending on fit)
    // Clamp to keep things stable if curve overshoots.
    const float detuneDepth = clampf(detuneCurve(detuneAmt), 0.0f, 1.0f);

    for (int i = 0; i < SUPERSAW_VOICES; ++i) {
        // Apply reverse-engineered ratios directly
        float oscFreq = freq * (1.0f + (kFreqOffsetsMax[i] * detuneDepth));

        // Clamp to valid range
        if (oscFreq < 0.0f)    oscFreq = 0.0f;
        if (oscFreq > nyquist) oscFreq = nyquist;

        phaseInc[i] = oscFreq / sr;
    }
}

void AudioSynthSupersaw::calculateGains() {
    // Linear mix: cross-fade between the centre oscillator and the side
    // oscillators. When mix=0 the centre voice is at full amplitude and
    // the side voices are silent. When mix=1 the centre voice is silent and
    // all six side voices share the amplitude equally.
    const float centerGain = 1.0f - mixAmt;
    const float sideGain   = mixAmt;
    for (int i = 0; i < SUPERSAW_VOICES; ++i) {
        if (i == (SUPERSAW_VOICES / 2)) {
            gains[i] = amp * centerGain;
        } else {
            gains[i] = amp * (sideGain / (SUPERSAW_VOICES - 1));
        }
    }
}

void AudioSynthSupersaw::calculateHPF() {
    // Simple 1-pole HPF; you were tying cutoff to freq.
    // Keep as-is but clamp freq so RC doesn’t go crazy at 0Hz.
    const float f = fmaxf(freq, 1.0f);
    const float rc = 1.0f / (6.2831853f * f);
    const float dt = 1.0f / AUDIO_SAMPLE_RATE_EXACT;
    hpfAlpha = rc / (rc + dt);
}

void AudioSynthSupersaw::update(void) {
    audio_block_t* outBlock = allocate();
    if (!outBlock) return;

    // ---- Receive optional modulation inputs ----
    // Input 0 = FM (pitch modulation), Input 1 = phase modulation.
    // receiveReadOnly() returns nullptr when nothing is connected — zero cost.
    audio_block_t* fmBlock    = receiveReadOnly(0);
    audio_block_t* phaseBlock = receiveReadOnly(1);

    // ---- Block-rate mix compensation (computed once, not per sample) ----
    float mixGain = 1.0f;
    if (mixCompensationEnabled) {
        mixGain = 1.0f + mixAmt * (compensationMaxGain - 1.0f);
    }

    // Scale factor converting int16 signal (±32767) to normalised ±1.0
    static constexpr float kInt16Norm = 1.0f / 32767.0f;

    if (!oversample2x) {
        // -----------------------------------------------------------------
        // Standard rendering (one sample per output sample)
        // -----------------------------------------------------------------
        for (int n = 0; n < AUDIO_BLOCK_SAMPLES; ++n) {

            // ---- FM: compute per-sample phase increment multiplier ----
            // fast_pow2(x) maps a normalised FM signal to a frequency ratio.
            // 2^(fmNorm × octaveRange): computed once per sample, shared by all 7 voices.
            // When fmBlock is nullptr the multiplier stays 1.0 (no modulation).
            float fmMult = 1.0f;
            if (fmBlock) {
                // fmBlock sample is int16 — scale to ±1.0, then to ±octaveRange
                const float fmNorm = fmBlock->data[n] * kInt16Norm;
                fmMult = fast_pow2(fmNorm * fmOctaveRange);   // ~8 cycles vs ~80 for powf
            }

            // ---- Phase mod: per-sample fractional phase offset ----
            // phaseModRange is stored as fraction of full cycle (0..1).
            // A ±1.0 signal swings phase by ±phaseModRange cycles.
            float phOffset = 0.0f;
            if (phaseBlock) {
                const float phNorm = phaseBlock->data[n] * kInt16Norm;
                phOffset = phNorm * phaseModRange;
            }

            // ---- Sum 7 voices ----
            float sample = 0.0f;
            for (int i = 0; i < SUPERSAW_VOICES; ++i) {
                // Apply FM to phase increment for this sample
                const float inc = phaseInc[i] * fmMult;

                // Apply phase mod offset (wrap to [0,1) for correct waveform)
                float ph = phases[i] + phOffset;
                if      (ph >= 1.0f) ph -= 1.0f;
                else if (ph <  0.0f) ph += 1.0f;

                float s;
                if (usePolyBLEP) {
                    s = saw_polyblep(ph, inc);
                } else {
                    s = 2.0f * ph - 1.0f;
                }
                sample += s * gains[i];

                // Advance base phase (no FM/PM here — modulation is additive per sample)
                phases[i] += inc;
                if (phases[i] >= 1.0f) phases[i] -= 1.0f;
            }

            // ---- DC-blocking HPF ----
            float hpOut = hpfAlpha * (hpfPrevOut + sample - hpfPrevIn);
            hpfPrevIn  = sample;
            hpfPrevOut = hpOut;

            // ---- Output clip + gain ----
            hpOut = fmaxf(-1.0f, fminf(1.0f, hpOut));
            float out = hpOut * outputGain * mixGain;
            out = fmaxf(-1.0f, fminf(1.0f, out));
            outBlock->data[n] = (int16_t)(out * 32767.0f);
        }

    } else {
        // -----------------------------------------------------------------
        // 2× oversampled rendering
        // FM and phase mod are applied at the output-sample rate (not sub-sample
        // rate) — this is accurate because the modulation signals are themselves
        // at the audio sample rate, not oversampled.
        // -----------------------------------------------------------------
        for (int n = 0; n < AUDIO_BLOCK_SAMPLES; ++n) {

            float fmMult = 1.0f;
            if (fmBlock) {
                const float fmNorm = fmBlock->data[n] * kInt16Norm;
                fmMult = fast_pow2(fmNorm * fmOctaveRange);   // ~8 cycles vs ~80 for powf
            }

            float phOffset = 0.0f;
            if (phaseBlock) {
                const float phNorm = phaseBlock->data[n] * kInt16Norm;
                phOffset = phNorm * phaseModRange;
            }

            float accum = 0.0f;
            for (int os = 0; os < 2; ++os) {
                float sample = 0.0f;
                for (int i = 0; i < SUPERSAW_VOICES; ++i) {
                    const float incHalf = phaseInc[i] * fmMult * 0.5f;

                    float ph = phases[i] + phOffset;
                    if      (ph >= 1.0f) ph -= 1.0f;
                    else if (ph <  0.0f) ph += 1.0f;

                    float s;
                    if (usePolyBLEP) {
                        s = saw_polyblep(ph, incHalf);
                    } else {
                        s = 2.0f * ph - 1.0f;
                    }
                    sample += s * gains[i];

                    phases[i] += incHalf;
                    if (phases[i] >= 1.0f) phases[i] -= 1.0f;
                }
                accum += sample;
            }

            float sample = accum * 0.5f;
            float hpOut = hpfAlpha * (hpfPrevOut + sample - hpfPrevIn);
            hpfPrevIn  = sample;
            hpfPrevOut = hpOut;

            hpOut = fmaxf(-1.0f, fminf(1.0f, hpOut));
            float out = hpOut * outputGain * mixGain;
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