// =============================================================================
// Lfo.h — one free-running, block-rate LFO for JT-8000 v2 (Phase 3 §5)
// =============================================================================
//
// ROLE
//   SynthCore owns two of these (LFO1/LFO2), one phase pair SHARED by every
//   voice.  This class only produces the delayed, unit (-1..1) waveform; the
//   four per-destination depths and the routing to pitch/filter/PWM/amp live
//   in SynthCore (docs/PHASE3_LFO_SPEC.md §5) — keeping the oscillator itself
//   destination-agnostic, same split as EnvGen/Voice.
//
// UPDATE RATE (FLAGGED DEVIATION from v1 — spec §3.1 / §9 item 3)
//   v1's LFO was audio-rate (AudioSynthWaveform).  v2 ticks ONCE PER BLOCK
//   (~344 Hz), exactly like the envelopes: cheaper, and per-sample ramps on
//   the pitch/amp destinations (done by the consumers, not here) hide the
//   step for anything below ~20 Hz of LFO rate.  Filter and PWM are left
//   block-rate STEPPED — inaudible at typical LFO rates, per the sign-off.
//
// WAVEFORMS (bipolar, phase p in [0,1), option order == kOpt_l_f_o_wave)
//   SIN = FastMath::fastSin01(p)   TRI = 1-4|p-0.5|      SAW = 2p-1
//   SQR = p<0.5 ? +1 : -1          S&H = held random, redrawn on phase wrap
//   NOISE = fresh random every block (not gated by phase wrap)
//   NOTE: FastMath.h reserves fastSin01 for AUDIO-RATE inner loops; using it
//   here at block rate is a LOCKED spec decision (§5), not an oversight —
//   the LFO doesn't need libm precision and the call matches OscCore's own
//   waveform math family.
//
// DELAY RAMP (spec §3 decision #7 — v1's shared, global retrigger, §1.3)
//   retrigger() (called from SynthCore::drainNoteEvents on ANY note-on)
//   restarts delayGain at 0 and ramps it linearly to 1.0 over delayMs,
//   advanced by kBlockMs per block — no millis() in core/.  delayMs == 0 is
//   a no-op: the LFO stays at full amplitude (matches v1's CC-0 boundary).
//
// ENGAGEMENT (spec §4 "Engaged")
//   SynthCore only calls tickBlock() for an LFO with at least one non-zero
//   destination depth; a disengaged LFO's phase is simply never advanced
//   here ("do not calculate if not required") — CPU-only, inaudible.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>

#include "core/AudioConfig.h"

namespace JT {

class Lfo {
public:
    // Waveform option indices — frozen with kOpt_l_f_o_wave in ParamTable.h.
    enum : int { kSin = 0, kTri = 1, kSaw = 2, kSqr = 3, kSampleHold = 4, kNoise = 5 };

    // seed: deterministic per-instance RNG seed (S&H / NOISE), so tests and
    // renders reproduce bit-for-bit — see SynthCore's two fixed seeds.
    explicit Lfo(uint32_t seed) : _rng(seed) {}

    void setWave(int opt)     { _wave = opt; }
    void setRateHz(float hz)  { _rateHz = hz; }
    void setDelayMs(float ms) { _delayMs = ms; }

    void retrigger()
    {
        if (_delayMs > 0.0f) { _delayGain = 0.0f; _ramping = true; }
    }

    // Advance one block; returns unit waveform (bipolar, ~-1..1) x delayGain.
    float tickBlock();

#ifdef JT_TESTING
    // Test-only: the rate SynthCore last pushed via setRateHz.  Lets the BPM
    // tempo-sync tests assert the exact synced Hz deterministically instead of
    // measuring it statistically from rendered audio.  Compiled out of the
    // firmware — no runtime cost.  (Additive to PHASE3_BPMCLOCK_SPEC §6's "no
    // Lfo change": a test accessor, not a behaviour change — flagged in handoff.)
    float debugRateHz() const { return _rateHz; }
#endif

private:
    float nextRandom01();   // xorshift32, same family as OscCore/OscSection

    int      _wave      = kSin;
    float    _rateHz    = 1.0f;
    float    _delayMs   = 0.0f;
    float    _phase     = 0.0f;
    float    _delayGain = 1.0f;   // 1.0 = no delay pending (default: instant)
    bool     _ramping   = false;
    uint32_t _rng;
    float    _shValue   = 0.0f;   // S&H: held across a cycle, redrawn at wrap
};

} // namespace JT
