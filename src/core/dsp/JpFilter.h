// =============================================================================
// JpFilter.h — the JP-8000 filter, fitted to the JE-8086 (VA types 17..22)
// =============================================================================
//
// ROLE
//   The JP-8000's LPF / HPF / BPF at -12 or -24 dB/oct, as six filter.va_type
//   options (17 JP LPF-24, 18 JP LPF-12, 19 JP HPF-24, 20 JP HPF-12,
//   21 JP BPF-24, 22 JP BPF-12).  The JP .syx importer maps the patch's type
//   x slope bytes straight onto these, so an imported patch gets the JP's own
//   cutoff law, slopes and resonance instead of a nearest JT topology.
//
// WHAT THE JE DOES (syx-import/scene14-noise-filter-analysis-2026-10-26.md)
//   * ONE cutoff law for all six modes, exponential in the knob.
//   * The slope byte applies to every type: LPF -12/-24, HPF +12/+24,
//     BPF +-6 (-12) / +-12 (-24) dB/oct.
//   * -24 is TWO IDENTICAL resonant 2-pole sections in cascade: its
//     peak-over-passband is exactly twice the -12 section's (within ~1 dB).
//   * The -24 output is trimmed by ~1/Q, which is the JP's "bass loss with
//     resonance" — and it hits the HPF -24 passband identically, which a
//     ladder cannot do.  The -12 modes lose only ~(R/R0)^0.1.
//
// MODEL (fitted at the firmware's 48 kHz, fitjp_final.py, capture_tools)
//   Section: Zavalishin TPT SVF (§4.1), R = damping = 1/(2Q):
//       LP = 1/D,  HP = s^2/D,  BP = s/D,   D = s^2 + 2Rs + 1
//   -12 = section 1,                 output x (R/R0)^0.1
//   -24 = section 1 -> section 2,    output x (R/R0)
//   fc  = 549.66 Hz * 2^((code - 64) / 11.959),  code = knob * 127
//         (C1: fitted to the JE's high-resonance ring lines = pole frequency)
//         ceiling: LP 0.49 fs (open at the top, as the JE); HP/BP 12.40 kHz
//   R   = log-linear between knots at reso 0/32/64/96 (0.4802, 0.3297, 0.1503,
//         0.03845; C1 refit) and, calibrated to the JE's decay (R4), 104/112/116/120/
//         124/127 (0.01938, 0.00875, 0.00657, 0.00333, 0.00148, 0.000852):
//         Q 1 ... 587
//   trim: -24 x R/R0 (also checked against the JE's level at 104..127)
//         x levelComp (R5a): at reso 120..127 the JE's resonance does not
//         get louder with cutoff the way a fixed-trim linear filter does;
//         -8.2 / -9.2 / -9.9 dB from cutoff 64 to 112 (scene 16).
//   Fit error against the JE: 1.75 dB RMS at reso 0 (about the noise
//   estimate's own scatter), 1.87 dB at reso 32..96.
//
// COST
//   Control rate: one exp2f for the cutoff and one for R, plus one powf for
//   the -12 trim — only when a knob moves (FilterSection's dirty flag).  The
//   per-block cutoff fold adds one divide (setCoeffs) to the tanf it already
//   pays.  Audio rate: one section is 2 mul-adds + 1 mul + 4 adds more than
//   nothing, with NO divide (the SVF2 core divides per sample; this one
//   precomputes d).  -24 runs two sections ~= the Moog LP4 cost.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>

namespace JT {

class JpFilter {
public:
    // Option order == filter.va_type 17..22 minus 17.  Frozen with the table.
    enum class Mode : uint8_t { LP24 = 0, LP12, HP24, HP12, BP24, BP12 };
    static constexpr int kNumModes = 6;

    // ---- laws (control rate; static so tests can assert them directly) ----

    // Cutoff in Hz for a knob code 0..127.  Fractional codes are fine (a
    // gliding knob arrives as norm * 127), so the sweep stays smooth.
    static float cutoffHz(float code);

    // Upper clamp for the MODULATED cutoff: LP 0.49 fs, HP/BP 12.35 kHz.
    static float ceilingHz(Mode m, float fs);

    // Section damping R = 1/(2Q) for a resonance code 0..127.
    static float damping(float resoCode);

    // Output trim for mode m at resonance code 0..127 (the JP's level law):
    // -24 x R/R0, -12 x (R/R0)^0.1, R = damping(resoCode).
    static float outputTrim(Mode m, float resoCode);

    // R5a: extra level reduction of the -24 modes at HIGH resonance and HIGH
    // cutoff (linear gain, 1.0 = none).  Uses the cutoff KNOB code, not the
    // modulated cutoff, so it is control-rate only.  1.0 for the -12 modes,
    // at reso <= 116 and at cutoff <= 64.
    static float levelComp(Mode m, float resoCode, float cutCode);

    static bool isCascade(Mode m)
    {
        return m == Mode::LP24 || m == Mode::HP24 || m == Mode::BP24;
    }

    // ---- block rate ----
    // g = tan(pi fc / fs) (FilterSection already computes it), R = damping.
    void setCoeffs(float g, float R);
    void reset();

    // ---- audio rate ----
    // One sample.  The mode is a template parameter so FilterSection's
    // hoisted type switch gets a branch-free loop per mode.
    template <Mode M>
    inline float tick(float x)
    {
        float hp, bp, lp;
        section(x, _s1a, _s2a, hp, bp, lp);
        if constexpr (M == Mode::LP12) return lp;
        if constexpr (M == Mode::HP12) return hp;
        if constexpr (M == Mode::BP12) return bp;

        // -24: the same section again, fed from section 1's matching tap.
        const float x2 = (M == Mode::LP24) ? lp : (M == Mode::HP24) ? hp : bp;
        section(x2, _s1b, _s2b, hp, bp, lp);
        if constexpr (M == Mode::LP24) return lp;
        if constexpr (M == Mode::HP24) return hp;
        return bp;
    }

private:
    // Zavalishin §4.1 p.95, the exact linear ZDF SVF solve with the divide
    // hoisted:  hp = (x - (2R+g) s1 - s2) / (1 + 2Rg + g^2).  Identical maths
    // to SVF2::process; only the per-sample reciprocal is gone.
    inline void section(float x, float& s1, float& s2,
                        float& hp, float& bp, float& lp) const
    {
        hp = (x - _a * s1 - s2) * _d;
        bp = _g * hp + s1;
        lp = _g * bp + s2;
        s1 = 2.0f * bp - s1;          // trapezoidal state commit
        s2 = 2.0f * lp - s2;
    }

    float _g = 0.0f;                  // tan(pi fc / fs)
    float _a = 1.0f;                  // 2R + g
    float _d = 1.0f;                  // 1 / (1 + 2Rg + g^2)
    float _s1a = 0.0f, _s2a = 0.0f;   // section 1 states (BP, LP)
    float _s1b = 0.0f, _s2b = 0.0f;   // section 2 states (-24 modes only)
};

} // namespace JT
