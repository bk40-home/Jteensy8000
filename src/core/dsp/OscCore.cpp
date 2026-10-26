// =============================================================================
// OscCore.cpp — implementation (design and contracts in OscCore.h)
// =============================================================================
// Layout: the templated step() helper handles phase advance + FM + sync for
// every wave; render() dispatches ONCE per block to the right template
// instantiation, then renderImpl() switches ONCE on the waveform.  The
// compiler emits a dedicated tight loop per (wave × feature-set) actually
// used — hand-unrolled clarity without hand-unrolled duplication.
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "core/dsp/OscCore.h"

#include "core/dsp/FastMath.h"

namespace JT {

namespace {

// PolyBLEP residual — identical maths to OscSaw.cpp (Phase 1); duplicated
// deliberately: OscSaw retires when OscSection replaces it in Step 3, and
// this file then owns the only copy.
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

// -----------------------------------------------------------------------------
// reflectFold — fold x back into [-1, +1] by mirroring at the rails, repeating
// until it lands in range (a classic triangle wavefolder).  Used by the vTRI
// SHAPE morph: the base triangle is amplitude-scaled by a shape-driven gain
// (1..4) then folded, so overshoot past +-1 reflects inward and each reflection
// adds a lobe — matching the JP-8000 triangle SHAPE diagram (plain -> dense).
// The 4.0 period is the length of one up-down cycle of the reflection triangle;
// fmodf keeps the cost O(1) no matter how large the gain drives x.
inline float reflectFold(float x)
{
    // Shift so the mirror maths is symmetric, reduce modulo one fold period.
    float t = fmodf(x + 1.0f, 4.0f);
    if (t < 0.0f) t += 4.0f;                 // fmodf can return negative
    t -= 1.0f;                               // back to centred range
    if (t >  1.0f) t =  2.0f - t;            // reflect at the top rail
    if (t < -1.0f) t = -2.0f - t;            // reflect at the bottom rail
    return t;
}

// -----------------------------------------------------------------------------
// JP-8000 SAW SHAPE law ("JvSAW", Wave::JpVarSaw) — MEASURED, closed form.
// Source: JE-8086 calibrated Ctrl1 ramp, 2026-10-22 (osc-shapes doc §1).
//   y = (2φ − 1) − β(k) · (4π²/3) · B3(φ),   B3(φ) = φ(φ − ½)(φ − 1)
// i.e. a fixed saw plus β × the saw integrated twice (a 1/n³ series in phase
// with the saw — B3 is the closed form of that series; 4π²/3 converts the
// series weight to B3 units).  Fit error 0.2–1.1 % across the sweep.
//   β(k) = 1.2118 − 4.70·k, linear to 0.01; pure saw at k ≈ 0.258.
//   Below k = 0.0334 the JP holds β (end-stop dead zone) — clamped here.
// B3 is C1-continuous, so the ONLY discontinuity is the saw edge at φ = 0:
// one polyBLEP, exactly as BlSaw.  JP itself is band-limited (−45 dB alias
// at C7, measured) so the band-limited form is the faithful one.
// Level: kJpSawGain = 1 / peak at k = 1 (1.657) -> output peak ±1 nominal at
// the loudest shape, while the JP's relative level change across SHAPE
// (RMS dip ~10 dB at the centre) is kept exactly.
// -----------------------------------------------------------------------------
constexpr float kJpSawBeta0    = 1.2118f;
constexpr float kJpSawBetaK    = 4.70f;
constexpr float kJpSawKMin     = 0.0334f;           // measured dead zone
constexpr float kJpSawB3Scale  = 13.1594725f;       // 4π²/3
constexpr float kJpSawGain     = 0.6034671f;        // 1 / 1.657091

// -----------------------------------------------------------------------------
// JP-8000 TRIANGLE MOD law ("JTRIM", Wave::JpTriMod) — MEASURED, closed form.
// Source: JE-8086 calibrated Ctrl1 ramp, 2026-10-22 (osc-shapes doc §3).
//   y = wrap( g(k) · tri(φ) ),  wrap(v) = ((v + 1) mod 2) − 1  (WRAP-around,
//   not the reflect-fold of vTRI), tri rising −1 -> +1 over φ 0 -> ½.
// Fit error 2–9 % across the sweep; odd harmonics only; constant level.
//   g(k) = 1.635 − 0.967·(k − ½), clamped to [1.375, 2.02] — the JP holds g
//   below k ≈ 0.10 and above k ≈ 0.77 (end-stop dead zones, measured).
// Because 1 < g < 3 there are exactly FOUR wrap jumps per cycle, at phases
// solved once per block from g:
//   rising half  (v = g(4φ − 1)):  v = −1 at φa = (1 − 1/g)/4, v = +1 at
//                                   φb = (1 + 1/g)/4   — both jumps −2
//   falling half (v = g(3 − 4φ)):  v = +1 at φc = (3 − 1/g)/4, v = −1 at
//                                   φd = (3 + 1/g)/4   — both jumps +2
// Each jump gets one polyBLEP (correction h/2 · polyBlep, h = jump height —
// the same convention as BlSaw's −polyBlep for its −2 edge).  The JP's own
// Tri-mod measures −35 dB alias at C7, so polyBLEP is the matching quality.
// -----------------------------------------------------------------------------
constexpr float kJpTriModGMid   = 1.635f;
constexpr float kJpTriModGSlope = 0.967f;
constexpr float kJpTriModGMin   = 1.375f;
constexpr float kJpTriModGMax   = 2.02f;

#if JT_SYNC_ANTIALIAS
// F3: band-limit a HARD-SYNC reset — a step discontinuity of arbitrary height
// (unlike the periodic edges polyBlep handles).  `frac` is the sub-sample
// reset position (0..1, from syncIn); `stepH` is the output jump (post-reset
// minus the level held just before).  We split an ideal band-limited step's
// residual between the sample containing the reset and the next one; `carry`
// receives the next-sample part.  Returns the current-sample correction.
inline float syncStepBlep(float frac, float stepH, float& carry)
{
    const float d = 1.0f - frac;                    // fraction of sample after reset
    carry        = -stepH * 0.5f * d * d;
    return         stepH * 0.5f * (1.0f - d * d);
}
#endif

} // namespace

// -----------------------------------------------------------------------------
// Control plane
// -----------------------------------------------------------------------------

void OscCore::setFrequency(float hz)
{
    if (hz == _freq) return;          // glide/FM call this per block: skip
    _freq = hz;                       // untouched pitches entirely
    if (hz < 0.0f)                hz = 0.0f;      // 0 Hz is legal: DC hold
    if (hz > kSampleRate * 0.45f) hz = kSampleRate * 0.45f;
    _inc = hz / kSampleRate;
}

void OscCore::setShape(float s)
{
    // Table morph keeps the FULL range so SHAPE reaches every JP frame; the
    // 5..95% clamp below exists for pulse/TriVar only.
    _morph = (s < 0.0f) ? 0.0f : ((s > 1.0f) ? 1.0f : s);

    // 5%..95%: a pulse at 0 or 1 width is silence-with-DC, and TriVar at
    // the extremes degenerates into the plain saws that already exist.
    // The floor is 50 % under the JP PWM law (setJpPwmClamp) — see header.
    _shapeIn = s;
    if (s < 0.05f) s = 0.05f;
    if (s > 0.95f) s = 0.95f;
    _shape = s;
    // Pulse width: same clamp under JtPwm (byte-identical), 50 % floor under
    // JpPwm.  Kept apart from _shape so the law never touches TriVar.
    _pw = (s < _pwFloor) ? _pwFloor : s;
    updatePulseDc();
}

void OscCore::updatePulseDc()
{
    // Control plane: called from setWave / setShape / setJpPwmClamp only.
    const bool pulse = (_wave == Wave::Pulse) || (_wave == Wave::BlPulse);
    _pulseDcRaw = pulse ? (2.0f * _pw - 1.0f) : 0.0f;
    // JpPwm <=> the 50 % floor (setJpPwmClamp): remove the DC there only.
    _dcOut      = (_pwFloor >= 0.5f) ? _pulseDcRaw : 0.0f;
}

void OscCore::setJpPwmClamp(bool on)
{
    const float floor = on ? 0.5f : 0.05f;
    if (floor == _pwFloor) return;           // dirty check: no-op re-sends
    _pwFloor = floor;
    setShape(_shapeIn);                      // re-clamp the current width
}

void OscCore::setArbTable(const int16_t* data, uint16_t length)
{
    // Table swap is glitch-safe by construction: it lands at a block
    // boundary (control plane) and phase is preserved, so mid-note bank
    // browsing morphs rather than clicks — same behaviour as v1.
    // Stored as v1 did; render() wraps it as a 1-frame, 1-level set for the
    // shared reader.  An invalid table -> the Arb case keeps v1's naive saw.
    _arbData = (length >= 2) ? data : nullptr;
    _arbLen  = (length >= 2) ? length : 0;
}

void OscCore::resetPhase(float phase01)
{
    _phase = phase01 - (float)(int)phase01;
    if (_phase < 0.0f) _phase += 1.0f;
}

float OscCore::nextNoise()
{
    // xorshift32: 3 shifts + 3 xors, period 2^32-1, spectrally white far
    // beyond audio needs.  Never zero (seed forced odd in seedNoise).
    uint32_t x = _rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    _rng = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

// -----------------------------------------------------------------------------
// The shared phase step.  Returns the NEW phase; wrap/FM/sync handled here
// so every waveform loop below stays a pure "phase -> sample" mapping.
// -----------------------------------------------------------------------------
template <int FmMode, bool HasSyncIn, bool HasSyncOut>
inline float OscCore::step(size_t i, const float* fmBuf, float fmOctaves,
                           const float* syncIn, float* syncOut)
{
    // Effective increment.  FmMode is a compile-time constant, so each mode
    // compiles to its own loop with no per-sample mode test:
    //   kFmNone  (0): base increment only.
    //   kFmExp2  (1): exponential FM exactly as v1 routed it (±1.0 modulator
    //                 × N octaves through the fast pow2) — JtXmod + pitch env.
    //   kFmRatio (2): fmBuf[i] IS the increment multiplier, any sign — the
    //                 caller pre-composes 2^pitch × (1 + k·osc2) for JpXmod
    //                 (through-zero linear FM: a negative ratio runs the
    //                 phase BACKWARDS, exactly as measured on the JE-8086).
    float inc = _inc;
    if (FmMode == kFmExp2)
        inc = _inc * FastMath::fastPow2(fmBuf[i] * fmOctaves);
    else if (FmMode == kFmRatio)
        inc = _inc * fmBuf[i];

    float ph = _phase + inc;

    bool  wrapped  = false;
    float wrapFrac = -1.0f;
    if (FmMode != kFmRatio) {
        // Forward-only modes: the original single-wrap test, untouched, so
        // their output stays byte-identical to the pre-JpXmod build.
        if (ph >= 1.0f) {
            ph -= 1.0f;
            wrapped = true;
            // Fraction of THIS sample at which the wrap occurred — lets the
            // slave re-start with sub-sample alignment instead of quantising
            // sync to sample edges.
            wrapFrac = (inc > 0.0f) ? (1.0f - ph / inc) : 0.0f;
        }
    } else if (ph >= 1.0f || ph < 0.0f) {
        // Through-zero: the phase may leave [0,1) in EITHER direction, and at
        // full depth by more than one cycle per sample (|ratio| up to ~35 ×
        // the base increment).  floorf folds any excursion back into range.
        // Wrap fraction = where in this sample the 1->0 (forward) or 0->1
        // (backward) boundary was crossed; clamped for the multi-cycle case.
        const float startPh = _phase;
        ph -= floorf(ph);
        wrapped = true;
        float fr = (inc > 0.0f) ? (1.0f - startPh) / inc
                 : (inc < 0.0f) ? startPh / -inc : 0.0f;
        wrapFrac = (fr < 0.0f) ? 0.0f : ((fr > 1.0f) ? 1.0f : fr);
    }

    if (HasSyncOut)
        syncOut[i] = wrapped ? wrapFrac : -1.0f;

    if (HasSyncIn && syncIn[i] >= 0.0f) {
        // Hard sync: master wrapped syncIn[i] into the sample; the slave's
        // phase restarts and advances through the REMAINDER of the sample.
        ph = (1.0f - syncIn[i]) * inc;
        if (FmMode == kFmRatio) ph -= floorf(ph);   // remainder may run backwards
        wrapped = true;               // a sync reset also refreshes S&H
#if JT_SYNC_ANTIALIAS
        _blepFrac = syncIn[i];        // F3: consumed by JT_SYNC_BLEP in the loop
#endif
    }

    _phase = ph;

    // Sample & Hold is the one wave that reacts to the wrap event itself.
    if (_wave == Wave::SampleHold && wrapped)
        _shValue = nextNoise();

    return ph;
}

// -----------------------------------------------------------------------------
// Loop macros shared by renderTable() and renderImpl() (both are templated on
// the same <FmMode, HasSyncIn, HasSyncOut> set, and both loop on `i`).  Defined
// once here, #undef'd after renderImpl.
// -----------------------------------------------------------------------------
// Local shorthand: advances phase with the active feature set.
#define JT_STEP() step<FmMode, HasSyncIn, HasSyncOut>(i, fmBuf, fmOctaves, syncIn, syncOut)

#if JT_SYNC_ANTIALIAS
// F3: applied after each sample write in a braced loop.  On a sync-reset
// sample it band-limits the step (out[i] vs the previously held level) and
// carries the residual to the next sample; otherwise just applies any
// pending carry.  Compiles away for non-slave instantiations (no HasSyncIn)
// and when the flag is off.
#define JT_SYNC_BLEP()                                                    \
        if (HasSyncIn) {                                                      \
            out[i] += _blepCarry; _blepCarry = 0.0f;                          \
            if (_blepFrac >= 0.0f) {                                          \
                out[i] -= syncStepBlep(_blepFrac, out[i] - _preResetOut,     \
                                       _blepCarry);                          \
                _blepFrac = -1.0f;                                           \
            }                                                                 \
            _preResetOut = out[i];                                            \
        } else ((void)0)
#else
#define JT_SYNC_BLEP() ((void)0)
#endif


// -----------------------------------------------------------------------------
// THE wavetable reader — ARB (1 frame, 1 level) and the measured JP morphs
// (N frames × M band-limited levels) share this one loop.
//
// Per BLOCK (control work, done once):
//   * level: first level whose highest harmonic × base increment is below
//     Nyquist (0.5 cycles/sample).  Levels are stored most-harmonics-first;
//     harm 0 means "unknown/full band" and is always accepted (ARB).  Chosen
//     from the BASE pitch: FM/X-MOD excursions within the block can exceed it
//     (same trade-off as every block-rate mip selector; brightness under deep
//     FM is authentic to the table, aliasing is bounded by the next level).
//   * morph: _morph (0..1, full range) -> frame pair (a, a+1) + fraction.
//     Block-rate like PWM; 128 samples is far below audible stepping.
// Per SAMPLE: linear interpolation along the cycle (multiply-indexed, any
// length — AKWF is 600), then, for Morph, a linear crossfade between frames.
//
// BYTE-IDENTITY: with Morph=false the per-sample maths is exactly the old
// ARB loop — pos = ph × (float)len, idx/frac/nxt, a + (b − a) × frac,
// × (1/32768) — verified by cmp of rendered output before/after the merge.
// -----------------------------------------------------------------------------
template <int FmMode, bool HasSyncIn, bool HasSyncOut, bool Morph>
void OscCore::renderTable(float* out, size_t n,
                          const float* fmBuf, float fmOctaves,
                          const float* syncIn, float* syncOut,
                          const WaveTableSet& ts)
{
    // --- band-limited level for this block's base pitch ---
    uint16_t lvl = 0;
    while (lvl + 1u < ts.levels
           && ts.levelHarm[lvl] != 0u
           && (float)ts.levelHarm[lvl] * _inc > 0.5f)
        ++lvl;
    const uint32_t len  = ts.levelLen[lvl];
    const float    lenF = (float)len;

    // --- morph frames (Morph only; frames >= 2 guaranteed by the caller) ---
    uint32_t fA = 0;
    float    mf = 0.0f;
    if (Morph) {
        const float pos = _morph * (float)(ts.frames - 1u);
        fA = (uint32_t)pos;
        if (fA > ts.frames - 2u) fA = ts.frames - 2u;     // _morph == 1 edge
        mf = pos - (float)fA;
    }
    const int16_t* A = ts.data + fA * ts.samplesPerFrame + ts.levelOffset[lvl];
    const int16_t* B = A + ts.samplesPerFrame;              // read only if Morph

    for (size_t i = 0; i < n; ++i) {
        const float pos  = JT_STEP() * lenF;
        uint32_t idx     = (uint32_t)pos;
        const float frac = pos - (float)idx;
        if (idx >= len) idx = 0;                            // phase==1 edge
        const uint32_t nxt = (idx + 1u < len) ? idx + 1u : 0u;
        const float a = (float)A[idx];
        const float b = (float)A[nxt];
        float v = a + (b - a) * frac;
        if (Morph) {
            const float c = (float)B[idx];
            const float d = (float)B[nxt];
            const float w = c + (d - c) * frac;
            v += (w - v) * mf;
        }
        out[i] = v * (1.0f / 32768.0f);
        JT_SYNC_BLEP();
    }
}

// -----------------------------------------------------------------------------
// Waveform loops.  Every wave outputs ±1 nominal.
// -----------------------------------------------------------------------------
template <int FmMode, bool HasSyncIn, bool HasSyncOut>
void OscCore::renderImpl(float* out, size_t n,
                         const float* fmBuf, float fmOctaves,
                         const float* syncIn, float* syncOut)
{
    switch (_wave) {

    case Wave::Sine:
        for (size_t i = 0; i < n; ++i) {
            out[i] = FastMath::fastSin01(JT_STEP());
            JT_SYNC_BLEP();
        }
        break;

    case Wave::Saw:                   // naive: the JP-8000's own edge
        for (size_t i = 0; i < n; ++i) {
            out[i] = 2.0f * JT_STEP() - 1.0f;
            JT_SYNC_BLEP();
        }
        break;

    case Wave::SawRev:
        for (size_t i = 0; i < n; ++i) {
            out[i] = 1.0f - 2.0f * JT_STEP();
            JT_SYNC_BLEP();
        }
        break;

    case Wave::Square:
        for (size_t i = 0; i < n; ++i) {
            out[i] = (JT_STEP() < 0.5f) ? 1.0f : -1.0f;
            JT_SYNC_BLEP();
        }
        break;

    case Wave::Pulse:
        for (size_t i = 0; i < n; ++i) {
            out[i] = (JT_STEP() < _pw) ? 1.0f : -1.0f;
            JT_SYNC_BLEP();
        }
        break;

    case Wave::Triangle:
        for (size_t i = 0; i < n; ++i) {
            const float ph = JT_STEP();
            // 0..0.5 rises -1..+1, 0.5..1 falls back — branch-free fabs form.
            const float x = ph + ph;                    // 0..2
            out[i] = 2.0f * ((x < 1.0f ? x : 2.0f - x)) - 1.0f;
            JT_SYNC_BLEP();
        }
        break;

    case Wave::TriVar: {
        // Variable-skew triangle: rise completes at phase == shape.
        // shape 0.5 = symmetric; extremes approach (still band-unlimited)
        // saws — the classic "tri-to-saw" morph.
        const float riseK = 1.0f / _shape;
        const float fallK = 1.0f / (1.0f - _shape);
        for (size_t i = 0; i < n; ++i) {
            const float ph = JT_STEP();
            const float v = (ph < _shape) ? ph * riseK
                                          : (1.0f - ph) * fallK;
            out[i] = 2.0f * v - 1.0f;
            JT_SYNC_BLEP();
        }
        break;
    }

    case Wave::SampleHold:
        // step() refreshes _shValue on each wrap; between wraps we hold.
        for (size_t i = 0; i < n; ++i) {
            (void)JT_STEP();
            out[i] = _shValue;
            JT_SYNC_BLEP();
        }
        break;

    case Wave::Arb:
        if (_arbData == nullptr) {
            // v1 guard behaviour: no table selected yet -> naive saw.
            for (size_t i = 0; i < n; ++i) {
                out[i] = 2.0f * JT_STEP() - 1.0f;
                JT_SYNC_BLEP();
            }
            break;
        }
        {
            // AKWF (600-sample, 1 frame, 1 level, full band) through the
            // shared reader.  Descriptor built here: 7 stores per block.
            static constexpr uint16_t kZero[1] = { 0 };
            const uint16_t len[1] = { _arbLen };
            WaveTableSet ts;
            ts.data = _arbData;  ts.samplesPerFrame = _arbLen;
            ts.frames = 1;       ts.levels = 1;
            ts.levelOffset = kZero; ts.levelLen = len; ts.levelHarm = kZero;
            renderTable<FmMode, HasSyncIn, HasSyncOut, false>(out, n, fmBuf, fmOctaves,
                                                             syncIn, syncOut, ts);
        }
        break;

    case Wave::JpVarSaw: {
        // Measured JP SAW SHAPE — closed form (law + provenance at the top of
        // this file).  Per-BLOCK: β from the morph position (block-rate like
        // every other SHAPE consumer, so LFO1 Ctrl2 modulation works through
        // setShape exactly as before).  Per-SAMPLE: ~6 multiply-adds + one
        // polyBLEP — cheaper than the 2-frame table read it replaces, and no
        // flash data at all.
        const float k    = (_morph < kJpSawKMin) ? kJpSawKMin : _morph;
        const float beta = kJpSawBeta0 - kJpSawBetaK * k;
        const float cB3  = beta * kJpSawB3Scale * kJpSawGain;   // folded gains
        for (size_t i = 0; i < n; ++i) {
            const float ph  = JT_STEP();
            const float saw = 2.0f * ph - 1.0f - polyBlep(ph, _inc);
            const float b3  = ph * (ph - 0.5f) * (ph - 1.0f);    // B3(φ)
            out[i] = kJpSawGain * saw - cB3 * b3;
            JT_SYNC_BLEP();
        }
        break;
    }

    case Wave::JpTriMod: {
        // Measured JP TRIANGLE MOD — closed form (law at the top of this
        // file).  Per-BLOCK: gain and the four jump phases.  Per-SAMPLE:
        // triangle, one multiply, one conditional wrap, four polyBLEP tests
        // (each returns 0 after two compares unless within one sample of
        // its jump).
        float g = kJpTriModGMid - kJpTriModGSlope * (_morph - 0.5f);
        if (g < kJpTriModGMin) g = kJpTriModGMin;
        if (g > kJpTriModGMax) g = kJpTriModGMax;
        const float invG = 1.0f / g;
        const float pA = 0.25f * (1.0f - invG);     // rising,  −2
        const float pB = 0.25f * (1.0f + invG);     // rising,  −2
        const float pC = 0.25f * (3.0f - invG);     // falling, +2
        const float pD = 0.25f * (3.0f + invG);     // falling, +2
        for (size_t i = 0; i < n; ++i) {
            const float ph = JT_STEP();
            const float x  = ph + ph;                               // 0..2
            float v = g * (2.0f * ((x < 1.0f) ? x : 2.0f - x) - 1.0f);
            if (v >  1.0f) v -= 2.0f;                               // wrap: |v| < 3
            if (v < -1.0f) v += 2.0f;
            // Distance past each jump, wrapped to 0..1 (subtraction from the
            // same 'ph' — the BlSquare rule, so the comparator and the BLEP
            // can never disagree across the wrap).
            float dA = ph - pA; if (dA < 0.0f) dA += 1.0f;
            float dB = ph - pB; if (dB < 0.0f) dB += 1.0f;
            float dC = ph - pC; if (dC < 0.0f) dC += 1.0f;
            float dD = ph - pD; if (dD < 0.0f) dD += 1.0f;
            v -= polyBlep(dA, _inc) + polyBlep(dB, _inc);           // h = −2
            v += polyBlep(dC, _inc) + polyBlep(dD, _inc);           // h = +2
            out[i] = v;
            JT_SYNC_BLEP();
        }
        break;
    }

    case Wave::JpVarTri:
        if (_morphSet == nullptr) {
            // Defensive: no set attached (OscSection attaches one on setWave).
            for (size_t i = 0; i < n; ++i) {
                out[i] = 2.0f * JT_STEP() - 1.0f;
                JT_SYNC_BLEP();
            }
            break;
        }
        if (_morphSet->frames >= 2u)
            renderTable<FmMode, HasSyncIn, HasSyncOut, true >(out, n, fmBuf, fmOctaves,
                                                             syncIn, syncOut, *_morphSet);
        else
            renderTable<FmMode, HasSyncIn, HasSyncOut, false>(out, n, fmBuf, fmOctaves,
                                                             syncIn, syncOut, *_morphSet);
        break;

    case Wave::BlSaw:
        for (size_t i = 0; i < n; ++i) {
            const float ph = JT_STEP();
            out[i] = 2.0f * ph - 1.0f - polyBlep(ph, _inc);
            JT_SYNC_BLEP();
        }
        break;

    case Wave::BlSawRev:
        for (size_t i = 0; i < n; ++i) {
            const float ph = JT_STEP();
            out[i] = -(2.0f * ph - 1.0f - polyBlep(ph, _inc));
            JT_SYNC_BLEP();
        }
        break;

    case Wave::BlSquare:
        for (size_t i = 0; i < n; ++i) {
            const float ph = JT_STEP();
            float v = (ph < 0.5f) ? 1.0f : -1.0f;
            v += polyBlep(ph, _inc);                              // rising edge
            // Falling-edge distance derived by SUBTRACTION from the same
            // 'ph' the comparator used (exact for nearby floats — Sterbenz),
            // never by adding the complement: ph + 0.5 can round across the
            // wrap and disagree with the comparator, doubling the residual
            // into a +2 spike right at the edge (found by test, of course).
            float ph2 = ph - 0.5f;
            if (ph2 < 0.0f) ph2 += 1.0f;
            v -= polyBlep(ph2, _inc);
            out[i] = v;
            JT_SYNC_BLEP();
        }
        break;

    case Wave::BlPulse:
        for (size_t i = 0; i < n; ++i) {
            const float ph = JT_STEP();
            float v = (ph < _pw) ? 1.0f : -1.0f;
            v += polyBlep(ph, _inc);                              // rising edge
            float ph2 = ph - _pw;            // same rule as BlSquare above
            if (ph2 < 0.0f) ph2 += 1.0f;
            v -= polyBlep(ph2, _inc);
            out[i] = v;
            JT_SYNC_BLEP();
        }
        break;

    case Wave::VarSaw: {
        // JP-8000 saw SHAPE (naive).  out = (1-k)*saw(ph) + k*saw(2*ph), where
        // k = 1 - |2*shape - 1| runs 0 at the slider extremes and 1 at centre.
        // At the extremes it is a plain saw (full fundamental, "thick bass");
        // at centre the octave dominates and the fundamental cancels, giving
        // the thin/HPF-like tone the manual describes.  Peak stays +-1 for all
        // shape (verified), so JT_VSHAPE_NORMALISE is a no-op here — kept as a
        // compile hook so the A/B switch covers every morph uniformly.
        const float k = 1.0f - fabsf(2.0f * _shape - 1.0f);
        const float kBase = 1.0f - k;
        for (size_t i = 0; i < n; ++i) {
            const float ph  = JT_STEP();
            float oct = ph + ph;                    // 2*ph
            oct -= (float)(int)oct;                 // frac(2*ph)
            out[i] = kBase * (2.0f * ph - 1.0f) + k * (2.0f * oct - 1.0f);
#if !JT_VSHAPE_NORMALISE
            // (reserved) raw-level path — identical here since level is flat.
#endif
            JT_SYNC_BLEP();
        }
        break;
    }

    case Wave::BlVarSaw: {
        // Band-limited saw SHAPE: three saw edges per cycle — the base saw
        // wraps once (dt), the octave saw wraps twice (rate 2x, so its BLEP
        // uses 2*dt).  Same blend law as VarSaw.
        const float k = 1.0f - fabsf(2.0f * _shape - 1.0f);
        const float kBase = 1.0f - k;
        const float dt  = _inc;
        const float dt2 = _inc + _inc;              // octave runs twice as fast
        for (size_t i = 0; i < n; ++i) {
            const float ph = JT_STEP();
            float oct = ph + ph;
            oct -= (float)(int)oct;
            const float base = (2.0f * ph  - 1.0f) - polyBlep(ph,  dt);
            const float oc   = (2.0f * oct - 1.0f) - polyBlep(oct, dt2);
            out[i] = kBase * base + k * oc;
            JT_SYNC_BLEP();
        }
        break;
    }

    case Wave::VarTri: {
        // JP-8000 triangle SHAPE (naive): amplitude-gain the base triangle by
        // g = 1 + 3*shape (shape 0 -> g=1 plain, shape 1 -> g=4, ~four folds,
        // matching the manual's densest trace) then reflect-fold to +-1.  The
        // fold is bounded by construction, so peak stays +-1 (normalise no-op).
        const float g = 1.0f + 3.0f * _shape;
        for (size_t i = 0; i < n; ++i) {
            const float ph = JT_STEP();
            const float x  = ph + ph;               // 0..2
            const float t  = 2.0f * ((x < 1.0f) ? x : 2.0f - x) - 1.0f; // tri -1..1
            out[i] = reflectFold(g * t);
            JT_SYNC_BLEP();
        }
        break;
    }

    case Wave::BlVarTri: {
        // Band-limited triangle SHAPE: the fold injects many corners whose
        // positions move with shape, so per-corner BLEP is impractical; instead
        // we run the naive fold at 2x sample rate and average adjacent pairs (a
        // 2-tap box decimator).  This halves the alias energy of the folded
        // corners for two triangle evals per output sample — bounded, cheap,
        // and per your sign-off (oversample rather than per-corner BLEP).
        const float g   = 1.0f + 3.0f * _shape;
        const float hInc = _inc * 0.5f;             // half-step for 2x rate
        for (size_t i = 0; i < n; ++i) {
            // First sub-sample advances phase through step() as usual...
            const float ph1 = JT_STEP();
            const float x1  = ph1 + ph1;
            const float t1  = 2.0f * ((x1 < 1.0f) ? x1 : 2.0f - x1) - 1.0f;
            const float a   = reflectFold(g * t1);
            // ...the second sub-sample sits half an increment further on, taken
            // WITHOUT calling step() again (that would double the pitch); we
            // read the intermediate phase directly.  _phase already holds ph1.
            float ph2 = ph1 + hInc;
            if (ph2 >= 1.0f) ph2 -= 1.0f;
            const float x2  = ph2 + ph2;
            const float t2  = 2.0f * ((x2 < 1.0f) ? x2 : 2.0f - x2) - 1.0f;
            const float b   = reflectFold(g * t2);
            out[i] = 0.5f * (a + b);                // 2-tap box decimation
            JT_SYNC_BLEP();
        }
        break;
    }

    case Wave::Supersaw:
    case Wave::JpFeedback:
        // Owned by SupersawOsc / JpFeedbackOsc; OscSection never routes them
        // here.  Emit silence rather than trusting the caller — cheap insurance.
        for (size_t i = 0; i < n; ++i) out[i] = 0.0f;
        break;
    }

}

#undef JT_STEP
#undef JT_SYNC_BLEP

// -----------------------------------------------------------------------------
// Dispatch: pick the template instantiation for the features present, once
// per block.  FM mode × sync-in × sync-out = 12 instantiations; only the ones
// a build actually calls are emitted.
// -----------------------------------------------------------------------------
template <int FmMode>
void OscCore::renderSyncVariant(float* out, size_t n,
                                const float* fmBuf, float fmOctaves,
                                const float* syncIn, float* syncOut)
{
    const bool si = (syncIn != nullptr);
    const bool so = (syncOut != nullptr);
    if      (!si && !so) renderImpl<FmMode, false, false>(out, n, fmBuf, fmOctaves, syncIn, syncOut);
    else if ( si && !so) renderImpl<FmMode, true,  false>(out, n, fmBuf, fmOctaves, syncIn, syncOut);
    else if (!si &&  so) renderImpl<FmMode, false, true >(out, n, fmBuf, fmOctaves, syncIn, syncOut);
    else                 renderImpl<FmMode, true,  true >(out, n, fmBuf, fmOctaves, syncIn, syncOut);
}

void OscCore::render(float* out, size_t n,
                     const float* fmBuf, float fmOctaves,
                     const float* syncIn, float* syncOut,
                     bool fmIsRatio)
{
    if (fmBuf == nullptr)  renderSyncVariant<kFmNone >(out, n, fmBuf, fmOctaves, syncIn, syncOut);
    else if (!fmIsRatio)   renderSyncVariant<kFmExp2 >(out, n, fmBuf, fmOctaves, syncIn, syncOut);
    else                   renderSyncVariant<kFmRatio>(out, n, fmBuf, fmOctaves, syncIn, syncOut);

    // JpPwm pulse: AC-couple (subtract the width's mean, 2w-1).  Exact for the
    // block's width; PWM moves it per block with setShape.  Skipped entirely
    // (no loop) whenever there is nothing to remove — JtPwm, non-pulse waves,
    // a 50 % square.
    if (_dcOut != 0.0f)
        for (size_t i = 0; i < n; ++i) out[i] -= _dcOut;
}

} // namespace JT
