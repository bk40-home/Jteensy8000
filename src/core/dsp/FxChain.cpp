// =============================================================================
// FxChain.cpp — JT-8000 v2 per-patch FX chain implementation
// =============================================================================
// DSP ported 1:1 from v1 AudioEffectJPFX.cpp (see docs/PHASE6_FXCHAIN_SPEC.md
// for the file:line diagnosis).  Only new logic vs v1: the D-1 drive-mode
// collapse (continuous → Select) and the mono-sum / stereo-blend plumbing that
// replaces v1's AudioStream node (spec §5).
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#include "core/dsp/FxChain.h"

namespace JT {

namespace {
constexpr float kPi    = 3.14159265f;
constexpr float kTwoPi = 6.28318530f;

// Helper clamp — v2 core has no Arduino constrain().
inline float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

// D-1: fixed representative drive per Select mode (spec §5).  Soft/Hard are the
// midpoints of v1's continuous soft (0..0.5) and hard (0.5..1) bands, so each
// mode sounds representative of its v1 region.
constexpr float kSoftDrive = 0.25f;
constexpr float kHardDrive = 0.75f;
} // namespace

// ===========================================================================
// Preset tables — verbatim from v1 AudioEffectJPFX.cpp:72-105.
// FORMAT (mod):   { baseL, baseR, depthL, depthR, rate, fb, mix, taps, triLfo }
// FORMAT (delay): { delayL, delayR, feedback, mix }
// ===========================================================================
const FxChain::ModParams FxChain::kModPresets[kNumModPresets] = {
    //                  baseL  baseR  dpthL dpthR  rate    fb   mix taps tri
    /* CHORUS1      */ { 5.0f,  5.0f, 1.5f, 2.0f, 0.80f, 0.00f, 1.0f, 1, true  },
    /* CHORUS2      */ { 8.0f,  8.0f, 2.5f, 3.0f, 0.50f, 0.00f, 1.0f, 1, true  },
    /* CHORUS3      */ {12.0f, 12.0f, 3.5f, 4.5f, 0.30f, 0.00f, 1.0f, 1, true  },
    /* FLANGER1     */ { 1.5f,  1.5f, 1.5f, 1.5f, 0.30f, 0.60f, 1.0f, 1, false },
    /* FLANGER2     */ { 2.0f,  2.0f, 2.0f, 2.0f, 0.15f, 0.75f, 1.0f, 1, false },
    /* DEEP_FLANGER */ { 3.0f,  3.0f, 3.0f, 3.0f, 0.08f, 0.85f, 1.0f, 1, false },
    /* PHASER1      */ { 1.0f,  1.0f, 4.0f, 4.0f, 0.25f, 0.60f, 1.0f, 1, false },
    /* PHASER2      */ { 1.0f,  1.0f, 5.0f, 5.0f, 0.50f, 0.70f, 1.0f, 1, false },
    /* PHASER3      */ { 1.0f,  1.0f, 6.0f, 6.0f, 0.10f, 0.80f, 1.0f, 1, false },
    /* PHASER4      */ { 1.0f,  1.0f, 3.0f, 3.0f, 1.20f, 0.50f, 1.0f, 1, false },
    /* SUPER_CHORUS */ { 8.0f,  8.0f, 3.0f, 4.0f, 0.50f, 0.00f, 1.0f, 3, true  },
};

const FxChain::DelayParams FxChain::kDelayPresets[kNumDelayPresets] = {
    //                   delayL   delayR    fb    mix
    /* MONO_SHORT */   {  80.0f,   80.0f, 0.25f, 0.40f },
    /* MONO_LONG  */   { 500.0f,  500.0f, 0.35f, 0.40f },
    /* PAN_LR     */   { 120.0f,  250.0f, 0.35f, 0.50f },
    /* PAN_RL     */   { 250.0f,  120.0f, 0.35f, 0.50f },
    /* PAN_STEREO */   { 200.0f,  400.0f, 0.40f, 0.50f },
};

// ===========================================================================
// begin — attach + carve the caller pool, fix tone coeffs, clear state.
// ===========================================================================
void FxChain::begin(float* pool)
{
    _pool = pool;
    if (!_pool) return;                     // inert; processBlock bails on null

    // Carve: [delayL][delayR][modL][modR].  Contiguous, sequential access.
    _delayBufL = _pool;
    _delayBufR = _delayBufL + kDelayLen;
    _modBufL   = _delayBufR + kDelayLen;
    _modBufR   = _modBufL   + kModLen;

    memset(_pool, 0, kPoolFloats * sizeof(float));
    _delayWriteIdx = 0;
    _modWriteIdx   = 0;

    // Tone crossover LP coeffs — fixed, depend only on sample rate (v1 ctor
    // AudioEffectJPFX.cpp:117-118): alpha = 2π·fc / (2π·fc + fs).
    const float fs = kSampleRate;
    _toneBassAlpha = kTwoPi * 200.0f  / (kTwoPi * 200.0f  + fs);
    _toneTrebAlpha = kTwoPi * 3000.0f / (kTwoPi * 3000.0f + fs);
}

// ===========================================================================
// Saturation setters + block-rate recompute (v1 :406-435)
// ===========================================================================
void FxChain::setDriveMode(int mode)
{
    mode = (int)clampf((float)mode, 0.0f, 2.0f);
    if (mode != _driveMode) {
        _driveMode = mode;
        _satDirty  = true;
    }
}

void FxChain::computeSat()
{
    _satDirty = false;

    if (_driveMode == 0) {                  // OFF: unity, applySaturation bypasses
        _satInputGain = 1.0f; _satOutputGain = 1.0f;
        return;
    }

    // D-1: resolve the Select mode to v1's fixed representative drive, then run
    // v1's exact gain-staging maths (:415-433).
    if (_driveMode == 1) {                  // Soft (tanh)
        const float drive = kSoftDrive;
        _satIsSoft    = true;
        _satInputGain = 1.0f + (drive / 0.5f) * 3.0f;              // 1..4×
        const float testOut = tanhf(_satInputGain * 0.5f);
        _satOutputGain = (testOut > 0.0f) ? (0.5f / testOut) : 1.0f;
    } else {                                // Hard (asymmetric clip)
        const float drive = kHardDrive;
        _satIsSoft    = false;
        _satInputGain = 4.0f + ((drive - 0.5f) / 0.5f) * 4.0f;     // 4..8×
        _satOutputGain = 1.0f / 0.7f;
    }
}

// ===========================================================================
// Tone setters + block-rate recompute (v1 :325-347)
// ===========================================================================
void FxChain::setBassGain(float dB)
{
    dB = clampf(dB, -12.0f, 12.0f);
    if (dB != _targetBassDb) { _targetBassDb = dB; _toneDirty = true; }
}

void FxChain::setTrebleGain(float dB)
{
    dB = clampf(dB, -12.0f, 12.0f);
    if (dB != _targetTrebleDb) { _targetTrebleDb = dB; _toneDirty = true; }
}

void FxChain::computeTone()
{
    _toneDirty = false;

    float bassLin = powf(10.0f, _targetBassDb   / 20.0f);
    float trebLin = powf(10.0f, _targetTrebleDb / 20.0f);

    // Snap to unity within ±0.1 dB (v1 :335-336) — CC midpoints that land at
    // ~0.094 dB shouldn't leave a residual filter running.
    if (fabsf(_targetBassDb)   < 0.1f) bassLin = 1.0f;
    if (fabsf(_targetTrebleDb) < 0.1f) trebLin = 1.0f;

    _toneBassBase = bassLin - 1.0f;
    _toneTrebBase = trebLin - 1.0f;

    // Effective deltas default to base; the per-block tilt step adds to these.
    _toneBassDelta = _toneBassBase;
    _toneTrebDelta = _toneTrebBase;

    _toneActive = (fabsf(_toneBassDelta) > 0.0001f ||
                   fabsf(_toneTrebDelta) > 0.0001f);
}

// ===========================================================================
// Modulation setters (v1 :460-490)
// ===========================================================================
void FxChain::setModEffect(int v1Type)
{
    if (v1Type != _modType) {
        _modType   = v1Type;
        _lfoPhaseL = 0.0f;
        _lfoPhaseR = 0.5f;
        updateLfoIncrements();
    }
}

void FxChain::setModMix(float mix)   { _modMix = clampf(mix, 0.0f, 1.0f); }

void FxChain::setModRate(float rateHz)
{
    if (rateHz < 0.0f) return;
    _modRateOverride = (rateHz == 0.0f) ? -1.0f : rateHz;
    updateLfoIncrements();
}

void FxChain::setModFeedback(float fb)
{
    _modFeedbackOverride = (fb < 0.0f) ? -1.0f : clampf(fb, 0.0f, 0.99f);
}

void FxChain::updateLfoIncrements()
{
    if (_modType < 0) { _lfoIncL = _lfoIncR = 0.0f; return; }

    float rateHz = kModPresets[_modType].rateHz;
    if (_modRateOverride > 0.0f) rateHz = _modRateOverride;

    const float inc = kTwoPi * rateHz / kSampleRate;
    _lfoIncL = inc;
    _lfoIncR = inc * 1.01f;                 // 1 % stereo offset (v1 :620)
}

// ===========================================================================
// Delay setters (v1 :495-575)
// ===========================================================================
void FxChain::setDelayEffect(int v1Type)
{
    if (v1Type != _delayType) {
        _delayType = v1Type;
        _delayTimeOverrideL = -1.0f;
        _delayTimeOverrideR = -1.0f;
        // Mute wet for one buffer lap on a real preset (input-only writes flush
        // stale PSRAM before feedback can re-inject it — v1 :528-534).
        _delayMuteCounter = (v1Type >= 0 && v1Type < (int)kNumDelayPresets)
                          ? kDelayLen : 0u;
    }
}

void FxChain::setDelayMix(float mix)
{
    // v2 CC path is 0..1 magnitude only — no phase inversion (D-6).
    _delayMix = clampf(mix, 0.0f, 1.0f);
}

// ---- Stage D aux-lane mod setters -----------------------------------------
void FxChain::setToneTiltMod(float bipolar)
{
    _tiltTarget = clampf(bipolar, -1.0f, 1.0f);
}

void FxChain::setDriveAmountMod(float bipolar)
{
    _driveModTarget = clampf(bipolar, -1.0f, 1.0f);
}

void FxChain::setDelayMixMod(float offset)
{
    // Stored raw; prepareDelay() sums it onto the knob base and clamps 0..1.
    _delayMixMod = clampf(offset, -1.0f, 1.0f);
}

void FxChain::setDelayFeedback(float fb)
{
    _delayFeedbackOverride = (fb < 0.0f) ? -1.0f : clampf(fb, 0.0f, 0.99f);
}

void FxChain::setDelayTime(float ms)
{
    // Disable override (revert to preset times) at 0 (v1 :560-564).
    if (ms <= 0.0f) {
        _delayTimeOverrideL = -1.0f;
        _delayTimeOverrideR = -1.0f;
        return;
    }
    // Preserve the active preset's L/R ratio so panning presets stay stereo.
    float ratio = 1.0f;
    if (_delayType >= 0 && _delayType < (int)kNumDelayPresets) {
        const DelayParams& p = kDelayPresets[_delayType];
        if (p.delayMsL > 0.0f) ratio = p.delayMsR / p.delayMsL;
    }
    _delayTimeOverrideL = ms;
    _delayTimeOverrideR = ms * ratio;
}

// ===========================================================================
// Output blend setters (spec §1.4 / Q5)
// ===========================================================================
void FxChain::setDryMix(float m)  { _dryMix  = clampf(m, 0.0f, 1.0f); }
void FxChain::setJpfxMix(float m) { _jpfxMix = clampf(m, 0.0f, 1.0f); }

// ===========================================================================
// prepareDelay — block-constant delay cache (v1 :770-838)
// ===========================================================================
void FxChain::prepareDelay()
{
    if (_delayType < 0 || !_delayBufL || !_delayBufR) {
        _delaySampLCached = 0.0f; _delaySampRCached = 0.0f;
        _delayFbCached = 0.0f; _delayWetCached = 0.0f; _delayDryCached = 1.0f;
        return;
    }

    const DelayParams& p = kDelayPresets[_delayType];

    float timeMsL = (_delayTimeOverrideL >= 0.0f) ? _delayTimeOverrideL : p.delayMsL;
    float timeMsR = (_delayTimeOverrideR >= 0.0f) ? _delayTimeOverrideR : p.delayMsR;

    const float msToSamp = 0.001f * kSampleRate;
    const float maxSamp  = (float)(kDelayLen - 2);

    float dSampL = timeMsL * msToSamp;
    float dSampR = timeMsR * msToSamp;
    dSampL = clampf(dSampL, kMinDelaySamp, maxSamp);
    dSampR = clampf(dSampR, kMinDelaySamp, maxSamp);

    // v2: mix is 0..1 magnitude, no phase invert (D-6) — so wet = mix directly.
    // Stage D: aux-lane offset added on top of the knob base, clamped (Q19).
    // _delayMixMod defaults to 0 → wet == _delayMix, byte-identical.
    const float wet = clampf(_delayMix + _delayMixMod, 0.0f, 1.0f);

    _delaySampLCached = dSampL;
    _delaySampRCached = dSampR;
    _delayFbCached    = (_delayFeedbackOverride >= 0.0f) ? _delayFeedbackOverride
                                                         : p.feedback;
    _delayWetCached   = wet;
    _delayDryCached   = 1.0f - wet;
}

// ===========================================================================
// processModulation — chorus / flanger / phaser (v1 :625-763), verbatim DSP.
// ===========================================================================
inline void FxChain::processModulation(float inL, float inR,
                                       float& outL, float& outR)
{
    if (_modType < 0 || !_modBufL || !_modBufR) { outL = inL; outR = inR; return; }

    const ModParams& p = kModPresets[_modType];

    const float feedback = (_modFeedbackOverride >= 0.0f) ? _modFeedbackOverride
                                                          : p.feedback;
    const float wetMix = _modMix * p.mix;
    const float dryMix = 1.0f - wetMix;

    auto triangleLfo = [](float phase) -> float {
        const float norm = phase * (1.0f / kTwoPi);
        return (norm < 0.5f) ? (-1.0f + 4.0f * norm) : (3.0f - 4.0f * norm);
    };
    static constexpr float k5PiSq = 5.0f * kPi * kPi;
    auto fastSin = [](float phase) -> float {
        float x = phase, sign = 1.0f;
        if (x > kPi) { x -= kPi; sign = -1.0f; }
        const float xpi = x * (kPi - x);
        return sign * (16.0f * xpi) / (k5PiSq - 4.0f * xpi);
    };

    const float msToSamples = 0.001f * kSampleRate;
    const float maxIdx      = (float)(kModLen - 2);

    float lfoL, lfoR;
    if (p.useTriangleLfo) { lfoL = triangleLfo(_lfoPhaseL); lfoR = triangleLfo(_lfoPhaseR); }
    else                  { lfoL = fastSin(_lfoPhaseL);     lfoR = fastSin(_lfoPhaseR);     }

    _lfoPhaseL += _lfoIncL; if (_lfoPhaseL >= kTwoPi) _lfoPhaseL -= kTwoPi;
    _lfoPhaseR += _lfoIncR; if (_lfoPhaseR >= kTwoPi) _lfoPhaseR -= kTwoPi;

    auto readInterp = [](const float* buf, uint32_t bufSize,
                         uint32_t writeIdx, float delaySamples) -> float
    {
        float readIdx = (float)writeIdx - delaySamples;
        if (readIdx < 0.0f) readIdx += (float)bufSize;
        const uint32_t i0 = (uint32_t)readIdx;
        const uint32_t i1 = (i0 + 1) % bufSize;
        const float frac  = readIdx - (float)i0;
        return buf[i0] + (buf[i1] - buf[i0]) * frac;
    };

    float delayedL, delayedR;

    if (p.tapCount >= 3) {
        // Super Chorus: 3 taps at 120° LFO offsets, averaged (v1 :686-742).
        static constexpr float kOffset1 = kTwoPi / 3.0f;
        static constexpr float kOffset2 = kTwoPi * 2.0f / 3.0f;

        float phase1L = _lfoPhaseL + kOffset1; if (phase1L >= kTwoPi) phase1L -= kTwoPi;
        float phase2L = _lfoPhaseL + kOffset2; if (phase2L >= kTwoPi) phase2L -= kTwoPi;
        float phase1R = _lfoPhaseR + kOffset1; if (phase1R >= kTwoPi) phase1R -= kTwoPi;
        float phase2R = _lfoPhaseR + kOffset2; if (phase2R >= kTwoPi) phase2R -= kTwoPi;

        float lfo1L, lfo2L, lfo1R, lfo2R;
        if (p.useTriangleLfo) {
            lfo1L = triangleLfo(phase1L); lfo2L = triangleLfo(phase2L);
            lfo1R = triangleLfo(phase1R); lfo2R = triangleLfo(phase2R);
        } else {
            lfo1L = fastSin(phase1L); lfo2L = fastSin(phase2L);
            lfo1R = fastSin(phase1R); lfo2R = fastSin(phase2R);
        }

        float dL0 = (p.baseDelayMsL + p.depthMsL * 1.00f * lfoL ) * msToSamples;
        float dL1 = (p.baseDelayMsL + p.depthMsL * 0.90f * lfo1L) * msToSamples;
        float dL2 = (p.baseDelayMsL + p.depthMsL * 1.10f * lfo2L) * msToSamples;
        float dR0 = (p.baseDelayMsR + p.depthMsR * 1.00f * lfoR ) * msToSamples;
        float dR1 = (p.baseDelayMsR + p.depthMsR * 0.90f * lfo1R) * msToSamples;
        float dR2 = (p.baseDelayMsR + p.depthMsR * 1.10f * lfo2R) * msToSamples;

        dL0 = clampf(dL0, 0.0f, maxIdx); dL1 = clampf(dL1, 0.0f, maxIdx); dL2 = clampf(dL2, 0.0f, maxIdx);
        dR0 = clampf(dR0, 0.0f, maxIdx); dR1 = clampf(dR1, 0.0f, maxIdx); dR2 = clampf(dR2, 0.0f, maxIdx);

        const float t0L = readInterp(_modBufL, kModLen, _modWriteIdx, dL0);
        const float t1L = readInterp(_modBufL, kModLen, _modWriteIdx, dL1);
        const float t2L = readInterp(_modBufL, kModLen, _modWriteIdx, dL2);
        delayedL = (t0L + t1L + t2L) * (1.0f / 3.0f);

        const float t0R = readInterp(_modBufR, kModLen, _modWriteIdx, dR0);
        const float t1R = readInterp(_modBufR, kModLen, _modWriteIdx, dR1);
        const float t2R = readInterp(_modBufR, kModLen, _modWriteIdx, dR2);
        delayedR = (t0R + t1R + t2R) * (1.0f / 3.0f);
    } else {
        float dSampL = (p.baseDelayMsL + p.depthMsL * lfoL) * msToSamples;
        float dSampR = (p.baseDelayMsR + p.depthMsR * lfoR) * msToSamples;
        dSampL = clampf(dSampL, 0.0f, maxIdx);
        dSampR = clampf(dSampR, 0.0f, maxIdx);
        delayedL = readInterp(_modBufL, kModLen, _modWriteIdx, dSampL);
        delayedR = readInterp(_modBufR, kModLen, _modWriteIdx, dSampR);
    }

    _modBufL[_modWriteIdx] = inL + delayedL * feedback;
    _modBufR[_modWriteIdx] = inR + delayedR * feedback;
    _modWriteIdx = (_modWriteIdx + 1) % kModLen;

    outL = dryMix * inL + wetMix * delayedL;
    outR = dryMix * inR + wetMix * delayedR;
}

// ===========================================================================
// processDelay — mono / panning delay (v1 :840-897), verbatim DSP.
// ===========================================================================
inline void FxChain::processDelay(float inL, float inR, float& outL, float& outR)
{
    if (_delayType < 0 || !_delayBufL || !_delayBufR) { outL = inL; outR = inR; return; }

    const float feedback = _delayFbCached;
    const float wetLevel = _delayWetCached;
    const float dryLevel = _delayDryCached;
    const float dSampL   = _delaySampLCached;
    const float dSampR   = _delaySampRCached;

    float readIdxL = (float)_delayWriteIdx - dSampL;
    if (readIdxL < 0.0f) readIdxL += (float)kDelayLen;
    const uint32_t iL0 = (uint32_t)readIdxL;
    const uint32_t iL1 = (iL0 + 1) % kDelayLen;
    const float fracL  = readIdxL - (float)iL0;
    float delayL = _delayBufL[iL0] + (_delayBufL[iL1] - _delayBufL[iL0]) * fracL;

    float readIdxR = (float)_delayWriteIdx - dSampR;
    if (readIdxR < 0.0f) readIdxR += (float)kDelayLen;
    const uint32_t iR0 = (uint32_t)readIdxR;
    const uint32_t iR1 = (iR0 + 1) % kDelayLen;
    const float fracR  = readIdxR - (float)iR0;
    float delayR = _delayBufR[iR0] + (_delayBufR[iR1] - _delayBufR[iR0]) * fracR;

    // Sanitise PSRAM reads (v1 :870-882): clamp corruption/NaN, scrub source so
    // it cannot re-enter the feedback loop on the next lap.
    if (delayL > 2.0f || delayL < -2.0f || delayL != delayL) {
        delayL = 0.0f; _delayBufL[iL0] = 0.0f; _delayBufL[iL1] = 0.0f;
    }
    if (delayR > 2.0f || delayR < -2.0f || delayR != delayR) {
        delayR = 0.0f; _delayBufR[iR0] = 0.0f; _delayBufR[iR1] = 0.0f;
    }

    if (_delayMuteCounter > 0) {
        // Building fresh buffer content — write input only, output dry only.
        _delayBufL[_delayWriteIdx] = inL;
        _delayBufR[_delayWriteIdx] = inR;
        _delayWriteIdx = (_delayWriteIdx + 1) % kDelayLen;
        --_delayMuteCounter;
        outL = inL; outR = inR;
        return;
    }

    _delayBufL[_delayWriteIdx] = inL + delayL * feedback;
    _delayBufR[_delayWriteIdx] = inR + delayR * feedback;
    _delayWriteIdx = (_delayWriteIdx + 1) % kDelayLen;

    // No phase inversion on the 0..1 CC path (D-6).
    outL = dryLevel * inL + wetLevel * delayL;
    outR = dryLevel * inR + wetLevel * delayR;
}

// ===========================================================================
// processBlock — v1 update() per-sample chain, float-native, in place.
// Mono-sums the stereo bus (v1 single mono input), runs sat→tone→mod→delay→
// limiter, then blends the wet result back against the dry bus (spec §5).
// ===========================================================================
void FxChain::processBlock(float* left, float* right, size_t n)
{
    if (!_pool) return;                     // inert without a pool

    // Block-rate parameter recompute (only when dirty) + delay cache.
    if (_satDirty)  computeSat();
    if (_toneDirty) computeTone();

    // Stage D: aux 'Drive' → bass↔treble TILT on the Tone EQ (level-neutral
    // colour).  Smooth the tilt once per block (no zipper), convert ±kTiltMaxDb
    // to linear shelf deltas, and add treble-up / bass-down (see-saw) on top of
    // whatever the tone knobs set.  Independent of drive mode — it's a tone
    // colour.  Skipped (and tone left exactly as computeTone left it) when the
    // tilt is flat and already settled, so an unmodulated chain is byte-identical.
    {
        constexpr float kTiltMaxDb  = 6.0f;                    // +/-6 dB at full depth
        constexpr float kTiltSmooth = 0.25f;                   // ~4-block one-pole
        // dB -> linear without powf: 10^(d/20) == 2^(d * log2(10)/20).
        // Two powf per block became two exp2f, which the M7 does far cheaper.
        constexpr float kDbToLog2 = 0.16609640f;               // log2(10) / 20

        if (_tiltCur != _tiltTarget)
            _tiltCur += (_tiltTarget - _tiltCur) * kTiltSmooth;

        if (fabsf(_tiltCur) > 0.0001f) {
            // Effective = base + tilt, rebuilt from base each block so it can
            // never accumulate.  Treble up / bass down see-saw.
            const float db = _tiltCur * kTiltMaxDb;
            _toneTrebDelta = _toneTrebBase + (exp2f( db * kDbToLog2) - 1.0f);
            _toneBassDelta = _toneBassBase + (exp2f(-db * kDbToLog2) - 1.0f);
            _toneActive = true;                                // force the stage on
        } else if (_toneBassDelta != _toneBassBase || _toneTrebDelta != _toneTrebBase) {
            // Tilt just settled to flat — restore the pure base deltas and
            // re-evaluate whether the tone stage is needed at all.
            _toneBassDelta = _toneBassBase;
            _toneTrebDelta = _toneTrebBase;
            _toneActive = (fabsf(_toneBassBase) > 0.0001f ||
                           fabsf(_toneTrebBase) > 0.0001f);
        }
    }

    // Aux 'Drive' -> saturator INPUT gain.  Smoothed on the same one-pole as
    // the tilt, then folded into _satInputGain ONCE here so applySaturation
    // keeps its single per-sample multiply.  At rest this is exactly
    // _satInputGain, so an unmodulated chain is bit-identical.
    {
        constexpr float kDriveSmooth   = 0.25f;   // ~4-block one-pole
        constexpr float kDriveModOct   = 1.5f;    // +/-1.5 octaves ~ +/-9 dB
        if (_driveModCur != _driveModTarget)
            _driveModCur += (_driveModTarget - _driveModCur) * kDriveSmooth;

        _satInputEff = (fabsf(_driveModCur) > 0.0001f)
                     ? _satInputGain * exp2f(_driveModCur * kDriveModOct)
                     : _satInputGain;
    }

    prepareDelay();

    const float dryMix  = _dryMix;
    const float jpfxMix = _jpfxMix;

    float blockPeak = 0.0f;

    for (size_t i = 0; i < n; ++i) {
        const float dryL = left[i];
        const float dryR = right[i];

        // v1 JPFX mono input = the summed voice bus.
        float s = (dryL + dryR) * 0.5f;

        // Stage 1: Saturation (mono).
        s = applySaturation(s);

        // Fan mono → stereo for the remaining stages (v1 :939).
        float sl = s, sr = s;

        // Stage 2: Tone EQ.
        applyTone(sl, sr);

        // Stage 3: Modulation.
        float ml, mr; processModulation(sl, sr, ml, mr);

        // Stage 4: Delay.
        float wl, wr; processDelay(ml, mr, wl, wr);

        // Track peak for the block-rate limiter (post-effect, pre-blend — v1
        // limited the JPFX output before the dry/wet mixer summed it).
        const float pL = fabsf(wl), pR = fabsf(wr);
        if (pL > blockPeak) blockPeak = pL;
        if (pR > blockPeak) blockPeak = pR;

        // Apply current limiter gain (D-7: float clamp, no int16 round-trip).
        wl *= _limGain;
        wr *= _limGain;
        if (wl >  kLimCeiling) wl =  kLimCeiling;
        if (wl < -kLimCeiling) wl = -kLimCeiling;
        if (wr >  kLimCeiling) wr =  kLimCeiling;
        if (wr < -kLimCeiling) wr = -kLimCeiling;

        // Blend wet against the dry bus (spec §1.4 / Q5), in place.
        left[i]  = dryMix * dryL + jpfxMix * wl;
        right[i] = dryMix * dryR + jpfxMix * wr;
    }

    // Stage 5: block-rate brick-wall limiter gain update (v1 :980-993).
    if (blockPeak * _limGain > kLimCeiling) {
        _limGain = kLimCeiling / blockPeak;     // snap down on overload
    } else {
        _limGain += kLimRelease;                // recover slowly toward unity
        if (_limGain > 1.0f) _limGain = 1.0f;
    }
}

} // namespace JT
