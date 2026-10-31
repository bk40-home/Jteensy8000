// =============================================================================
// JpEnvLaw.h — JP-8000 envelope TIME law (measured on a JE-8086)
// =============================================================================
//
// ROLE
//   Maps an envelope TIME knob position (attack, decay, release, and LFO1
//   FADE) to the time constant tau, the way the JP-8000 does, for the
//   voice.env_law = JpEnv option (sign-off 1a, 2026-10-30/31).  One table
//   drives every envelope: amp, filter and pitch, and the LFO1 fade.
//
// MEASURED SHAPES (fm1-port/scene18 + scene19 analyses)
//   decay / release : exponential toward the target, level' = target +
//                     (level - target)·e^(−t/τ)   (straight lines in dB)
//   attack          : charge toward kAttackTarget (1.25 x full), stopped at
//                     full:  a(t) = min(1, 1.25·(1 − e^(−t/τ)))  — fit
//                     residual 0.1 % of full scale; reaches full at 1.61 τ
//   LFO1 fade       : the attack generator on the LFO1 DEPTH control
//                     (full depth at ≈ 1.6 τ; confirmed at 80..127)
//
// TABLE (byte -> τ seconds), log-linear between knots
//   bytes 10..127 measured (decay, cross-checked by release/attack/fade);
//   steps kept as measured: 116 -> 117 (×2.6) and the top end in PAIRS
//   (117 = 118, 119 = 120: the JP indexes its table by (byte+1) >> 1 there).
//   121/122 and 125/126 are log-interpolated pairs (not captured), and 0..9
//   are provisional (two captures disagree at 8/9 — see scene 19 §3).
//
// UNITS
//   Input is the knob's normalised position 0..1 (byte / 127 on the 7-bit
//   wire; finer 14-bit steps interpolate smoothly).
//
// COST
//   A short scan + one expf, control plane only (a parameter edit).  The
//   envelopes then run one multiply per stage per block.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

namespace JT {
namespace JpEnvLaw {

// Attack charges toward this level and stops at 1.0 (measured k = 1.22..1.30).
inline constexpr float kAttackTarget = 1.25f;

// A release (or a decay to sustain 0) that falls below this level ends the
// stage: −60 dB, inaudible; the voice's per-block ramp lands it on 0.  The JE
// itself cuts its voices somewhere below about −45 dB (scene 17).
inline constexpr float kOffLevel = 0.001f;

// τ in seconds for a normalised knob position (clamped to 0..1).
float tauFromNorm(float norm01);

// Per-block multiplier for an exponential stage with time constant τ:
// e^(−blockSeconds / τ).  τ <= 0 returns 0 (complete in one block).
float blockCoef(float tauSeconds);

} // namespace JpEnvLaw
} // namespace JT
