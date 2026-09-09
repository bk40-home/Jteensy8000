// =============================================================================
// Asrc.cpp — see Asrc.h.
// =============================================================================
#include "Asrc.h"

#include <cstring>

namespace JT {

namespace {

// Full scale for a 24-bit signed sample.
constexpr float kSample24Max = 8388607.0f;

// Ring capacity is a power of two, so wrapping is a mask rather than a
// modulo — this runs per output sample.
constexpr uint32_t kRingMask = static_cast<uint32_t>(kAsrcRingFrames - 1u);
static_assert((kAsrcRingFrames & (kAsrcRingFrames - 1u)) == 0u,
              "kAsrcRingFrames must be a power of two");

// 4-point third-order Hermite, the standard formulation.  'frac' is the
// position between y1 and y2, in [0,1).  y0 and y3 are the neighbours that
// give the curve its slope at each end.
inline float hermite(float y0, float y1, float y2, float y3, float frac)
{
    const float c0 = y1;
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - (2.5f * y1) + (2.0f * y2) - (0.5f * y3);
    const float c3 = (0.5f * (y3 - y1)) + (1.5f * (y1 - y2));
    return (((((c3 * frac) + c2) * frac) + c1) * frac) + c0;
}

// Convert to a 24-bit sample, clamping rather than wrapping.  Interpolation
// can overshoot slightly past a full-scale input, and a wrap would turn that
// into a click at maximum amplitude.
inline int32_t toSample24(float value)
{
    const float scaled = value * kSample24Max;
    if (scaled >= kSample24Max)  { return  static_cast<int32_t>(kSample24Max); }
    if (scaled <= -kSample24Max) { return -static_cast<int32_t>(kSample24Max); }
    return static_cast<int32_t>(scaled);
}

} // namespace

// -----------------------------------------------------------------------------

void Asrc::begin(uint32_t ratioQ16)
{
    memset(ringL, 0, sizeof(ringL));
    memset(ringR, 0, sizeof(ringR));
    writeIndex.store(0u, std::memory_order_relaxed);
    readIndex.store(0u, std::memory_order_relaxed);
    ratio.store(ratioQ16, std::memory_order_relaxed);
    phase = 0u;
    resetCounters();

    // Prime the ring to its target so the first pull has history to
    // interpolate from and the servo starts at zero error rather than
    // chasing a ring that began empty.
    writeIndex.store(static_cast<uint32_t>(kAsrcTargetFill),
                     std::memory_order_release);
}

void Asrc::resetCounters(void)
{
    underrunCount.store(0u, std::memory_order_relaxed);
    overrunCount.store(0u, std::memory_order_relaxed);
}

size_t Asrc::fill(void) const
{
    const uint32_t w = writeIndex.load(std::memory_order_acquire);
    const uint32_t r = readIndex.load(std::memory_order_acquire);
    return static_cast<size_t>((w - r) & kRingMask);
}

size_t Asrc::push(const float* left, const float* right, size_t frames)
{
    if (left == nullptr || right == nullptr) { return 0u; }

    uint32_t w = writeIndex.load(std::memory_order_relaxed);
    const uint32_t r = readIndex.load(std::memory_order_acquire);

    // One slot is left unused so full and empty stay distinguishable.
    const size_t space = static_cast<size_t>((r - w - 1u) & kRingMask);
    const size_t take  = (frames > space) ? space : frames;

    for (size_t i = 0u; i < take; ++i) {
        ringL[w] = left[i];
        ringR[w] = right[i];
        w = (w + 1u) & kRingMask;
    }
    writeIndex.store(w, std::memory_order_release);

    if (take < frames) {
        // The consumer stopped taking, or the ratio has drifted far enough
        // that the ring filled.  Either way samples were lost.
        overrunCount.fetch_add(1u, std::memory_order_relaxed);
    }
    return take;
}

void Asrc::pull(int32_t* interleaved, size_t frames, uint8_t channels)
{
    if (interleaved == nullptr || channels == 0u) { return; }

    const uint32_t step = ratio.load(std::memory_order_relaxed);
    uint32_t r = readIndex.load(std::memory_order_relaxed);
    size_t at = 0u;

    for (size_t frame = 0u; frame < frames; ++frame) {
        const uint32_t w = writeIndex.load(std::memory_order_acquire);
        const size_t available = static_cast<size_t>((w - r) & kRingMask);

        if (available < kAsrcInterpTaps) {
            // Nothing to interpolate from.  Emit silence for the rest of the
            // block rather than repeating the last sample: a held value is a
            // DC step, which is both audible and worse for a speaker.
            underrunCount.fetch_add(1u, std::memory_order_relaxed);
            const size_t remaining = (frames - frame) * channels;
            memset(&interleaved[at], 0, remaining * sizeof(int32_t));
            break;
        }

        // Hermite needs the sample before the read position as well, and the
        // ring wraps, so every index is masked.
        const uint32_t i1 = r;
        const uint32_t i0 = (i1 - 1u) & kRingMask;
        const uint32_t i2 = (i1 + 1u) & kRingMask;
        const uint32_t i3 = (i1 + 2u) & kRingMask;

        const float frac = static_cast<float>(phase) * (1.0f / 65536.0f);
        const float l = hermite(ringL[i0], ringL[i1], ringL[i2], ringL[i3], frac);
        const float rr = hermite(ringR[i0], ringR[i1], ringR[i2], ringR[i3], frac);

        const int32_t sl = toSample24(l);
        const int32_t sr = toSample24(rr);

        // Mono sinks get the left channel; anything wider than stereo repeats
        // the pair, which is the least surprising behaviour for a monitor.

        for (uint8_t ch = 0u; ch < channels; ++ch) {
            interleaved[at++] = ((ch & 1u) == 0u) ? sl : sr;
        }

        // Advance the fractional read position by the ratio.  The integer
        // carry is how many input frames this output sample consumed: one at
        // unity, sometimes zero when upsampling.
        phase += step;
        r = (r + (phase >> 16)) & kRingMask;
        phase &= 0xFFFFu;
    }

    readIndex.store(r, std::memory_order_release);
}

// -----------------------------------------------------------------------------

void AsrcServo::begin(uint32_t nominalRatioQ16)
{
    nominalQ16 = nominalRatioQ16;
    ratioQ16   = nominalRatioQ16;
    lastError  = 0;
    // Start the average at target so the loop begins with zero error rather
    // than spending its first second chasing a filter that started at zero.
    filteredFillQ16 = static_cast<int32_t>(kAsrcTargetFill) << 16;
}

uint32_t AsrcServo::update(size_t currentFill)
{
    // Average the fill before acting on it.  The raw value sawtooths by a
    // whole engine block; only the slow component carries rate information.
    const int32_t rawQ16 = static_cast<int32_t>(currentFill) << 16;
    filteredFillQ16 += (rawQ16 - filteredFillQ16) >> kAsrcFillFilterShift;

    // Positive error means the ring is filling: the engine is outrunning the
    // device, so more input must be consumed per output sample.
    lastError = (filteredFillQ16 >> 16) - static_cast<int32_t>(kAsrcTargetFill);

    if ((lastError > -kAsrcServoDeadband) && (lastError < kAsrcServoDeadband)) {
        return ratioQ16;
    }

    // Proportional, from NOMINAL rather than from the current ratio: this is
    // what keeps the loop first-order.  Accumulating onto the previous ratio
    // would make it an integrator, and the plant is one already.
    const int32_t correction = lastError * kAsrcServoGain;
    int32_t updated = static_cast<int32_t>(nominalQ16) + correction;

    const int32_t low  = static_cast<int32_t>(nominalQ16 - kAsrcRatioMaxDeviation);
    const int32_t high = static_cast<int32_t>(nominalQ16 + kAsrcRatioMaxDeviation);
    if (updated < low)  { updated = low; }
    if (updated > high) { updated = high; }

    // Slew limit: a step in ratio is a step in pitch, however small.
    const int32_t previous = static_cast<int32_t>(ratioQ16);
    if (updated > (previous + kAsrcRatioSlewMax)) {
        updated = previous + kAsrcRatioSlewMax;
    } else if (updated < (previous - kAsrcRatioSlewMax)) {
        updated = previous - kAsrcRatioSlewMax;
    }

    ratioQ16 = static_cast<uint32_t>(updated);
    return ratioQ16;
}

} // namespace JT
