// =============================================================================
// AudioConfig.h — audio format constants for JT-8000 v2 (decision F4)
// =============================================================================
// 44.1 kHz / 128-sample blocks, signed off in the design brief §13.
// Constants only (header-only is allowed for constexpr tables per §9).
// Everything in core/ derives timing from these two numbers — never from
// the Teensy Audio library — so the same code renders identically in the
// host test harness and on the hardware.
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace JT {

// -----------------------------------------------------------------------------
// SAMPLE RATE — the single switch (decision: Daisy-port Phase 1).
//
// Change THIS ONE constant to retune the whole engine.  Every coefficient in
// core/ derives from kSampleRate (directly, or via the sizing helpers below),
// so 44100 and 48000 both produce a correctly-tuned instrument from the same
// source.  Was 44100.0f through v1/v2; moved to 48000.0f for the Daisy codec,
// which does not offer 44.1 kHz, and for a stock-48k USB interface.
//
// On the Teensy, main.cpp's AudioSettings_F32 MUST be constructed from this
// same symbol (not a literal) so the F32 graph and the engine agree.
// -----------------------------------------------------------------------------
inline constexpr float  kSampleRate   = 48000.0f;
inline constexpr size_t kBlockSize    = 128;

// -----------------------------------------------------------------------------
// Compile-time buffer sizing.
//
// Delay-line lengths used to be hand-computed integers baked at 44.1 kHz
// (ceilf is not constexpr, so the values were pre-evaluated in comments).  That
// silently mistuned every delay and reverb the instant kSampleRate changed.
// These constexpr helpers let each length derive from the ACTIVE rate instead,
// so the switch above is the only edit a rate change needs.
//
//   ceilU32(x)          — constexpr ceiling, positive inputs only.
//   msToSamples(ms)     — samples for an absolute time at kSampleRate; use for
//                         lengths defined by a duration (delay, mod, predelay,
//                         the 5 ms feedback comb).  Matches the old hand values
//                         exactly at 44.1 kHz.
//   samplesAtRate(n)    — rescale a length authored at kRefSampleRate to
//                         kSampleRate, preserving the ratio.  Use for the
//                         "prime-ish" reverb decorrelation lengths, whose intent
//                         is their relative geometry, not a round millisecond.
//
// Both round UP so a time-based line never loses its final sample; the +N
// interpolation guards at the call sites are preserved on top.
// -----------------------------------------------------------------------------
inline constexpr float kRefSampleRate = 44100.0f;   // rate the legacy integers were authored at

constexpr uint32_t ceilU32(float x)
{
    const uint32_t truncated = (uint32_t)x;
    return ((float)truncated < x) ? truncated + 1u : truncated;
}

// Nudge below the mathematical result before ceiling, so a duration that lands
// exactly on an integer sample count (e.g. 5 ms @ 48 kHz = 240.0) is not pushed
// to N+1 by float error (0.005f*48000 evaluates to 240.0000048).  Genuine
// fractional lengths here sit >0.1 sample from an integer, well clear of this.
inline constexpr float kSizeEps = 1e-3f;

constexpr uint32_t msToSamples(float ms)
{
    return ceilU32(ms * 0.001f * kSampleRate - kSizeEps);
}

constexpr uint32_t samplesAtRate(uint32_t refSamples)
{
    return ceilU32((float)refSamples * (kSampleRate / kRefSampleRate) - kSizeEps);
}

// -----------------------------------------------------------------------------
// primeAtRate — for RECIRCULATING delay-loop lengths whose intent is mutual
// primality (an FDN reverb's lines: any shared factor puts two loops on a common
// modal grid and the network rings at that period).  Plain samplesAtRate rounding
// destroys primality, so these lengths rescale to the active rate and then snap
// to the nearest prime.  Distinct seeds hundreds of samples apart snap to
// distinct primes, so a set stays pairwise coprime.  At 44.1 kHz the ratio is 1
// and the authored values (already prime) pass straight through.
// -----------------------------------------------------------------------------
constexpr bool isPrime(uint32_t n)
{
    if (n < 2u)        return false;
    if (n % 2u == 0u)  return n == 2u;
    for (uint32_t d = 3u; d * d <= n; d += 2u)
        if (n % d == 0u) return false;
    return true;
}

constexpr uint32_t nearestPrime(uint32_t n)
{
    if (n <= 2u) return 2u;
    // Search outward from n, preferring the equal-or-higher prime on a tie so a
    // length never rounds below the time it represents.  512 covers every real
    // prime gap in the low-thousands range these buffers occupy.
    for (uint32_t off = 0u; off < 512u; ++off) {
        if (isPrime(n + off))                       return n + off;
        if (off != 0u && off <= n && isPrime(n - off)) return n - off;
    }
    return n;   // unreachable for realistic sizes; keeps the function total
}

constexpr uint32_t primeAtRate(uint32_t refSamples)
{
    return nearestPrime(samplesAtRate(refSamples));
}

// Control rate: one block = kBlockSize / kSampleRate s (≈ 2.90 ms @ 44.1 kHz,
// ≈ 2.67 ms @ 48 kHz).  Envelopes, LFOs and parameter application all run at
// this rate (brief §6.1) — authentic to the JP-8000's control-rate modulation
// and roughly kBlockSize× cheaper than per-sample.
inline constexpr float  kBlockMs      = 1000.0f * (float)kBlockSize / kSampleRate;
inline constexpr float  kBlocksPerSec = kSampleRate / (float)kBlockSize;

// LFO pitch-depth ceiling (semitones at depth 1.0).  Was private to
// SynthCore; shared here since G1 routing needs the same constant in Voice
// to convert the routed lane back to the knob-normalised X-MOD range.
inline constexpr float  kLfoPitchMaxSemis = 7.0f;

// Mod wheel (CC 1) -> LFO1 pitch depth, as a FRACTION of the knob's full
// range.  With kLfoPitchMaxSemis at 7, 0.10 gives a full wheel about +-0.7
// semitones: a musical vibrato, and roughly what a JP-8000 does out of the
// box.  Wheel at rest contributes exactly 0.0f, so this constant cannot
// affect any patch that does not move the wheel.
//
// This is the single tunable for wheel feel — raise it for a deeper wheel;
// it is NOT a patch parameter, and deliberately so, until the mod matrix
// makes wheel routing user-assignable.
inline constexpr float  kModWheelPitchDepth = 0.10f;

// Per-voice mix contribution.  8 voices at full level sum to 1.0 exactly —
// headroom staging is finalised in the Phase 4 SignalPath doc; until then
// this conservative value cannot clip regardless of patch.
inline constexpr float  kVoiceGain    = 0.125f;

// -----------------------------------------------------------------------------
// JT_COLD — place a function in FLASH instead of ITCM (Teensy 4.1).
//
// MEMORY MODEL, in one breath: RAM1 (512 KB) is split between ITCM (code)
// and DTCM (data) in 32 KB blocks — code size directly steals data space.
// Functions marked JT_COLD execute from flash through the cache instead:
// a few-cycle first-fetch penalty, ZERO once cached.  Rule of use:
//   * audio-plane code (render loops, per-sample DSP)  -> never JT_COLD
//   * control-plane code (MIDI parse, patch codec, CRC,
//     curve conversion, table lookup)                  -> JT_COLD
// This is the expansion of FLASHMEM, written out so core/ needs no
// Arduino header.  On the host it vanishes.
// -----------------------------------------------------------------------------
#if defined(__IMXRT1062__)
#define JT_COLD __attribute__((section(".flashmem"), noinline))
// Const DATA equivalent of JT_COLD: on the IMXRT1062, .rodata is COPIED
// into DTCM by default — big lookup tables must opt back into flash.
// Directly addressable and cached; use for any table not read per-sample
// in a hot loop (and even those are usually fine once cached).
#define JT_FLASH_DATA __attribute__((section(".progmem")))
#else
#define JT_COLD
#define JT_FLASH_DATA
#endif

} // namespace JT
