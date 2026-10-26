// =============================================================================
// JpLfoRate.cpp — implementation (law, measurements and units in the .h)
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "core/dsp/JpLfoRate.h"

#include <cmath>   // expf — f-suffixed only (-Wdouble-promotion)
#include <cstddef>

#include "core/AudioConfig.h"   // JT_COLD

namespace JT {
namespace JpLfoRate {

namespace {

struct Point { float rate; float lnHz; };

// JE-8086 LFO1 free rate, knob byte -> ln(Hz).  Stored as ln so the
// interpolation is one expf (log-linear between the measured points).
constexpr Point kTable[] = {
    {   0.0f, -3.020025f },   // 0.0488 Hz
    {   4.0f, -2.772589f },   // 0.0625 Hz
    {   8.0f, -2.515778f },   // 0.0808 Hz
    {  12.0f, -2.251892f },   // 0.1052 Hz
    {  16.0f, -1.985587f },   // 0.1373 Hz
    {  20.0f, -1.723727f },   // 0.1784 Hz
    {  24.0f, -1.461880f },   // 0.2318 Hz
    {  28.0f, -1.202973f },   // 0.3003 Hz
    {  32.0f, -0.944433f },   // 0.3889 Hz
    {  40.0f, -0.422052f },   // 0.6557 Hz
    {  48.0f, +0.098940f },   // 1.104 Hz
    {  56.0f, +0.618478f },   // 1.8561 Hz
    {  64.0f, +1.171181f },   // 3.2258 Hz   <- knee
    {  72.0f, +1.430837f },   // 4.1822 Hz
    {  80.0f, +1.690963f },   // 5.4247 Hz
    {  88.0f, +1.951225f },   // 7.0373 Hz
    {  96.0f, +2.206570f },   // 9.0845 Hz
    { 104.0f, +2.464406f },   // 11.7565 Hz
    { 112.0f, +2.731969f },   // 15.3631 Hz
    { 120.0f, +2.994346f },   // 19.9723 Hz
    { 127.0f, +3.224858f },   // 25.15 Hz    (extrapolated, upper segment)
};
constexpr size_t kCount = sizeof(kTable) / sizeof(kTable[0]);
constexpr float  kRateMax = 127.0f;

} // namespace

JT_COLD float hzFromNorm(float norm01)
{
    const float n = (norm01 < 0.0f) ? 0.0f : (norm01 > 1.0f) ? 1.0f : norm01;
    const float r = n * kRateMax;

    // Segment [i, i+1] holding r; the last segment also takes r == 127.
    size_t i = 0;
    while (i + 2 < kCount && r > kTable[i + 1].rate) ++i;

    const Point& a = kTable[i];
    const Point& b = kTable[i + 1];
    const float  t = (r - a.rate) / (b.rate - a.rate);
    return expf(a.lnHz + t * (b.lnHz - a.lnHz));
}

} // namespace JpLfoRate
} // namespace JT
