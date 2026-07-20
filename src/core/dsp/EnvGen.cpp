// =============================================================================
// EnvGen.cpp — implementation (model and rationale in EnvGen.h)
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "core/dsp/EnvGen.h"

#include <cmath>   // powf only — float-suffixed per project rules

namespace JT {

void EnvGen::noteOn()
{
    // Attack always launches from the CURRENT level — a retrigger of a
    // sounding voice continues smoothly instead of clicking to zero.
    _stageStart = _level;
    _phase      = 0.0f;
    _stage      = Stage::Attack;
}

void EnvGen::noteOff()
{
    if (_stage == Stage::Idle) return;
    if (_stage == Stage::FadeOut) return;   // already dying faster than a release
    _stageStart = _level;         // release from wherever we are (mid-attack
    _phase      = 0.0f;           // included), never a jump to sustain first
    _stage      = Stage::Release;
}

void EnvGen::hardKill()
{
    _stage = Stage::Idle;
    _level = 0.0f;
    _phase = 0.0f;
}

void EnvGen::quickFade()
{
    // Steal fade (see header): one FadeOut tick lands the level on exactly 0
    // and the stage on Idle; the voice's per-sample ramp does the actual
    // 128-sample linear fade.  A voice that is already silent needs nothing.
    if (_stage == Stage::Idle) return;
    _stage = Stage::FadeOut;
}

float EnvGen::tickBlock()
{
    switch (_stage) {
        case Stage::Idle:
            return 0.0f;                       // free: no math at all

        case Stage::FadeOut:
            // Single-shot: report zero for this block (the voice ramps down
            // to it), then park.  After this tick the voice sees the env as
            // inactive and fires its pending restart on the next block.
            hardKill();
            return 0.0f;

        case Stage::Sustain:
            // Track sustain edits live (a knob move mid-note is audible on
            // real hardware); still zero powf while parked.
            _level = _sustain;
            return _level;

        case Stage::Attack: {
            _phase += phaseIncFor(_attackMs);
            if (_phase >= 1.0f) {
                // Stage complete: land EXACTLY on 1.0 (no float dust), then
                // fall into decay next block starting from the true peak.
                _level      = 1.0f;
                _stageStart = 1.0f;
                _phase      = 0.0f;
                _stage      = Stage::Decay;
                return _level;
            }
            // t^(1/slope): slope>1 rises fast then eases — see header.
            const float shaped = powf(_phase, 1.0f / _attackSlope);
            _level = _stageStart + (1.0f - _stageStart) * shaped;
            return _level;
        }

        case Stage::Decay: {
            _phase += phaseIncFor(_decayMs);
            if (_phase >= 1.0f) {
                _level = _sustain;
                _stage = Stage::Sustain;
                return _level;
            }
            const float shaped = powf(1.0f - _phase, _decaySlope);
            _level = _sustain + (_stageStart - _sustain) * shaped;
            return _level;
        }

        case Stage::Release: {
            const float inc = phaseIncFor(_releaseMs);
            _phase += inc;
            if (_phase >= 1.0f) {
                hardKill();                    // done: voice becomes stealable
                return 0.0f;
            }
            float shaped = powf(1.0f - _phase, _releaseSlope);

            // Tail taper (header §RELEASE TAIL TAPER): slope < 1 curves hold
            // a large level until the last block — spread the landing across
            // the final kReleaseTailBlocks instead.  Slope >= 1 skips this
            // entirely and remains byte-identical to previous baselines.
            if (_releaseSlope < 1.0f) {
                const float remain = 1.0f - _phase;
                float window = kReleaseTailBlocks * inc;
                if (window > 1.0f) window = 1.0f;   // ultra-short releases
                if (remain < window) shaped *= remain / window;
            }

            _level = _stageStart * shaped;
            return _level;
        }
    }
    return 0.0f;   // unreachable; keeps -Werror=return-type honest
}

} // namespace JT
