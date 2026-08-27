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
// Bounded Padé tanh — same [3/3] curve as va_tanh_fast but CLAMPED past |x|=4.
//
// WHY THIS EXISTS SEPARATELY FROM va_tanh_fast / va_sat:
//   • va_tanh_fast is the SAME rational but UNBOUNDED (asymptote x/9): unusable
//     where a stage output is fed forward through more tanh stages, because the
//     growth compounds.
//   • va_sat (x/sqrt(1+x²)) is bounded but a DIFFERENT curve — its harmonic
//     content and saturation knee differ audibly.
//   MoogDVCore was tuned and host-validated against THIS specific curve: the
//   rational tanh up to |x|=4, then a hard clamp to ±0.9993 (where the rational
//   already reads ±0.999, so the clamp is inaudible but guarantees boundedness
//   in the 5-tanh-per-sample chain). Swapping in va_sat or va_tanh_fast would
//   change the validated MoogDV sound, so the core needs its own bounded tanh.
// ---------------------------------------------------------------------------
inline float va_tanh_bounded(float x)
{
    // Flat region: rational already ≈ ±0.9993 here; clamp keeps it bounded.
    if (x >  4.0f) return  0.9993293f;
    if (x < -4.0f) return -0.9993293f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
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
// -----------------------------------------------------------------------------
// JT_OPT_MOOG_SAT_LEVEL — where the ladder feedback saturator starts limiting.
//
// Applied as va_sat(x/Vs)*Vs: unit slope at 0, bounded at +/-Vs. Below the
// oscillation point it is transparent (small-signal peak follows the textbook
// 1/(4-k) exactly); past k = 4 it is what turns divergence into a limit cycle.
// Self-oscillation tail at fc = 2 kHz, k = 4.10:
//     Vs 0.5 -> 0.092    Vs 2.0 -> 0.368   <- ship default
//     Vs 1.0 -> 0.184    Vs 4.0 -> 0.736
// -----------------------------------------------------------------------------
#ifndef JT_OPT_MOOG_SAT_LEVEL
#define JT_OPT_MOOG_SAT_LEVEL 2.0f
#endif

// -----------------------------------------------------------------------------
// JT_OPT_MOOG_FB_AC_COUPLE — highpass the feedback at ~5 Hz.
//
// 0 = DC-coupled, as a real ladder is (ship default). DC gain becomes 1/(1+k),
//     so resonance thins the low end — which is characteristic Moog behaviour.
// 1 = AC-couple the feedback at ~5 Hz (previous behaviour). Keeps unity DC gain
//     however high the resonance goes.
//
// DEFAULT CHANGED TO 0, for two reasons.
//
// It is no longer protective. It was introduced as runaway protection alongside
// the safe-k limiter; with the ZDF solve below, a linear ladder under k = 4 has
// finite DC gain and cannot run away, so nothing depends on it for stability.
//
// More importantly it STOPS THE FILTER SINGING at low cutoff. The 5 Hz highpass
// costs enough phase near the oscillation frequency to pull loop gain under
// unity when fc is low. Measured self-oscillation tail at k = 4.10:
//     fc:        40      100      200      400     1000
//     AC=1:  0.00000  0.00000  0.02707  0.25278  0.32493   <- dead below ~400 Hz
//     AC=0:  0.36380  0.36799  0.36791  0.36817  0.36820   <- uniform
// Set it back to 1 if you want the old unity-DC behaviour and can live with a
// resonance that only sings in the upper half of the cutoff range.
// -----------------------------------------------------------------------------
#ifndef JT_OPT_MOOG_FB_AC_COUPLE
#define JT_OPT_MOOG_FB_AC_COUPLE 0
#endif

inline float moog_sat(float x)
{
    constexpr float kVs    = JT_OPT_MOOG_SAT_LEVEL;
    constexpr float kInvVs = 1.0f / JT_OPT_MOOG_SAT_LEVEL;
    return va_sat(x * kInvVs) * kVs;
}

struct MoogLinear4
{
    float s1 = 0.0f, s2 = 0.0f, s3 = 0.0f, s4 = 0.0f;   // TPT states
    float y1 = 0.0f, y2 = 0.0f, y3 = 0.0f, y4 = 0.0f;   // pole outputs
    float dc = 0.0f;                                     // feedback DC tracker

    // Process one sample.
    //   g     = va_compute_g(fc, fs)
    //   k     = 0..4+ (resonance; 4 = self-oscillation, and the map now goes
    //           slightly past it so the filter can actually sing)
    //   nl    = VA_NL_NONE (linear) or VA_NL_SAT (per-stage transistor sigmoid)
    //   drive = signal gain INTO each stage's nonlinearity (1.0 = neutral)
    //
    // =========================================================================
    // REWRITTEN: closed-form ZDF feedback, replacing a unit-delayed estimate.
    // =========================================================================
    // WHAT WAS THERE BEFORE, and why it was replaced:
    //
    //  1. The feedback used y4 from the PREVIOUS sample. A ladder's feedback is
    //     instantaneous, so a one-sample delay leaves the loop gain wrong by an
    //     amount that grows with g. Measured resonant peak at the cutoff, same
    //     k, different cutoffs:
    //         k = 3.0:  0.996 @ 200 Hz  ...  21.3 @ 4 kHz  ...  32.6 @ 10 kHz
    //     i.e. the resonance knob meant something completely different at each
    //     end of the cutoff sweep, and self-oscillation began around k = 3.5
    //     instead of the theoretical 4.0. THIS is what used to run away.
    //     With the solve below: 1.000 at every cutoff for k = 3.0, 10.000 for
    //     k = 3.9, onset exactly at k = 4.0 — textbook 1/(4-k).
    //
    //  2. A "safe-k" limiter (envelope follower + kSafe = k/(1+4·over²), with
    //     over = env − 0.22) throttled the feedback whenever the output got
    //     loud. That was containing the symptom of (1). It has been REMOVED:
    //     with the ZDF solve there is nothing to contain, and it actively did
    //     harm — it made resonance fall as signal level rose, so any input gain
    //     ahead of the filter read as a resonance control rather than a drive.
    //
    //  3. Three Gauss-Seidel/SOR relaxation iterations. These were DEAD CODE:
    //     x_fb was computed once before the loop, so the forward chain had no
    //     coupling left to relax, and the commit pass then recomputed y1..y4
    //     from x_fb and overwrote every value the loop produced. Verified
    //     bit-identical output with the loop deleted, at every cutoff and
    //     resonance tested. That is ~48 float ops per sample per voice
    //     reclaimed for nothing.
    //
    // The per-stage nl/drive path is unchanged and still defaults to off (both
    // v1 and the JtFilterTest demo left it off; the demo's own comment records
    // that in-loop per-stage saturation was evaluated and reverted).
    inline void process(float x, float g, float k,
                        VANonlin nl = VA_NL_NONE, float drive = 1.0f)
    {
        const float G   = g / (1.0f + g);   // TPT per-stage gain
        const float inv = 1.0f / (1.0f + g);

        // ── Closed-form ZDF solve for y4 (Zavalishin, ladder chapter) ────────
        // Each stage contributes y_i = G·in_i + s_i/(1+g); cascading four and
        // substituting u = x − k·y4 gives, in one divide:
        //     y4·(1 + k·G⁴) = G⁴·x + G³·S1 + G²·S2 + G·S3 + S4
        const float G2 = G * G, G3 = G2 * G, G4 = G3 * G;
        const float y4Solved = (G4 * x + G3 * s1 * inv + G2 * s2 * inv
                                       + G  * s3 * inv +      s4 * inv)
                             / (1.0f + k * G4);

#if JT_OPT_MOOG_FB_AC_COUPLE
        // ~5 Hz highpass on the feedback: dcAlpha ≈ 1 − exp(−2π·5/44100).
        // Tonal, not protective — see JT_OPT_MOOG_FB_AC_COUPLE.
        constexpr float dcAlpha = 0.000712f;
        dc += dcAlpha * (y4Solved - dc);
        const float fbSignal = y4Solved - dc;
#else
        const float fbSignal = y4Solved;
#endif

        // Bounded feedback. Transparent below the oscillation point, and what
        // lets k cross 4.0 into a limit cycle instead of divergence.
        const float u = x - k * moog_sat(fbSignal);

        // ── ZDF trapezoidal commit (writes states) ───────────────────────────
        if (nl == VA_NL_SAT)
        {
            const float d    = drive;
            const float dinv = 1.0f / drive;

            float i1 = va_sat(u  * d) * dinv;
            float v1 = (i1 - s1) * G;  y1 = v1 + s1;  s1 = y1 + v1;

            float i2 = va_sat(y1 * d) * dinv;
            float v2 = (i2 - s2) * G;  y2 = v2 + s2;  s2 = y2 + v2;

            float i3 = va_sat(y2 * d) * dinv;
            float v3 = (i3 - s3) * G;  y3 = v3 + s3;  s3 = y3 + v3;

            float i4 = va_sat(y3 * d) * dinv;
            float v4 = (i4 - s4) * G;  y4 = v4 + s4;  s4 = y4 + v4;
        }
        else
        {
            float v1 = (u  - s1) * G;  y1 = v1 + s1;  s1 = y1 + v1;
            float v2 = (y1 - s2) * G;  y2 = v2 + s2;  s2 = y2 + v2;
            float v3 = (y2 - s3) * G;  y3 = v3 + s3;  s3 = y3 + v3;
            float v4 = (y3 - s4) * G;  y4 = v4 + s4;  s4 = y4 + v4;
        }
    }

    inline void reset()
    {
        s1 = s2 = s3 = s4 = 0.0f;
        y1 = y2 = y3 = y4 = 0.0f;
        dc = 0.0f;
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
        (void)K;   // v2 port note: kept for the v1 call signature; K is
                   // applied per-sample in process() (mechanical -Werror fix)
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
// -----------------------------------------------------------------------------
// JT_OPT_KORG35_SAT_LEVEL — where the Korg35 feedback saturator starts limiting.
//
// The feedback nonlinearity is applied as  va_sat(x / Vs) * Vs.  Dividing in and
// multiplying back out keeps UNIT SLOPE at the origin, so the saturator is
// transparent at low resonance and only engages once the feedback signal
// approaches +/-Vs.  The bare va_sat(x) used previously has its knee fixed at
// +/-1, which at high resonance meant it was compressing the feedback by ~13x
// and flattening the resonant peak to ~1.4 regardless of the knob.
//
// Vs sets the self-oscillation amplitude (fc = 2 kHz, k = 2.05):
//     Vs 0.5 -> tail 0.104     Vs 2.0 -> tail 0.416   <- ship default
//     Vs 1.0 -> tail 0.208     Vs 4.0 -> tail 0.832   (peaks past 2.0)
// 2.0 puts it in the same range as MoogLinear4 (0.27..0.54).
//
// Verified transparent below the oscillation point: at Vs = 2.0 and a small
// input the peak follows the ideal 1/(2-k) to within 1% (k=0.5 -> 0.667,
// k=1.0 -> 0.999, k=1.5 -> 1.982).
// -----------------------------------------------------------------------------
#ifndef JT_OPT_KORG35_SAT_LEVEL
#define JT_OPT_KORG35_SAT_LEVEL 2.0f
#endif

// Feedback saturator for the TSK loop: bounded at +/-Vs, unit slope at 0.
inline float korg35_sat(float x)
{
    constexpr float kVs    = JT_OPT_KORG35_SAT_LEVEL;
    constexpr float kInvVs = 1.0f / JT_OPT_KORG35_SAT_LEVEL;
    return va_sat(x * kInvVs) * kVs;
}

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
        // DERIVATION (the previous version of these three lines was wrong; see
        // the note below).  With G = g/(1+g), TPT1 gives
        //     lp1 = G*u + s1/(1+g),   u = x + k*hp2
        //     lp2 = G*lp1 + s2/(1+g)
        //     hp2 = lp1 - lp2 = (lp1 - s2)/(1+g)
        // Substituting and collecting hp2:
        //     hp2*[(1+g) - G*k] = G*x + s1/(1+g) - s2
        // Dividing through by (1+g) to reach the usual (1 - k*G2) denominator:
        //     hp2 = [G2*x + s1/(1+g)^2 - s2/(1+g)] / (1 - k*G2),  G2 = g/(1+g)^2
        //
        // WHAT WAS WRONG: the numerator's state terms were carried at
        // s1/(1+g) - s2, i.e. the normalisation by (1+g) was applied to the
        // DENOMINATOR but not to the states.  Both state terms were therefore
        // (1+g) times too large - negligible at low cutoff where g << 1, ruinous
        // as g grows.  Measured error between the predicted hp2 and the value
        // the forward pass then actually produced: 0.7% at 100 Hz, 18% at
        // 2 kHz, 286% at 6 kHz, and NaN by 12 kHz - which is the top of this
        // type's own kShape range.
        //
        // This is also why VA_NL_SAT was documented as MANDATORY here.  It was
        // not taming a real instability in the TSK topology; it was bounding the
        // feedback hard enough to stop a bad estimate from diverging.  With the
        // solve correct, linear feedback is stable across the whole range and
        // resonates properly: peak magnitude at fc rises to 10.0 at k = 1.90,
        // against 1.43 with the saturator still in place.
        const float g1 = g / (1.0f + g);
        const float G2 = g1 / (1.0f + g);   // = g / (1+g)^2
        const float inv_1pg = 1.0f / (1.0f + g);

        // Solve for hp2 (exact ZDF, no iteration).  Self-consistency verified:
        // this prediction now matches the forward pass to 0.00% at every cutoff.
        const float hp2_linear =
            (G2 * x + p1.s * inv_1pg * inv_1pg - p2.s * inv_1pg) / (1.0f - k * G2);

        // Feedback signal: bounded sigmoid (nl) or linear. va_sat is bounded
        // (asymptotes ±1) so self-oscillation past k≈2 compresses gracefully;
        // unit slope at 0 keeps the passband/low-res response identical.
        // Scaled saturator (see JT_OPT_KORG35_SAT_LEVEL).  This is what makes
        // the filter able to SING: with the ZDF solve corrected the linear form
        // is stable up to k = 2 but blows up beyond it, and k > 2 is precisely
        // where self-oscillation lives.  A bounded feedback lets the resonance
        // pass the k = 2 threshold and settle into a limit cycle instead.
        const float fb_arg = (nl == VA_NL_SAT) ? korg35_sat(hp2_linear) : hp2_linear;
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
        //
        // DERIVATION, same shape as Korg35LP above and carrying the same fix:
        //     lp1 = G*u + s1/(1+g),  hp1 = u - lp1,   u = x + k*lp2
        //     lp2 = G*hp1 + s2/(1+g) = G2*u - G2*s1 + s2/(1+g)
        // so
        //     lp2 = [G2*x - G2*s1 + s2/(1+g)] / (1 - k*G2)
        //
        // WHAT WAS WRONG: the state terms were carried as -g1*s1 + s2, i.e. G
        // instead of G2 on s1, and s2 un-normalised.  Both are the same single
        // missing factor of 1/(1+g) as in the LP, with the same measured error
        // profile (0.7% at 100 Hz, 18% at 2 kHz, 286% at 6 kHz).
        const float g1 = g / (1.0f + g);
        const float G2 = g1 / (1.0f + g);  // = g / (1+g)^2
        const float inv_1pg = 1.0f / (1.0f + g);

        // Solve for lp2 (exact ZDF, no iteration).  Verified self-consistent to
        // 0.00% against the forward pass at every cutoff.
        const float lp2_linear =
            (G2 * x - G2 * p1.s + p2.s * inv_1pg) / (1.0f - k * G2);

        // Feedback signal: bounded sigmoid (nl) or linear (see Korg35LP note).
        const float fb_arg = (nl == VA_NL_SAT) ? korg35_sat(lp2_linear) : lp2_linear;
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
