// =============================================================================
// StepSequencer.h — JT-8000 v2 step sequencer (16-step, block-rate)
// =============================================================================
//
// PROVENANCE
//   Ported logic-for-logic from v1 `StepSequencer.{h,cpp}`.  Original design +
//   the v2.1 anti-click / ramp-freeze fixes: Kris Bishop.  See
//   docs/PHASE7_SEQUENCER_SPEC.md for the file:line diagnosis this port follows.
//
// WHAT THIS IS
//   A CONTROL-PLANE subsystem: it emits ONE modulation value per block (range
//   ±1.0 = unipolar step value × bipolar depth) that SynthCore routes to one of
//   four destinations (Pitch / Filter / PWM / Amp) — the SAME modulation lanes
//   the LFO section already drives.  It produces no audio samples of its own.
//   Ticked once per SynthCore::renderBlock at kBlockMs (≈ 2.9025 ms), which
//   reproduces v1's once-per-update tick exactly (same block rate).
//
// STEP / DEPTH CONVENTION
//   Steps read UNIPOLAR by default: 0..127 -> 0.0..1.0 ("how much").
//   Depth is BIPOLAR:  -1.0..+1.0 ("which direction" + amount).
//   Output = stepValue x depth.
//
//   Unipolar steps avoid the amp-mod zero-crossing noise that bipolar steps
//   caused, and the depth sign carries direction — but they can only depart
//   from base in ONE direction, which is why an aux Pan lane could sweep
//   centre->one side and never L<->R.  Each lane therefore has an optional
//   BIPOLAR interpretation (setStepBipolar / setAuxBipolar): a step reads
//   (2v - 1), so 0.5 is the centre and one pattern reaches both sides.
//   Default OFF, so existing patches are unchanged.
//
// GATE (v1, unchanged)
//   NOTE for anyone chasing "why is the modulation so subtle": BOTH lanes are
//   held at zero outside the gate window, so at the default gate of 0.5 each
//   lane is silent for half of every step.  That is inherited v1 behaviour and
//   is deliberate — it is a gated modulator, not a stepped one.
//   Gate open while phaseFrac < gateLength.  On close the output ramps LINEARLY
//   to zero over SEQ_GATE_RAMP_MS (2 ms) from a FROZEN gate-close value, not an
//   instant snap (which clicks) and not a compounding multiply (v1's original
//   geometric-decay bug, fixed in v2.1 by freezing _lastGateOutput).
//
// FLAGGED DEVIATIONS (CLAUDE.md rule 2, spec §7 — mirror the running ledger)
//   D-1  SEQ_TIMING_MODE tempo-sync DEFERRED: v2 TempoClock exposes freqForMode
//        (Hz) but no getTimeForMode (ms), the accessor v1's sync path needs.
//        setTimingMode/updateFromClock are inert stubs here; the sequencer stays
//        free-running at SEQ_RATE.  Bounded follow-up once the ms accessor lands.
//   D-2  RANDOM direction uses the house xorshift RNG (v2 core is Arduino-free),
//        not Arduino random().  Behaviour-equivalent; RANDOM is non-reproducible
//        by design.
//   (D-3/D-4 live in SynthCore: range-gate collapse + no destination-change
//    cleanup — see SynthCore.cpp / spec §7.)
//
// CPU DISCIPLINE ("do not calculate if not required")
//   tick() early-exits when disabled (only finishing an in-flight ramp).  All
//   maths is one branch + a couple of multiplies per block — trivial.  No pool,
//   no per-sample work.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>

#include "core/AudioConfig.h"
#include "core/dsp/TempoClock.h"

namespace JT {

// Direction modes — order matches kOpt_seq_dir {Fwd,Rev,Bounce,Random}.
enum class SeqDir : uint8_t { Forward = 0, Reverse, Bounce, Random, Count };

// Destination lanes — order matches kOpt_seq_dest {None,Pitch,Filter,PWM,Amp}.
// Shares the LFO destination space (v1 LFODestination).
enum class SeqDest : uint8_t { None = 0, Pitch, Filter, Pwm, Amp, Count };

// AUX-lane destinations — order matches kOpt_seq_aux_dest and is FROZEN
// (patches store the INDEX).  A DIFFERENT set and order from the gate lane's
// SeqDest, so it gets its own enum.
//
//   Tone  index 4.  Bass<->treble TILT on the FX Tone EQ, +/-6 dB at full
//         depth.  Previously LABELLED "Drive" while doing this; the label was
//         the thing that was wrong, so it was corrected rather than the
//         behaviour.  Works whatever the drive mode is.
//   Drive index 5.  The real thing: modulates the saturation INPUT gain.  It
//         can only be heard when fx.drive is not OFF, because the saturator
//         bypasses entirely in that mode.
enum class SeqAuxDest : uint8_t {
    None = 0, Filter, Pan, DelaySend, Tone, Drive, Count
};

class StepSequencer {
public:
    static constexpr int   kMaxSteps    = 16;    // v1 SEQ_MAX_STEPS
    static constexpr float kGateRampMs   = 2.0f; // v1 SEQ_GATE_RAMP_MS (anti-click)

    // Free-run rate limits.  Widened to COVER the synced extremes (fastest
    // synced = 1/32 @ 300 BPM = 40 Hz; slowest = 4 bars @ 40 BPM = 0.0417 Hz),
    // which the previous 0.05..50 clamp allowed but the 0.1..20 knob law in
    // SynthCore did not reach.  Both now agree.
    static constexpr float kFreeHzMin   = 0.02f;
    static constexpr float kFreeHzMax   = 50.0f;

    StepSequencer();

    // Tick once per block.  deltaMs is the block period (kBlockMs).
    void tick(float deltaMs);

    // Read after tick(): current output, scaled by depth (±|depth|, 0 idle).
    float   getOutput()      const { return _output; }
    int     currentStep()    const { return _currentStep; }
    bool    gateOpen()       const { return _gateOpen; }
    SeqDest destination()    const { return _destination; }

    // ---- AUX LANE (Stage B) ----------------------------------------------
    // A second modulation output computed in the SAME tick(), sharing the gate
    // lane's clock (position, rate, direction, gate timing, slide).  It carries
    // its own step values, depth and destination.  It does NOT carry the amp
    // anti-click ramp — aux destinations are cutoff/pan/send/drive, smoothed at
    // their own stage; the ramp is amp-specific.  Emits 0 while dest==None or
    // depth==0, so an unused aux lane costs one multiply and adds nothing.
    float      getAuxOutput()   const { return _auxOutput; }
    SeqAuxDest auxDestination() const { return _auxDest; }
    bool    retrigger()      const { return _retrigger; }
    bool    enabled()        const { return _enabled; }

    // ---- Parameter setters (clamp at the boundary) ----
    void setEnabled(bool on);
    void setStepCount(int count);              // 1..16
    void setStepValue(int step, uint8_t cc);   // 0..127 unipolar
    void setGateLength(float frac);            // 0..1
    void setSlide(float frac);                 // 0..1
    void setDirection(SeqDir dir);
    void setDestination(SeqDest dest);
    void setDepth(float d);                    // -1..+1 bipolar
    void setRate(float hz);                    // 0.05..50 Hz (free-run)
    void setRetrigger(bool on);
    void reset();

    // ---- AUX LANE setters (Stage B) --------------------------------------
    void setAuxStepValue(int step, uint8_t cc);  // 0..127 unipolar
    void setAuxDepth(float d);                    // -1..+1 bipolar
    void setAuxDestination(SeqAuxDest dest);

    // ---- Bipolar step interpretation (per lane, default OFF) --------------
    void setStepBipolar(bool on);
    void setAuxBipolar(bool on);
    bool stepBipolar() const { return _stepBipolar; }
    bool auxBipolar()  const { return _auxBipolar; }

    // D-1: tempo-sync deferred.  Stored but inert — the sequencer stays free-
    // running at SEQ_RATE until TempoClock gains a getTimeForMode(ms) accessor.
    void setTimingMode(int mode);
    int  timingMode() const { return _timingMode; }
    void updateFromClock(const TempoClock& clock);   // no-op while free (D-1)

#ifdef JT_TESTING
    uint8_t debugStepValue(int s) const
    { return (s >= 0 && s < kMaxSteps) ? _stepValues[s] : 0; }
    int   debugStepCount()  const { return _stepCount; }
    float debugStepDurMs()  const { return _stepDurationMs; }
    float debugDepth()      const { return _depth; }
#endif

private:
    void  advanceStep();
    int   nextStepIndex() const;
    void  recalcDuration();
    static float ccToUnipolar(uint8_t cc) { return (float)cc * (1.0f / 127.0f); }

    // One step's value in its lane's interpretation.  Unipolar 0..1, or
    // bipolar -1..+1 with 0.5 (cc 64) as the centre.
    static float stepValueOf(uint8_t cc, bool bipolar)
    {
        const float u = ccToUnipolar(cc);
        return bipolar ? (u * 2.0f - 1.0f) : u;
    }

    // D-2: house xorshift (matches SupersawOsc/VoiceAllocator).  Non-zero seed.
    inline uint32_t nextRand()
    { uint32_t x = _rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; _rng = x; return x; }

    // ---- Step data ----
    uint8_t _stepValues[kMaxSteps] = { 0 };   // default 0 = no modulation
    int     _stepCount   = 8;

    // ---- Playback state ----
    bool    _enabled     = false;
    int     _currentStep = 0;
    float   _phaseMs     = 0.0f;
    float   _stepDurationMs = 500.0f;
    bool    _gateOpen    = false;
    int     _bounceDir   = 1;

    // ---- Gate ramp (anti-click) ----
    float   _rampValue      = 0.0f;
    bool    _ramping        = false;
    float   _lastGateOutput = 0.0f;

    // ---- Parameters ----
    float   _gateLength = 1.0f;
    float   _slide      = 0.0f;
    float   _depth      = 0.0f;    // bipolar
    float   _rateHz     = 2.0f;
    SeqDir  _direction  = SeqDir::Forward;
    SeqDest _destination= SeqDest::None;
    int     _timingMode = TempoClock::kFree;
    bool    _retrigger  = false;   // matches seq.retrigger's table default

    // ---- Output ----
    float   _output = 0.0f;

    // ---- AUX LANE state (Stage B) ----------------------------------------
    // Parallel to the gate lane, sharing _currentStep / phaseFrac / _slide.
    // Defaults are the no-op values (dest None, depth 0, values 0) so an
    // untouched aux lane emits 0 and the default patch stays byte-identical.
    uint8_t    _auxValues[kMaxSteps] = { 0 };
    float      _auxDepth = 0.0f;                 // bipolar
    SeqAuxDest _auxDest  = SeqAuxDest::None;
    float      _auxOutput = 0.0f;

    // Per-lane bipolar interpretation.  Both default OFF so the default patch
    // and every existing patch behave exactly as before.
    bool       _stepBipolar = false;
    bool       _auxBipolar  = false;

    uint32_t _rng = 0x51F5A3C7u;
};

} // namespace JT
