// =============================================================================
// AudioFilterVABank.cpp  –  Switchable VA Filter Bank implementation
// =============================================================================
//
// Reference: Vadim Zavalishin, "The Art of VA Filter Design" rev 2.1.2 (2018)
//
// ─── CPU budget guide (Teensy 4.1 @ 600 MHz, 44100 Hz, 128-sample blocks) ──
//   Approx cycles per sample (rough; varies with cache/pipeline):
//     TPT 1-pole          :  ~15 cycles
//     SVF 2-pole          :  ~30 cycles
//     Moog LP4 (closed)   :  ~50 cycles
//     Diode LP4 (nested)  :  ~65 cycles  (3 divisions for nested solve)
//     Korg35 LP           :  ~45 cycles  (1 division + 1 tanh for ZDF solve)
//   tanf() coefficient    :  ~20 cycles  (computed ONCE per block = ~0.15/sample)
// =============================================================================

#include "AudioFilterVABank.h"
#include "JT8000_OptFlags.h"

// ---------------------------------------------------------------------------
// TEMPORARY FILTER-INPUT PROBE — ISR-safe capture (remove after diagnosis)
//
// update() runs in the audio ISR, where Serial.printf() cannot be used. This
// struct lets the ISR record the values feeding the filter into plain memory;
// loop() reads and prints them in the main context (where serial works).
//
// It keeps two things per window:
//   • the most recent live values (a "stream" snapshot), and
//   • the worst-magnitude fcBlock/g seen, plus a non-finite flag, so a brief
//     glitch between prints is not missed.
// Enable with JT_FILTER_INPUT_PROBE in JT8000_OptFlags.h.
// ---------------------------------------------------------------------------
#if JT_FILTER_INPUT_PROBE
// struct JtFiltProbe is declared in AudioFilterVABank.h (single definition).
// Here we define its methods and the single global instance.
void JtFiltProbe::capture(int t, float x0_, float fcT, float km, float env,
                          float fcB, float fcC, float g_, float k_) {
    type = t; x0 = x0_; fcTarget = fcT; keyMul = km; envShiftOct = env;
    fcBlock = fcB; fcClamped = fcC; g = g_; k = k_;
    blocks++;
    const float afc = fcB < 0 ? -fcB : fcB;
    if (afc > worstFcBlock) worstFcBlock = afc;
    const float ag = g_ < 0 ? -g_ : g_;
    if (ag > worstG) worstG = ag;
    if (!(fcB == fcB) || !(g_ == g_) || !(x0_ == x0_) ||
        fcB > 1e30f || fcB < -1e30f || g_ > 1e30f) sawNonFinite = true;
}
void JtFiltProbe::resetWindow() {
    worstFcBlock = 0; worstG = 0; sawNonFinite = false; blocks = 0;
}
JtFiltProbe jt_filtProbe;   // single global instance (defined here)
#endif

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
    // Filter states initialised by default constructors in VAFilterCore.h
}

// ---------------------------------------------------------------------------
// setFilterType  –  switch topology and reset state to avoid pops/clicks
// ---------------------------------------------------------------------------
void AudioFilterVABank::setFilterType(VAFilterType type)
{
    if (type >= FILTER_COUNT) type = FILTER_SVF_LP;
    if (type == _type) return;   // skip reset if type unchanged
    _type = type;
    // Re-map resonance for the new topology (same 0..1 input, different k/R)
    _kTarget = mapResonance(_res01, _type);
    reset();
}

// ---------------------------------------------------------------------------
// frequency  –  set cutoff in Hz
// Clamped to [5, 0.45*fs] for stability across all topologies.
// ---------------------------------------------------------------------------
void AudioFilterVABank::frequency(float hz)
{
    const float maxHz = 0.45f * AUDIO_SAMPLE_RATE_EXACT;
    _fcTarget = va_clamp(hz, 5.0f, maxHz);
}

// ---------------------------------------------------------------------------
// resonance  –  normalised 0..1 input mapped to topology-specific k/R
// ---------------------------------------------------------------------------
void AudioFilterVABank::resonance(float r)
{
    _res01   = va_clamp(r, 0.0f, 1.0f);
    _kTarget = mapResonance(_res01, _type);
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
    _jp.reset();
}

// ---------------------------------------------------------------------------
// mapResonance  –  map normalised [0..1] to topology-specific k or R
//
// Each topology has a different self-oscillation threshold and resonance
// character.  The mapping ensures res01 ≈ 0.95 is just below self-osc
// for all types, giving consistent knob feel across topologies.
//
// Reference thresholds (Zavalishin):
//   SVF:   R = 0 is self-osc (R = 1/(2Q), damping)  §4.1 p.95
//   Moog:  k = 4 is self-osc                        §5.1 p.134
//   Diode: k = 17 is self-osc                       §5.10 p.170
//   Korg35: k = 2 is self-osc                       §5.8  p.151
// ---------------------------------------------------------------------------
float AudioFilterVABank::mapResonance(float r, VAFilterType type) const
{
    switch (type)
    {
        // SVF: R = 1/(2Q).  r=0 → R=1.0 (Butterworth, no peak).
        //                   r=1 → R=0.01 (near self-osc).
        // R controls damping, so higher r means LESS damping (more resonance).
        case FILTER_SVF_LP:
        case FILTER_SVF_HP:
        case FILTER_SVF_BP:
        case FILTER_SVF_NOTCH:
        case FILTER_SVF_AP:
            return 1.0f - r * 0.99f;

        // Moog LP4: k=0..4; self-oscillation at k=4 (Zavalishin §5.1 p.134)
        // r=1 → k=3.95 (just below self-osc)
        case FILTER_MOOG_LP4:
        case FILTER_MOOG_LP2:
        case FILTER_MOOG_BP2:
            return r * 3.95f;

        // Diode ladder: self-osc at k=17 (Zavalishin §5.10 p.170)
        // r=1 → k=16.5 (just below self-osc)
        case FILTER_DIODE_LP:
            return r * 16.5f;

        // Korg 35 (TSK): self-osc at k=2 (Zavalishin §5.8 p.151)
        // r=1 → k=1.95 (just below self-osc).
        // The tanh saturation in the feedback path allows going slightly
        // past k=2 without instability, but the resonance character changes.
        case FILTER_KORG35_LP:
        case FILTER_KORG35_HP:
            return r * 1.95f;

        // JP / NLLadderNB ladder: self-oscillation onset verified at k=4.0.
        // r=1 -> k=4.0 reaches self-osc; the Newton-Bisection core survives it
        // stably (saturates rather than diverging), so we map to the threshold
        // itself rather than just below it as the old IR3109 model required.
        case FILTER_JP_LP24:
        case FILTER_JP_LP12:
        case FILTER_JP_HP24:
        case FILTER_JP_BP:
            return r * 4.0f;

        // 1-pole has no resonance
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
//  1. g (integrator gain via tanf) computed ONCE per block at control rate.
//  2. Mod buses checked for nullptr before use — avoids work when unconnected.
//  3. Key tracking and envelope shift computed at block rate, not sample rate.
//  4. All inner-loop filter calls are inlined from VAFilterCore.h.
//  5. Drive bypass is a branch predicted as "not taken" in the common case.
// ---------------------------------------------------------------------------
__attribute__((optimize("O3")))
void AudioFilterVABank::update(void)
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

    // ── Block-rate coefficient computation (ONCE per 128 samples) ────────────

    // Key tracking: shift cutoff by octaves from C4 (MIDI note 60).
    // keyTrack=1.0 means full 1v/oct tracking.
    const float keyOct  = (_midiNote - 60.0f) / 12.0f;
    const float keyMul  = powf(2.0f, _keyTrack * keyOct);   // block-rate only

    // Envelope shift (also block-rate — control envelope, not audio-rate mod)
    const float envShiftOct = _envValue * _envModOct;
    const float fcBlock     = _fcTarget * keyMul * powf(2.0f, envShiftOct);

    // Pre-compute integrator gain g from block-rate cutoff.
    // tanf() is ~20 cycles — amortised over 128 samples here.
    const bool hasCutoffMod = (in1 != nullptr) && (_cutoffModOct > 0.0f);
    const bool hasResMod    = (in2 != nullptr) && (_resModDepth  > 0.0f);

#if JT_OPT_VA_BLOCKRATE_MOD
    // =========================================================================
    // OPTIMISED PATH — block-rate cutoff modulation
    //
    // When a mod bus is wired, take the mid-block sample (index 64) as a
    // representative value and fold it into fcBlock once.  The resulting
    // g_block is then used for the entire 128-sample inner loop.
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
    // Reference path: g_block without mod (mod applied per-sample in loop)
    const float fcBlockClamped = va_clamp(fcBlock, 5.0f, 0.45f * AUDIO_SAMPLE_RATE_EXACT);
    const float g_block = va_compute_g(fcBlockClamped, AUDIO_SAMPLE_RATE_EXACT);
#endif // JT_OPT_VA_BLOCKRATE_MOD

    const float k_block = _kTarget;   // already mapped in resonance()

    // ── TEMPORARY FILTER-INPUT PROBE (remove once cause is found) ────────────
    // CRITICAL DESIGN NOTE: update() runs in the AUDIO ISR. Serial.printf() must
    // NEVER be called from here — USB serial TX depends on interrupts that are
    // blocked inside the ISR, so a print either deadlocks or is silently dropped.
    // (That is why every in-ISR probe so far has logged nothing.)
    //
    // Instead this records the per-block values into a plain global. loop() (main
    // context, where serial works) drains and prints it. It captures the WORST
    // values seen each window — finite or not — so we see the actual ranges the
    // filter is being fed, not just threshold trips.
    //
    // Enable with  #define JT_FILTER_INPUT_PROBE 1  in JT8000_OptFlags.h
    // (NOT the .ino — a .ino define does not reach this .cpp).
#if JT_FILTER_INPUT_PROBE
    {
        const float x0 = (in0 != nullptr)
                         ? (float)in0->data[0] * (1.0f / 32768.0f) : 0.0f;
        jt_filtProbe.capture(_type, x0, _fcTarget, keyMul, envShiftOct,
                             fcBlock, fcBlockClamped, g_block, k_block);
    }
#endif
    // ── END FILTER-INPUT PROBE ───────────────────────────────────────────────

    // Note: in-loop per-stage saturation was evaluated and reverted (cost ~20%
    // CPU for little sonic gain at 8-voice polyphony). Filters run linear here;
    // _satType still drives the cheap OUTPUT coloration stage in saturate().

    // Block-rate resonance modulation (consistent with block-rate cutoff mod).
    // Smooth control signal => one representative mid-block sample is enough,
    // and it avoids calling mapResonance() (a switch) per sample.
    float k_eff = k_block;
    if (hasResMod)
    {
        const float midRes = (float)in2->data[64] * (1.0f / 32768.0f);
        const float res01mod = va_clamp(_res01 + midRes * _resModDepth, 0.0f, 1.0f);
        k_eff = mapResonance(res01mod, _type);
    }

    // Pre-check drive (avoid per-sample branch when drive is unity)
    const bool hasDrive = (_drive != 1.0f);

    // ── Hoisted-dispatch sample loop ─────────────────────────────────────────
    // The topology switch is taken ONCE per block, not once per sample. Each
    // case runs a tight 128-sample inner loop over a single filter struct, so
    // the compiler keeps filter state in registers across the block and the
    // branch predictor never sees the switch inside the hot loop.
    //
    // Per-sample work that is common to all topologies (input scale, drive,
    // reference-path per-sample cutoff mod, output saturation, clip, store) is
    // factored into the `runBlock` lambda, which takes a callable `evalFilter`
    // returning the filtered sample for a given (x, g). With block-rate mod
    // enabled, g is constant and the per-sample mod branch compiles out.
    const float kIn = 1.0f / 32768.0f;
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

        // ── Moog ladder (Zavalishin §5.1 p.133; §5.3 p.139 nonlinear) ─────────
        case FILTER_MOOG_LP4:
            runBlock([&](float x, float g){ _moog.process(x, g, kf); return _moog.y4; });
            break;
        case FILTER_MOOG_LP2:
            runBlock([&](float x, float g){ _moog.process(x, g, kf); return _moog.y2; });
            break;
        case FILTER_MOOG_BP2:
            // BP from pole subtraction (Zavalishin §5.1 p.135)
            runBlock([&](float x, float g){ _moog.process(x, g, kf); return _moog.y2 - _moog.y4; });
            break;

        // ── Diode ladder (Pirkle AN-6) ───────────────────────────────────────
        // process() returns the passband-compensated LP directly. Coefficients
        // depend only on g and K; when cutoff is block-rate (the common path)
        // we set them once per block and run the cheap tick() per sample.
        case FILTER_DIODE_LP:
#if JT_OPT_VA_BLOCKRATE_MOD
            _diode.setCoeffs(g_block, kf);
            runBlock([&](float x, float){ return _diode.tick(x, kf); });
#else
            runBlock([&](float x, float g){ return _diode.process(x, g, kf); });
#endif
            break;

        // ── JP / NLLadderNB nonlinear ladder (Roland Jupiter / JP-8000) ──────
        // One core, four modes. Newton-Bisection implicit solve, stable by
        // construction (saturates when driven; cannot NaN). setCutoff() per
        // block warps g at the internal rate (2x when oversampling is on);
        // tick per sample. _jpQComp is the Jupiter "stays loud" Q compensation.
        case FILTER_JP_LP24:
#if JT_OPT_VA_BLOCKRATE_MOD
            _jp.setCutoff(fcBlockClamped, AUDIO_SAMPLE_RATE_EXACT);
            runBlock([&](float x, float){ _jp.tick(x, kf, _jpQComp); return _jp.lp4; });
#else
            runBlock([&](float x, float g){ (void)g; _jp.setCutoff(fcBlockClamped, AUDIO_SAMPLE_RATE_EXACT); _jp.tick(x, kf, _jpQComp); return _jp.lp4; });
#endif
            break;
        case FILTER_JP_LP12:
#if JT_OPT_VA_BLOCKRATE_MOD
            _jp.setCutoff(fcBlockClamped, AUDIO_SAMPLE_RATE_EXACT);
            runBlock([&](float x, float){ _jp.tick(x, kf, _jpQComp); return _jp.lp2; });
#else
            runBlock([&](float x, float g){ (void)g; _jp.setCutoff(fcBlockClamped, AUDIO_SAMPLE_RATE_EXACT); _jp.tick(x, kf, _jpQComp); return _jp.lp2; });
#endif
            break;
        case FILTER_JP_HP24:
#if JT_OPT_VA_BLOCKRATE_MOD
            _jp.setCutoff(fcBlockClamped, AUDIO_SAMPLE_RATE_EXACT);
            runBlock([&](float x, float){ _jp.tick(x, kf, _jpQComp); return _jp.hp4; });
#else
            runBlock([&](float x, float g){ (void)g; _jp.setCutoff(fcBlockClamped, AUDIO_SAMPLE_RATE_EXACT); _jp.tick(x, kf, _jpQComp); return _jp.hp4; });
#endif
            break;
        case FILTER_JP_BP:
#if JT_OPT_VA_BLOCKRATE_MOD
            _jp.setCutoff(fcBlockClamped, AUDIO_SAMPLE_RATE_EXACT);
            runBlock([&](float x, float){ _jp.tick(x, kf, _jpQComp); return _jp.bp; });
#else
            runBlock([&](float x, float g){ (void)g; _jp.setCutoff(fcBlockClamped, AUDIO_SAMPLE_RATE_EXACT); _jp.tick(x, kf, _jpQComp); return _jp.bp; });
#endif
            break;

        // ── Korg 35 / TSK (Zavalishin §5.8 p.151) ────────────────────────────
        // VA_NL_SAT: the TSK positive-feedback loop is UNBOUNDED with linear
        // feedback (VA_NL_NONE) and diverges to +/-1e29 at high resonance, worse
        // at high cutoff (verified: blows up even at r=0.5, fc=19k). The bounded
        // sigmoid feedback the struct already provides keeps it stable across the
        // whole fc/res range (verified bounded < 2.6) and gives the intended
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