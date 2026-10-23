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

// 1a (JP capture gap analysis 2026-10-22, G1) — FLAGGED DEVIATION FROM v1.
// Minimum modulation tap = 1 sample.  v1 clamped at 0: modReadTap(writeIdx-0)
// then reads the slot that processModulation writes only AFTER the read — the
// sample from one full buffer lap ago (kModLen = 50.04 ms).  PHASER1-4 sit in
// that clamp for 39-45 % of each LFO cycle and played a 50 ms slap; FLANGER1-3
// (base == depth) touched it at every trough.  1 sample = 20.8 us, inaudible as
// a delay change, and the read can never reach the unwritten slot.
constexpr float kMinModTapSamples = 1.0f;

// ---- JP Multi-FX helpers (pair engine) ----------------------------------------
// LFO shapes on a phase in CYCLES (0..1), output -1..+1.  Cycles rather than
// radians so voice / L-R offsets are plain fractions (0.5 = anti-phase).
// The sine is the shared FastMath::fastSin01 (also a 0..1 phase).
inline float jpTri(float ph)
{
    return (ph < 0.5f) ? (-1.0f + 4.0f * ph) : (3.0f - 4.0f * ph);
}

// Linear-interpolated read 'delaySamples' behind writeIdx.  Shared by the v1
// and JP paths (N2).  The JP pair engine writes before it reads, so for it a
// delay of 0 (the sample just written) is valid too.  Callers guarantee
// delaySamples >= kMinModTapSamples, so the unwritten slot is never read (1a).
inline float modReadTap(const float* buf, uint32_t len, uint32_t writeIdx, float delaySamples)
{
    float readIdx = (float)writeIdx - delaySamples;
    if (readIdx < 0.0f) readIdx += (float)len;
    // A readIdx a hair below 0 (e.g. writeIdx 72, tap 72.000003) rounds to
    // EXACTLY len after the wrap in float, which would read one past the end
    // of the buffer (past the pool for the R side).  One compare closes it.
    uint32_t i0 = (uint32_t)readIdx;
    const float frac  = readIdx - (float)i0;       // taken BEFORE the fold
    if (i0 >= len) i0 -= len;
    const uint32_t i1 = (i0 + 1u == len) ? 0u : i0 + 1u;
    return buf[i0] + (buf[i1] - buf[i0]) * frac;
}
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

// ---------------------------------------------------------------------------
// JP-8000 Multi-FX modulation types — MEASURED pair engine (2026-10-23).
// Source: chorus-fx/jp-noise-capture-analysis-2026-10-23.md (JE-8086, JP-8000
// keyboard ROM, mono noise in, Level 127).  Every type is
//   W = g * [ x(t - c - d) - x(t - c + d) ] + g3 * x(t - 2c),  d = D * LFO
//   L = dry + m^2 * W,  R = dry - m^2 * W
// c, D, shape and rate are measured (tap tracking on the deconvolved wet;
// c from the constant tap sum, +/-0.01 ms).  g / g3 / feedback / high-pass are
// CALIBRATED: the JE's own recorded dry noise was rendered through this engine
// and matched to the JE wet (sign-off 3a):
//   * overall wet/dry rms   within +/-0.3 dB on all 12 types
//   * octave-band shape     63 Hz..4 kHz: <= 1.1 dB rms on 9 types; FLANGER SLOW
//                           2.0 dB, FLANGER FAST 2.3 dB, DEEP PHASING 1.5 dB
// The JE high-passes its wet per type -- the column hpHz / hpPoles.
// Rates seen for < 1 cycle (SLW, Jet, Deep Phasing) are estimates (5a).
// SC CLR is provisional (4a): it did not resolve into one clean pair; level
// and spectrum are matched, the tap motion is the SLW-like best guess.
// Deep Phasing: c / D / feedback were fitted to the JE wet spectrum jointly;
// the loop gain is feedback * g = 0.40 (stable well inside 1).
// FORMAT: { c ms, D ms, shape, rate Hz, g, g3, feedback, hpHz, hpPoles }
// ---------------------------------------------------------------------------
// JT_FLASH_DATA: read only on a type change (control plane), so it stays in
// flash instead of being copied into tight DTCM.
const FxChain::JpModParams FxChain::kJpModTypes[kNumJpModTypes] JT_FLASH_DATA = {
    /* SC SLW       taps 13.9-29.7, tri ~0.07 Hz            */ { 21.80f, 7.90f,  kJpLfoTri,    0.070f, 0.790f,  0.000f, 0.00f,   70.0f, 2 },
    /* SC MID       taps 5.84-8.46, tri 0.42 Hz             */ {  7.15f, 1.31f,  kJpLfoTri,    0.420f, 0.793f,  0.000f, 0.00f,   70.0f, 2 },
    /* SC FST       taps 14.59-14.99, sine 4.04 Hz          */ { 14.79f, 0.20f,  kJpLfoSine,   4.040f, 0.745f,  0.000f, 0.00f,  400.0f, 1 },
    /* SC CLR       PROVISIONAL (4a): slow pair ~17-26 ms   */ { 21.90f, 4.40f,  kJpLfoTri,    0.140f, 0.872f,  0.000f, 0.00f, 1100.0f, 1 },
    /* FLANGER SLOW taps 1.45-2.61, tri 0.315 Hz, 2c=4.06   */ {  2.03f, 0.58f,  kJpLfoTri,    0.315f, 1.163f,  0.823f, 0.00f, 1100.0f, 1 },
    /* FLANGER DEEP taps 0.73-3.29, tri 0.357 Hz, 2c=4.02   */ {  2.01f, 1.28f,  kJpLfoTri,    0.357f, 1.037f,  0.923f, 0.00f,  100.0f, 1 },
    /* FLANGER FAST taps ~1.55-2.05, sine 1.68 Hz, 2c=3.60  */ {  1.80f, 0.25f,  kJpLfoSine,   1.680f, 0.920f,  0.807f, 0.00f,  560.0f, 2 },
    /* DEEP PHASING unipolar d 0..0.105, ~0.18 Hz, resonant */ {  0.30f, 0.105f, kJpLfoUni,    0.180f, 1.144f,  0.000f, 0.35f, 1000.0f, 1 },
    /* JET PHASING  taps ~1.7-17.9, tri ~0.08 Hz, 2c=19.62  */ {  9.81f, 8.10f,  kJpLfoTri,    0.080f, 0.897f, -0.801f, 0.00f,  400.0f, 1 },
    /* TWISTING     taps 17.42-17.90, tri 0.167 Hz, 2c=35.3 */ { 17.66f, 0.24f,  kJpLfoTri,    0.167f, 0.834f, -0.834f, 0.00f,  560.0f, 1 },
    /* FREEZE PH 1  static taps 11.65 / 12.62               */ { 12.135f,0.485f, kJpLfoStatic, 0.000f, 0.755f,  0.000f, 0.00f, 1500.0f, 1 },
    /* FREEZE PH 2  static taps 34.85 / 35.83               */ { 35.34f, 0.49f,  kJpLfoStatic, 0.000f, 0.718f,  0.000f, 0.00f, 1500.0f, 1 },
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
    _dSampLCur     = -1.0f;   // X2: unprimed — prepareDelay snaps on first block
    _dSampRCur     = -1.0f;

    // D4 tape state.  Reset with the buffers: a stale LP value or drift phase
    // surviving a begin() would put a thump or a pitch jump on the first repeat.
    _fbLpStateL = 0.0f;
    _fbLpStateR = 0.0f;
    _wowPhase1  = 0.0f;
    _wowPhase2  = 0.41f;
    _flutPhase  = 0.25f;
    _tapeActive = false;
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
    // Out-of-range = OFF.  Before 3a nothing guarded this and the index went
    // straight into kModPresets[]; the table now has two halves.
    if (v1Type < 0 || v1Type >= (int)kNumModTypes) v1Type = -1;
    if (v1Type != _modType) {
        _modType   = v1Type;
        _lfoPhaseL = 0.0f;
        _lfoPhaseR = 0.5f;
        _jpPhase   = 0.0f;
        updateLfoIncrements();
    }
}

// 2a (JP capture gap analysis 2026-10-22, G2) — FLAGGED DEVIATION FROM v1.
// The JE-8086 capture shows the JP adds the chorus as a SEND: the dry term
// stays at unity through the whole Level sweep, and the wet gain follows
// ~Level^2.16 on the static FREEZE PHASE types.  v1 crossfaded
// ((1-m)*dry + m*wet) with a linear law.  The square is taken HERE, once per
// knob move, so the per-sample path keeps a single multiply.
void FxChain::setModMix(float mix)
{
    _modMix     = clampf(mix, 0.0f, 1.0f);
    _modWetGain = _modMix * _modMix;      // square-law Level taper (JP ~ m^2.16)
}

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
    if (_modType >= (int)kNumModPresets) { prepareJpMod(); return; }   // 3a/4a

    float rateHz = kModPresets[_modType].rateHz;
    if (_modRateOverride > 0.0f) rateHz = _modRateOverride;

    const float inc = kTwoPi * rateHz / kSampleRate;
    _lfoIncL = inc;
    _lfoIncR = inc * 1.01f;                 // 1 % stereo offset (v1 :620)
}

// ===========================================================================
// prepareJpMod — resolve a JP Multi-FX type into the per-sample cache (3a/4a).
// Runs only on a type change (via updateLfoIncrements), never per block.
//
// FLAGGED: the JP types IGNORE fx.mod_rate and fx.mod_feedback.  The JP has
// no rate or feedback control for these types, and the measured rates are the
// point of adding them; the knob's table default (norm 0.5) also maps to a
// 10 Hz override, which would turn every slow type into a fast warble.
// ===========================================================================
JT_COLD void FxChain::prepareJpMod()      // control plane: type change only
{
    const JpModParams& t = kJpModTypes[_modType - (int)kNumModPresets];
    const float msToSamp = 0.001f * kSampleRate;

    _jpCentre   = t.centreMs * msToSamp;
    _jpDepth    = t.depthMs  * msToSamp;
    _jpShape    = t.shape;
    _jpStaticD  = (t.shape == kJpLfoStatic) ? _jpDepth : 0.0f;   // fixed d, LFO skipped
    _jpInc      = (t.shape == kJpLfoStatic) ? 0.0f : t.rateHz / kSampleRate;
    _jpG        = t.g;
    _jpG3       = t.g3;
    _jpFeedback = t.feedback;
    _jpLastW    = 0.0f;       // no stale feedback across a type change

    // Wet high-pass: TPT one-pole, G = g / (1 + g), g = tan(pi fc / fs).
    // One tanf per type change, never per sample.
    _jpHpPoles  = (t.hpPoles > 2) ? 2 : t.hpPoles;
    const float gHp = tanf(kPi * t.hpHz / kSampleRate);
    _jpHpG      = gHp / (1.0f + gHp);
    _jpHpS[0]   = _jpHpS[1] = 0.0f;
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
    // E2: the v1 "ms <= 0 reverts to the preset time" sentinel is gone — see
    // the setDelayTime comment in FxChain.h.  A non-positive value can now only
    // arrive from a caller bug, so clamp into the legal range rather than
    // reinstating a hidden mode: prepareDelay would clamp it to kMinDelaySamp
    // regardless, and clamping here keeps debugDelayMs() honest about what the
    // engine is actually running.
    ms = clampf(ms, kMinDelayMs, kMaxDelayMs);

    // Preserve the active preset's L/R ratio so panning presets stay stereo.
    // NOTE: a preset whose ratio exceeds 1 can push the R tap past kMaxDelayMs
    // near the top of the sweep; prepareDelay clamps it to kDelayLen - 2, so
    // the buffer is safe and the only consequence is that the stereo spread
    // closes up at the very top of the knob.
    float ratio = 1.0f;
    if (_delayType >= 0 && _delayType < (int)kNumDelayPresets) {
        const DelayParams& p = kDelayPresets[_delayType];
        if (p.delayMsL > 0.0f) ratio = p.delayMsR / p.delayMsL;
    }
    _delayTimeOverrideL = ms;
    _delayTimeOverrideR = ms * ratio;
}

// ===========================================================================
// D4 tape-engine setters.  Control plane only — every one of these just stores
// and lets prepareDelay derive the per-sample cache at block rate.
// ===========================================================================
void FxChain::setDelayEngine(int engine)
{
    const int e = (engine == 1) ? 1 : 0;
    if (e == _delayEngine) return;
    _delayEngine = e;
    // Reset the loop state on a switch.  The LP holds a DC-ish value and the
    // drift phases hold a position; carrying either across an engine change
    // would put a thump or a pitch jump on the first repeat.
    _fbLpStateL = 0.0f;
    _fbLpStateR = 0.0f;
    _wowPhase1  = 0.0f;
    _wowPhase2  = 0.41f;
    _flutPhase  = 0.25f;
}

void FxChain::setDelayTone(float hz)     { _delayToneHz   = clampf(hz, 500.0f, 12000.0f); }
void FxChain::setDelaySat(float norm)    { _delaySatNorm  = clampf(norm, 0.0f, 1.0f); }
void FxChain::setDelayWow(float norm)    { _delayWowNorm  = clampf(norm, 0.0f, 1.0f); }
void FxChain::setDelayWowRate(float hz)  { _delayWowHz    = clampf(hz, kWowHzMin, kWowHzMax); }
void FxChain::setDelayFlutter(float norm){ _delayFlutNorm = clampf(norm, 0.0f, 1.0f); }

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

    // --- mute-lap clamp (regression fix, 10 s buffer) ------------------------
    // setDelayEffect arms _delayMuteCounter with kDelayLen — "at most one full
    // buffer lap".  That was 1.50 s when kDelayLen was 66152 and became 10.00 s
    // when the buffer grew to 441002, which is audible as the delay dropping
    // out for several seconds after a preset change.
    //
    // A full lap was always more than required.  begin() zeroes the pool, so
    // the mute is not guarding against uninitialised PSRAM; it is stopping the
    // PREVIOUS preset's audio, still resident in the buffer, from being read
    // back and fed round the loop at the new preset's time.  The read head only
    // ever reaches dSamp samples behind the write head, so flushing dSamp
    // samples is sufficient — everything deeper is unreachable.
    //
    // Clamping HERE rather than at the set site covers every case in one place:
    // preset changes, patch loads and knob moves all pass through prepareDelay,
    // and the clamp re-evaluates whenever the time does.  Block rate, two
    // compares — free.  At the 1500 ms preset the mute is now 1.5 s, exactly
    // what it was before the buffer grew.
    if (_delayMuteCounter > 0) {
        const float    deepest = (dSampL > dSampR) ? dSampL : dSampR;
        const uint32_t needed  = (uint32_t)deepest + 2u;   // +2: interpolation pair
        if (_delayMuteCounter > needed) _delayMuteCounter = needed;
    }

    _delayFbCached    = (_delayFeedbackOverride >= 0.0f) ? _delayFeedbackOverride
                                                         : p.feedback;
    _delayWetCached   = wet;
    _delayDryCached   = 1.0f - wet;

    // X2: one-pole coefficient for the tap-distance slew.  Block rate — the
    // per-sample loop must not run an expf.
    _timeSlewCoeff = 1.0f - expf(-1.0f / (kTimeSlewMs * 0.001f * kSampleRate));

    // First block after begin(), or after a preset change reset the cursors:
    // snap rather than ramping up from nothing, which would sweep the delay in
    // from zero on the very first note.
    if (_dSampLCur < 0.0f) _dSampLCur = dSampL;
    if (_dSampRCur < 0.0f) _dSampRCur = dSampR;

    prepareTape(dSampL, dSampR);
}

// ---------------------------------------------------------------------------
// prepareTape — D4 block-rate cache for the tape engine.
//
// EVERYTHING expensive lives here: the expf for the one-pole coefficient, the
// divide for the gain compensation, the delay-proportional drift amplitudes.
// processDelay then reads plain floats and does no transcendental work at all
// (rule 6: do not calculate per-sample what block rate can answer).
//
// _tapeActive is the ONE branch the Digital path pays.  It is false unless the
// engine is Tape AND at least one tape control is doing something, so a Tape
// patch with everything at zero also costs nothing.
// ---------------------------------------------------------------------------
void FxChain::prepareTape(float dSampL, float dSampR)
{
    if (_delayEngine != 1) { _tapeActive = false; return; }

    // 12 kHz at 44.1 kHz is above the audible top of the repeats, so treat the
    // knob's maximum as "filter off" rather than burning an expf on a no-op.
    const bool toneOn = (_delayToneHz < 11999.0f);
    const bool satOn  = (_delaySatNorm  > 0.0f);
    const bool wowOn  = (_delayWowNorm  > 0.0f);
    const bool flutOn = (_delayFlutNorm > 0.0f);

    _tapeActive = toneOn || satOn || wowOn || flutOn;
    if (!_tapeActive) return;

    // One-pole LP: g = 1 - exp(-2*pi*fc/fs).  g == 1 is a mathematically exact
    // bypass (state jumps straight to the input), which is why "off" is
    // represented as g = 1 rather than as another branch in the inner loop.
    _fbLpG = toneOn
           ? (1.0f - expf(-2.0f * kPi * _delayToneHz * (1.0f / kSampleRate)))
           : 1.0f;

    // Drive 1..4.  comp = 1/drive restores unity for small signals, since the
    // soft clip is approximately linear there.
    _satDrive = 1.0f + _delaySatNorm * 3.0f;
    _satComp  = 1.0f / _satDrive;

    // W3: derive the delay amplitude from the WANTED PITCH DEVIATION.
    //     dev = 2^(cents/1200) - 1      (fractional pitch change)
    //     A   = dev / (2*pi*f)          (seconds of delay modulation)
    // so the audible wobble is the same at any delay time and any rate, which
    // the previous ms-based depth emphatically was not.
    const float invSr   = 1.0f / kSampleRate;
    const float twoPi   = 2.0f * kPi;
    const float wowHz   = _delayWowHz;

    auto centsToSamples = [&](float cents, float rateHz) -> float {
        if (cents <= 0.0f || rateHz <= 0.0f) return 0.0f;
        const float dev = exp2f(cents * (1.0f / 1200.0f)) - 1.0f;
        return (dev / (twoPi * rateHz)) * kSampleRate;   // seconds -> samples
    };

    // Two wow components at incommensurate rates, so the drift never settles
    // into an audible repeating cycle the way a single sine does.
    const float wowCents  = kWowMaxCents     * _delayWowNorm;
    const float flutCents = kFlutterMaxCents * _delayFlutNorm;

    float wowAmp1 = centsToSamples(wowCents * kWowMix1, wowHz);
    float wowAmp2 = centsToSamples(wowCents * kWowMix2, wowHz * kWowRate2Ratio);
    float flutAmp = centsToSamples(flutCents, kFlutterHz);

    // Safety clamp: total excursion must stay well inside the delay itself, or
    // a slow deep wow on a short slapback modulates through zero and folds the
    // read pointer.  The SHALLOWER tap governs — it is the one that runs out.
    const float shallowest = (dSampL < dSampR) ? dSampL : dSampR;
    const float budget     = shallowest * kDriftMaxFrac;
    const float wanted     = wowAmp1 + wowAmp2 + flutAmp;
    if (wanted > budget && wanted > 0.0f) {
        const float scale = budget / wanted;
        wowAmp1 *= scale; wowAmp2 *= scale; flutAmp *= scale;
    }

    _wowSampAmp1 = wowOn  ? wowAmp1 : 0.0f;
    _wowSampAmp2 = wowOn  ? wowAmp2 : 0.0f;
    _flutSampAmp = flutOn ? flutAmp : 0.0f;

    _wowInc1 = wowHz                    * invSr;
    _wowInc2 = wowHz * kWowRate2Ratio   * invSr;
    _flutInc = kFlutterHz               * invSr;
}

// ===========================================================================
// processModulation — chorus / flanger / phaser (v1 :625-763), verbatim DSP.
// ===========================================================================
inline void FxChain::processModulation(float inL, float inR,
                                       float& outL, float& outR)
{
    if (_modType < 0 || !_modBufL || !_modBufR) { outL = inL; outR = inR; return; }
    if (_modType >= (int)kNumModPresets) { processModulationJp(inL, inR, outL, outR); return; }

    const ModParams& p = kModPresets[_modType];

    const float feedback = (_modFeedbackOverride >= 0.0f) ? _modFeedbackOverride
                                                          : p.feedback;
    // 2a: additive send.  The input passes at unity; only the wet tap is
    // scaled (square-law Level, cached in setModMix).  p.mix stays a per-preset
    // wet trim (1.0 for every v1 preset).
    const float wetMix = _modWetGain * p.mix;

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

    // N2 (2026-10-22) — FLAGGED: the v1 lambda here read buf[bufSize] (one past
    // the end; past the POOL on the R side) whenever writeIdx - delay was a
    // hair below 0, because the wrap rounds to exactly bufSize in float.  The
    // v1 path now calls modReadTap directly (inlined, no function pointer),
    // which folds that case.  Every other read returns the identical value
    // (same i0 / i1 / frac arithmetic).

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

        dL0 = clampf(dL0, kMinModTapSamples, maxIdx); dL1 = clampf(dL1, kMinModTapSamples, maxIdx); dL2 = clampf(dL2, kMinModTapSamples, maxIdx);
        dR0 = clampf(dR0, kMinModTapSamples, maxIdx); dR1 = clampf(dR1, kMinModTapSamples, maxIdx); dR2 = clampf(dR2, kMinModTapSamples, maxIdx);

        const float t0L = modReadTap(_modBufL, kModLen, _modWriteIdx, dL0);
        const float t1L = modReadTap(_modBufL, kModLen, _modWriteIdx, dL1);
        const float t2L = modReadTap(_modBufL, kModLen, _modWriteIdx, dL2);
        delayedL = (t0L + t1L + t2L) * (1.0f / 3.0f);

        const float t0R = modReadTap(_modBufR, kModLen, _modWriteIdx, dR0);
        const float t1R = modReadTap(_modBufR, kModLen, _modWriteIdx, dR1);
        const float t2R = modReadTap(_modBufR, kModLen, _modWriteIdx, dR2);
        delayedR = (t0R + t1R + t2R) * (1.0f / 3.0f);
    } else {
        float dSampL = (p.baseDelayMsL + p.depthMsL * lfoL) * msToSamples;
        float dSampR = (p.baseDelayMsR + p.depthMsR * lfoR) * msToSamples;
        dSampL = clampf(dSampL, kMinModTapSamples, maxIdx);   // 1a
        dSampR = clampf(dSampR, kMinModTapSamples, maxIdx);   // 1a
        delayedL = modReadTap(_modBufL, kModLen, _modWriteIdx, dSampL);
        delayedR = modReadTap(_modBufR, kModLen, _modWriteIdx, dSampR);
    }

    _modBufL[_modWriteIdx] = inL + delayedL * feedback;
    _modBufR[_modWriteIdx] = inR + delayedR * feedback;
    _modWriteIdx = (_modWriteIdx + 1) % kModLen;

    outL = inL + wetMix * delayedL;      // 2a: dry at unity (JP send)
    outR = inR + wetMix * delayedR;
}

// ===========================================================================
// processModulationJp — JP-8000 Multi-FX, measured pair engine (2026-10-23).
// ONE mono wet path; the stereo is the polarity of W (R = -L), as measured.
// Per sample: one phase add + shape (skipped when static), two interpolated
// reads, a third only on the types with a 2c tap, then 1-2 high-pass stages.
// ===========================================================================
inline void FxChain::processModulationJp(float inL, float inR,
                                         float& outL, float& outR)
{
    // The chain fans ONE mono signal to both sides before this stage, so
    // inL == inR here; averaging keeps it exact if that ever changes.
    const float x = 0.5f * (inL + inR);

    // Write FIRST, with the previous sample's W as feedback (Deep Phasing's
    // resonance; 0 elsewhere).  Writing before reading lets a tap sit anywhere
    // down to 0 samples: Deep Phasing's near tap reaches ~0.005 ms, and a
    // 0-sample read is then the current input, never the stale lap (cf. 1a).
    _modBufL[_modWriteIdx] = x + _jpFeedback * _jpLastW;

    float d = _jpStaticD;
    if (_jpShape != kJpLfoStatic) {
        _jpPhase += _jpInc;
        if (_jpPhase >= 1.0f) _jpPhase -= 1.0f;
        const float lfo = (_jpShape == kJpLfoSine) ? FastMath::fastSin01(_jpPhase)
                                                   : jpTri(_jpPhase);
        d = (_jpShape == kJpLfoUni) ? 0.5f * _jpDepth * (1.0f + lfo)   // 0..D, taps meet
                                    : _jpDepth * lfo;                   // -D..+D, taps cross
    }

    const float maxIdx = (float)(kModLen - 2);
    const float dA = clampf(_jpCentre + d, 0.0f, maxIdx);
    const float dB = clampf(_jpCentre - d, 0.0f, maxIdx);
    float w = _jpG * (modReadTap(_modBufL, kModLen, _modWriteIdx, dA)
                    - modReadTap(_modBufL, kModLen, _modWriteIdx, dB));
    if (_jpG3 != 0.0f)                                  // fixed tap at 2c
        w += _jpG3 * modReadTap(_modBufL, kModLen, _modWriteIdx, 2.0f * _jpCentre);
    _jpLastW = w;             // the loop (feedback) takes W BEFORE the high-pass

    // Wet high-pass, 0..2 cascaded TPT one-poles: hp = x - lp.
    for (uint8_t st = 0; st < _jpHpPoles; ++st) {
        const float v  = (w - _jpHpS[st]) * _jpHpG;
        const float lp = v + _jpHpS[st];
        _jpHpS[st]     = lp + v;
        w -= lp;
    }

    _modWriteIdx = (_modWriteIdx + 1) % kModLen;

    const float wet = _modWetGain * w;                  // m^2 send law (2a)
    outL = inL + wet;
    outR = inR - wet;                                   // mono wet, R inverted
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
    // ---- X2: slew the tap distance toward the target ------------------------
    // One-pole per sample, then an exact snap inside kSlewSnap so a settled
    // delay is bit-identical to the pre-slew engine.  Two multiply-adds and two
    // compares; the snap is what keeps the Digital path's render baseline.
    const float tgtL = _delaySampLCached;
    const float tgtR = _delaySampRCached;
    _dSampLCur = slewTap(_dSampLCur, tgtL, _timeSlewCoeff);
    _dSampRCur = slewTap(_dSampRCur, tgtR, _timeSlewCoeff);

    float dSampL   = _dSampLCur;
    float dSampR   = _dSampRCur;

    // ---- D4: transport drift (wow + flutter) --------------------------------
    // Digital pays ONE predictable branch here and nothing else.  The read head
    // moves, the write head does not — that is what makes this a pitch effect
    // rather than a delay-time change, and why the drift is applied to the tap
    // distance rather than to _delayWriteIdx.
    if (_tapeActive) {
        // Phase accumulators wrap in 0..1 for fastSin01.
        _wowPhase1 += _wowInc1;  if (_wowPhase1 >= 1.0f) _wowPhase1 -= 1.0f;
        _wowPhase2 += _wowInc2;  if (_wowPhase2 >= 1.0f) _wowPhase2 -= 1.0f;
        _flutPhase += _flutInc;  if (_flutPhase >= 1.0f) _flutPhase -= 1.0f;

        // W3: the two taps read the SAME drift at different phases, so the
        // wobble travels across the image.  One shared drift (the first
        // version) moved both sides in lockstep and read as flat.
        auto wrap = [](float p) { return (p >= 1.0f) ? p - 1.0f : p; };
        const float pR = kDriftPhaseR;

        const float driftL = FastMath::fastSin01(_wowPhase1)      * _wowSampAmp1
                           + FastMath::fastSin01(_wowPhase2)      * _wowSampAmp2
                           + FastMath::fastSin01(_flutPhase)      * _flutSampAmp;
        const float driftR = FastMath::fastSin01(wrap(_wowPhase1 + pR)) * _wowSampAmp1
                           + FastMath::fastSin01(wrap(_wowPhase2 + pR)) * _wowSampAmp2
                           + FastMath::fastSin01(wrap(_flutPhase + pR)) * _flutSampAmp;

        dSampL += driftL;
        dSampR += driftR;

        // The drift can push a short tap below the minimum or a near-maximum
        // tap past the end of the buffer; both would be an out-of-bounds read,
        // so clamp exactly as prepareDelay does for the un-drifted value.
        const float maxSamp = (float)(kDelayLen - 2);
        dSampL = clampf(dSampL, kMinDelaySamp, maxSamp);
        dSampR = clampf(dSampR, kMinDelaySamp, maxSamp);
    }

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

    // ---- D4: feedback-loop conditioning -------------------------------------
    // Order is the physical one: playback head -> electronics -> record head.
    // The LP is INSIDE the loop, so repeat N has been filtered N times and the
    // tail darkens progressively.  The soft clip sits after it and doubles as
    // the runaway limiter a real tape loop has — at feedback near 1 the repeats
    // compress instead of clipping into digital nastiness.
    float fbL = delayL;
    float fbR = delayR;
    if (_tapeActive) {
        _fbLpStateL += _fbLpG * (fbL - _fbLpStateL);
        _fbLpStateR += _fbLpG * (fbR - _fbLpStateR);
        fbL = tapeSoftClip(_fbLpStateL, _satDrive, _satComp);
        fbR = tapeSoftClip(_fbLpStateR, _satDrive, _satComp);
    }

    _delayBufL[_delayWriteIdx] = inL + fbL * feedback;
    _delayBufR[_delayWriteIdx] = inR + fbR * feedback;
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
