// =============================================================================
// EnvGen.h — block-rate ADSR envelope with variable-slope curves
// =============================================================================
//
// DESIGN (brief §6.1)
//   The envelope advances once per audio block (2.9 ms) and returns its new
//   level; the VOICE ramps linearly between the previous and new level
//   across the block's 128 samples.  Control-rate envelopes are authentic
//   to the JP-8000 and ~128x cheaper than per-sample evaluation.
//
// SHAPE MODEL
//   Each stage owns a slope from the table's Seg2 curve params
//   (0.15 .. 1.0 .. 5.0, centre = 1.0 = linear).  A stage runs a phase
//   accumulator t: 0 -> 1 over its duration and shapes it:
//     attack : level = start + (1 - start)      * t^(1/slope)
//     decay  : level = sus   + (start - sus)    * (1 - t)^slope
//     release: level =         start            * (1 - t)^slope
//   slope > 1 gives the fast-start/slow-tail feel of an analog RC stage;
//   slope < 1 the opposite.  V1 LESSON APPLIED: the shape is evaluated from
//   the accumulated phase EVERY block (one powf), not converted to a
//   per-block multiplier at the stage transition — the multiplier approach
//   is what made v1's curved envelopes finish at the wrong time.
//   NOTE: this is a v2 formulation, not a byte-for-byte port of v1's
//   envelope; matching v1 "feel" is the Phase 2 exit A/B task (render
//   harness WAVs vs v1 captures) — flagged per the no-silent-functionality-
//   change rule.
//
// RELEASE TAIL TAPER (envelope-click fix, this delivery)
//   (1 - t)^slope with slope < 1 has INFINITE gradient at t = 1: the curve
//   holds a large fraction of the start level until the very last block,
//   then the discrete phase check truncates it — a fade from (say) half
//   amplitude to zero inside one 2.9 ms block, audible as a soft thump at
//   the end of long releases.  Fix: over the final kReleaseTailBlocks the
//   shaped level is multiplied by a linear taper (remaining-phase / window)
//   so the landing is spread across ~12 ms instead of one block.  GATED ON
//   slope < 1.0 only — slope >= 1 curves already approach zero smoothly and
//   stay byte-identical to previous baselines.  Cost: one compare per
//   release block, one extra multiply inside the final four blocks only.
//
// CLICK-FREE GATES
//   noteOn() starts the attack FROM THE CURRENT LEVEL (retrigger of a
//   sounding voice does not snap to zero); noteOff() releases from the
//   current level (a release during attack does not jump to sustain).
//   quickFade() (steal/retrigger fix, this delivery) is the third gate:
//   it parks the envelope in FadeOut, whose single tick returns 0 and goes
//   Idle — the VOICE's per-sample ramp then carries the level linearly to
//   silence across that one block.  The voice uses it to fade a still-
//   audible note before restarting, so the osc-phase / filter resets in a
//   steal happen at zero gain and cannot click.
//
// CPU
//   Cost per active envelope per block: one powf (~30 cycles) + arithmetic.
//   Idle and sustain-hold states return immediately — nothing is computed
//   that isn't required.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>

#include "core/AudioConfig.h"

namespace JT {

class EnvGen {
public:
    // --- control plane-set parameters (engineering units, from Curves) ---
    // Called from the engine's dirty-application, i.e. block boundaries only.
    void setAttackMs(float ms)    { _attackMs  = ms; }
    void setDecayMs(float ms)     { _decayMs   = ms; }
    void setSustain(float level)  { _sustain   = level; }   // 0..1
    void setReleaseMs(float ms)   { _releaseMs = ms; }
    void setAttackSlope(float s)  { _attackSlope  = s; }    // 0.15..5, 1=linear
    void setDecaySlope(float s)   { _decaySlope   = s; }
    void setReleaseSlope(float s) { _releaseSlope = s; }

    // --- gate ---
    void noteOn();
    void noteOff();
    void hardKill();              // instant silence (panic / mode switch)
    // One-block forced fade for a click-free voice steal (see header notes).
    // The envelope stays isActive() until its FadeOut tick has run, so the
    // engine still renders the fade block; after that tick it is Idle and
    // the level is exactly 0.  No-op when already Idle.
    void quickFade();

    // --- audio plane ---
    // Advance one block; returns the level at the END of the block.
    float tickBlock();

    float level() const    { return _level; }
    bool  isActive() const { return _stage != Stage::Idle; }
    bool  isReleasing() const { return _stage == Stage::Release; }

private:
    enum class Stage : uint8_t { Idle, Attack, Decay, Sustain, Release, FadeOut };

    // Final-blocks taper window for slope < 1 releases (~12 ms at 2.9 ms
    // blocks) — long enough to kill the truncation thump, short enough to be
    // inaudible as a shape change on any musical release time.
    static constexpr float kReleaseTailBlocks = 4.0f;

    // Convert a stage time to a per-block phase increment.  A zero-ish time
    // completes in a single block — instant but still ramped across 128
    // samples by the voice, so even 0 ms attacks stay click-free.
    static float phaseIncFor(float ms)
    {
        const float blocks = ms / kBlockMs;
        return (blocks <= 1.0f) ? 1.0f : (1.0f / blocks);
    }

    Stage _stage = Stage::Idle;
    float _phase = 0.0f;          // 0..1 through the current stage
    float _level = 0.0f;          // last returned output level
    float _stageStart = 0.0f;     // level the current stage began from

    float _attackMs  = 1.0f,  _decayMs = 100.0f;
    float _sustain   = 1.0f,  _releaseMs = 5.0f;
    float _attackSlope = 1.0f, _decaySlope = 1.0f, _releaseSlope = 1.0f;
};

} // namespace JT
