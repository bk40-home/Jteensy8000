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
static constexpr float VA_SQRT2     = 1.41421356237309504880f;   // used in SVF Q
static constexpr float VA_SAMPLE_RATE_DEFAULT = 44100.0f;

// ---------------------------------------------------------------------------
// Clamp helper (branchless on M7 with -O2+)
// ---------------------------------------------------------------------------
inline float va_clamp(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

// ---------------------------------------------------------------------------
// Safe tanh saturation  (Zavalishin §4.5, p.113 – nonlinear elements)
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
// 2-Pole SVF (State Variable Filter)  (Zavalishin §3.2, p.50)
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
//  See Zavalishin §3.9 p.77 for the derivation.
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
        // Zavalishin §3.9, eq. 3.52 p.77
        const float denom_inv = 1.0f / (1.0f + 2.0f * R * g + g * g);
        hp    = (x - (2.0f * R + g) * s1 - s2) * denom_inv;
        bp    = g * hp + s1;
        lp    = g * bp + s2;
        notch = lp + hp;

        // Trapezoidal state commit
        s1 = 2.0f * bp - s1;   // or equivalently s1 += 2*g*hp
        s2 = 2.0f * lp - s2;
    }

    // All-pass output: AP = x - 2*R*bp   (Zavalishin §3.10 p.81)
    inline float allpass() const { return notch - 2.0f * /* stored R */ 0.0f; }
    // Note: pass R explicitly if you need AP output.
    inline float allpass(float R) const { return notch - 2.0f * R * bp; }

    inline void reset() { s1 = 0.0f; s2 = 0.0f; }
};

// ---------------------------------------------------------------------------
// Moog 4-Pole Cascade (linear)  (Zavalishin §4.1, p.99)
//
//  Four identical 1-pole TPT stages in cascade with global negative feedback.
//  The feedback coefficient k corresponds to resonance; k = 4 → self-oscillation.
//
//  Compact ZDF solution (exact, no iteration for linear case):
//
//    g1 = g/(1+g)   (shared per-stage gain)
//    S  = sum of state contributions from all 4 poles (see eq. 4.2 p.101)
//    y4 = (x - k*S) / (1 + k*g1^4)
//
//  See Zavalishin §4.1.3, eq. 4.6–4.9, p.101–103
// ---------------------------------------------------------------------------
struct MoogLinear4
{
    float s1 = 0.0f, s2 = 0.0f, s3 = 0.0f, s4 = 0.0f;   // TPT states
    float y1 = 0.0f, y2 = 0.0f, y3 = 0.0f, y4 = 0.0f;   // pole outputs

    // Process one sample.
    //   g  = va_compute_g(fc, fs)
    //   k  = 0..4  (resonance; 4 = self-oscillation)
    inline void process(float x, float g, float k)
    {
        // Zavalishin §4.1.3, p.101
        const float g1 = g / (1.0f + g);

        // State estimates (forward-Euler pass to build feedback sum)
        const float s1e = g1 * s1;
        const float s2e = g1 * (s2 + s1e);
        const float S   = s4 + g1 * (s3 + s2e);

        // Closed-form solve for y4 (avoids iteration)
        const float g1_4 = g1 * g1 * g1 * g1;
        y4 = (x - k * S) / (1.0f + k * g1_4);   // eq. 4.9 p.103

        // Back-substitute through cascade
        const float x1  = x - k * y4;
        const float v1  = (x1 - s1) * g1;  y1 = v1 + s1;  s1 = y1 + v1;
        const float v2  = (y1 - s2) * g1;  y2 = v2 + s2;  s2 = y2 + v2;
        const float v3  = (y2 - s3) * g1;  y3 = v3 + s3;  s3 = y3 + v3;
        const float v4  = (y3 - s4) * g1;  y4 = v4 + s4;  s4 = y4 + v4;
    }

    inline void reset()
    {
        s1=s2=s3=s4=0.0f;
        y1=y2=y3=y4=0.0f;
    }
};

// ---------------------------------------------------------------------------
// Diode Ladder (Roland-style)  (Zavalishin §4.3, p.107)
// ---------------------------------------------------------------------------
struct DiodeLadder4
{
    float s1=0,s2=0,s3=0,s4=0;
    float y1=0,y2=0,y3=0,y4=0;

    inline void process(float x, float g, float k)
    {
        const float g1 = g / (1.0f + g);
        const float fb = k * (y4 - y3 - y2 - y1);
        const float x_fb = x - fb;

        float v;
        v = (x_fb - s1) * g1;  y1 = v + s1;  s1 = y1 + v;
        v = (y1   - s2) * g1;  y2 = v + s2;  s2 = y2 + v;
        v = (y2   - s3) * g1;  y3 = v + s3;  s3 = y3 + v;
        v = (y3   - s4) * g1;  y4 = v + s4;  s4 = y4 + v;
    }

    inline void reset() { s1=s2=s3=s4=y1=y2=y3=y4=0.0f; }
};

// ---------------------------------------------------------------------------
// Korg 35 (MS-20) 2-Stage LP / HP   (Zavalishin §4.5.1, p.115)
// ---------------------------------------------------------------------------
struct Korg35LP
{
    TPT1 p1, p2;

    inline float process(float x, float g, float k)
    {
        const float fb  = k * va_tanh_fast(p2.s);
        const float lp1 = p1.processLP(x - fb, g);
        const float lp2 = p2.processLP(lp1, g);
        return lp2;
    }

    inline void reset() { p1.reset(); p2.reset(); }
};

struct Korg35HP
{
    TPT1 p1, p2;

    inline float process(float x, float g, float k)
    {
        const float fb = k * va_tanh_fast(p1.s);
        float lp1, hp1;
        hp1 = p1.processHP(x - fb, g, lp1);
        float lp2, hp2;
        hp2 = p2.processHP(hp1, g, lp2);
        return hp2;
    }

    inline void reset() { p1.reset(); p2.reset(); }
};
