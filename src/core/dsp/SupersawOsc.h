// =============================================================================
// SupersawOsc.h — JP-8000 Super Saw for JT-8000 v2 (Pass 4, Step 2)
// =============================================================================
//
// PROVENANCE
//   Float port of v1's AudioSynthSupersaw (Szabó, "How to Emulate the Super
//   Saw", KTH 2010).  Every constant that defines the SOUND is byte-for-byte
//   v1: the 256-entry non-linear detune LUT, the seven frequency-offset
//   ratios (Szabó Table 1), the centre/side mix polynomials (§3.2 p.15),
//   the pitch-tracked 1-pole HPF (§3.3), and fastPow2 for FM.  What changed
//   is PACKAGING: int16 AudioStream -> plain float renderer, so it runs in
//   the monolithic voice and in host tests.
//
// V1 RUNTIME CONFIGURATION == V2 DEFAULTS
//   v1's OscillatorBlock configured the object it allocated; those calls
//   ARE the sound of every existing patch, so they are v2's defaults:
//     FM range 10 octaves · phase-mod range 180° · oversample OFF ·
//     polyBLEP OFF (naive saws — the aliasing is the "air", Szabó §3.4) ·
//     mix compensation ON, max gain 1.5.
//   (The bare v1 constructor defaulted compensation OFF, but no v1 build
//   ever ran it that way — OscillatorBlock always switched it on.)
//
// DELIBERATE DIFFERENCES (flagged per the no-silent-change rule)
//   1. noteOn() phases come from a SEEDED xorshift instead of Arduino
//      random().  Random phases are the requirement (Szabó §3.4); which
//      random makes no audible difference, and seeding makes renders and
//      tests reproducible.
//   2. The 2x-oversample path is NOT ported: v1 shipped with it off, and
//      carrying a dead second render loop into v2 is maintenance without
//      sound.  If ever wanted, it returns as its own pass.
//   3. Amplitude/velocity staging: v1 folded velocity into _amp here; v2
//      applies velocity and envelope at the Voice stage like every other
//      source.  Identical math, one stage later.
//   4. MIX LAW = JP-8000, NOT v1 (signed off 2026-10-22, item 1b).  v1 split
//      the Szabó side gain across the six side voices (sideTotal / 6); the
//      JE-8086 captures show the JP gives EACH side voice the full Szabó
//      side value (measured side/centre amplitude within ~0.05 of Szabó at
//      every mix; v1 was ~6x too quiet).  The JP also applies NO mix
//      compensation: its supersaw RMS is a constant multiple of the
//      per-voice Szabó sum (0.53..0.60 x the UNFILTERED sum, relative to its
//      own plain saw, flat across the whole mix sweep).  So v1's
//      (1 + 0.5·mix) boost is gone.  kOutputGain = 0.71 is that level
//      re-calibrated THROUGH this class's pitch-tracked HPF (rendered at the
//      capture pitch, 20 seeds: JE/JT RMS ratio 1.17..1.27 at 0.57, flat
//      across mix -> x1.24).  Existing patches using SSAW change sound
//      (accepted at sign-off).  See osc-shapes/jp-osc1-capture-analysis.
//   5. OUTPUT BOUND.  v1's ±1 double clip existed for a sum that rarely
//      exceeded 1; the JP-law sum peaks near 1.5-2 on ordinary settings and
//      the JP output is NOT clipped (capture peaks ~3x its saw peak).  The
//      pre-gain clip is removed; the output clip stays only as a safety
//      bound at ±kClip = 3.0, above the worst-case coherent peak
//      0.71 × (centre + 6 × side) ≈ 2.8, so it never touches the sound.
//
// SUPERSAW ON BOTH OSCILLATORS
//   Per the Pass 4 sign-off this class is owned by BOTH OscUnits (v1
//   restricted it to OSC1 to save ~10 audio-graph objects per instance —
//   an object-count problem v2's monolithic renderer doesn't have).  State
//   is 7 phases + params; CPU is only spent when the wave is selected.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "core/AudioConfig.h"

namespace JT {

class SupersawOsc {
public:
    static constexpr int kVoices    = 7;   // 3 below + centre + 3 above
    static constexpr int kCentreIdx = 3;

    SupersawOsc();

    // --- control plane (block boundaries; all dirty-checked) ---
    void setFrequency(float hz);          // also retunes the tracked HPF
    void setDetune(float amount01);       // through the Szabó LUT
    void setMix(float mix01);             // centre/side polynomial curves
    void setBandLimited(bool enable);     // default false == v1 sound

    // Randomise all 7 phases — REQUIRED on every note trigger (Szabó §3.4:
    // the per-note variation is what makes the Super Saw organic).
    void noteOn();
    void seedNoise(uint32_t seed) { _rng = seed | 1u; }

    // --- audio plane ---
    // WRITES one block (replaces).  fmBuf/pmBuf optional (nullptr = off):
    //   fmBuf: ±1 modulator, pitch × 2^(fm × fmOctaves) — all 7 voices share
    //          one fastPow2 per sample, exactly as v1's inner loop.
    //   pmBuf: ±1 modulator, additive phase offset of pmBuf × 0.5 cycles
    //          (v1's 180° default), applied at read time, not accumulated.
    void render(float* out, size_t n,
                const float* fmBuf, float fmOctaves,
                const float* pmBuf);

private:
    float detuneCurve(float x) const;     // LUT + linear interpolation
    void  calculateIncrements();
    void  calculateGains();
    void  calculateHpf();
    float nextRandom01();

    // parameters (dirty-checked in setters)
    float _freq      = 440.0f;
    float _detuneAmt = 0.5f;
    float _mixAmt    = 0.5f;
    bool  _polyBlep  = false;

    // Output staging (see deviations 4 and 5 in the header):
    //   kOutputGain — measured JP level relative to a plain ±1 saw, through
    //                 the tracked HPF below (header deviation 4).  The ONE
    //                 trim point if an A/B against the JE wants it nudged.
    //   kClip       — safety bound only; above the worst-case coherent peak.
    static constexpr float kOutputGain    = 0.71f;
    static constexpr float kClip          = 3.0f;
    static constexpr float kPmRangeCycles = 0.5f;  // 180° (v1)

    // derived per-voice state
    float _phase[kVoices]    = { 0.0f };
    float _phaseInc[kVoices] = { 0.0f };
    float _gain[kVoices]     = { 0.0f };

    // pitch-tracked 1-pole HPF state
    float _hpfPrevIn  = 0.0f;
    float _hpfPrevOut = 0.0f;
    float _hpfAlpha   = 0.0f;

    uint32_t _rng = 0xA5A5A5A5u;
};

} // namespace JT
