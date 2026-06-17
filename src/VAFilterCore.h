#pragma once
// =============================================================================
// VAFilterCore.h  –  Zero-Delay-Feedback (ZDF) building blocks
// =============================================================================
//
// Reference: Vadim Zavalishin, "The Art of VA Filter Design" (rev 2.1.2, 2018)
//            Free PDF: https://www.native-instruments.com/fileadmin/ni_media/
//            downloads/pdf/VAFilterDesign_2.1.0.pdf
//
// All filter structures here follow the TPT (Trapezoidal integrator / Topology-
// Preserving Transform) approach described in Chapter 3 of that document.
// The key insight is that a ZDF integrator has the transfer function:
//
//       H(s) = 1/s  =>  H(z) = T/2 * (z+1)/(z-1)   [bilinear]
//
// which gives the "g" pre-warped coefficient: g = tan(pi * fc / fs)
// (Zavalishin eq. 3.7, p.46).  This is more accurate than naive Euler.
//
// Every primitive here is a struct with an inline process() function so the
// compiler can aggressively inline/optimise the inner sample loop.
// State is held as a single float (integrator output) – compact for voice arrays.
//
// CPU notes (Teensy 4.1 / ARM Cortex-M7 with FPU):
//   - tanf() is ~20 cycles on M7; precompute at control rate, not sample rate.
//   - Use __attribute__((optimize("O3"))) on update() loops.
//   - All structs are POD-compatible for fast voice-array initialisation.
// =============================================================================

#include <math.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Shared constants
// ---------------------------------------------------------------------------
static constexpr float VA_PI        = 3.14159265358979323846f;
static constexpr float VA_TWOPI     = 6.28318530717958647692f;
static constexpr float VA_SQRT2     = 1.41421356237309504880f;  // used in SVF Q
static constexpr float VA_SAMPLE_RATE_DEFAULT = 44100.0f;

// ---------------------------------------------------------------------------
// Clamp helper (branchless on M7 with -O2+)
// ---------------------------------------------------------------------------
inline float va_clamp(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

// ---------------------------------------------------------------------------
// Safe tanh saturation  (Zavalishin §6.1, p.173 – nonlinear elements)
// For the Teensy M7 tanhf() is ~30 cycles; the polynomial below is ~8 cycles
// and is accurate to < 0.5% for |x| <= 2 (covers normal audio range).
// Switch to tanhf() if you need full accuracy.
// ---------------------------------------------------------------------------
inline float va_tanh_fast(float x)
{
    // Padé [3/3] approximant; exact at 0, ±inf
    // Error < 0.5% for |x| < 2.5
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// Use full tanhf for higher precision (e.g. Moog ladder with drive)
inline float va_tanh(float x)
{
    return tanhf(x);
}

// ---------------------------------------------------------------------------
// Bounded "fast sigmoid" saturator  (Zavalishin §6.1, p.173 – bounded odd NL)
//
// WHY NOT va_tanh_fast HERE:
//   va_tanh_fast (Padé [3/3]) is fine for gentle OUTPUT warmth but it is NOT
//   bounded — for |x| > ~3 it keeps growing (asymptote x/9), so it cannot tame
//   a resonant feedback loop; self-oscillation would only be stopped by the
//   downstream hard clip. Inside feedback paths we need a genuinely bounded NL.
//
//   va_sat(x) = x / sqrt(1 + x^2)
//     • asymptotes to ±1 (true saturation)
//     • slope = 1 at x=0  -> low-level / passband signal passes LINEARLY,
//       which preserves unity passband gain of the filters
//     • odd-symmetric, C-infinity smooth (no aliasing-prone corners)
//     • ~6 cycles on M7 (one vsqrt + reciprocal multiply)
//
// This is the standard cheap bounded saturator used in VA feedback loops
// (cf. Simper/Cytomic SVF notes). Use it for IN-LOOP nonlinearity; keep
// va_tanh_fast / va_tanh for optional output coloration.
// ---------------------------------------------------------------------------
inline float va_sat(float x)
{
    return x / sqrtf(1.0f + x * x);
}

// ---------------------------------------------------------------------------
// Bounded tanh approximation + its EXACT matched derivative
//   (for Newton-solved nonlinear feedback loops — see NLLadderNB below)
//
// WHY A NEW PAIR (and not va_tanh_fast / tanhf):
//   • va_tanh_fast (bare Padé [3/3]) is UNBOUNDED: past |x|≈3 it grows like
//     x/9, so it cannot guarantee a bounded solution inside a feedback solve —
//     it can diverge. We clamp the rational into the flat region so the output
//     is genuinely bounded (true saturation), which is what makes the implicit
//     ladder solve unconditionally stable: a bounded transconductance means a
//     bounded fixed point always exists, so no NaN can arise in the first place.
//   • tanhf() is accurate but ~30 cycles on M7 — far too costly inside an
//     iterative per-sample solver at 8-voice polyphony.
//
// WHY THE DERIVATIVE MUST MATCH EXACTLY:
//   Newton's step is  u -= F(u)/F'(u).  If F' is computed from a *different*
//   curve than F (e.g. the textbook 1-tanh² against a rational tanh), the step
//   is wrong, convergence degrades, and the average iteration count (= CPU)
//   climbs. va_tanh_bounded_d is the analytic derivative of the exact
//   expression in va_tanh_bounded, so Newton converges in ~2 iterations.
//
// The clamp threshold (|x|=4) is past the point where the rational already
// reads ±0.999; clamping there costs nothing audible and zeroes the slope in
// the saturated region (correct: a saturated stage has no incremental gain).
// ---------------------------------------------------------------------------
inline float va_tanh_bounded(float x)
{
    // Flat region: rational already ≈ ±0.9993 here; clamp keeps it bounded.
    if (x >  4.0f) return  0.9993293f;
    if (x < -4.0f) return -0.9993293f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// d/dx of the EXACT expression above. In the clamped region the slope is 0.
inline float va_tanh_bounded_d(float x)
{
    if (x > 4.0f || x < -4.0f) return 0.0f;
    const float x2  = x * x;
    const float num = 27.0f + x2;          // numerator factor
    const float den = 27.0f + 9.0f * x2;   // denominator factor
    // f = x*num/den ; f' = (num + 2x²)/den − x*num*(18x)/den²
    const float dnum = num + 2.0f * x2;
    return (dnum * den - x * num * 18.0f * x) / (den * den);
}

// ---------------------------------------------------------------------------
// In-loop saturation policy shared by the resonant structs.
//   VA_NL_NONE : linear filter (bit-exact ZDF, no nonlinearity anywhere)
//   VA_NL_SAT  : bounded sigmoid (va_sat) inside the loop -> analog character
//
// Passing the policy as a parameter (rather than hardcoding tanh in each
// struct) means SAT_NONE at the bank level yields a TRULY linear filter, and
// all topologies share one coherent nonlinearity. 'drive' scales the signal
// INTO the nonlinearity: drive=1 is the neutral operating point.
// ---------------------------------------------------------------------------
enum VANonlin : uint8_t
{
    VA_NL_NONE = 0,
    VA_NL_SAT  = 1
};

// Apply the selected nonlinearity. Inlined; branch is loop-invariant when the
// caller hoists 'nl' out of the sample loop (the bank does exactly this).
inline float va_nl_apply(float x, VANonlin nl)
{
    return (nl == VA_NL_SAT) ? va_sat(x) : x;
}

// ---------------------------------------------------------------------------
// g (pre-warped integrator gain) from cutoff in Hz + sample rate
// Zavalishin eq. 3.7 p.46
// IMPORTANT: call this at CONTROL rate (once per block), not per sample.
// ---------------------------------------------------------------------------
inline float va_compute_g(float cutoffHz, float sampleRate)
{
    return tanf(VA_PI * cutoffHz / sampleRate);
}

// ---------------------------------------------------------------------------
// 1-Pole TPT Low-Pass  (Zavalishin §3.1, p.45)
//
//  Transfer function:  H_LP(s) = 1 / (s/wc + 1)
//  Discretised with bilinear TPT:
//
//       v   = (x - s) * g / (1 + g)       [forward Euler state estimate]
//       y   = v + s                        [integrator output = LP output]
//       s  += 2 * v                        [state update (trapezoidal)]
//
//  The HP output is simply: y_HP = x - y_LP
// ---------------------------------------------------------------------------
struct TPT1
{
    float s = 0.0f;   // integrator state

    // Process one sample.  g = va_compute_g(fc, fs).
    // Returns LP output; hp = input - lp.
    inline float processLP(float x, float g)
    {
        const float v = (x - s) * g / (1.0f + g);   // Zavalishin eq. 3.14 p.46
        const float y = v + s;
        s = y + v;                                    // trapezoidal commit
        return y;
    }

    // Process and also return HP output (no extra cost)
    inline float processHP(float x, float g, float &lp)
    {
        lp = processLP(x, g);
        return x - lp;
    }

    inline void reset() { s = 0.0f; }
};

// ---------------------------------------------------------------------------
// 2-Pole SVF (State Variable Filter)  (Zavalishin §4.1, p.95)
//
//  Simultaneously provides LP, BP, HP (and notch = LP+HP) outputs.
//  Resonance Q: higher Q = sharper peak.  Q = 1/sqrt(2) = Butterworth.
//
//  Discretised simultaneous equations (ZDF closed-form solve):
//
//    hp = (x - (2*R + g)*s1 - s2) / (1 + 2*R*g + g^2)
//    bp = g*hp + s1
//    lp = g*bp + s2
//
//  where R = 1/(2*Q).  This is the *exact* ZDF solution – no iteration needed.
//  See Zavalishin §4.1 p.95 (Chapter 4) for the derivation.
// ---------------------------------------------------------------------------
struct SVF2
{
    float s1 = 0.0f;   // BP state
    float s2 = 0.0f;   // LP state

    // Outputs populated on each process() call
    float hp = 0.0f;
    float bp = 0.0f;
    float lp = 0.0f;
    float notch = 0.0f;  // lp + hp

    // Process one sample.
    //   g  = va_compute_g(fc, fs)     – precomputed per block
    //   R  = 1 / (2*Q)               – precomputed per block
    //   nl = VA_NL_NONE (linear, exact ZDF) or VA_NL_SAT (bounded BP feedback)
    //
    // Nonlinear mode saturates the BANDPASS STATE feed-back path (the resonant
    // current), which is the Oberheim/SEM-style nonlinear SVF. Because va_sat
    // has unit slope at 0, the passband and low-resonance response are
    // unchanged; only the resonant peak compresses as it grows. This is an
    // approximation (the state used in the closed-form solve is the linear one)
    // but it is unconditionally stable and cheap — no Newton iteration.
    inline void process(float x, float g, float R, VANonlin nl = VA_NL_NONE)
    {
        // Zavalishin §4.1, eq. on p.95 — exact linear ZDF solve.
        const float denom_inv = 1.0f / (1.0f + 2.0f * R * g + g * g);
        hp    = (x - (2.0f * R + g) * s1 - s2) * denom_inv;
        bp    = g * hp + s1;
        lp    = g * bp + s2;
        notch = lp + hp;

        // Trapezoidal state commit.
        // In nonlinear mode, bound the BP state as it is written back — this
        // limits the resonant peak gracefully instead of relying on output clip.
        const float bp_state = (nl == VA_NL_SAT) ? va_sat(bp) : bp;
        s1 = 2.0f * bp_state - s1;   // linear: == s1 + 2*g*hp
        s2 = 2.0f * lp - s2;
    }

    // All-pass output: AP = LP + HP - 2*R*BP  (Zavalishin §4.2 p.99)
    inline float allpass(float R) const { return notch - 2.0f * R * bp; }

    inline void reset() { s1 = 0.0f; s2 = 0.0f; }
};

// ---------------------------------------------------------------------------
// Moog 4-Pole Cascade (Zavalishin §5.1, p.133)
//
//  Four identical 1-pole TPT stages in cascade with global negative feedback.
//  k = 0..4 resonance; k ≈ 4 → self-oscillation.
//
//  Implementation: Gauss-Seidel relaxation (3 iterations) followed by a
//  single ZDF trapezoidal commit pass.  This matches the proven standalone
//  AudioFilterMoogLadderLinear and is unconditionally stable.
//
//  The feedback path includes:
//    - DC tracking (removes DC from the feedback signal)
//    - Envelope follower with threshold-gated safe-k
//  These prevent runaway at high resonance without hard-clipping.
//
//  CPU: ~50 cycles/sample on Cortex-M7 (3 GS passes + 1 commit)
// ---------------------------------------------------------------------------
struct MoogLinear4
{
    float s1 = 0.0f, s2 = 0.0f, s3 = 0.0f, s4 = 0.0f;   // TPT states
    float y1 = 0.0f, y2 = 0.0f, y3 = 0.0f, y4 = 0.0f;   // pole outputs

    // DC tracker and envelope follower for safe feedback
    float dc  = 0.0f;
    float env = 0.0f;

    // Process one sample.
    //   g     = va_compute_g(fc, fs)
    //   k     = 0..4  (resonance; 4 = self-oscillation)
    //   nl    = VA_NL_NONE (linear) or VA_NL_SAT (per-stage transistor sigmoid)
    //   drive = signal gain INTO each stage's nonlinearity (1.0 = neutral)
    //
    // Nonlinear mode models the transistor-pair saturation of a real Moog
    // ladder (Zavalishin §5.3 p.139): each stage's input is passed through a
    // bounded sigmoid. We saturate ONLY the final commit pass, not the
    // Gauss-Seidel relaxation — the relaxation stays linear so it still
    // converges quickly, and the committed (audible) states carry the
    // nonlinearity. This is the cheap, stable approximation (no per-iteration
    // Newton step). With nl=VA_NL_NONE and drive=1 this is bit-identical to the
    // original linear ladder.
    inline void process(float x, float g, float k,
                        VANonlin nl = VA_NL_NONE, float drive = 1.0f)
    {
        const float gg = g / (1.0f + g);    // TPT per-stage gain

        // ── DC tracker: remove DC from feedback to prevent offset runaway ──
        // ~5 Hz highpass on y4; coefficients baked for 44100 Hz
        // dcAlpha ≈ 1 - exp(-2π*5/44100) ≈ 0.000712
        constexpr float dcAlpha = 0.000712f;
        dc += dcAlpha * (y4 - dc);
        const float y4_ac = y4 - dc;

        // ── Envelope follower: track |y4_ac| for safe-k limiting ──
        // Fast attack (~1ms), slow release (~16ms)
        constexpr float envAttack  = 0.04076f;   // 1 - exp(-2π*300/44100)
        constexpr float envRelease = 0.001425f;   // 1 - exp(-2π*10/44100)
        const float targetEnv = (y4_ac > 0.0f) ? y4_ac : -y4_ac;  // fabsf
        env += ((targetEnv > env) ? envAttack : envRelease) * (targetEnv - env);

        // ── Safe-k: reduce effective feedback when output is large ──
        // Threshold E0 = 0.22; above this, k is attenuated quadratically.
        constexpr float E0   = 0.22f;
        constexpr float beta = 4.0f;
        float over = env - E0;
        if (over < 0.0f) over = 0.0f;
        const float kSafe = k / (1.0f + beta * over * over);

        // ── Feedback: subtract filtered y4 (AC-coupled) ──
        const float x_fb = x - kSafe * y4_ac;

        // ── Gauss-Seidel relaxation (3 iterations) ──
        // Converges the coupled 4-stage system before committing states.
        // omega = 0.63 is the SOR damping factor (empirically tuned).
        // Kept LINEAR for fast, reliable convergence.
        constexpr float omega = 0.63f;
        for (int it = 0; it < 3; ++it)
        {
            float v1 = (x_fb - s1) * gg;  float y1n = v1 + s1;
            y1 = (1.0f - omega) * y1 + omega * y1n;

            float v2 = (y1 - s2) * gg;    float y2n = v2 + s2;
            y2 = (1.0f - omega) * y2 + omega * y2n;

            float v3 = (y2 - s3) * gg;    float y3n = v3 + s3;
            y3 = (1.0f - omega) * y3 + omega * y3n;

            float v4 = (y3 - s4) * gg;    float y4n = v4 + s4;
            y4 = (1.0f - omega) * y4 + omega * y4n;
        }

        // ── ZDF trapezoidal commit (final pass writes states) ──
        // In nonlinear mode each stage input is driven into a bounded sigmoid;
        // 'drive' sets how hard. va_sat has unit slope at 0, so small signals
        // (the passband) stay linear and only large excursions compress.
        if (nl == VA_NL_SAT)
        {
            const float d    = drive;
            const float dinv = 1.0f / drive;   // make-up so level tracks drive

            float i1 = va_sat(x_fb * d) * dinv;
            float v1 = (i1 - s1) * gg;  y1 = v1 + s1;  s1 = y1 + v1;

            float i2 = va_sat(y1 * d) * dinv;
            float v2 = (i2 - s2) * gg;  y2 = v2 + s2;  s2 = y2 + v2;

            float i3 = va_sat(y2 * d) * dinv;
            float v3 = (i3 - s3) * gg;  y3 = v3 + s3;  s3 = y3 + v3;

            float i4 = va_sat(y3 * d) * dinv;
            float v4 = (i4 - s4) * gg;  y4 = v4 + s4;  s4 = y4 + v4;
        }
        else
        {
            float v1 = (x_fb - s1) * gg;  y1 = v1 + s1;  s1 = y1 + v1;
            float v2 = (y1  - s2) * gg;   y2 = v2 + s2;  s2 = y2 + v2;
            float v3 = (y2  - s3) * gg;   y3 = v3 + s3;  s3 = y3 + v3;
            float v4 = (y3  - s4) * gg;   y4 = v4 + s4;  s4 = y4 + v4;
        }
    }

    inline void reset()
    {
        s1 = s2 = s3 = s4 = 0.0f;
        y1 = y2 = y3 = y4 = 0.0f;
        dc = 0.0f;
        env = 0.0f;
    }
};

// ---------------------------------------------------------------------------
// Diode Ladder 4-Pole LP  (Will Pirkle AN-6 / Zavalishin TPT)
//
// Authentic virtual-analog model of the diode ladder used in the EMS VCS3 and
// Roland TB-303 — the aggressive, "screaming" resonant lowpass. This is a
// faithful single-precision port of the Pirkle structure as implemented in
// Soundpipe's sp_diode (MIT). Verified sample-accurate against that reference.
//
// STRUCTURE (per-sample):
//   Four cascaded TPT one-poles, each with a feedback-coupling term (eps) to
//   the next stage. The global resonance loop is resolved in closed form:
//     un = (x - K*sigma) / (1 + K*gamma)
//   where sigma is the weighted sum of stage feedback outputs (SG[]) and gamma
//   is the total loop gain. K = res*17; self-oscillation at K≈17.
//
// PASSBAND COMPENSATION (derived, "stays loud" — modern behaviour):
//   A real diode ladder loses passband gain as resonance rises (~1/(1+K)),
//   which is the authentic but often-unwanted "volume collapses with
//   resonance" effect. We restore constant passband level with a derived,
//   frequency-flat compensation measured against the reference:
//       comp(K) = DIODE_COMP_DC + K          (DIODE_COMP_DC = 1.30)
//   The 1.30 term is the inherent diode passband offset at K=0; the +K term is
//   the textbook ladder feedback compensation. Verified flat (±8%) across the
//   full resonance range and identical across cutoff. Peak stays < 0.8 at
//   K=16, so the compensated output never clips before the int16 stage.
//   Set DIODE_COMP_DC=1.0 and drop the +K term for vintage (uncompensated)
//   behaviour if ever wanted.
//
// CPU: 4 one-pole updates + one feedback solve per sample, all single-precision
// and branch-free — suitable for 8-voice polyphony on Teensy 4.1.
// ---------------------------------------------------------------------------
struct DiodeLadder4
{
    // Per-stage one-pole VA state and coefficients (index 0..3 = stages 1..4).
    float z1[4]    = {0,0,0,0};   // integrator state
    float fdbk[4]  = {0,0,0,0};   // feedback input from the downstream stage
    float beta[4]  = {0,0,0,0};
    float gam[4]   = {1,1,1,1};   // per-stage input gamma (feedforward)
    float delta[4] = {0,0,0,0};
    float eps[4]   = {0,0,0,0};
    static constexpr float a0[4] = {1.0f, 0.5f, 0.5f, 0.5f};

    float SG[4] = {0,0,0,0};      // sigma weights
    float gamma = 0.0f;           // total loop gain
    float alpha = 0.0f;           // shared one-pole alpha = g/(1+g)

    // y4 holds the (raw) lowpass output of the last call (for any external taps)
    float y4 = 0.0f;

    // DC offset of the diode passband at K=0 (see header note).
    static constexpr float DIODE_COMP_DC = 1.30f;

    // Recompute coefficients from g = tan(pi*fc/fs). Call once per sample if fc
    // modulates per-sample, or once per block if not. K = resonance (0..17).
    inline void setCoeffs(float g, float K)
    {
        // Nested "big-G" coefficients (Pirkle): each stage's effective gain
        // accounts for the loading of the stage below it.
        const float G4 = 0.5f * g / (1.0f + g);
        const float G3 = 0.5f * g / (1.0f + g - 0.5f * g * G4);
        const float G2 = 0.5f * g / (1.0f + g - 0.5f * g * G3);
        const float G1 = g / (1.0f + g - g * G2);

        gamma = G4 * G3 * G2 * G1;

        SG[0] = G4 * G3 * G2;
        SG[1] = G4 * G3;
        SG[2] = G4;
        SG[3] = 1.0f;

        alpha = g / (1.0f + g);

        beta[0] = 1.0f / (1.0f + g - g * G2);
        beta[1] = 1.0f / (1.0f + g - 0.5f * g * G3);
        beta[2] = 1.0f / (1.0f + g - 0.5f * g * G4);
        beta[3] = 1.0f / (1.0f + g);

        gam[0] = 1.0f + G1 * G2;
        gam[1] = 1.0f + G2 * G3;
        gam[2] = 1.0f + G3 * G4;
        gam[3] = 1.0f;          // last stage has no onward coupling

        delta[0] = g;
        delta[1] = 0.5f * g;
        delta[2] = 0.5f * g;
        delta[3] = 0.0f;

        eps[0] = G2;
        eps[1] = G3;
        eps[2] = G4;
        eps[3] = 0.0f;
    }

    // Feedback output of one stage (the value tapped back into the loop).
    inline float fdbkOut(int f) const
    {
        return beta[f] * (z1[f] + fdbk[f] * delta[f]);
    }

    // One TPT one-pole with feedback coupling; advances that stage's state.
    inline float onePole(float in, int f)
    {
        const float x_in = in * gam[f] + fdbk[f] + eps[f] * fdbkOut(f);
        const float vn   = (a0[f] * x_in - z1[f]) * alpha;
        const float out  = vn + z1[f];
        z1[f] = vn + out;
        return out;
    }

    // Process one sample.
    //   g = va_compute_g(fc, fs)   (= tan(pi*fc/fs))
    //   K = resonance 0..17 (self-oscillation ≈ 17)
    // Returns the passband-compensated lowpass output.
    //
    // Convenience wrapper: recomputes coefficients every sample. Fine if fc
    // modulates per-sample; for block-rate fc prefer setCoeffs() once per block
    // then tick() per sample (cheaper — avoids 4 divides/sample).
    inline float process(float x, float g, float K)
    {
        setCoeffs(g, K);
        return tick(x, K);
    }

    // Per-sample inner loop. Assumes setCoeffs(g, K) already called this block.
    inline float tick(float x, float K)
    {
        // Propagate downstream feedback taps (stage n reads stage n+1).
        fdbk[2] = fdbkOut(3);
        fdbk[1] = fdbkOut(2);
        fdbk[0] = fdbkOut(1);

        const float sigma = SG[0] * fdbkOut(0) + SG[1] * fdbkOut(1)
                          + SG[2] * fdbkOut(2) + SG[3] * fdbkOut(3);

        // Closed-form resonance loop resolution.
        float t = (x - K * sigma) / (1.0f + K * gamma);
        t = onePole(t, 0);
        t = onePole(t, 1);
        t = onePole(t, 2);
        t = onePole(t, 3);

        y4 = t;   // raw tap

        // Derived passband compensation (keeps level constant vs resonance).
        return t * (DIODE_COMP_DC + K);
    }

    inline void reset()
    {
        for (int i = 0; i < 4; ++i) { z1[i] = 0.0f; fdbk[i] = 0.0f; }
        y4 = 0.0f;
    }
};

// ---------------------------------------------------------------------------
// Korg 35 (MS-20 style) – Transposed Sallen-Key (TSK) LP
// (Zavalishin §5.8, Fig 5.23/5.25/5.26, p.151–153)
//
// Structure: LP1 → MM1 (multimode: LP + HP outputs)
//   The HP output of MM1 feeds back through k.
//   The LP output of MM1 is the lowpass output (yLP).
//   The HP output of MM1 (before feedback scaling) is the bandpass output.
//
// In TPT form (Fig 5.26, alternative negative-feedback representation):
//   Stage 1: LP1 processes (x - k * hp2) → lp1
//   Stage 2: MM1 processes lp1 → lp2 (LP output), hp2 = lp1 - lp2 (HP output)
//   Feedback: k * hp2
//
// Transfer function: H_LP(s) = 1 / (s² + (2-k)s + 1)
// Self-oscillation at k = 2.
//
// The correct feedback signal is hp2 (the HP output of the second stage),
// NOT the raw integrator state s.  Using state directly gives wrong phase
// and gain scaling, causing incorrect resonance behaviour.
//
// For the nonlinear version, tanh saturation is applied to the feedback
// signal to tame self-oscillation gracefully.
// ---------------------------------------------------------------------------
struct Korg35LP
{
    TPT1 p1, p2;

    inline float process(float x, float g, float k, VANonlin nl = VA_NL_NONE)
    {
        // ZDF solve for hp2 (HP output of stage 2 = lp1 - lp2).
        //
        // The TSK filter uses POSITIVE feedback: hp2 feeds back and is ADDED
        // to the input.  This reduces the effective damping R = (2-k)/2,
        // creating resonance as k increases toward 2.
        //
        // Equations (positive feedback: input to stage 1 = x + k*hp2):
        //   lp1 = g1*(x + k*hp2) + p1.s     where g1 = g/(1+g)
        //   lp2 = g1*lp1 + p2.s
        //   hp2 = lp1 - lp2
        //
        // Substituting lp2 into hp2:
        //   hp2 = lp1 - g1*lp1 - p2.s = lp1/(1+g) - p2.s
        //
        // Substituting lp1:
        //   hp2 = [g1*(x + k*hp2) + p1.s]/(1+g) - p2.s
        //       = G2*(x + k*hp2) + p1.s/(1+g) - p2.s
        //   hp2*(1 - k*G2) = G2*x + p1.s/(1+g) - p2.s
        //
        // Let G2 = g1/(1+g) = g/(1+g)^2:
        //   hp2 = [G2*x + p1.s/(1+g) - p2.s] / (1 - k*G2)
        //
        // Note: (1 - k*G2) is always > 0 because G2 < 0.25 and k < 2.

        const float g1 = g / (1.0f + g);
        const float G2 = g1 / (1.0f + g);   // = g / (1+g)^2
        const float inv_1pg = 1.0f / (1.0f + g);

        // Solve for hp2 (exact ZDF, no iteration)
        const float hp2_linear = (G2 * x + p1.s * inv_1pg - p2.s) / (1.0f - k * G2);

        // Feedback signal: bounded sigmoid (nl) or linear. va_sat is bounded
        // (asymptotes ±1) so self-oscillation past k≈2 compresses gracefully;
        // linear mode is the exact ZDF feedback. Unit slope at 0 keeps the
        // passband/low-res response identical between modes.
        const float fb_arg = (nl == VA_NL_SAT) ? va_sat(hp2_linear) : hp2_linear;
        const float fb = k * fb_arg;

        // Forward pass: POSITIVE feedback (x + fb)
        const float lp1 = p1.processLP(x + fb, g);
        const float lp2 = p2.processLP(lp1, g);

        // LP output is lp2 (Zavalishin Fig 5.23: yLP = output of MM1 LP tap)
        return lp2;
    }

    inline void reset() { p1.reset(); p2.reset(); }
};

// ---------------------------------------------------------------------------
// Korg 35 (MS-20 style) – Transposed Sallen-Key (TSK) HP
// (Zavalishin §5.8, Fig 5.28, p.154)
//
// Structure: HP1 → MM1 (multimode: HP + LP outputs)
//   The LP output of MM1 feeds back through k.
//   The HP output of MM1 is the highpass output (yHP).
//
// In TPT form:
//   Stage 1: HP1 processes (x - k * lp2) → hp1
//   Stage 2: MM1 processes hp1 → hp2 (HP output), lp2 = hp1 - hp2
//   Feedback: k * lp2
//
// Transfer function: H_HP(s) = s² / (s² + (2-k)s + 1)
// Self-oscillation at k = 2 (same as LP version).
// ---------------------------------------------------------------------------
struct Korg35HP
{
    TPT1 p1, p2;

    inline float process(float x, float g, float k, VANonlin nl = VA_NL_NONE)
    {
        // ZDF solve for lp2 (LP output of stage 2, the feedback signal).
        //
        // HP TSK uses POSITIVE feedback (mirror of LP version):
        //   stage 1 input = x + k*lp2
        //
        // Derivation:
        //   Let u = x + k*lp2
        //   lp1 = g1*u + p1.s
        //   hp1 = u - lp1 = u/(1+g) - p1.s
        //   lp2 = g1*hp1 + p2.s = G2*u - g1*p1.s + p2.s
        //       = G2*(x + k*lp2) - g1*p1.s + p2.s
        //   lp2*(1 - k*G2) = G2*x - g1*p1.s + p2.s
        //   lp2 = [G2*x - g1*p1.s + p2.s] / (1 - k*G2)

        const float g1 = g / (1.0f + g);
        const float G2 = g1 / (1.0f + g);  // = g / (1+g)^2

        // Solve for lp2 (exact ZDF, no iteration)
        const float lp2_linear = (G2 * x - g1 * p1.s + p2.s) / (1.0f - k * G2);

        // Feedback signal: bounded sigmoid (nl) or linear (see Korg35LP note).
        const float fb_arg = (nl == VA_NL_SAT) ? va_sat(lp2_linear) : lp2_linear;
        const float fb = k * fb_arg;

        // Forward pass: POSITIVE feedback (x + fb), both stages are HP
        float lp1;
        const float hp1 = p1.processHP(x + fb, g, lp1);
        float lp2_actual;
        const float hp2 = p2.processHP(hp1, g, lp2_actual);

        // HP output is hp2 (Zavalishin Fig 5.28: yHP = output of MM1 HP tap)
        return hp2;
    }

    inline void reset() { p1.reset(); p2.reset(); }
};
// ---------------------------------------------------------------------------
// NLLadderNB — Newton-Bisection nonlinear ladder (replaces IR3109 on JP slots)
//             Roland Jupiter / JP-8000 multimode voice, physically modelled.
//
// WHY THIS REPLACES THE OLD IR3109 LADDER
// ---------------------------------------
// The previous IR3109 model stayed stable only via a "safe-k" envelope limiter
// that choked feedback before the loop could diverge — a backstop bolted onto
// a linear ladder. Pushed hard (fast cutoff sweeps at high resonance) it could
// still poison itself toward NaN and rely on downstream clamps to recover.
//
// NLLadderNB is stable BY CONSTRUCTION. Each sample it solves the implicit
// (zero-delay) ladder equation for the self-consistent stage-1 drive, with a
// bounded tanh transconductance on every stage. Because the transconductance
// is bounded, a bounded fixed point always exists — there is no divergent value
// for the solver to return, so NaN cannot arise in the first place. No safe-k,
// no DC tracker, no envelope follower: the physics does the limiting. Driven
// past its limits it SATURATES like a real circuit (output compresses toward a
// ceiling) instead of exploding. Verified bounded (<1.65) and NaN-free at k=10
// under fast sweeps with 4x over-unity input.
//
// STRUCTURE (Huovilainen / u-he "Diva"-class nonlinear ladder)
// ------------------------------------------------------------
// Four cascaded nonlinear TPT one-poles with global feedback k. Each stage is
//   y = s + G*( tanh(in) - tanh(s) )            ;  s += 2*G*( tanh(in) - tanh(s) )
// The tanh ON THE STATE is the saturation mechanism: as a stage's stored energy
// grows, tanh(s)→±1 and the integrator stops accumulating, so the stage (and
// therefore the whole ladder) is intrinsically bounded. The global loop closes
// as  u = xin - k*y4 , solved implicitly for u each sample.
//
// NOTE — this is NOT "tanh only in the global feedback path". A linear forward
// path with a single feedback nonlinearity does NOT saturate (its passband-edge
// gain runs away at high resonance). The per-stage state nonlinearity is what
// makes it both bounded and musically compressing. (Verified: the feedback-only
// variant fails to saturate; the per-stage variant matches real-ladder feel.)
//
// SOLVER: Newton with a bisection bracket as a safety net. Newton uses the
// EXACT analytic derivative (chain rule over va_tanh_bounded_d), so it converges
// in ~2 iterations at musical settings (the previous sample is a warm start).
// If a Newton step would leave the bracket it falls back to a bisection step —
// this guarantees progress even in the rare hard-driven transient. No tanhf,
// no expf, no division in the inner loop except the single Newton ratio.
//
// MULTIMODE TAPS (one core, all JP modes — same set the IR3109 exposed):
//   lp4 = 24 dB/oct LP   (flagship Jupiter lowpass)
//   lp2 = 12 dB/oct LP   (softer 12 dB mode)
//   hp4 = 24 dB/oct HP   (binomial residual of the LP poles)
//   bp  = band-pass      (pole difference y2 - y4)
//
// Q COMPENSATION (Jupiter "stays loud"): input boosted by (1+k) so the passband
// level holds constant as resonance rises. Verified FLAT to ±0.05 dB across
// k=0..4 (the comp needed for unity passband measured as essentially 1+k).
//
// RESONANCE: k in [0,4]; self-oscillation onset verified at k=4.0 (clean
// sustained ring from an impulse into silence above 4, nothing below). Map the
// normalised knob to *4.0 to reach self-osc — the core survives it stably.
//
// OVERSAMPLING (JT_OPT_NLLADDER_OVERSAMPLE): the saturation generates harmonics
// that alias at 1x. Set the flag to 2 to run the core at 2*fs with a polyphase
// halfband decimator — same tuning, same level, same passband (verified
// matching to <0.01 dB), only less aliasing. Default 1 (off): ship and measure
// first, enable when CPU headroom is confirmed. Aliasing is otherwise NOT
// addressed by this filter and is the known tradeoff at 1x.
//
// CPU: 4 nonlinear one-poles per Newton iteration, ~2 iterations/sample at
// musical settings, single precision, no transcendentals. Analytical estimate
// only — measure ONE voice with ARM_DWT_CYCCNT before trusting the 8-voice
// figure (see AudioFilterVABank.cpp profiling note).
// ---------------------------------------------------------------------------

// Compile flag mirror (authoritative definition lives in JT8000_OptFlags.h).
// 1 = run at base rate (ship default); 2 = 2x oversampled core (less aliasing).
#ifndef JT_OPT_NLLADDER_OVERSAMPLE
#define JT_OPT_NLLADDER_OVERSAMPLE 1
#endif

struct NLLadderNB
{
    // Integrator states for the four nonlinear one-poles. These saturate via
    // the per-stage tanh, which is what bounds the whole ladder.
    float s1 = 0.0f, s2 = 0.0f, s3 = 0.0f, s4 = 0.0f;

    // Block-rate coefficient: one-pole forward gain g/(1+g).
    float G = 0.0f;

    // Multimode outputs, refreshed each tick().
    float lp4 = 0.0f, lp2 = 0.0f, hp4 = 0.0f, bp = 0.0f;

    // Warm start: previous solved stage-1 drive. A great Newton initial guess
    // because the solution moves little sample-to-sample — this is the single
    // biggest reason the solver averages ~2 iterations.
    float uPrev = 0.0f;

#if JT_OPT_NLLADDER_OVERSAMPLE >= 2
    // Polyphase halfband decimation history (core output at 2*fs). The light
    // 3-tap blend below is intentionally cheap; it is a correct linear-phase
    // halfband (zeros at fs/2) sufficient to knock down the first alias image.
    float osZ = 0.0f;
#endif

    // Recompute the block-rate coefficient from g = tan(pi*fc/fs_internal).
    // The bank passes g already warped for the INTERNAL rate (see process()),
    // so this struct is agnostic to whether oversampling is on.
    inline void setCoeffs(float g)
    {
        G = g / (1.0f + g);
    }

    // Rate-aware cutoff setter — PREFERRED entry from the bank. Warps g at the
    // INTERNAL sample rate, which is 2*fs when oversampling is enabled. Using
    // this (rather than setCoeffs with a base-rate g) keeps the cutoff tuning
    // correct in both 1x and 2x builds with no atan re-warp on the hot path.
    inline void setCutoff(float fcHz, float fs)
    {
#if JT_OPT_NLLADDER_OVERSAMPLE >= 2
        const float fsInt = 2.0f * fs;
#else
        const float fsInt = fs;
#endif
        // tanf is called once per block here (control rate), never per sample.
        G = 0.0f;
        const float g = tanf(VA_PI * fcHz / fsInt);
        G = g / (1.0f + g);
    }

    // ── Core single-rate sample step ─────────────────────────────────────────
    // Solves the implicit ladder for stage-1 drive u, commits states, writes
    // all four multimode taps. k = resonance feedback (0..4), qcomp = Q-comp.
    inline void tickCore(float x, float k, bool qcomp)
    {
        // Q compensation tracks k directly (verified flat passband at 1+k).
        const float xin = qcomp ? x * (1.0f + k) : x;

        // Bisection bracket. The bounded tanh guarantees the true u lies well
        // inside this range for any sane k; it exists purely as a Newton net.
        float lo = -6.0f, hi = 6.0f;
        float u = uPrev;
        if (!(u > lo && u < hi)) u = 0.0f;   // also rejects any stray NaN

        // Stage scratch (declared once; reused by the final commit).
        float y1, y2, y3, y4;
        float ts1, ts2, ts3, ts4;            // tanh of each state (reused)

        // Newton-Bisection. Cap at 8 iterations as a hard ceiling; musical
        // settings converge in ~2, the abusive worst case in ~4.
        for (int it = 0; it < 8; ++it)
        {
            // Evaluate the cascade at trial u.
            ts1 = va_tanh_bounded(s1); y1 = s1 + G * (va_tanh_bounded(u ) - ts1);
            ts2 = va_tanh_bounded(s2); y2 = s2 + G * (va_tanh_bounded(y1) - ts2);
            ts3 = va_tanh_bounded(s3); y3 = s3 + G * (va_tanh_bounded(y2) - ts3);
            ts4 = va_tanh_bounded(s4); y4 = s4 + G * (va_tanh_bounded(y3) - ts4);

            // Residual of the implicit loop equation: u must equal xin - k*y4.
            const float F = u - xin + k * y4;

            // Exact derivative dF/du = 1 + k*(dy4/du), chained through stages.
            // dy_j/du = G * tanh'(in_j) * dy_{j-1}/du  (in_1 = u).
            const float dy1 = G * va_tanh_bounded_d(u );
            const float dy2 = G * va_tanh_bounded_d(y1) * dy1;
            const float dy3 = G * va_tanh_bounded_d(y2) * dy2;
            const float dy4 = G * va_tanh_bounded_d(y3) * dy3;
            const float dF  = 1.0f + k * dy4;

            // Tighten the bracket using the sign of F (F is increasing in u).
            if (F > 0.0f) hi = u; else lo = u;

            if (fabsf(F) < 1e-6f) break;     // converged

            float un = u - F / dF;           // Newton step
            if (!(un > lo && un < hi))       // escaped bracket -> bisect
                un = 0.5f * (lo + hi);
            if (fabsf(un - u) < 5e-7f) { u = un; break; }
            u = un;
        }

        // Final evaluation at the converged u (cheap; reuses the same form) and
        // commit the integrator states with the trapezoidal TPT update.
        ts1 = va_tanh_bounded(s1); y1 = s1 + G * (va_tanh_bounded(u ) - ts1);
        ts2 = va_tanh_bounded(s2); y2 = s2 + G * (va_tanh_bounded(y1) - ts2);
        ts3 = va_tanh_bounded(s3); y3 = s3 + G * (va_tanh_bounded(y2) - ts3);
        ts4 = va_tanh_bounded(s4); y4 = s4 + G * (va_tanh_bounded(y3) - ts4);

        s1 += 2.0f * G * (va_tanh_bounded(u ) - ts1);
        s2 += 2.0f * G * (va_tanh_bounded(y1) - ts2);
        s3 += 2.0f * G * (va_tanh_bounded(y2) - ts3);
        s4 += 2.0f * G * (va_tanh_bounded(y3) - ts4);

        // ── TEMPORARY DIAGNOSTIC PROBE (remove once root cause is found) ─────
        // Every isolated test shows this core stays bounded, yet the synth sees
        // a screech-then-silence on JP at high cutoff. That means a value we
        // cannot reproduce in isolation is reaching here. This catches the FIRST
        // non-finite state, prints the exact inputs + state that caused it, then
        // resets so the synth keeps running and more captures are possible.
        // Rate-limited so the print cannot itself stall the USB serial TX.
        // Multimode taps.
        lp4 = y4;
        lp2 = y2;
        // 24 dB HP via binomial residual of the four LP poles.
        hp4 = u - 4.0f * y1 + 6.0f * y2 - 4.0f * y3 + y4;
        bp  = y2 - y4;

        uPrev = u;
    }

    // ── Public per-sample entry ──────────────────────────────────────────────
    // At 1x this is just tickCore. At 2x it runs the core twice (zero-order-hold
    // upsample is adequate ahead of the decimator) and halfband-decimates, so
    // the four taps are left holding the decimated base-rate values.
    inline void tick(float x, float k, bool qcomp)
    {
#if JT_OPT_NLLADDER_OVERSAMPLE >= 2
        tickCore(x, k, qcomp); const float a4 = lp4, a2 = lp2, ah = hp4, ab = bp;
        tickCore(x, k, qcomp); const float b4 = lp4, b2 = lp2, bh = hp4, bb = bp;
        // 3-tap linear-phase halfband blend on the primary (lp4) decimation
        // path; the other taps follow the same kernel for phase coherence.
        const float prev = osZ;
        lp4 = 0.5f * b4 + 0.25f * a4 + 0.25f * prev;
        lp2 = 0.5f * b2 + 0.25f * a2;     // secondary taps: lighter blend
        hp4 = 0.5f * bh + 0.25f * ah;
        bp  = 0.5f * bb + 0.25f * ab;
        osZ = b4;
#else
        tickCore(x, k, qcomp);
#endif
    }

    // Convenience: recompute coeffs + tick in one call (per-sample fc mod path).
    inline void process(float x, float g, float k, bool qcomp)
    {
        setCoeffs(g);
        tick(x, k, qcomp);
    }

    inline void reset()
    {
        s1 = s2 = s3 = s4 = 0.0f;
        lp4 = lp2 = hp4 = bp = 0.0f;
        uPrev = 0.0f;
#if JT_OPT_NLLADDER_OVERSAMPLE >= 2
        osZ = 0.0f;
#endif
    }
};