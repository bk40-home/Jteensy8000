// =============================================================================
// JpModLaw.h — JP-8000 modulation-depth curve (measured on a JE-8086)
// =============================================================================
//
// ROLE
//   One curve, measured 2026-10-24 (xmod/xmod-sync-capture-analysis), sets how
//   far every "depth" knob in the JP's LFO1 & ENV section reaches:
//
//     D(d) = 0.8035·d² − 0.003236·d³  cents,   d = JP depth knob, |d| <= 63
//
//       LFO1 → pitch       ±D(d)            (+63 ≈ ±23.8 st)
//       pitch env → pitch   2·D(d)           (+63 ≈ +47.6 st at note-on)
//       env → X-MOD         D(d)/18 knob steps, added to the X-MOD knob
//       LFO1 → X-MOD        D(d)/16.25 knob steps (peak), added likewise
//
//   Measured points (JE / curve, cents): 16: 187/192, 32: 721/717,
//   48: 1491/1493, 63: 2380/2380.  Env: 377/385, 1445/1433, 4768/4760.
//   A negative depth inverts (LFO half a cycle out, env subtracts).
//
// UNITS
//   The JT stores depths normalised: |1.0| == JP 63.  Callers pass that
//   normalised value; the sign is kept.
//
// COST
//   A cubic per call — control rate only (per block or per param change).
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

namespace JT {
namespace JpModLaw {

// Cents for a normalised depth (sign kept): D(63·|x|) · sign(x).
float depthCents(float depth01);

// LFO1 → pitch, peak semitones for a normalised depth (= D/100).
float lfoPitchSemis(float depth01);

// Pitch env → pitch, full-scale semitones for a normalised depth (= 2·D/100).
float envPitchSemis(float depth01);

// LFO1 / env → X-MOD.  The JP adds knob steps (0..127) to the X-MOD knob:
//   env  : D/18     (JE +32: +39 / +41; −32: −39; +63: 133 -> clamps)
//   LFO1 : D/16.25  (peak; JE fits 11.5 / 12.0 at 16, 46.0 / 42.8 at 32 —
//          1.11 × the env's reach, same clamped-triangle fit on both)
// Expressed against the semitone values above, so the routing in Voice can
// reuse them: LFO semitones (D/100) × kXmodNormPerLfoSemi, env semitones
// (2·D/100) × kXmodNormPerEnvSemi.
inline constexpr float kXmodNormPerLfoSemi = 100.0f / 16.25f / 127.0f;
inline constexpr float kXmodNormPerEnvSemi = 0.5f * 100.0f / 18.0f / 127.0f;

} // namespace JpModLaw
} // namespace JT
