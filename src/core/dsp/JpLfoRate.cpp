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

// JE-8086 LFO2 free rate (2026-10-31: scene 18 C + scene 19 L, CC1 = 127,
// pitch tracker <= 112, FM demodulation above).  LFO2 does NOT share LFO1's
// law: one log segment, x1.542 per 16 bytes (0.78 Hz at 0 .. 16.2 Hz at 112,
// every point within 0.2 %), flattening to ~x1.11 per 4 bytes above 112, and
// a single top step to 50 Hz at 127.  (LFO1's law gives 5.42 Hz at byte 80
// where the JE's LFO2 runs at 6.81 Hz — the gap scene 19 exposed.)
constexpr Point kTable2[] = {
    {   0.0f, -0.246924f },   // 0.7812 Hz
    {  16.0f, +0.184985f },   // 1.203 Hz
    {  32.0f, +0.618531f },   // 1.856 Hz
    {  48.0f, +1.052254f },   // 2.864 Hz
    {  64.0f, +1.485393f },   // 4.417 Hz
    {  80.0f, +1.918612f },   // 6.811 Hz
    {  96.0f, +2.351842f },   // 10.5 Hz
    { 112.0f, +2.785166f },   // 16.2 Hz
    { 116.0f, +2.899937f },   // 18.17 Hz
    { 120.0f, +2.995232f },   // 19.99 Hz
    { 124.0f, +3.120910f },   // 22.67 Hz
    { 126.0f, +3.183749f },   // 24.14 Hz   (extrapolated: 120-124 slope; 125/126 not measured)
    { 127.0f, +3.911563f },   // 49.98 Hz    (measured: the JP's top step, FM demod)
};
constexpr size_t kCount2 = sizeof(kTable2) / sizeof(kTable2[0]);

// Log-linear interpolation over a byte -> ln(Hz) table (shared by both LFOs).
float interp(const Point* tab, size_t count, float norm01)
{
    const float n = (norm01 < 0.0f) ? 0.0f : (norm01 > 1.0f) ? 1.0f : norm01;
    const float r = n * kRateMax;

    // Segment [i, i+1] holding r; the last segment also takes r == 127.
    size_t i = 0;
    while (i + 2 < count && r > tab[i + 1].rate) ++i;

    const Point& a = tab[i];
    const Point& b = tab[i + 1];
    const float  t = (r - a.rate) / (b.rate - a.rate);
    return expf(a.lnHz + t * (b.lnHz - a.lnHz));
}

} // namespace

JT_COLD float hzFromNorm(float norm01)
{
    return interp(kTable, kCount, norm01);
}

JT_COLD float lfo2HzFromNorm(float norm01)
{
    return interp(kTable2, kCount2, norm01);
}

} // namespace JpLfoRate
} // namespace JT
