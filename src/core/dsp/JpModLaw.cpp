// =============================================================================
// JpModLaw.cpp — implementation (curve, measurements and units in the .h)
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "core/dsp/JpModLaw.h"

namespace JT {
namespace JpModLaw {

namespace {
// D(d) = kA·d² + kB·d³ cents, least-squares fit to the four JE-8086 LFO1
// depth points (2026-10-24 capture, L0 group).
constexpr float kA = 0.8035f;
constexpr float kB = -0.003236f;
constexpr float kJpDepthMax = 63.0f;     // JP signed depth knob range ±63
} // namespace

float depthCents(float depth01)
{
    const float a = (depth01 < 0.0f) ? -depth01 : depth01;
    const float d = a * kJpDepthMax;
    const float c = d * d * (kA + kB * d);
    return (depth01 < 0.0f) ? -c : c;
}

float lfoPitchSemis(float depth01)
{
    return depthCents(depth01) * 0.01f;
}

float envPitchSemis(float depth01)
{
    return depthCents(depth01) * 0.02f;
}

} // namespace JpModLaw
} // namespace JT
