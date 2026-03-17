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

    // ── Moog passband gain compensation ──────────────────────────────────────
    // The Moog ladder drops passband gain as resonance increases: H(0) = 1/(1+k)
    // (Zavalishin §5.1 p.134).  Full compensation (1+k) is mathematically correct
    // for a DC signal but massively amplifies the resonance peak, causing clipping.
    //
    // Use sqrt(1 + k) instead: recovers roughly half the passband loss in dB
    // while keeping the resonance peak at manageable levels.
    //   k=0:    comp=1.00  (no change)
    //   k=2:    comp=1.73  (+4.8dB vs -9.5dB passband loss)
    //   k=3.95: comp=2.22  (+6.9dB vs -13.9dB passband loss)
    const bool isMoog = (_type == FILTER_MOOG_LP4 ||
                         _type == FILTER_MOOG_LP2 ||
                         _type == FILTER_MOOG_BP2);
    const float moogCompensation = isMoog ? sqrtf(1.0f + k_block) : 1.0f;

    // Pre-check drive (avoid per-sample branch when drive is unity)
    const bool hasDrive = (_drive != 1.0f);

    // ── Sample loop ──────────────────────────────────────────────────────────
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i)
    {
        // Input sample
        float x = (float)in0->data[i] * (1.0f / 32768.0f);

        // Pre-filter drive (adds harmonic content before filtering)
        if (hasDrive) x *= _drive;

        // ── Audio-rate cutoff modulation ─────────────────────────────────────
        float g = g_block;
        if (hasCutoffMod)
        {
#if JT_OPT_VA_BLOCKRATE_MOD
            // Block-rate: g_block already includes mod — no per-sample work
            (void)0;
#else
            // Reference path: full per-sample exponential
            const float modSample = (float)in1->data[i] * (1.0f / 32768.0f);
            const float fcInst    = fcBlock * powf(2.0f, modSample * _cutoffModOct);
            const float fcClamped = va_clamp(fcInst, 5.0f, 0.45f * AUDIO_SAMPLE_RATE_EXACT);
            g = va_compute_g(fcClamped, AUDIO_SAMPLE_RATE_EXACT);
#endif
        }

        // ── Audio-rate resonance modulation ──────────────────────────────────
        float k = k_block;
        if (hasResMod)
        {
            const float resSample = (float)in2->data[i] * (1.0f / 32768.0f);
            const float res01mod  = va_clamp(_res01 + resSample * _resModDepth, 0.0f, 1.0f);
            k = mapResonance(res01mod, _type);
        }

        // ── Run active filter topology ────────────────────────────────────────
        float y = 0.0f;

        switch (_type)
        {
            // ── SVF variants (Zavalishin §4.1 p.95) ──────────────────────────
            // k is used as R = 1/(2Q) for SVF
            case FILTER_SVF_LP:
                _svf.process(x, g, k);
                y = _svf.lp;
                break;

            case FILTER_SVF_HP:
                _svf.process(x, g, k);
                y = _svf.hp;
                break;

            case FILTER_SVF_BP:
                _svf.process(x, g, k);
                y = _svf.bp;
                break;

            case FILTER_SVF_NOTCH:
                _svf.process(x, g, k);
                y = _svf.notch;
                break;

            case FILTER_SVF_AP:
                _svf.process(x, g, k);
                y = _svf.allpass(k);   // AP = notch - 2*R*bp
                break;

            // ── Moog ladder (Zavalishin §5.1 p.133) ──────────────────────────
            // Passband gain compensated by moogCompensation factor.
            case FILTER_MOOG_LP4:
                _moog.process(x, g, k);
                y = _moog.y4 * moogCompensation;
                break;

            case FILTER_MOOG_LP2:
                _moog.process(x, g, k);
                y = _moog.y2 * moogCompensation;
                break;

            case FILTER_MOOG_BP2:
                // BP approximated as y2 - y4 from Moog cascade
                // (Zavalishin §5.1 p.135 – pole subtraction gives bandpass character)
                _moog.process(x, g, k);
                y = (_moog.y2 - _moog.y4) * moogCompensation;
                break;

            // ── Diode ladder (Zavalishin §5.10 p.165) ────────────────────────
            // Nested ZDF solve, self-osc at k=17
            case FILTER_DIODE_LP:
                _diode.process(x, g, k);
                y = _diode.y4;
                break;

            // ── Korg 35 / TSK (Zavalishin §5.8 p.151) ───────────────────────
            // ZDF solved feedback with tanh saturation
            case FILTER_KORG35_LP:
                y = _k35lp.process(x, g, k);
                break;

            case FILTER_KORG35_HP:
                y = _k35hp.process(x, g, k);
                break;

            // ── Simple 1-pole (Zavalishin §3.1 p.45) ─────────────────────────
            case FILTER_TPT1_LP:
                y = _tpt1.processLP(x, g);
                break;

            case FILTER_TPT1_HP:
            {
                float lp;
                y = _tpt1.processHP(x, g, lp);
                break;
            }

            default:
                y = x;   // passthrough for unknown types
                break;
        }

        // ── Optional output saturation (bypassed when SAT_NONE) ──────────────
        y = saturate(y);

        // Hard clip to int16 range before output
        y = va_clamp(y, -1.0f, 1.0f);

        out->data[i] = (int16_t)(y * 32767.0f);
    }

    // ── Transmit and release ─────────────────────────────────────────────────
    transmit(out);
    release(out);
    release(in0);
    if (in1) release(in1);
    if (in2) release(in2);
}
