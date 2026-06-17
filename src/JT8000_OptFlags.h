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

// =============================================================================
// JT8000_OptFlags_CrossModSync.h — ADDITIONS for CrossModSync features
// =============================================================================
// MERGE THESE INTO JT8000_OptFlags.h AFTER the existing OPT 3 block.
// Do NOT replace the existing file — append these sections.
// =============================================================================

// -----------------------------------------------------------------------------
// OPT 4 — Cross Modulation: OSC2 audio output → OSC1 FM pitch (audio-rate FM)
//
// Adds an AudioMixer4 pre-stage before OSC1's _frequencyModMixer slot 0.
// When depth = 0.0, the mixer passes the static pitch DC at unity — no
// audible change, minimal CPU cost (one AudioMixer4::update() per voice).
//
// When DISABLED (0): No cross-mod pre-mixer is created.  OSC1's pitch DC
// connects directly to the FM mixer as before.  Zero CPU overhead.
//
// When ENABLED (1): The pre-mixer is always in the audio graph (Teensy
// scheduler is unconditional).  Cost: ~2.5 µs per voice per block (8 voices
// = ~20 µs total).  When depth is 0.0, slot 1 receives a null block (nothing
// connected), so the mixer just copies slot 0 through.
// -----------------------------------------------------------------------------
#ifndef JT_OPT_CROSS_MOD
#define JT_OPT_CROSS_MOD  1   // 1 = enabled (recommended)
#endif

// Cross-mod depth curve selection.
//   0 = Linear:      depth increases uniformly across CC range.
//                     Most action in first quarter of knob — gets aggressive fast.
//   1 = Exponential:  more resolution at low depths where subtle FM lives.
//                     Musical sweet spot covers more of the knob range.
//
// Change this flag and rebuild to compare curves.  Both produce identical
// output at CC 0 (silent) and CC 127 (full depth).  Only the shape between
// those endpoints differs.
#ifndef JT_CROSS_MOD_CURVE
#define JT_CROSS_MOD_CURVE  1   // 1 = exponential (recommended)
#endif

// -----------------------------------------------------------------------------
// OPT 5 — Oscillator Hard Sync: sample-accurate coupled dual oscillator
//
// AudioSynthOscSync contains BOTH oscillator phase accumulators in a single
// AudioStream::update().  On every sample, if the master (OSC2) phase wraps,
// the slave (OSC1) phase is reset to zero — producing the characteristic
// hard-sync harmonic tearing.
//
// ACTIVATION:
//   The sync engine is a separate AudioStream object, NOT a modification to
//   OscillatorBlock.  VoiceBlock swaps audio connections at runtime:
//     Sync OFF → existing OscillatorBlock path (unchanged, no CPU difference)
//     Sync ON  → AudioSynthOscSync replaces both oscillator outputs
//
//   When this flag is 0, the AudioSynthOscSync class is not compiled at all.
//   When 1, the class exists but only enters the audio graph when sync is
//   enabled via CC — no CPU cost when sync is off.
//
// LIMITATIONS WHEN SYNC IS ACTIVE:
//   - Supersaw is not available on OSC1 (forced to standard waveform).
//   - Band-limited waveforms are not used (sync discontinuity breaks PolyBLEP).
//   - Glide still works (changes base frequency; sync follows).
//   - Cross-mod is integrated directly into the sync engine (no separate mixer).
//   - Feedback comb continues to work (downstream of oscillator output).
//
// CPU:
//   Replaces two AudioSynthWaveformJT::update() calls with one
//   AudioSynthOscSync::update().  Net cost is similar — two phase accumulators
//   + two waveform lookups + one comparison per sample.  The 4-input overhead
//   (FM × 2 + shape × 2) adds ~4 receiveReadOnly() calls (null when not
//   connected = zero cost).
// -----------------------------------------------------------------------------
#ifndef JT_OPT_OSC_SYNC
#define JT_OPT_OSC_SYNC  1   // 1 = enabled (recommended)
#endif

// -----------------------------------------------------------------------------
// OPT 6 — NLLadderNB (JP slots): filter-core oversampling for alias reduction.
//
// The JP filter slots now use NLLadderNB, a Newton-Bisection nonlinear ladder
// that saturates like an analog circuit when driven. That saturation generates
// harmonics which, at the base sample rate, fold back as aliasing when the
// filter is pushed hard. Oversampling the filter core reduces that folding.
//
//   1 = base rate (SHIP DEFAULT). Cheapest. Aliases somewhat when driven hard.
//   2 = 2x oversampled core (~doubles the JP filter cost). Same tuning, level
//       and passband as 1x (verified matching to <0.01 dB) — the only audible
//       difference is reduced aliasing. Enable once one-voice CPU is measured
//       on hardware (see ARM_DWT_CYCCNT note in AudioFilterVABank.cpp) and the
//       8-voice headroom is confirmed.
//
// Rationale for default-off: a previous effort spent ~20% CPU on saturation
// tone with no audible payoff. This flag spends CPU on a DIFFERENT, measurable
// payoff (aliasing), but the discipline is the same — ship the verified core,
// measure, then turn the knob and A/B it. One optimisation at a time.
//
// NOTE: the cutoff warp must use the INTERNAL rate (2x fs when this is 2). That
// is handled inside NLLadderNB::setCutoff(); the bank passes Hz, not g, to the
// JP cases so the tuning stays correct in both builds. No other code changes.
// -----------------------------------------------------------------------------
#ifndef JT_OPT_NLLADDER_OVERSAMPLE
#define JT_OPT_NLLADDER_OVERSAMPLE  1   // 1 = base rate (recommended to ship)
#endif

// RETIRED: JT_OPT_IR3109_PERSTAGE_NL — the IR3109Ladder struct it guarded has
// been replaced by NLLadderNB, whose per-stage nonlinearity is intrinsic and
// always on. The flag is intentionally no longer defined.

// -----------------------------------------------------------------------------
// OPT 7 — Filter engine idle gating: skip the inactive filter engine's DSP.
//
// FilterBlock holds BOTH AudioFilterOBXa and AudioFilterVABank permanently in
// the audio graph. Only one is selected at a time; the other is silenced at
// the output mixer but, without this flag, still runs its full ZDF core every
// audio block. That is ~half the filter CPU spent on a signal nobody hears
// (8 voices × one idle filter core per block).
//
// FIX: each core gains a lightweight _active flag and an early-out at the top
// of update() — it releases its input blocks and transmits nothing when idle.
// FilterBlock drives the flag from setFilterEngine(): the newly-selected core
// is marked active and reset() (clean start, ~1 block to settle, masked by the
// existing output-mixer crossfade); the deselected core is marked idle.
//
//   1 = inactive engine skips DSP (SHIP DEFAULT — saves CPU, no tonal change).
//   0 = both engines always run (previous behaviour). The _active flag is
//       still set but ignored in update(), so turning this off is a pure
//       A/B with zero other code differences — measure with
//       AudioProcessorUsage() to confirm the saving on hardware.
// -----------------------------------------------------------------------------
#ifndef JT_OPT_FILTER_IDLE_GATING
#define JT_OPT_FILTER_IDLE_GATING  1   // 1 = gate inactive engine (recommended)
#endif
