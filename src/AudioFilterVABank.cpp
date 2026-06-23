// =============================================================================
// AudioFilterVABank.cpp  –  Switchable VA Filter Bank implementation
// =============================================================================
//
// Reference: Vadim Zavalishin, "The Art of VA Filter Design" rev 2.1.2 (2018)
//
// ─── CPU budget guide (Teensy 4.1, 44100 Hz, 128-sample blocks) ─────────────
//   Approx cycles per sample (rough; varies with cache/pipeline):
//     TPT 1-pole          :  ~15 cycles
//     SVF 2-pole          :  ~30 cycles
//     Moog LP4 (closed)   :  ~50 cycles
//     Diode LP4 (Pirkle)  :  ~55 cycles  (block-rate setCoeffs, cheap tick)
//     Korg35 LP           :  ~45 cycles  (1 division + 1 sigmoid in ZDF solve)
//   tanf() coefficient    :  ~20 cycles  (computed ONCE per block ≈ 0.15/sample)
//   Norm-API powf         :  ~25 cycles  (control rate, ~once per CC)
//   SlewedValue::tickBlock:  ~5 cycles   (1 mul + settled check, free when idle)
// =============================================================================

#include "AudioFilterVABank.h"
#include "FilterShape.h"
#include "JT8000_OptFlags.h"

// ---------------------------------------------------------------------------
// fast_pow2 for VA bank cutoff modulation — same polynomial as OBXa version.
// Only compiled when JT_OPT_VA_BLOCKRATE_MOD is enabled.
// ---------------------------------------------------------------------------
#if JT_OPT_VA_BLOCKRATE_MOD
static inline float va_fast_pow2(float x)
{
    if (x >  126.0f) return 67108864.0f;
    if (x < -126.0f) return 1.49e-38f;
    const int32_t xi = (int32_t)x;
    const float   xf = x - (float)xi;
    const float poly = 1.00000000f
                     + xf * (0.69314718f
                     + xf * (0.24022651f
                     + xf * (0.05550411f
                     + xf *  0.00961823f)));
    return ldexpf(poly, xi);
}
#endif // JT_OPT_VA_BLOCKRATE_MOD

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
AudioFilterVABank::AudioFilterVABank()
    : AudioStream(3, _inQ)
{
    // Configure the parameter slews to the audio engine's rate/block size and
    // the per-class default time constants. Initial current values match the
    // raw defaults (cutoff norm 0.5, res 0.0, drive 1.0 → norm 0.0). Callers
    // invoke snap() after setting boot defaults to avoid an audible glide.
    _cutoffSlew.setSampleRate(AUDIO_SAMPLE_RATE_EXACT);
    _cutoffSlew.setBlockSize (AUDIO_BLOCK_SAMPLES);
    _cutoffSlew.setTimeMs    (kCutoffSlewMs);
    _cutoffSlew.reset(0.5f);

    _resSlew.setSampleRate(AUDIO_SAMPLE_RATE_EXACT);
    _resSlew.setBlockSize (AUDIO_BLOCK_SAMPLES);
    _resSlew.setTimeMs    (kResSlewMs);
    _resSlew.reset(0.0f);

    // Drive slew works in MULTIPLIER space (×1..4), not 0..1 — reset to unity.
    _driveSlew.setSampleRate(AUDIO_SAMPLE_RATE_EXACT);
    _driveSlew.setBlockSize (AUDIO_BLOCK_SAMPLES);
    _driveSlew.setTimeMs    (kDriveSlewMs);
    _driveSlew.reset(1.0f);
}

// ---------------------------------------------------------------------------
// setFilterType  –  switch topology and reset DSP state to avoid pops/clicks.
//
// The slew TARGETS are untouched (knob position = user intent, preserved across
// a topology change). _kTarget is re-mapped for the new topology, and if the
// norm API is in use the new filter's FilterShape row is pre-expanded so
// external getters see correct Hz/k before the next audio block runs.
// ---------------------------------------------------------------------------
void AudioFilterVABank::setFilterType(VAFilterType type)
{
    if (type >= FILTER_COUNT) type = FILTER_SVF_LP;
    if (type == _type) return;   // skip work if type unchanged
    _type = type;

    if (_normPrimed)
    {
        // Re-expand under the new filter's row (also refreshes _kTarget).
        applyShape(_cutoffSlew.current(), _resSlew.current());
    }
    else
    {
        // Raw-API path: re-map resonance for the new topology (same 0..1 input,
        // different k/R scale).
        _kTarget = mapResonance(_res01, _type);
    }

    reset();   // glitch-free: clear DSP state so the new topology starts clean
}

// ---------------------------------------------------------------------------
// frequency  –  RAW setter: cutoff in Hz, clamped to [5, 0.45*fs].
// Bypasses the slew — used by env / key-track / audio-rate code paths that
// already produce continuous trajectories. Does NOT touch _cutoffSlew.
// ---------------------------------------------------------------------------
void AudioFilterVABank::frequency(float hz)
{
    const float maxHz = 0.45f * AUDIO_SAMPLE_RATE_EXACT;
    _fcTarget = va_clamp(hz, 5.0f, maxHz);
}

// ---------------------------------------------------------------------------
// resonance  –  RAW setter: 0..1 fed directly into mapResonance() (no γ, no
// slew). For CC handlers / UI prefer setResonanceNorm().
// ---------------------------------------------------------------------------
void AudioFilterVABank::resonance(float r)
{
    _res01   = va_clamp(r, 0.0f, 1.0f);
    _kTarget = mapResonance(_res01, _type);
}

// ---------------------------------------------------------------------------
// setCutoffNorm  –  NORM setter: writes the slew target. The actual Hz value
// is computed in update() from the slew's smoothed current value, so the user
// hears a glide rather than a step. One float write here; no powf, no DSP.
// ---------------------------------------------------------------------------
void AudioFilterVABank::setCutoffNorm(float c01)
{
    _cutoffSlew.setTarget(va_clamp(c01, 0.0f, 1.0f));
    _normPrimed = true;
}

// ---------------------------------------------------------------------------
// setResonanceNorm  –  NORM setter: writes the slew target. Same shape rules
// as setCutoffNorm but driven through the resonance γ + mapResonance pipeline
// each block.
// ---------------------------------------------------------------------------
void AudioFilterVABank::setResonanceNorm(float r01)
{
    _resSlew.setTarget(va_clamp(r01, 0.0f, 1.0f));
    _normPrimed = true;
}

// ---------------------------------------------------------------------------
// setDriveNorm  –  NORM setter: 0..1 knob → ×1..4 multiplier, block-rate
// slewed so CC drive sweeps are click-free. The slew lives in multiplier space
// so update() reads the smoothed ×-value straight into the loop.
// ---------------------------------------------------------------------------
void AudioFilterVABank::setDriveNorm(float d01)
{
    const float mult = 1.0f + va_clamp(d01, 0.0f, 1.0f) * 3.0f;   // 0..1 → ×1..4
    _driveSlew.setTarget(mult);
}

// ---------------------------------------------------------------------------
// snap  –  settle all slews to their current targets. Call at boot / patch
// change so the configured starting values do not glide in audibly.
// ---------------------------------------------------------------------------
void AudioFilterVABank::snap()
{
    _cutoffSlew.reset(_cutoffSlew.target());
    _resSlew.reset(_resSlew.target());
    _driveSlew.reset(_driveSlew.target());
    _drive = _driveSlew.current();
    if (_normPrimed) applyShape(_cutoffSlew.current(), _resSlew.current());
}

// ---------------------------------------------------------------------------
// applyShape  –  expand normalised (cn, rn) under the active filter's row.
// Centralised so update() and setFilterType()/snap() can't drift.
// ---------------------------------------------------------------------------
void AudioFilterVABank::applyShape(float cn, float rn)
{
    const FilterShape& s = kFilterShape[_type];
    // Cutoff: exponential (musical) curve from per-filter [fcMin, fcMax].
    _fcTarget = s.fcMinHz * powf(s.fcMaxHz / s.fcMinHz, cn);
    // Resonance: per-filter γ lift, then existing mapResonance() → k/R.
    const float rShaped = powf(rn, s.resGamma);
    _res01    = rShaped;
    _kTarget  = mapResonance(rShaped, _type);
}

// ---------------------------------------------------------------------------
// reset  –  clear all filter states
// ---------------------------------------------------------------------------
void AudioFilterVABank::reset()
{
    _tpt1.reset();
    _svf.reset();
    _moog.reset();
    _diode.reset();
    _k35lp.reset();
    _k35hp.reset();
}

// ---------------------------------------------------------------------------
// mapResonance  –  map normalised [0..1] to topology-specific k or R
//
// Each topology has a different self-oscillation threshold and resonance
// character. The mapping keeps res01 ≈ 0.95 just below self-osc for all types,
// giving consistent knob feel across topologies.
//
// Reference thresholds (Zavalishin):
//   SVF:    R = 0 is self-osc (R = 1/(2Q), damping)  §4.1 p.95
//   Moog:   k = 4 is self-osc                        §5.1 p.134
//   Diode:  k = 17 is self-osc                       §5.10 p.170
//   Korg35: k = 2 is self-osc                        §5.8  p.151
// ---------------------------------------------------------------------------
float AudioFilterVABank::mapResonance(float r, VAFilterType type) const
{
    switch (type)
    {
        // SVF: R = 1/(2Q).  r=0 → R=1.0 (Butterworth, no peak),
        //                   r=1 → R=0.01 (near self-osc).
        // R controls damping, so higher r means LESS damping (more resonance).
        case FILTER_SVF_LP:
        case FILTER_SVF_HP:
        case FILTER_SVF_BP:
        case FILTER_SVF_NOTCH:
        case FILTER_SVF_AP:
            return 1.0f - r * 0.99f;

        // Moog LP4: k=0..4; self-oscillation at k=4 (Zavalishin §5.1 p.134).
        // r=1 → k=3.95 (just below self-osc).
        case FILTER_MOOG_LP4:
        case FILTER_MOOG_LP2:
        case FILTER_MOOG_BP2:
            return r * 3.95f;

        // Diode ladder: self-osc at k=17 (Zavalishin §5.10 p.170).
        // r=1 → k=16.5 (just below self-osc).
        case FILTER_DIODE_LP:
            return r * 16.5f;

        // Korg 35 (TSK): self-osc at k=2 (Zavalishin §5.8 p.151).
        // r=1 → k=1.95 (just below self-osc). The bounded sigmoid in the
        // feedback path (VA_NL_SAT) keeps it stable past k=2 with graceful
        // compression rather than divergence.
        case FILTER_KORG35_LP:
        case FILTER_KORG35_HP:
            return r * 1.95f;

        // 1-pole has no resonance.
        case FILTER_TPT1_LP:
        case FILTER_TPT1_HP:
        default:
            return 0.0f;
    }
}

// ---------------------------------------------------------------------------
// update  –  Teensy audio callback (called every AUDIO_BLOCK_SAMPLES samples)
//
// CPU optimisation strategy:
//  1. Slew tick is block-rate (1 mul/block) — settled fast path costs nothing.
//  2. g (integrator gain via tanf) computed ONCE per block at control rate.
//  3. Mod buses checked for nullptr before use — avoids work when unconnected.
//  4. Key tracking and envelope shift computed at block rate, not sample rate.
//  5. The topology switch is hoisted OUT of the sample loop (taken once/block).
//  6. Drive bypass is a branch predicted as "not taken" in the common case.
// ---------------------------------------------------------------------------
__attribute__((optimize("O3")))
void AudioFilterVABank::update(void)
{
    audio_block_t *in0 = receiveReadOnly(0);  // audio input

    // No audio input — nothing to filter, skip all DSP.
    if (!in0) return;

#if JT_OPT_FILTER_ENGINE_SKIP
    // Inactive engine: drain every input so upstream buffers are freed (no pool
    // leak), then return before any coefficient or sample-loop work.
    if (!_active) {
        release(in0);
        audio_block_t *m1 = receiveReadOnly(1);
        audio_block_t *m2 = receiveReadOnly(2);
        if (m1) release(m1);
        if (m2) release(m2);
        return;
    }
#endif

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

    // ── Slew advance (block-rate) + shape expansion ──────────────────────────
    // Done first so _fcTarget/_kTarget reflect the smoothed user knob before
    // key tracking / env modulation multiply on top. Raw setters bypass this
    // and write _fcTarget directly — _normPrimed gates whether to overwrite.
    if (_normPrimed)
    {
        const float cn = _cutoffSlew.tickBlock();
        const float rn = _resSlew.tickBlock();
        applyShape(cn, rn);
    }

    // Drive smoother (multiplier space). Always ticked — cheap when settled.
    _drive = _driveSlew.tickBlock();

    // ── Block-rate coefficient computation (ONCE per 128 samples) ────────────

    // Key tracking: shift cutoff by octaves from C4 (MIDI note 60).
    // keyTrack=1.0 means full 1v/oct tracking.
    const float keyOct  = (_midiNote - 60.0f) / 12.0f;
    const float keyMul  = powf(2.0f, _keyTrack * keyOct);   // block-rate only

    // Envelope shift (also block-rate — control envelope, not audio-rate mod).
    const float envShiftOct = _envValue * _envModOct;
    const float fcBlock     = _fcTarget * keyMul * powf(2.0f, envShiftOct);

    const bool hasCutoffMod = (in1 != nullptr) && (_cutoffModOct > 0.0f);
    const bool hasResMod    = (in2 != nullptr) && (_resModDepth  > 0.0f);

#if JT_OPT_VA_BLOCKRATE_MOD
    // =========================================================================
    // OPTIMISED PATH — block-rate cutoff modulation.
    // When a mod bus is wired, take the mid-block sample (index 64) as a
    // representative value and fold it into fcBlock once. The resulting g_block
    // is then used for the entire 128-sample inner loop.
    // =========================================================================
    float blockModOct = 0.0f;
    if (hasCutoffMod) {
        const float midSample = (float)in1->data[64] * (1.0f / 32768.0f);
        blockModOct = midSample * _cutoffModOct;
    }
    const float fcBlockModded  = fcBlock * va_fast_pow2(blockModOct);
    const float fcBlockClamped = va_clamp(fcBlockModded, 5.0f, 0.45f * AUDIO_SAMPLE_RATE_EXACT);
    const float g_block = va_compute_g(fcBlockClamped, AUDIO_SAMPLE_RATE_EXACT);
#else
    // Reference path: g_block without mod (mod applied per-sample in loop).
    const float fcBlockClamped = va_clamp(fcBlock, 5.0f, 0.45f * AUDIO_SAMPLE_RATE_EXACT);
    const float g_block = va_compute_g(fcBlockClamped, AUDIO_SAMPLE_RATE_EXACT);
#endif // JT_OPT_VA_BLOCKRATE_MOD

    const float k_block = _kTarget;   // already mapped in resonance()/applyShape()

    // ── Block-rate resonance modulation → k_eff ──────────────────────────────
    // The audio-rate res-mod bus shifts the SMOOTHED user knob position, then
    // γ + mapResonance run on top. Uses the mid-block sample so mod sits on the
    // same base the static path uses. Folded to a block constant here so the
    // per-sample loop sees a fixed k (cheaper, and SVF uses it as R = 1/(2Q)).
    float k_eff = k_block;
    if (hasResMod)
    {
        const float midRes   = (float)in2->data[64] * (1.0f / 32768.0f);
        // Base = the (post-γ) res value the static path is using this block.
        const float baseNorm = _normPrimed ? _resSlew.current() : _res01;
        const float gamma    = _normPrimed ? kFilterShape[_type].resGamma : 1.0f;
        const float resMod   = va_clamp(baseNorm + midRes * _resModDepth, 0.0f, 1.0f);
        const float rShaped  = (gamma == 1.0f) ? resMod : powf(resMod, gamma);
        k_eff = mapResonance(rShaped, _type);
    }

    // Pre-check drive (avoid per-sample branch when drive is unity).
    const bool hasDrive = (_drive != 1.0f);

    // ── Hoisted-dispatch sample loop ─────────────────────────────────────────
    // The topology switch is taken ONCE per block, not once per sample. Each
    // case runs a tight 128-sample inner loop over a single filter struct, so
    // the compiler keeps filter state in registers across the block and the
    // branch predictor never sees the switch inside the hot loop.
    //
    // Per-sample work common to all topologies (input scale, drive, reference-
    // path per-sample cutoff mod, output saturation, clip, store) is factored
    // into the runBlock lambda, which takes a callable evalFilter returning the
    // filtered sample for a given (x, g). With block-rate mod enabled, g is
    // constant and the per-sample mod branch compiles out.
    const float kIn  = 1.0f / 32768.0f;
    const float kOut = 32767.0f;

    auto runBlock = [&](auto evalFilter)
    {
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i)
        {
            float x = (float)in0->data[i] * kIn;
            if (hasDrive) x *= _drive;

            float g = g_block;
            if (hasCutoffMod)
            {
#if JT_OPT_VA_BLOCKRATE_MOD
                (void)0;   // g_block already includes block-rate mod
#else
                const float modSample = (float)in1->data[i] * kIn;
                const float fcInst    = fcBlock * powf(2.0f, modSample * _cutoffModOct);
                const float fcClamped = va_clamp(fcInst, 5.0f, 0.45f * AUDIO_SAMPLE_RATE_EXACT);
                g = va_compute_g(fcClamped, AUDIO_SAMPLE_RATE_EXACT);
#endif
            }

            float y = evalFilter(x, g);

            // Optional cheap OUTPUT coloration (SAT_NONE => passthrough).
            y = saturate(y);
            y = va_clamp(y, -1.0f, 1.0f);

            // Round (not truncate) to int16 — removes truncation DC bias.
            out->data[i] = (int16_t)lroundf(y * kOut);
        }
    };

    // k is block-constant here (audio-rate res-mod folded to k_eff at block
    // rate above). SVF uses k as R = 1/(2Q); others use k as feedback amount.
    const float kf = k_eff;

    switch (_type)
    {
        // ── SVF variants (Zavalishin §4.1 p.95) ──────────────────────────────
        case FILTER_SVF_LP:
            runBlock([&](float x, float g){ _svf.process(x, g, kf); return _svf.lp; });
            break;
        case FILTER_SVF_HP:
            runBlock([&](float x, float g){ _svf.process(x, g, kf); return _svf.hp; });
            break;
        case FILTER_SVF_BP:
            runBlock([&](float x, float g){ _svf.process(x, g, kf); return _svf.bp; });
            break;
        case FILTER_SVF_NOTCH:
            runBlock([&](float x, float g){ _svf.process(x, g, kf); return _svf.notch; });
            break;
        case FILTER_SVF_AP:
            runBlock([&](float x, float g){ _svf.process(x, g, kf); return _svf.allpass(kf); });
            break;

        // ── Moog ladder (Zavalishin §5.1 p.133) ──────────────────────────────
        case FILTER_MOOG_LP4:
            runBlock([&](float x, float g){ _moog.process(x, g, kf); return _moog.y4; });
            break;
        case FILTER_MOOG_LP2:
            runBlock([&](float x, float g){ _moog.process(x, g, kf); return _moog.y2; });
            break;
        case FILTER_MOOG_BP2:
            // BP from pole subtraction (Zavalishin §5.1 p.135).
            runBlock([&](float x, float g){ _moog.process(x, g, kf); return _moog.y2 - _moog.y4; });
            break;

        // ── Diode ladder (Pirkle AN-6) ───────────────────────────────────────
        // process() returns the passband-compensated LP directly. Coefficients
        // depend only on g and K; when cutoff is block-rate (the common path)
        // set them once per block and run the cheap tick() per sample.
        case FILTER_DIODE_LP:
#if JT_OPT_VA_BLOCKRATE_MOD
            _diode.setCoeffs(g_block, kf);
            runBlock([&](float x, float){ return _diode.tick(x, kf); });
#else
            runBlock([&](float x, float g){ return _diode.process(x, g, kf); });
#endif
            break;

        // ── Korg 35 / TSK (Zavalishin §5.8 p.151) ────────────────────────────
        // VA_NL_SAT: the TSK positive-feedback loop is UNBOUNDED with linear
        // feedback and diverges at high resonance/cutoff. The bounded sigmoid
        // keeps it stable across the whole fc/res range and gives the intended
        // graceful self-oscillation compression past k≈2.
        case FILTER_KORG35_LP:
            runBlock([&](float x, float g){ return _k35lp.process(x, g, kf, VA_NL_SAT); });
            break;
        case FILTER_KORG35_HP:
            runBlock([&](float x, float g){ return _k35hp.process(x, g, kf, VA_NL_SAT); });
            break;

        // ── Simple 1-pole (Zavalishin §3.1 p.45) — always linear ─────────────
        case FILTER_TPT1_LP:
            runBlock([&](float x, float g){ return _tpt1.processLP(x, g); });
            break;
        case FILTER_TPT1_HP:
            runBlock([&](float x, float g){ float lp; return _tpt1.processHP(x, g, lp); });
            break;

        default:
            runBlock([&](float x, float){ return x; });   // passthrough
            break;
    }

    // ── Transmit and release ─────────────────────────────────────────────────
    transmit(out);
    release(out);
    release(in0);
    if (in1) release(in1);
    if (in2) release(in2);
}
