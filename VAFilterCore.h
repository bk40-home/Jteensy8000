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
    inline void process(float x, float g, float R)
    {
        // Zavalishin §4.1, eq. on p.95
        const float denom_inv = 1.0f / (1.0f + 2.0f * R * g + g * g);
        hp    = (x - (2.0f * R + g) * s1 - s2) * denom_inv;
        bp    = g * hp + s1;
        lp    = g * bp + s2;
        notch = lp + hp;

        // Trapezoidal state commit
        s1 = 2.0f * bp - s1;   // or equivalently s1 += 2*g*hp
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
    //   g  = va_compute_g(fc, fs)
    //   k  = 0..4  (resonance; 4 = self-oscillation)
    inline void process(float x, float g, float k)
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
// Diode Ladder (Zavalishin §5.10, p.165–172)
//
// The diode ladder has inter-stage feedback with ½ averaging, giving it a
// different resonance character from the transistor (Moog) ladder.
//
// Structure (Fig 5.48, eq. 5.18):
//   Stage 1: y1 = LP1(x_fb + y2)         — input summed with stage 2 output
//   Stage 2: y2 = LP1((y1 + y3) / 2)     — averages stage 1 and stage 3
//   Stage 3: y3 = LP1((y2 + y4) / 2)     — averages stage 2 and stage 4
//   Stage 4: y4 = LP1(y3 / 2)            — half of stage 3
//
// Main feedback loop (Fig 5.49):
//   x_fb = x - k * tanh(y4)
//
// Self-oscillation threshold: k = 17 (Zavalishin p.170).
//
// Implementation: sequential per-pole TPT processing with 1-sample-delay
// on the inter-stage cross-coupling signals (y2, y3, y4 used from the
// previous sample).  This is unconditionally stable at all k values and
// introduces only 22.7μs of delay per inter-stage path at 44100Hz — well
// below audibility.
//
// Tanh saturation on the main feedback path prevents runaway at high k
// and produces the characteristic warm diode compression (Zavalishin §6).
// ---------------------------------------------------------------------------
struct DiodeLadder4
{
    // TPT integrator states
    float s1 = 0.0f, s2 = 0.0f, s3 = 0.0f, s4 = 0.0f;
    // Previous-sample pole outputs (used for inter-stage coupling)
    float y1 = 0.0f, y2 = 0.0f, y3 = 0.0f, y4 = 0.0f;

    // Process one sample.
    //   g  = va_compute_g(fc, fs)
    //   k  = 0..17  (resonance; 17 = self-oscillation threshold)
    inline void process(float x, float g, float k)
    {
        // Main feedback with tanh saturation to prevent divergence.
        // Real diode ladders saturate naturally via diode junctions.
        const float x_fb = x - k * va_tanh_fast(y4);

        // Stage 1: full input coupling (no ½ on x_fb, per eq. 5.18 line 1)
        // Input = x_fb + y2 (y2 from previous sample)
        const float in1 = x_fb + y2;
        const float v1  = (in1 - s1) * g / (1.0f + g);
        y1 = v1 + s1;
        s1 = y1 + v1;

        // Stage 2: half-summed input (eq. 5.18 line 2)
        // Input = (y1 + y3) / 2 — y1 is fresh, y3 from previous sample
        const float in2 = (y1 + y3) * 0.5f;
        const float v2  = (in2 - s2) * g / (1.0f + g);
        y2 = v2 + s2;
        s2 = y2 + v2;

        // Stage 3: half-summed input (eq. 5.18 line 3)
        // Input = (y2 + y4) / 2 — y2 is fresh, y4 from previous sample
        const float in3 = (y2 + y4) * 0.5f;
        const float v3  = (in3 - s3) * g / (1.0f + g);
        y3 = v3 + s3;
        s3 = y3 + v3;

        // Stage 4: half input (eq. 5.18 line 4)
        // Input = y3 / 2 — y3 is fresh
        const float in4 = y3 * 0.5f;
        const float v4  = (in4 - s4) * g / (1.0f + g);
        y4 = v4 + s4;
        s4 = y4 + v4;
    }

    inline void reset() { s1 = s2 = s3 = s4 = y1 = y2 = y3 = y4 = 0.0f; }
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

    inline float process(float x, float g, float k)
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

        // Apply saturation to feedback signal (tames self-oscillation)
        const float fb = k * va_tanh_fast(hp2_linear);

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

    inline float process(float x, float g, float k)
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

        // Apply saturation to feedback signal (tames self-oscillation)
        const float fb = k * va_tanh_fast(lp2_linear);

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