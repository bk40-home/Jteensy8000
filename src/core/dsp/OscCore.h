// =============================================================================
// OscCore.h — multi-waveform oscillator core for JT-8000 v2 (Pass 4, Step 1)
// =============================================================================
//
// ROLE
//   One phase-accumulator oscillator implementing every wave in the table's
//   'osc_wave' option set EXCEPT supersaw (its own class, own state).  Two
//   of these plus a SupersawOsc form an OscUnit inside OscSection; the enum
//   order below matches the generated option set EXACTLY — patches store
//   the option index, so this ordering is frozen alongside the table:
//     ["SIN","SAW","SQR","TRI","ARB","PLS","rSAW","S&H","vTRI",
//      "BLS","rBLS","BLSQ","BLP","SSAW"]
//
// NAIVE vs BAND-LIMITED — BOTH ON PURPOSE
//   v1 (like the JP-8000 itself) offers aliased naive waves alongside
//   polyBLEP versions; the aliasing is part of the instrument's character
//   and several factory sounds rely on it.  Do not "fix" the naive modes.
//
// SYNC AND FM HOOKS (v1 CrossModSync semantics, confirmed from source)
//   * Hard sync: the MASTER fills a per-sample wrap buffer (fraction of the
//     sample at which its phase wrapped, or <0 for none); the SLAVE resets
//     its phase at those positions.  In the v2 voice OSC2 is master, OSC1
//     slave — exactly v1's assignment.
//   * FM (cross-mod): an optional per-sample buffer of raw modulator output
//     scales the slave's phase increment by 2^(mod × octaves) via
//     FastMath::fastPow2 — the same exponential-FM path v1 routed through
//     the FM mixer at ±10 octaves full scale.
//   Both hooks are nullptr when unused and cost ONE branch per block.
//
// CPU DISCIPLINE
//   Waveform dispatch happens once per block (switch outside the loop).
//   setFrequency() dirty-checks.  Phase stepping (with its optional FM and
//   sync conditionals) is a forced-inline helper shared by every wave loop,
//   so the generated code stays tight without 14 copy-pasted loops.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "core/AudioConfig.h"

namespace JT {

// Option indices of the generated 'osc_wave' set — see ordering note above.
enum class Wave : uint8_t {
    Sine        = 0,
    Saw         = 1,    // naive (aliased — intentional, see header note)
    Square      = 2,    // naive
    Triangle    = 3,
    Arb         = 4,    // AKWF wavetable (int16, linear interpolation)
    Pulse       = 5,    // naive, width from shape
    SawRev      = 6,    // naive reverse saw
    SampleHold  = 7,    // stepped noise at the oscillator rate
    TriVar      = 8,    // variable-skew triangle, skew from shape
    BlSaw       = 9,    // polyBLEP saw
    BlSawRev    = 10,   // polyBLEP reverse saw
    BlSquare    = 11,   // polyBLEP square
    BlPulse     = 12,   // polyBLEP pulse, width from shape
    Supersaw    = 13,   // handled by SupersawOsc — never reaches this class
};

class OscCore {
public:
    // --- control plane (block boundaries) ---
    void setWave(Wave w)              { _wave = w; }
    void setFrequency(float hz);      // dirty-checked
    // Shape 0..1: pulse width for Pulse/BlPulse, rise-fraction for TriVar.
    // Clamped to 0.05..0.95 so pulse never degenerates to DC.
    void setShape(float s);
    // Arbitrary wavetable: any length ≥ 2 (AKWF uses 600), int16 samples.
    // Passing nullptr falls back to naive saw (matches v1's arbdata guard).
    void setArbTable(const int16_t* data, uint16_t length);

    void resetPhase(float phase01);   // caller-supplied randomisation
    // Sample & Hold needs a noise source; seeding keeps voices decorrelated
    // yet the whole engine deterministic for tests and renders.
    void seedNoise(uint32_t seed)     { _rng = seed | 1u; }

    // --- audio plane ---
    // WRITES one block (replaces, no accumulate — OscSection owns mixing).
    //   fmBuf   : optional ±1-ish modulator samples; pitch scales by
    //             2^(fmBuf[i] × fmOctaves).  nullptr = no FM.
    //   syncIn  : optional master wrap buffer (see fillsSyncOut); slave
    //             resets phase where syncIn[i] >= 0.  nullptr = free-run.
    //   syncOut : optional wrap buffer THIS osc fills as master.
    //             Values: fraction 0..1 of the sample where the wrap
    //             happened, or -1 when none.  nullptr = don't record.
    void render(float* out, size_t n,
                const float* fmBuf, float fmOctaves,
                const float* syncIn, float* syncOut);

private:
    // Shared per-sample phase step with the two optional behaviours.
    // Templated on the presence flags so each wave loop compiles to the
    // minimal variant actually needed (the switch below picks it) — zero
    // per-sample cost for features not in use.
    template <bool HasFm, bool HasSyncIn, bool HasSyncOut>
    inline float step(size_t i, const float* fmBuf, float fmOctaves,
                      const float* syncIn, float* syncOut);

    // One rendering loop per waveform family; templated the same way and
    // instantiated from render()'s dispatch.
    template <bool HasFm, bool HasSyncIn, bool HasSyncOut>
    void renderImpl(float* out, size_t n,
                    const float* fmBuf, float fmOctaves,
                    const float* syncIn, float* syncOut);

    float nextNoise();                // xorshift32, ±1.0

    Wave     _wave   = Wave::Saw;
    float    _phase  = 0.0f;          // 0..1
    float    _inc    = 0.0f;          // per-sample increment
    float    _freq   = -1.0f;         // dirty check
    float    _shape  = 0.5f;          // pulse width / triangle skew
    float    _shValue = 0.0f;         // current Sample & Hold level
    uint32_t _rng    = 0x9E3779B9u;

    const int16_t* _arbData = nullptr;
    uint16_t       _arbLen  = 0;
};

} // namespace JT
