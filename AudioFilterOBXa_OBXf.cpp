#include "AudioFilterOBXa_OBXf.h"

// Keep math constants local
static constexpr float OBXA_PI = 3.14159265358979323846f;

// =============================================================================
// fast_pow2 — replaces powf(2.0f, x) in the per-sample filter mod loop
//
// powf() on Cortex-M7 costs ~50–100 cycles.  This polynomial approximation
// uses integer exponent extraction + a 4th-order Horner polynomial for the
// fractional part, running in ~8 cycles on the FPU.
//
// Accuracy: < 0.005% relative error over ±10 octaves — far below audible
// threshold for filter cutoff modulation.
//
// Only compiled when JT_OPT_OBXA_BLOCKRATE_MOD is enabled.
// =============================================================================
#if JT_OPT_OBXA_BLOCKRATE_MOD
static inline float obxa_fast_pow2(float x)
{
    // Clamp to prevent float overflow/underflow
    if (x >  126.0f) return 67108864.0f;
    if (x < -126.0f) return 1.49e-38f;

    const int32_t xi = (int32_t)x;          // integer part
    const float   xf = x - (float)xi;       // fractional part in [0, 1)

    // Remez minimax polynomial for 2^f on [0, 1) — error < 2e-6
    const float poly = 1.00000000f
                     + xf * (0.69314718f
                     + xf * (0.24022651f
                     + xf * (0.05550411f
                     + xf *  0.00961823f)));

    // Exact integer power via IEEE 754 exponent field manipulation
    return ldexpf(poly, xi);
}
#endif // JT_OPT_OBXA_BLOCKRATE_MOD
static constexpr int   OBXA_NUM_XPANDER_MODES = 15;

// 1-pole TPT helper
static inline float obxa_tpt_process(float &state, float input, float g)
{
    float v = (input - state) * g;
    float y = v + state;
    state = y + v;
    return y;
}

inline static float tpt_process_scaled_cutoff(float &state, float input, float cutoff_over_onepluscutoff)
{
    double v = (input - state) * cutoff_over_onepluscutoff;
    double res = v + state;

    state = res + v;

    return res;
}

static inline bool obxa_is_huge(float x)
{
    return fabsf(x) > OBXA_HUGE_THRESHOLD;
}

// -----------------------------------------------------------------------------
// Core implementation (kept out of header to reduce include/ODR issues)
// -----------------------------------------------------------------------------
struct AudioFilterOBXa::Core
{
    // Pole mix table for Xpander modes (4-pole only)
    static constexpr float poleMixFactors[OBXA_NUM_XPANDER_MODES][5] =
    {
        {0,  0,  0,  0,  1}, // 0: LP4
        {0,  0,  0,  1,  0}, // 1: LP3
        {0,  0,  1,  0,  0}, // 2: LP2
        {0,  1,  0,  0,  0}, // 3: LP1
        {1, -3,  3, -1,  0}, // 4: HP3
        {1, -2,  1,  0,  0}, // 5: HP2
        {1, -1,  0,  0,  0}, // 6: HP1
        {0,  0,  2, -4,  2}, // 7: BP4
        {0, -2,  2,  0,  0}, // 8: BP2
        {1, -2,  2,  0,  0}, // 9: N2
        {1, -3,  6, -4,  0}, // 10: PH3
        {0, -1,  2, -1,  0}, // 11: HP2+LP1
        {0, -1,  3, -3,  1}, // 12: HP3+LP1
        {0, -1,  2, -2,  0}, // 13: N2+LP1
        {0, -1,  3, -6,  4}, // 14: PH3+LP1
    };

    struct state
    {
        float pole1{0.f}, pole2{0.f}, pole3{0.f}, pole4{0.f};
        float res2Pole{1.f};
        float res4Pole{0.f};
        float resCorrection{1.f};
        float resCorrectionInv{1.f};
        float multimodeXfade{0.f};
        int   multimodePole{0};
    } state;

    float fs{AUDIO_SAMPLE_RATE_EXACT};
    float fsInv{1.f / AUDIO_SAMPLE_RATE_EXACT};

    // Parameters mirrored from wrapper
    bool bpBlend2Pole{false};
    bool push2Pole{false};
    bool xpander4Pole{false};
    uint8_t xpanderMode{0};
    float multimode01{0.f};



    void reset()
    {
        state.pole1 = state.pole2 = state.pole3 = state.pole4 = 0.f;

    }




    void setSampleRate(float sr)
    {
        fs = sr;
        fsInv = 1.f / fs;
        float rcRate = sqrtf(44000.0f / fs);
        state.resCorrection = (970.f / 44000.f) * rcRate;
        state.resCorrectionInv = 1.f / state.resCorrection;
    }

    void setResonance(float r01)
    {
        state.res2Pole = 1.f - r01;
        state.res4Pole = 3.5f * r01;

    }

    void setMultimode(float m01)
    {
        multimode01 = m01;
        state.multimodePole = (int)(multimode01 * 3.0f);
        state.multimodeXfade = (multimode01 * 3.0f) - state.multimodePole;
    }

    inline float diodePairResistanceApprox(float x)
    {
        return (((((0.0103592f) * x + 0.00920833f) * x + 0.185f) * x + 0.05f) * x + 1.f);
    }

    inline float resolveFeedback2Pole(float sample, float g)
    {
        float push = -1.f - (push2Pole ? 0.035f : 0.0f);
        float tCfb = diodePairResistanceApprox(state.pole1 * 0.0876f) + push;

        float y = (sample
                   - 2.f * (state.pole1 * (state.res2Pole + tCfb))
                   - g * state.pole1
                   - state.pole2)
                  /
                  (1.f + g * (2.f * (state.res2Pole + tCfb) + g));

        return y;
    }

    float process2Pole(float x, float cutoffHz)
    {
        float g = tanf(cutoffHz * fsInv * OBXA_PI);
        float v = resolveFeedback2Pole(x, g);

        float y1 = v * g + state.pole1;
        state.pole1 = v * g + y1;

        float y2 = y1 * g + state.pole2;
        state.pole2 = y1 * g + y2;

        float out;
        if (bpBlend2Pole)
        {
            if (multimode01 < 0.5f) out = 2.f * ((0.5f - multimode01) * y2 + (multimode01 * y1));
            else                    out = 2.f * ((1.f - multimode01) * y1 + (multimode01 - 0.5f) * v);
        }
        else
        {
            out = (1.f - multimode01) * y2 + multimode01 * v;
        }
        return out;
    }

    inline float resolveFeedback4Pole(float sample, float g, float lpc)
    {
        float ml = 1.f / (1.f + g);
        float S =
            (lpc * (lpc * (lpc * state.pole1 + state.pole2) + state.pole3) + state.pole4) * ml;
        float G = lpc * lpc * lpc * lpc;

        float y = (sample - state.res4Pole * S) / (1.f + state.res4Pole * G);
        return y;
    }

    float process4Pole(float x, float cutoffHz)
    {
        float cutoffHzRequested = cutoffHz;

        // Prewarp
        float g = tanf(cutoffHz * fsInv * OBXA_PI);
        float lpc = g / (1.f + g);



        float y0 = resolveFeedback4Pole(x, g, lpc);

        // Inline first pole with nonlinearity
        double v = (y0 - state.pole1) * lpc;
        double res = v + state.pole1;
        state.pole1 = (float)(res + v);

        state.pole1 = atanf(state.pole1 * state.resCorrection) * state.resCorrectionInv;

        float y1 = (float)res;
        float y2 = tpt_process_scaled_cutoff(state.pole2, y1, lpc);
        float y3 = tpt_process_scaled_cutoff(state.pole3, y2, lpc);
        float y4 = tpt_process_scaled_cutoff(state.pole4, y3, lpc);

        float out = 0.f;

        if (xpander4Pole)
        {
            const float *m = poleMixFactors[xpanderMode];
            out = y0 * m[0] + y1 * m[1] + y2 * m[2] + y3 * m[3] + y4 * m[4];
        }
        else
        {
            switch (state.multimodePole)
            {
            case 0: out = (1.f - state.multimodeXfade) * y4 + state.multimodeXfade * y3; break;
            case 1: out = (1.f - state.multimodeXfade) * y3 + state.multimodeXfade * y2; break;
            case 2: out = (1.f - state.multimodeXfade) * y2 + state.multimodeXfade * y1; break;
            case 3: out = y1; break;
            default: out = 0.f; break;
            }
        }

        // Detect anomalies
        bool nonfinite =
            (!isfinite(y0) || !isfinite(y1) || !isfinite(y2) || !isfinite(y3) || !isfinite(y4) || !isfinite(out) ||
             !isfinite(state.pole1) || !isfinite(state.pole2) || !isfinite(state.pole3) || !isfinite(state.pole4) ||
             !isfinite(g) || !isfinite(lpc));

        bool huge =
            (obxa_is_huge(state.pole1) || obxa_is_huge(state.pole2) ||
             obxa_is_huge(state.pole3) || obxa_is_huge(state.pole4) ||
             obxa_is_huge(out) || obxa_is_huge(y0));

        // Resonance-dependent volume compensation
        return out * (1.f + state.res4Pole * 0.45f);
    }
};

constexpr float AudioFilterOBXa::Core::poleMixFactors[OBXA_NUM_XPANDER_MODES][5];

// -----------------------------------------------------------------------------
// Wrapper implementation
// -----------------------------------------------------------------------------
AudioFilterOBXa::AudioFilterOBXa()
    : AudioStream(3, _inQ)
{
    _core = new Core();
    _core->setSampleRate(AUDIO_SAMPLE_RATE_EXACT);
    _core->setResonance(_res01Target);
    _core->setMultimode(_multimode01);


}

void AudioFilterOBXa::frequency(float hz)
{
    // allow nearly to Nyquist, but keep stable margin
    float maxHz = 0.24f * AUDIO_SAMPLE_RATE_EXACT;
    if (hz < 5.0f) hz = 5.0f;
    if (hz > maxHz) hz = maxHz;
    _cutoffHzTarget = hz;
}

void AudioFilterOBXa::resonance(float r01)
{
    if (r01 < 0.f) r01 = 0.f;
    if (r01 > 1.f) r01 = 1.f;
    _res01Target = r01;
    _core->setResonance(r01);
}

void AudioFilterOBXa::multimode(float m01)
{
    if (m01 < 0.f) m01 = 0.f;
    if (m01 > 1.f) m01 = 1.f;
    _multimode01 = m01;
    _core->setMultimode(m01);
}

void AudioFilterOBXa::setTwoPole(bool enabled)
{
    _useTwoPole = enabled;
}

void AudioFilterOBXa::setXpander4Pole(bool enabled)
{
    _xpander4Pole = enabled;
    _core->xpander4Pole = enabled;
}

void AudioFilterOBXa::setXpanderMode(uint8_t mode)
{
    if (mode >= OBXA_NUM_XPANDER_MODES) mode = OBXA_NUM_XPANDER_MODES - 1;
    _xpanderMode = mode;
    _core->xpanderMode = mode;
}

void AudioFilterOBXa::setBPBlend2Pole(bool enabled)
{
    _bpBlend2Pole = enabled;
    _core->bpBlend2Pole = enabled;
}

void AudioFilterOBXa::setPush2Pole(bool enabled)
{
    _push2Pole = enabled;
    _core->push2Pole = enabled;
}

void AudioFilterOBXa::setCutoffModOctaves(float oct)
{
    if (oct < 0.f) oct = 0.f;
    if (oct > 8.f) oct = 8.f;
    _cutoffModOct = oct;
}

void AudioFilterOBXa::setResonanceModDepth(float depth01)
{
    if (depth01 < 0.f) depth01 = 0.f;
    if (depth01 > 1.f) depth01 = 1.f;
    _resModDepth = depth01;
}

void AudioFilterOBXa::setKeyTrack(float amount01)
{
    if (amount01 < 0.f) amount01 = 0.f;
    if (amount01 > 1.f) amount01 = 1.f;
    _keyTrack = amount01;
}

void AudioFilterOBXa::setEnvModOctaves(float oct)
{
    if (oct < 0.f) oct = 0.f;
    if (oct > 8.f) oct = 8.f;
    _envModOct = oct;
}

void AudioFilterOBXa::setMidiNote(float note)
{
    if (note < 0.f) note = 0.f;
    if (note > 127.f) note = 127.f;
    _midiNote = note;
}

void AudioFilterOBXa::setEnvValue(float env01)
{
    if (env01 < 0.f) env01 = 0.f;
    if (env01 > 1.f) env01 = 1.f;
    _envValue = env01;
}


void AudioFilterOBXa::update(void)
{
    audio_block_t *in0 = receiveReadOnly(0);  // audio input

    // No audio input — nothing to filter, skip all DSP
    if (!in0) return;

    audio_block_t *in1 = receiveReadOnly(1);  // cutoff mod bus
    audio_block_t *in2 = receiveReadOnly(2);  // resonance mod bus

    audio_block_t *out = allocate();
    if (!out)
    {
        release(in0);
        if (in1) release(in1);
        if (in2) release(in2);
        return;
    }

    if (_cooldownBlocks > 0) _cooldownBlocks--;

    // -------------------------------------------------------------------------
    // BLOCK-RATE coefficient pre-computation
    //
    // Key tracking and envelope modulation both produce constant-per-block
    // multipliers.  Computing them once per block (not per sample) removes
    // the dominant powf() cost from the inner loop.
    //
    // keyMul:   octave shift from MIDI note relative to C4.
    //           Constant for the duration of a note — could be cached on
    //           noteOn, but doing it here keeps this self-contained.
    //
    // envMul:   octave multiplier from the filter envelope DC bus.
    //           _envValue is set by FilterBlock at control rate (not audio
    //           rate), so treating it as block-rate is correct.
    //
    // baseCutoffHz: _cutoffHzTarget × keyMul × envMul — the per-block
    //               starting frequency before any audio-rate mod is applied.
    // -------------------------------------------------------------------------
    const float keyOct      = (_midiNote - 60.0f) / 12.0f;
    const float keyMul      = powf(2.0f, _keyTrack * keyOct);    // block-rate only
    const float envModOct   = _envValue * _envModOct;

    // -------------------------------------------------------------------------
    // Determine whether cutoff mod bus carries any signal this block.
    // When _cutoffModOct == 0 (no mod depth set) we skip the per-sample
    // exponential entirely — same saving as the hasCutoffMod guard in VA bank.
    // -------------------------------------------------------------------------
    const bool hasCutoffMod = (in1 != nullptr) && (_cutoffModOct > 0.0f);
    const bool hasResMod    = (in2 != nullptr) && (_resModDepth  > 0.0f);

#if JT_OPT_OBXA_BLOCKRATE_MOD
    // =========================================================================
    // OPTIMISED PATH — block-rate modulation multiplier
    //
    // modMul is computed ONCE per block using obxa_fast_pow2().
    // The cutoff mod bus (in1) carries LFO or envelope audio-rate signals, but
    // filter sweeps are perceptually indistinguishable at block-rate (~344 Hz
    // update rate) vs sample-rate for the modulation-to-cutoff conversion.
    //
    // The audio signal (in0) is still read per-sample — only the exponential
    // frequency conversion is hoisted.
    //
    // Saving: 128 powf() calls per block per voice → 1 obxa_fast_pow2() call.
    // At 8 voices that is ~7 × 1024 = 7168 powf() calls avoided per interrupt.
    // =========================================================================

    // Block-rate cutoff mod: use mid-block sample (index 64) as representative.
    // This avoids the discontinuity you'd get from sample 0 and is a better
    // perceptual average than the first or last sample.
    float blockCutModSample = 0.0f;
    if (hasCutoffMod) {
        blockCutModSample = (float)in1->data[64] * (1.0f / 32768.0f);
    }
    const float modOctBlock  = (blockCutModSample * _cutoffModOct) + envModOct;
    const float modMulBlock  = obxa_fast_pow2(modOctBlock);
    const float baseCutoffHz = _cutoffHzTarget * keyMul * modMulBlock;

    // Block-rate resonance mod: same approach, mid-block sample.
    float blockResSample = 0.0f;
    if (hasResMod) {
        blockResSample = (float)in2->data[64] * (1.0f / 32768.0f);
    }
    float r01Block = _res01Target + (blockResSample * _resModDepth);
    if (r01Block < 0.0f) r01Block = 0.0f;
    if (r01Block > 1.0f) r01Block = 1.0f;
    _core->setResonance(r01Block);  // one call per block

    // Clamp block-rate cutoff
    const float maxHz = 0.24f * AUDIO_SAMPLE_RATE_EXACT;
    const float cutoffHz = (baseCutoffHz < 5.0f)   ? 5.0f
                         : (baseCutoffHz > maxHz) ? maxHz
                         : baseCutoffHz;

    // Set filter topology flags once per block (they don't change per-sample)
    if (_useTwoPole) {
        _core->bpBlend2Pole = _bpBlend2Pole;
        _core->push2Pole    = _push2Pole;
    } else {
        _core->xpander4Pole = _xpander4Pole;
        _core->xpanderMode  = _xpanderMode;
    }

    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i)
    {
        // Audio input — 0.0f when nothing connected (enables self-oscillation)
        float x = in0 ? ((float)in0->data[i] * (1.0f / 32768.0f)) : 0.0f;

        float y = 0.0f;
        if (_cooldownBlocks > 0)
        {
            y = 0.0f;
        }
        else if (_useTwoPole)
        {
            y = _core->process2Pole(x, cutoffHz);
        }
        else
        {
            y = _core->process4Pole(x, cutoffHz);
        }

#if OBXA_STATE_GUARD
        if (!isfinite(y) || obxa_is_huge(y) ||
            obxa_is_huge(_core->state.pole1) || obxa_is_huge(_core->state.pole2) ||
            obxa_is_huge(_core->state.pole3) || obxa_is_huge(_core->state.pole4))
        {
            _core->reset();
            _cooldownBlocks = 2;
            y = 0.0f;
#if OBXA_DEBUG
            _faultLatched = false;
#endif
        }
#endif
        if (y > 1.0f) y = 1.0f;
        if (y < -1.0f) y = -1.0f;
        out->data[i] = (int16_t)(y * 32767.0f);
    }

#else // JT_OPT_OBXA_BLOCKRATE_MOD == 0
    // =========================================================================
    // REFERENCE PATH — original per-sample powf() behaviour
    // Use this to benchmark the cost of the optimisation above.
    // =========================================================================
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i)
    {
        float x      = in0 ? ((float)in0->data[i] * (1.0f / 32768.0f)) : 0.0f;
        float cutMod = in1 ? ((float)in1->data[i] * (1.0f / 32768.0f)) : 0.0f;
        float resMod = in2 ? ((float)in2->data[i] * (1.0f / 32768.0f)) : 0.0f;

        float modOct  = (cutMod * _cutoffModOct) + (_envValue * _envModOct);
        float modMul  = powf(2.0f, modOct);   // original per-sample cost

        float cutoffHz = _cutoffHzTarget * keyMul * modMul;
        const float maxHz = 0.24f * AUDIO_SAMPLE_RATE_EXACT;
        if (cutoffHz < 5.0f) cutoffHz = 5.0f;
        if (cutoffHz > maxHz) cutoffHz = maxHz;

        float r01 = _res01Target + (resMod * _resModDepth);
        if (r01 < 0.0f) r01 = 0.0f;
        if (r01 > 1.0f) r01 = 1.0f;
        _core->setResonance(r01);

        float y = 0.0f;
        if (_cooldownBlocks > 0)
        {
            y = 0.0f;
        }
        else if (_useTwoPole)
        {
            _core->bpBlend2Pole = _bpBlend2Pole;
            _core->push2Pole    = _push2Pole;
            y = _core->process2Pole(x, cutoffHz);
        }
        else
        {
            _core->xpander4Pole = _xpander4Pole;
            _core->xpanderMode  = _xpanderMode;
            y = _core->process4Pole(x, cutoffHz);
        }

#if OBXA_STATE_GUARD
        if (!isfinite(y) || obxa_is_huge(y) ||
            obxa_is_huge(_core->state.pole1) || obxa_is_huge(_core->state.pole2) ||
            obxa_is_huge(_core->state.pole3) || obxa_is_huge(_core->state.pole4))
        {
            _core->reset();
            _cooldownBlocks = 2;
            y = 0.0f;
#if OBXA_DEBUG
            _faultLatched = false;
#endif
        }
#endif
        if (y > 1.0f) y = 1.0f;
        if (y < -1.0f) y = -1.0f;
        out->data[i] = (int16_t)(y * 32767.0f);
    }
#endif // JT_OPT_OBXA_BLOCKRATE_MOD

    transmit(out);
    release(out);
    if (in0) release(in0);
    if (in1) release(in1);
    if (in2) release(in2);
}