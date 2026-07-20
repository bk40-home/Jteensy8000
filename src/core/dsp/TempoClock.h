// =============================================================================
// TempoClock.h — internal BPM clock for JT-8000 v2 (Phase 3 subsystem 2)
// =============================================================================
//
// ROLE
//   Pure math: holds the current internal BPM and converts a musical
//   division (the same 12-entry TimingMode set v1 used) into a rate in Hz
//   for a synced LFO.  One instance lives on SynthCore (control-plane
//   member only — never touched from the audio inner loop, see the
//   SynthCore.h note on _clock).
//
// SCOPE THIS PASS (docs/PHASE3_BPMCLOCK_SPEC.md §3, decisions #1/#2)
//   Internal clock + LFO sync only.  External MIDI clock is ACCEPTED as a
//   source selection (setSource) but is INERT: there is no micros()/24-PPQN
//   measurement path in the Arduino-free core yet, and no consumer needs it
//   before the external-clock pass lands.  getTimeForMode() (v1's ms
//   conversion, used by the delay) is deliberately NOT ported — the delay/FX
//   subsystem doesn't exist in v2 yet; add it alongside that pass.
//
// FLAGGED DEVIATION FROM V1 (spec §3 decision #3, user-signed-off)
//   v1's SynthEngine::getFrequencyForMode() computed `(BPM/60) * mult`, which
//   MULTIPLIES by the quarter-notes-per-cycle multiplier where the math
//   requires a DIVIDE — a bug (v1's own header comment, ".h:141", claims
//   "1/8 -> 4 Hz" but the code it describes produces 1 Hz at 120 BPM).  v2
//   corrects this: freqForMode() DIVIDES by the multiplier, so a synced LFO
//   matches the musical label (1/8 @ 120 BPM = 4 Hz).  Consequence: a v2
//   synced LFO will NOT reproduce v1's audible rate for any sub-1/4 division
//   — this is deliberate, not a porting slip.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

namespace JT {

class TempoClock {
public:
    // Timing-mode option order is FROZEN with kOpt_timing_mode in
    // ParamTable.h (== v1's TimingMode enum, verified against
    // BPMClockManager.h) — do not reorder without a patch-compat pass.
    enum Mode : int {
        kFree = 0, k4Bars, k2Bars, k1Bar, k1_2, k1_4,
        k1_8, k1_16, k1_32, k1_4T, k1_8T, k1_16T, kNumModes
    };

    // Clock-source option order matches kOpt_clock_source.  kExtMidi is
    // stored but inert this pass (spec decision #2) — see file header.
    enum Source : int { kInternal = 0, kExtMidi = 1 };

    // Clamp 40..300 BPM — v1's internal-clock range (BPMClockManager.cpp:88).
    void  setBpm(float bpm);
    float bpm() const { return _bpm; }

    // Stores the selection only; kExtMidi does not change any behaviour
    // until the external-clock pass adds a BPM-injection path.
    void  setSource(int opt) { _source = opt; }
    int   source() const { return _source; }

    // Hz for a synced LFO at the current BPM.  <= 0 means "not synced"
    // (kFree) — the caller falls back to the LFO's own freeHz knob value.
    // CORRECTED vs v1 (see file header): divides by the multiplier.
    float freqForMode(int mode) const;

private:
    // 170.0f matches CLOCK_TEMPO's table default (ParamTable.h), NOT v1's
    // 120 BPM default (spec §2 flagged pre-existing table value) — so a
    // sync engaged before any CLOCK_TEMPO write uses the same BPM the store
    // will report once its default is applied.
    float _bpm    = 170.0f;
    int   _source = kInternal;

    // v1's _beatMultipliers table, ported verbatim (spec §3 decision #4),
    // including the rounded triplet decimals — quarter-notes per cycle.
    // Source: BPMClockManager.cpp:219-235 (updateBeatMultipliers).
    static constexpr float kMult[kNumModes] = {
        0.0f,                          // kFree — unused, freqForMode short-circuits
        16.0f, 8.0f, 4.0f, 2.0f, 1.0f, // 4 Bars, 2 Bars, 1 Bar, 1/2, 1/4
        0.5f, 0.25f, 0.125f,           // 1/8, 1/16, 1/32
        0.6667f, 0.3333f, 0.1667f      // 1/4T, 1/8T, 1/16T (v1's rounded decimals)
    };
};

} // namespace JT
