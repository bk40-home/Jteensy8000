// =============================================================================
// Asrc.h — asynchronous sample rate conversion for the USB host audio path
// =============================================================================
// The engine runs at JT::kSampleRate (AudioConfig.h — 48 kHz since the Daisy
// port) and the USB host audio device runs at whatever its own crystal says,
// so the two ends of this path never agree and never will.  This converts
// between them continuously, at a ratio that may be adjusted while running.
// The ratio is SEEDED from kSampleRate by the caller (UsbHostPort) — see the
// note there; a stale seed rate garbles this path even though the resampler
// itself is correct.
//
// SPLIT OF RESPONSIBILITY — read before changing the ratio logic:
//   * how many samples leave per USB frame is decided by the packet pacer
//     from the device's feedback endpoint (UsbAudioRate.h),
//   * at what ratio input is consumed to produce them is decided here.
// Those are different questions.  The feedback endpoint says nothing about
// the ENGINE's rate, so the ratio cannot be derived from it: the only signal
// that carries both ends is how the input ring's fill level drifts.  That is
// what AsrcServo watches.
//
// THREADING: push() runs on the audio update (the engine's block rate) and
// pull() runs on the USB host thread.  Single producer, single consumer, no
// locking.  Neither ever waits for the other; a shortfall produces silence
// and a counted underrun rather than a stall.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace JT {

// Ring capacity in sample frames.  The engine delivers 128 frames every
// ~2.9 ms while USB takes ~48 every 1 ms, so the ring has to absorb a whole
// block arriving at once plus scheduling jitter on both sides.  1024 frames
// is ~21 ms at 48 kHz: comfortably more than either, and 16 kB of OCRAM for
// stereo floats.
inline constexpr size_t kAsrcRingFrames = 1024u;

// Target fill, as a fraction of capacity.  Half leaves equal room to absorb
// a late producer and an early consumer.
inline constexpr size_t kAsrcTargetFill = kAsrcRingFrames / 2u;

// Ratio is input frames consumed per output frame, in 16.16 fixed point.
// 44100/48000 gives 0.91875, or 60211.
inline constexpr uint32_t kAsrcRatioUnity = 0x00010000u;

// Hermite interpolation reads one frame behind and two ahead, so this many
// frames must be resident before an output sample can be produced.
inline constexpr size_t kAsrcInterpTaps = 4u;

// -----------------------------------------------------------------------------
// Asrc — fractional-rate stereo resampler over a lock-free ring.
//
// Interpolation is 4-point third-order Hermite.  Linear would be about eight
// times cheaper and audibly worse: at 44.1 to 48 kHz its error rises with
// frequency and lands around -60 dB near the top of the band, which is
// perfectly visible on a synth that has real energy up there.  Hermite costs
// roughly ten multiply-adds per output frame per channel — under 1% of one
// core at 48 kHz stereo — and pushes the error far below the 24-bit floor
// this path is quantised to anyway.
// -----------------------------------------------------------------------------
class Asrc
{
public:
    void begin(uint32_t ratioQ16);

    // Producer side, audio update.  Returns the frames actually accepted;
    // fewer than requested means the ring was full, which is an overrun.
    size_t push(const float* left, const float* right, size_t frames);

    // Consumer side, USB host thread.  Writes interleaved 24-bit signed
    // samples, one value per channel per frame.  Always writes exactly
    // 'frames' frames: a shortfall is filled with silence and counted.
    void pull(int32_t* interleaved, size_t frames, uint8_t channels);

    // Ratio may be changed at any time; it takes effect on the next output
    // sample, with no discontinuity because the phase accumulator is
    // untouched.
    void setRatio(uint32_t ratioQ16) { ratio.store(ratioQ16, std::memory_order_relaxed); }
    uint32_t getRatio(void) const { return ratio.load(std::memory_order_relaxed); }

    // Frames currently resident.  Read from either side.
    size_t fill(void) const;

    uint32_t underruns(void) const { return underrunCount.load(std::memory_order_relaxed); }
    uint32_t overruns(void)  const { return overrunCount.load(std::memory_order_relaxed); }
    void resetCounters(void);

private:
    float ringL[kAsrcRingFrames];
    float ringR[kAsrcRingFrames];

    std::atomic<uint32_t> writeIndex{0u};   // producer
    std::atomic<uint32_t> readIndex{0u};    // consumer
    std::atomic<uint32_t> ratio{kAsrcRatioUnity};
    std::atomic<uint32_t> underrunCount{0u};
    std::atomic<uint32_t> overrunCount{0u};

    // Consumer only: fractional position between readIndex and the next frame.
    uint32_t phase = 0u;

};

// -----------------------------------------------------------------------------
// AsrcServo — steers the ratio so the ring settles near its target fill.
//
// The fill reading MUST be filtered before it is acted on.  The producer
// delivers a whole engine block at once while the consumer takes a USB
// frame at a time, so instantaneous fill sawtooths across a full block and
// the phase between the two drifts.  Feeding that raw into the loop
// modulates the ratio at a fraction of a percent, which is several cents of
// pitch: audible as a warble on any sustained tone.  The genuine rate error
// is tens of ppm and changes over seconds, so averaging away everything
// faster than that costs nothing and removes the warble entirely.
//
// PROPORTIONAL, deliberately, and the reason is worth stating because the
// obvious choice is wrong.  Fill is itself the integral of the rate error
// this corrects: d(fill)/dt is proportional to how far the ratio is off.
// The plant is therefore already an integrator, and adding an integrating
// controller on top gives two in series — a system that does not settle but
// hunts, slowly detuning the output first one way and then the other.
//
// A proportional controller on an integrating plant is first-order and
// unconditionally stable, at the price of a standing fill offset: holding a
// ratio correction requires holding the error that produces it.  That offset
// is pure latency and nothing else.  At the gain below, a 0.2% rate mismatch
// settles at roughly 33 frames, well under a millisecond, which is a price
// worth paying for a loop that cannot oscillate.
// -----------------------------------------------------------------------------
class AsrcServo
{
public:
    void begin(uint32_t nominalRatioQ16);

    // Call at a steady rate — once per audio block is ideal.  Returns the
    // updated ratio, which the caller applies to the Asrc.
    uint32_t update(size_t currentFill);

    uint32_t ratio(void) const { return ratioQ16; }

    // Distance from target at the last update, in frames, signed, measured on
    // the FILTERED fill.  Positive means the ring is filling: the engine is
    // outrunning the device.
    int32_t error(void) const { return lastError; }

    // Smoothed fill in whole frames, for reporting.
    int32_t filteredFill(void) const { return filteredFillQ16 >> 16; }

private:
    uint32_t nominalQ16 = kAsrcRatioUnity;
    uint32_t ratioQ16   = kAsrcRatioUnity;
    int32_t  lastError  = 0;
    // Fill average, 16.16 frames.  Fixed point because this runs in the audio
    // update and there is no reason to spend a float on it.
    int32_t  filteredFillQ16 = 0;
};

// Servo gain: Q16 ratio units of correction per frame of fill error.
//
// Sized from both ends.  A realistic worst case is about 0.5% — the engine's
// true 44117.6 Hz against the nominal 44100 is already 0.04%, and two free
// crystals add a little more — which at this gain settles at 82 frames of
// offset, comfortably inside the ring.  Stability is the other end: one
// block moves the error by roughly 0.85% of itself, a time constant near a
// third of a second, so the ratio changes far too slowly to be heard as
// pitch movement.
inline constexpr int32_t kAsrcServoGain = 4;

// Ratio is never allowed further than this from nominal.  At 0.5% the servo
// cannot run away on a bad fill reading and detune the output audibly; if it
// saturates here, something else is wrong and the counters will show it.
inline constexpr uint32_t kAsrcRatioMaxDeviation = kAsrcRatioUnity / 200u;

// Fill errors smaller than this are ignored.  Applied to the FILTERED fill,
// where a few frames really is negligible; against the raw sawtooth it would
// have been meaningless.
inline constexpr int32_t kAsrcServoDeadband = 4;

// Fill averaging, as a right shift per update.  Eight gives a time constant
// of about 256 engine blocks — roughly three quarters of a second at 44.1 kHz
// — which is far slower than the block-rate sawtooth and far faster than
// crystal drift.
inline constexpr int32_t kAsrcFillFilterShift = 8;

// Largest ratio change permitted in one update, in Q16 units.  Even a
// correctly filtered loop can step if the ring is disturbed, and a step in
// ratio is a step in pitch.  Two units per block limits the slew to about
// 30 ppm per millisecond: inaudible, and still able to traverse the whole
// clamp range in under a second.
inline constexpr int32_t kAsrcRatioSlewMax = 2;

} // namespace JT