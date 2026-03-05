#include "AudioFilterOBXa_OBXf.h"

// Keep math constants local
static constexpr float OBXA_PI = 3.14159265358979323846f;
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
    audio_block_t *in0 = receiveReadOnly(0);
    audio_block_t *in1 = receiveReadOnly(1); // cutoff mod bus
    audio_block_t *in2 = receiveReadOnly(2); // resonance mod bus

    audio_block_t *out = allocate();
    if (!out)
    {
        // Release any inputs we received
        if (in0) release(in0);
        if (in1) release(in1);
        if (in2) release(in2);
        return;
    }

    // If we recently had a reset, optionally mute a couple blocks to avoid thumps
    if (_cooldownBlocks > 0) _cooldownBlocks--;

    // Precompute keytrack factor (control-rate)
    // note=60 => 1.0; note+12 => x2; note-12 => x0.5
    float keyOct = (_midiNote - 60.0f) / 12.0f;
    float keyMul = powf(2.0f, _keyTrack * keyOct);

    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i)
    {
        // *** KEY CHANGE: Use 0.0f if no input, allowing self-oscillation ***
        float x = in0 ? ((float)in0->data[i] * (1.0f / 32768.0f)) : 0.0f;

        // Audio-rate mods
        float cutMod = in1 ? ((float)in1->data[i] * (1.0f / 32768.0f)) : 0.0f;  // -1..+1
        float resMod = in2 ? ((float)in2->data[i] * (1.0f / 32768.0f)) : 0.0f;  // -1..+1

        // Convert cutoff mods to a multiplier in octaves:
        float modOct = (cutMod * _cutoffModOct) + (_envValue * _envModOct);
        float modMul = powf(2.0f, modOct);

        float cutoffHz = _cutoffHzTarget * keyMul * modMul;

        // Keep stable
        float maxHz = 0.24f * AUDIO_SAMPLE_RATE_EXACT;
        if (cutoffHz < 5.0f) cutoffHz = 5.0f;
        if (cutoffHz > maxHz) cutoffHz = maxHz;

        // Resonance (0..1) plus optional audio-rate modulation depth
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
            _core->push2Pole = _push2Pole;
            y = _core->process2Pole(x, cutoffHz);
        }
        else
        {
            _core->xpander4Pole = _xpander4Pole;
            _core->xpanderMode = _xpanderMode;
            y = _core->process4Pole(x, cutoffHz);
        }

#if OBXA_STATE_GUARD
        // Recovery: if output goes non-finite or runaway, reset and cool down.
        if (!isfinite(y) || obxa_is_huge(y) ||
            obxa_is_huge(_core->state.pole1) || obxa_is_huge(_core->state.pole2) ||
            obxa_is_huge(_core->state.pole3) || obxa_is_huge(_core->state.pole4))
        {
            _core->reset();
            _cooldownBlocks = 2; // mute 2 blocks after reset
            y = 0.0f;
#if OBXA_DEBUG
            // allow a new fault to be captured after recovery
            _faultLatched = false;
#endif
        }
#endif

        if (y > 1.0f) y = 1.0f;
        if (y < -1.0f) y = -1.0f;

        out->data[i] = (int16_t)(y * 32767.0f);
    }

    transmit(out);

    release(out);
    if (in0) release(in0);
    if (in1) release(in1);
    if (in2) release(in2);
}