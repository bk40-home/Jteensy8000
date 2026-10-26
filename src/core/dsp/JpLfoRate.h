// =============================================================================
// JpLfoRate.h — JP-8000 LFO RATE law (measured on a JE-8086, LFO1)
// =============================================================================
//
// ROLE
//   Maps an LFO RATE knob position to Hz the way the JP-8000 does, for the
//   lfo1.rate_law / lfo2.rate_law = JpRate options (sign-off 2026-10-25,
//   option b; both LFOs per Kris).  Measured on LFO1; LFO2 is ASSUMED to share
//   the curve until an LFO2 capture confirms it.  The JT law
//   (JtRate, unchanged) is a single log sweep 0.03 · 1300^t Hz; the JP's is
//   two log segments with the knee at RATE 64:
//
//     RATE <= 64 : ~10.6 knob steps per octave   (0.049 Hz at 0, 3.23 at 64)
//     RATE >  64 : ~21.1 knob steps per octave   (9.08 Hz at 96, ~25 at 127)
//
//   Source: lfo/lfo1-rate-capture-analysis-2026-10-25.md — 21 free-running
//   JE-8086 notes, RATE 0..120 measured (zero-crossing and FFT agree <1 %),
//   127 extrapolated along the upper segment (the pitch tracker cannot follow
//   a 25 Hz LFO at C2).  The table holds the measured points directly and
//   interpolates log-linearly between them, so every measured byte is exact
//   and the knee keeps its measured roundness.
//
// UNITS
//   Input is the knob's normalised position 0..1 (byte / 127 on the 7-bit
//   wire; finer steps from 14-bit hosts interpolate smoothly).
//
// COST
//   A short linear scan + one expf.  Control plane only: called when an LFO
//   RATE or its rate law changes, never per block.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

namespace JT {
namespace JpLfoRate {

// JP-8000 LFO free-running rate (Hz) for a normalised knob position.
// Out-of-range input is clamped to 0..1.
float hzFromNorm(float norm01);

} // namespace JpLfoRate
} // namespace JT
