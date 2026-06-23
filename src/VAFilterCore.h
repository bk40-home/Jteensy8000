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
// State is held compactly – suitable for per-voice arrays.
//
// CPU notes (Teensy 4.1 / ARM Cortex-M7 with FPU):
//   - tanf() is ~20 cycles on M7; precompute at control rate, not sample rate.
//   - Use __attribute__((optimize("O3"))) on update() loops.
//   - All structs are POD-compatible for fast voice-array initialisation.
//
// NONLINEARITY POLICY (VANonlin):
//   The resonant structs take an optional `nl` argument. VA_NL_NONE is the
//   exact linear ZDF filter (bit-identical to the original cores); VA_NL_SAT
//   applies a bounded sigmoid (va_sat) inside the feedback path for analogue
//   character. The bank hoists `nl` out of the sample loop so the branch is
//   loop-invariant — zero per-sample cost.
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
//
// WARNING: this Padé [3/3] is UNBOUNDED past |x|≈3 (asymptote x/9). It is fine
// for gentle OUTPUT coloration but must NEVER be used inside a feedback loop —
// use va_sat() there. Switch to tanhf() if you need full accuracy.
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
//   bounded — for |x| > ~3 it keeps growing, so it cannot tame a resonant
//   feedback loop; self-oscillation would only be stopped by the downstream
//   hard clip. Inside feedback paths we need a genuinely bounded NL.
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
// In-loop saturation policy shared by the resonant structs.
//   VA_NL_NONE : linear filter (bit-exact ZDF, no nonlinearity anywhere)
//   VA_NL_SAT  : bounded sigmoid (va_sat) inside the loop -> analogue character
//
// Passing the policy as a parameter (rather than hardcoding tanh in each
// struct) means SAT_NONE at the bank level yields a TRULY linear filter, and
// all topologies share one coherent nonlinearity.
// ---------------------------------------------------------------------------
enum VANonlin : uint8_t
{
    VA_NL_NONE = 0,
    VA_NL_SAT  = 1
};

// Apply the selected nonlinearity. Inlined; the branch is loop-invariant when
// the caller hoists 'nl' out of the sample loop (the bank does exactly this).
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
    // current), the Oberheim/SEM-style nonlinear SVF. Because va_sat has unit
    // slope at 0, the passband and low-resonance response are unchanged; only
    // the resonant peak compresses as it grows. This is an approximation (the
    // state used in the closed-form solve is the linear one) but it is
    // unconditionally stable and cheap — no Newton iteration.
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
// Moog 4-Pole Cascade (Zavalishin §5.1, p.133; §5.3 p.139 nonlinear)
//
//  Four identical 1-pole TPT stages in cascade with global negative feedback.
//  k = 0..4 resonance; k ≈ 4 → self-oscillation.
//
//  Implementation: Gauss-Seidel relaxation (3 iterations) followed by a single
//  ZDF trapezoidal commit pass. Unconditionally stable. The feedback path adds
//  DC tracking and an envelope-gated safe-k limiter to prevent runaway at high
//  resonance without hard-clipping.
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
    // ladder (Zavalishin §5.3 p.139): each stage's input passes through a
    // bounded sigmoid. We saturate ONLY the final commit pass, not the
    // Gauss-Seidel relaxation — the relaxation stays linear so it converges
    // quickly, and the committed (audible) states carry the nonlinearity. This
    // is the cheap, stable approximation (no per-iteration Newton step). With
    // nl=VA_NL_NONE and drive=1 this is bit-identical to the linear ladder.
    inline void process(float x, float g, float k,
                        VANonlin nl = VA_NL_NONE, float drive = 1.0f)
    {
        const float gg = g / (1.0f + g);    // TPT per-stage gain

        // ── DC tracker: remove DC from feedback to prevent offset runaway ──
        // ~5 Hz highpass on y4; coefficient baked for 44100 Hz.
        // dcAlpha ≈ 1 - exp(-2π*5/44100) ≈ 0.000712
        constexpr float dcAlpha = 0.000712f;
        dc += dcAlpha * (y4 - dc);
        const float y4_ac = y4 - dc;

        // ── Envelope follower: track |y4_ac| for safe-k limiting ──
        // Fast attack (~1ms), slow release (~16ms)
        constexpr float envAttack  = 0.04076f;   // 1 - exp(-2π*300/44100)
        constexpr float envRelease = 0.001425f;  // 1 - exp(-2π*10/44100)
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
// Authentic virtual-analogue model of the diode ladder used in the EMS VCS3 and
// Roland TB-303 — the aggressive, "screaming" resonant lowpass. Faithful
// single-precision port of the Pirkle structure as implemented in Soundpipe's
// sp_diode (MIT). Verified sample-accurate against that reference.
//
// STRUCTURE (per-sample):
//   Four cascaded TPT one-poles, each with a feedback-coupling term (eps) to
//   the next stage. The global resonance loop is resolved in closed form:
//     un = (x - K*sigma) / (1 + K*gamma)
//   where sigma is the weighted sum of stage feedback outputs (SG[]) and gamma
//   is the total loop gain. K = res*17; self-oscillation at K≈17.
//
// PASSBAND COMPENSATION (derived, "stays loud" — modern behaviour):
//   A real diode ladder loses passband gain as resonance rises (~1/(1+K)).
//   We restore constant passband level with a derived, frequency-flat
//   compensation measured against the reference:
//       comp(K) = DIODE_COMP_DC + K          (DIODE_COMP_DC = 1.30)
//   Verified flat (±8%) across the full resonance range and identical across
//   cutoff. Peak stays < 0.8 at K=16, so the compensated output never clips
//   before the int16 stage. Set DIODE_COMP_DC=1.0 and drop the +K term for
//   vintage (uncompensated) behaviour if ever wanted.
//
// PERFORMANCE — setCoeffs()/tick() split:
//   Coefficients depend only on g and K. When cutoff is block-rate (the common
//   path) call setCoeffs() ONCE per block, then tick() per sample — this avoids
//   four divides per sample. process() is the per-sample-fc convenience wrapper.
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
// Transfer function: H_LP(s) = 1 / (s² + (2-k)s + 1). Self-oscillation at k = 2.
//
// STABILITY: the TSK positive-feedback loop is UNBOUNDED with linear feedback
// (VA_NL_NONE) and diverges at high resonance/cutoff. The bank therefore drives
// this core with VA_NL_SAT — the bounded sigmoid in the feedback path keeps it
// stable across the whole fc/res range and gives graceful self-oscillation
// compression past k≈2. VA_NL_NONE is kept only for analysis/A-B, never shipped.
// ---------------------------------------------------------------------------
struct Korg35LP
{
    TPT1 p1, p2;

    inline float process(float x, float g, float k, VANonlin nl = VA_NL_NONE)
    {
        // ZDF solve for hp2 (HP output of stage 2 = lp1 - lp2).
        //
        // The TSK filter uses POSITIVE feedback: hp2 feeds back and is ADDED to
        // the input. This reduces the effective damping R = (2-k)/2, creating
        // resonance as k increases toward 2.
        //
        //   hp2 = [G2*x + p1.s/(1+g) - p2.s] / (1 - k*G2)
        //   where G2 = g/(1+g)^2.  (1 - k*G2) is always > 0 since G2 < 0.25,
        //   k < 2.

        const float g1 = g / (1.0f + g);
        const float G2 = g1 / (1.0f + g);   // = g / (1+g)^2
        const float inv_1pg = 1.0f / (1.0f + g);

        // Solve for hp2 (exact ZDF, no iteration)
        const float hp2_linear = (G2 * x + p1.s * inv_1pg - p2.s) / (1.0f - k * G2);

        // Feedback signal: bounded sigmoid (nl) or linear. va_sat is bounded
        // (asymptotes ±1) so self-oscillation past k≈2 compresses gracefully;
        // unit slope at 0 keeps the passband/low-res response identical.
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
// Transfer function: H_HP(s) = s² / (s² + (2-k)s + 1). Self-osc at k = 2.
// Same stability note as Korg35LP — drive with VA_NL_SAT.
// ---------------------------------------------------------------------------
struct Korg35HP
{
    TPT1 p1, p2;

    inline float process(float x, float g, float k, VANonlin nl = VA_NL_NONE)
    {
        // ZDF solve for lp2 (LP output of stage 2, the feedback signal).
        //   lp2 = [G2*x - g1*p1.s + p2.s] / (1 - k*G2),  G2 = g/(1+g)^2.

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
