// =============================================================================
// JpEnvLaw.cpp — implementation (law, measurements and units in the .h)
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "core/dsp/JpEnvLaw.h"

#include <cmath>   // expf — f-suffixed only (-Wdouble-promotion)
#include <cstddef>

#include "core/AudioConfig.h"   // JT_COLD, kBlockMs

namespace JT {
namespace JpEnvLaw {

namespace {

struct Point { float byte; float lnTau; };

// JE-8086 envelope time constant, knob byte -> ln(τ / s).  Stored as ln so the
// interpolation is one expf (log-linear between measured points).  Adjacent
// integer bytes with different values (116/117, 118/119, ...) are the JP's
// own steps, kept exactly.
constexpr Point kTable[] = {
    {   0.0f, -6.907755f },   // 0.001 s   (provisional)
    {   4.0f, -6.214608f },   // 0.002 s   (provisional)
    {   8.0f, -5.809143f },   // 0.003 s   (provisional (scene 19 disagrees))
    {   9.0f, -5.809143f },   // 0.003 s   (provisional)
    {  10.0f, -4.382027f },   // 0.0125 s
    {  11.0f, -4.342806f },   // 0.013 s
    {  12.0f, -4.219908f },   // 0.0147 s
    {  16.0f, -3.932226f },   // 0.0196 s
    {  20.0f, -3.653512f },   // 0.0259 s
    {  24.0f, -3.390257f },   // 0.0337 s
    {  28.0f, -3.134994f },   // 0.0435 s
    {  32.0f, -2.850767f },   // 0.0578 s
    {  40.0f, -2.391416f },   // 0.0915 s
    {  56.0f, -1.456717f },   // 0.233 s
    {  72.0f, -0.858022f },   // 0.424 s
    {  88.0f, -0.234457f },   // 0.791 s
    { 100.0f, +0.392042f },   // 1.48 s
    { 104.0f, +0.548121f },   // 1.73 s
    { 108.0f, +0.732368f },   // 2.08 s
    { 112.0f, +0.959350f },   // 2.61 s
    { 116.0f, +1.091923f },   // 2.98 s
    { 117.0f, +2.060514f },   // 7.85 s   (step)
    { 118.0f, +2.060514f },   // 7.85 s   (pair)
    { 119.0f, +2.161022f },   // 8.68 s
    { 120.0f, +2.161022f },   // 8.68 s   (pair)
    { 121.0f, +2.279316f },   // 9.77 s   (interpolated pair)
    { 122.0f, +2.279316f },   // 9.77 s   (interpolated pair)
    { 123.0f, +2.397895f },   // 11 s   (pair)
    { 124.0f, +2.397895f },   // 11 s
    { 125.0f, +2.557227f },   // 12.9 s   (interpolated pair)
    { 126.0f, +2.557227f },   // 12.9 s   (interpolated pair)
    { 127.0f, +2.714695f },   // 15.1 s
};
constexpr size_t kCount   = sizeof(kTable) / sizeof(kTable[0]);
constexpr float  kByteMax = 127.0f;

} // namespace

JT_COLD float tauFromNorm(float norm01)
{
    const float n = (norm01 < 0.0f) ? 0.0f : (norm01 > 1.0f) ? 1.0f : norm01;
    const float b = n * kByteMax;

    // Segment [i, i+1] holding b; the last segment also takes b == 127.
    size_t i = 0;
    while (i + 2 < kCount && b > kTable[i + 1].byte) ++i;

    const Point& p0 = kTable[i];
    const Point& p1 = kTable[i + 1];
    const float  t  = (b - p0.byte) / (p1.byte - p0.byte);
    return expf(p0.lnTau + t * (p1.lnTau - p0.lnTau));
}

JT_COLD float blockCoef(float tauSeconds)
{
    if (tauSeconds <= 0.0f) return 0.0f;
    return expf(-(kBlockMs * 0.001f) / tauSeconds);
}

} // namespace JpEnvLaw
} // namespace JT
