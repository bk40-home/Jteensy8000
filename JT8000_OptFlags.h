#pragma once
// =============================================================================
// JT8000_OptFlags.h  —  Compile-time CPU optimisation switches
// =============================================================================
//
// Each flag guards a specific optimisation.  Set to 1 to enable, 0 to disable.
// Disable a flag, rebuild, and measure AudioProcessorUsage() to isolate the
// contribution of each optimisation.
//
// Benchmark procedure (from Jteensy8000.ino loop()):
//   Serial.printf("CPU: %.1f%%  Mem: %d\n",
//       AudioProcessorUsage(), AudioMemoryUsageMax());
//
// All flags default ON (1).  Turn off one at a time to measure.
//
// =============================================================================

// -----------------------------------------------------------------------------
// OPT 1 — OBXa filter: hoist powf() out of the per-sample loop.
//
// The filter modulation multiplier (cutoff mod bus → Hz) was being recomputed
// every sample via powf(2.0f, modOct).  powf() costs ~50–100 cycles on
// Cortex-M7.  At 128 samples/block × 8 voices = 1,024 powf() calls per
// audio interrupt — the single largest idle CPU contributor.
//
// FIX: Compute modMul ONCE per block (block-rate, ~2.9 ms intervals).
// Audible difference: none — filter sweeps are far slower than 344 Hz.
// The in-loop code path retains the per-sample audio signal read; only the
// exponential conversion is hoisted.
//
// fast_pow2() (defined in AudioFilterOBXa_OBXf.cpp when this flag is 1)
// is a Remez minimax polynomial — < 0.005% error, ~8 cycles vs ~80 for powf.
// The block-rate powf() for key tracking is NOT changed (called only once).
// -----------------------------------------------------------------------------
#ifndef JT_OPT_OBXA_BLOCKRATE_MOD
#define JT_OPT_OBXA_BLOCKRATE_MOD  1   // 1 = enabled (recommended)
#endif

// -----------------------------------------------------------------------------
// OPT 2 — VA filter bank: hoist powf() out of the per-sample loop.
//
// Same issue as OPT 1 but in AudioFilterVABank::update().  The audio-rate
// cutoff modulation path calls powf(2.0f, modSample * _cutoffModOct) every
// sample.  Hoisted to block-rate using fast_pow2().
//
// NOTE: VA bank is not active in the current test configuration (OBXa is).
// This flag exists for parity and future benchmarking.
// -----------------------------------------------------------------------------
#ifndef JT_OPT_VA_BLOCKRATE_MOD
#define JT_OPT_VA_BLOCKRATE_MOD  1   // 1 = enabled (recommended)
#endif

// -----------------------------------------------------------------------------
// OPT 3 — Supersaw: skip update() entirely when amplitude is zero.
//
// AudioSynthSupersaw::update() runs its 7-voice synthesis loop every audio
// interrupt regardless of whether the supersaw waveform is selected.  When
// a standard waveform (sine, saw, pulse etc.) is active, the supersaw output
// mixer gain is 0.0 so the result is discarded — but the 7-voice loop still
// consumes CPU (128 samples × 7 voices × 8 voices = 7,168 wasted samples
// per audio interrupt at idle).
//
// FIX: Early-exit at the top of update() when amp < JT_SUPERSAW_IDLE_THRESHOLD.
// The supersaw is ONLY present on OSC1 (OSC2 passes enableSupersaw=false),
// so this affects 8 objects.
//
// Threshold is deliberately small (not exact zero) to handle float precision
// and the brief ramp-down period when switching waveforms.
// -----------------------------------------------------------------------------
#ifndef JT_OPT_SUPERSAW_IDLE_GATE
#define JT_OPT_SUPERSAW_IDLE_GATE  1   // 1 = enabled (recommended)
#endif

// Amplitude below which the supersaw update() exits without generating audio.
// Must be > 0 to handle float rounding; small enough to be inaudible.
#ifndef JT_SUPERSAW_IDLE_THRESHOLD
#define JT_SUPERSAW_IDLE_THRESHOLD  0.001f
#endif
