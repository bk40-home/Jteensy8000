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

namespace JT {

inline constexpr float  kSampleRate   = 44100.0f;
inline constexpr size_t kBlockSize    = 128;

// Control rate: one block = 128 / 44100 s ≈ 2.9025 ms.  Envelopes, LFOs and
// parameter application all run at this rate (brief §6.1) — authentic to the
// JP-8000's control-rate modulation and roughly 128x cheaper than per-sample.
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
