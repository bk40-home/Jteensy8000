/* Audio Library for Teensy
 * Copyright (c) 2025, Kris Bishop, bishopkris40@hotmail.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
 #pragma once
// =============================================================================
// JT8000_OptFlags.h  —  Compile-time feature and CPU optimisation switches
// =============================================================================
//
// Each flag guards a specific feature or optimisation.  Set to 1 to enable,
// 0 to disable.  Disable an optimisation flag, rebuild, and measure
// AudioProcessorUsage() to isolate the contribution of each change.
//
// Benchmark procedure (from Jteensy8000.ino loop()):
//   Serial.printf("CPU: %.1f%%  Mem: %d\n",
//       AudioProcessorUsage(), AudioMemoryUsageMax());
//
// =============================================================================

// -----------------------------------------------------------------------------
// FEATURE 1 — Cross Modulation: OSC2 audio → OSC1 FM pitch
//
// Adds a 2-slot AudioMixer4 pre-stage inside OscillatorBlock (OSC1 only).
// OSC2 output is injected at a depth-scaled gain before the FM mixer.
// Slot 0 of the pre-mixer carries the pitch DC at unity; slot 1 carries
// OSC2 audio at the cross-mod depth gain.
//
// When depth == 0.0 the gain on slot 1 is 0 — no modulation, no audio cost
// beyond the unconditional AudioMixer4 update (~1 µs/voice at 8 voices).
//
// Works with ALL waveform combinations.  Does NOT require sync.
//
// CPU COST: +1 AudioMixer4 per voice (8 total) ≈ +0.5% CPU at full depth.
//           Zero additional cost when depth == 0.
// -----------------------------------------------------------------------------
#ifndef JT_OPT_CROSS_MOD
#define JT_OPT_CROSS_MOD  1   // 1 = enabled (recommended)
#endif

// Maximum FM swing in octaves at full depth (CC 127).
// ±2 octaves covers classic JP-8000 cross-mod timbres without going chaotic.
// Increase to 4.0 for more extreme sounds.
#ifndef JT_CROSS_MOD_MAX_OCTAVES
#define JT_CROSS_MOD_MAX_OCTAVES  2.0f
#endif

// Depth curve: 0 = linear, 1 = exponential (more resolution at low depths).
#ifndef JT_CROSS_MOD_CURVE
#define JT_CROSS_MOD_CURVE  0
#endif

// -----------------------------------------------------------------------------
// FEATURE 2 — Oscillator Hard Sync: OSC2 (master) → OSC1 (slave) phase reset
//
// Sample-accurate sync: when OSC2 phase wraps, OSC1 phase resets to zero at
// that exact sample.  Implemented via AudioSynthOscSync — a single AudioStream
// containing both oscillator cores.
//
// When sync is ENABLED, VoiceBlock swaps audio connections to route through
// AudioSynthOscSync instead of the normal OscillatorBlock outputs.
// When sync is DISABLED, normal OscillatorBlock paths are restored and the
// AudioSynthOscSync is not in the audio graph.
//
// Works with ALL standard waveforms.  PolyBLEP band-limited waveforms are
// NOT supported in sync mode — hard sync phase resets disrupt PolyBLEP
// correction.  Raw sync harmonics are the intended sound.
//
// CPU COST: AudioSynthOscSync ≈ 2× AudioSynthWaveformJT + sync check.
//           Equivalent to two oscillators.  Only costs CPU when sync is ON.
// -----------------------------------------------------------------------------
#ifndef JT_OPT_OSC_SYNC
#define JT_OPT_OSC_SYNC  1   // 1 = enabled (recommended)
#endif

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
