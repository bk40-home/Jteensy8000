// =============================================================================
// Curves.cpp — implementation of normalized <-> engineering conversions
// =============================================================================
// See Curves.h for the architectural role and the CPU/precision contracts.
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "core/dsp/Curves.h"

#include "core/AudioConfig.h"   // JT_COLD

#include <cmath>   // expf, logf, lroundf — f-suffixed only (-Wdouble-promotion)

namespace JT {
namespace Curves {

float clamp01(float v)
{
    // Branches, not fminf/fmaxf: on Cortex-M7 GCC compiles these to VMAXNM/
    // VMINNM-free compare+select which also filters NaN to the 0.0 side —
    // a NaN from a corrupt transport packet must never propagate into the
    // parameter store.
    if (!(v > 0.0f)) return 0.0f;   // catches v <= 0 AND NaN
    if (v > 1.0f)    return 1.0f;
    return v;
}

// -----------------------------------------------------------------------------
// Internal: clamp an engineering value into the parameter's range.
// Same NaN-filtering pattern as clamp01 — NaN lands on min.
// -----------------------------------------------------------------------------
static float clampRange(float v, float lo, float hi)
{
    if (!(v > lo)) return lo;
    if (v > hi)    return hi;
    return v;
}

float toEngineering(const Params::ParamDesc& d, float norm)
{
    const float t = clamp01(norm);

    switch (d.curve) {
        case Params::Curve::Log:
            // min * (max/min)^t, computed as expf(t * ln(max/min)) which is
            // one expf + one logf instead of powf's two.  The generator
            // guarantees min > 0 for Log curves, so the logf argument is
            // always valid.  (ln(max/min) is a per-param constant; if
            // profiling ever shows this on a hot path the constant can be
            // hoisted into the generated table — control rate does not
            // justify that today.)
            return d.min * expf(t * logf(d.max / d.min));

        case Params::Curve::Seg2:
            // Two linear segments meeting at 'mid' when the control sits at
            // its physical centre.  Carries v1's envelope-slope feel exactly
            // (Mapping.h cc_to_curve: 0.15 at bottom, 1.0 centre, 5.0 top).
            if (t <= 0.5f) return d.min + (t * 2.0f) * (d.mid - d.min);
            return d.mid + ((t - 0.5f) * 2.0f) * (d.max - d.mid);

        case Params::Curve::Lin:
        default:
            return d.min + t * (d.max - d.min);
    }
}

JT_COLD float toNorm(const Params::ParamDesc& d, float engineering)
{
    // Degenerate range (min == max) would divide by zero below.  The
    // generator's validator forbids min >= max for continuous params, but a
    // single-option Select would legitimately produce min == max == 0 —
    // guard here rather than trust every future table.
    if (!(d.max > d.min)) return 0.0f;

    const float e = clampRange(engineering, d.min, d.max);

    switch (d.curve) {
        case Params::Curve::Log:
            // Inverse of the Log branch above: t = ln(e/min) / ln(max/min).
            return clamp01(logf(e / d.min) / logf(d.max / d.min));

        case Params::Curve::Seg2:
            if (e <= d.mid) {
                // Guard the lower segment the same way as the full range —
                // mid == min would divide by zero.
                if (!(d.mid > d.min)) return 0.0f;
                return clamp01(0.5f * (e - d.min) / (d.mid - d.min));
            }
            if (!(d.max > d.mid)) return 1.0f;
            return clamp01(0.5f + 0.5f * (e - d.mid) / (d.max - d.mid));

        case Params::Curve::Lin:
        default:
            return clamp01((e - d.min) / (d.max - d.min));
    }
}

int toOptionIndex(const Params::ParamDesc& d, float norm)
{
    if (d.optionCount == 0) return 0;                    // not a Select
    // Engineering value for a Select IS the index (Lin, 0..count-1), so
    // round-nearest gives the intended option even after 7-bit quantisation.
    const long idx = lroundf(toEngineering(d, norm));
    if (idx < 0)                    return 0;
    if (idx >= (long)d.optionCount) return (int)d.optionCount - 1;
    return (int)idx;
}

float normFromOptionIndex(const Params::ParamDesc& d, int index)
{
    if (d.optionCount == 0) return 0.0f;
    if (index < 0)                    index = 0;
    if (index >= (int)d.optionCount)  index = (int)d.optionCount - 1;
    return toNorm(d, (float)index);
}

// -----------------------------------------------------------------------------
// Wire-width quantisation.  Round-nearest on the way out; exact division on
// the way in.  toXbit(fromXbit(v)) == v for every code — the identity every
// transport relies on so a value echoed back over MIDI never drifts.
// -----------------------------------------------------------------------------

uint16_t normTo14bit(float norm)
{
    return (uint16_t)lroundf(clamp01(norm) * 16383.0f);
}

float normFrom14bit(uint16_t v)
{
    if (v > 16383u) v = 16383u;      // defensive: callers pass raw MSB<<7|LSB
    return (float)v * (1.0f / 16383.0f);
}

uint8_t normTo7bit(float norm)
{
    return (uint8_t)lroundf(clamp01(norm) * 127.0f);
}

float normFrom7bit(uint8_t v)
{
    if (v > 127u) v = 127u;
    return (float)v * (1.0f / 127.0f);
}

} // namespace Curves
} // namespace JT
