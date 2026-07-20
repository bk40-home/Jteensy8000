// =============================================================================
// Voice.cpp — implementation (contracts in Voice.h)
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "core/Voice.h"

#include <cmath>   // exp2f

namespace JT {

namespace {
// Applied-gain threshold below which a restart cannot click: -60 dBFS before
// kVoiceGain.  Above it, the strike is deferred behind a one-block fade.
constexpr float kAudibleRestartGain = 1.0e-3f;
} // namespace

void Voice::noteOn(uint8_t note, uint8_t velocity, float phaseRandom01)
{
    // STEAL FADE (Voice.h): a still-audible voice must reach silence before
    // startNote()'s phase / filter resets run, or they click.  Stash the
    // strike, fade this block, restart next block from render().
    if (_lastGain > kAudibleRestartGain) {
        _note        = note;   // updated NOW so a noteOff during the fade
                               // matches this voice in the allocator scan
        _pendNote    = note;
        _pendVel     = velocity;
        _pendPhase01 = phaseRandom01;
        _pendingRestart = true;
        _pendReleased   = false;   // fresh strike supersedes any earlier lift
        // The dying note's glide offset is meaningless (and the allocator has
        // already moved _baseHz for the NEW note's glide seed) — cancel it so
        // the fade block renders at the old note's settled pitch.
        _glideActive = false;
        _ampEnv.quickFade();       // one-block linear fade via the render ramp
        return;
    }

    // Silent voice: restart immediately (the common clean-strike path).
    // Clear any stale stash — a pending strike cannot outlive silence plus a
    // newer noteOn.
    _pendingRestart = false;
    _pendReleased   = false;
    startNote(note, velocity, phaseRandom01);
}

void Voice::startNote(uint8_t note, uint8_t velocity, float phaseRandom01)
{
    _note = note;

    // Equal temperament: A4 = MIDI 69 = 440 Hz.  exp2f is one FPU-friendly
    // call at note-on rate; the section derives per-unit offsets from it.
    const float hz = 440.0f * exp2f(((float)note - 69.0f) / 12.0f);

    // Glide seeding (v1 OscillatorBlock::noteOn, spec §4.1).  When armed with a
    // real rate and a valid previous pitch, slide FROM where we are (_baseHz)
    // toward the new note; otherwise jump straight to it.  The oscillators are
    // ALWAYS tuned to the true note Hz (hz) — glide is expressed as a semitone
    // OFFSET into the FM path (render()), never by retuning the cores, so it
    // stacks with env/LFO/bend and costs nothing once settled.
    if (_glideEnabled && _glideRate > 0.0f && _baseHz > 20.0f) {
        _glideTargetHz  = hz;
        _glideCurrentHz = _baseHz;     // slide starts from the previous pitch
        _glideActive    = true;
    } else {
        _glideCurrentHz = _glideTargetHz = hz;
        _glideActive    = false;
    }
    _baseHz = hz;                       // true frequency the cores are tuned to

    // The allocator's phase random doubles as the section's seed source —
    // deterministic under test, decorrelated across voices in play.
    _oscs.noteOn(hz, (uint32_t)(phaseRandom01 * 16777216.0f) ^ ((uint32_t)note << 20));

    // Velocity sensitivity (Pass 8), ported verbatim from v1 VoiceBlock::noteOn.
    // v1 caches velocity 0..1 and derives three static per-note scalars.  These
    // reduce to the Pass 7 behaviour at the default knobs (all 0): amp = velNorm
    // linear, no filter/env velocity — so the default patch is byte-identical.
    const float velNorm = (float)velocity * (1.0f / 127.0f);

    // (1) amp: v1 osc amplitude = velNorm · [(1−sens)+sens·velNorm].  We fold
    // that whole product into _velGain (v2 applies gain at the mix, not per osc;
    // net gain is identical).  sens 0 → linear velNorm; sens 1 → velNorm².
    const float velAmpScale = (1.0f - _velAmpSens) + (_velAmpSens * velNorm);
    _velGain = velNorm * velAmpScale;

    // Fresh filter state per note: a stolen voice must not smear the
    // previous note's resonance tail into the new one.  The note number
    // also feeds key tracking (wired in Pass 6).
    _filter.reset();
    _filter.noteOn(note);

    // (2) filter cutoff: v1 cutoffOct = sens·(velNorm−0.5)·3 (±1.5 oct bipolar,
    // neutral at velNorm 0.5).  Pushed as a per-note DC the filter adds to modOct
    // (independent of octaveControl, matching v1's base·2^offset).
    static constexpr float kVelFilterOctRange = 3.0f;  // v1 VoiceBlock constant
    _filter.setVelCutoffOffsetOct(_velFilterSens * (velNorm - 0.5f) * kVelFilterOctRange);

    // (3) filter-env depth: v1 envScale = (1−sens)+sens·velNorm scales the filter
    // env amount.  Pushed as a factor on the filter's envDC (1.0 = no scaling).
    _filter.setEnvVelScale((1.0f - _velEnvSens) + (_velEnvSens * velNorm));

    _ampEnv.noteOn();
    _filterEnv.noteOn();   // gated with the note; drives cutoff, not gain
    _pitchEnv.noteOn();    // gated with the note; drives oscillator pitch
}

void Voice::noteOff()
{
    // Key lifted while the steal fade is still in flight (< 2.9 ms strike):
    // remember it — render() strikes the pending note then releases it, so
    // even the shortest re-strike speaks instead of vanishing or sticking.
    if (_pendingRestart) { _pendReleased = true; return; }
    _ampEnv.noteOff();
    _filterEnv.noteOff();
    _pitchEnv.noteOff();
}

void Voice::hardKill()
{
    _ampEnv.hardKill();
    _filterEnv.hardKill();
    _pitchEnv.hardKill();
    _filter.reset();
    _lastGain = 0.0f;
    // A panic / mode switch also drops any stashed strike — nothing may
    // resurrect after an emergency stop.
    _pendingRestart = false;
    _pendReleased   = false;
    // Cancel any in-flight glide so a reused voice starts clean (no residual
    // slide from the killed note's pitch).
    _glideActive = false;
}

void Voice::render(float* mixL, float* mixR, size_t n)
{
    // Steal-fade drain (Voice.h): the fade block has run (amp env parked
    // Idle at level 0, _lastGain ramped to 0) — fire the stashed strike NOW,
    // so this block is the new note's first.  All of startNote()'s resets
    // land at zero gain: inaudible by construction.  Total added latency is
    // exactly the one fade block.
    if (_pendingRestart && !_ampEnv.isActive()) {
        _pendingRestart = false;
        startNote(_pendNote, _pendVel, _pendPhase01);
        if (_pendReleased) {       // key already lifted during the fade:
            _pendReleased = false; // strike then release — the note still
            noteOff();             // speaks through its natural release
        }
    }

    // Mono voice into a stereo bus (Phase 1).  Pan/spread lands with the
    // Phase 2 OscSection.  Scratch lives on the stack: on the Teensy the
    // audio ISR stack is DTCM, exactly where hot buffers belong.
    float buf[kBlockSize];

    // Pitch envelope: one block-rate tick × depth (semitones), pushed to the
    // section BEFORE it renders so this block's oscillators are tuned to the
    // current env level.  The section applies it as a per-sample ramp through
    // the FM input, so even a 0 ms attack shifts pitch WITHOUT zipper (v1 fed
    // the pitch env through the audio-rate FM mixer — same smoothness).
    // When depth is 0 (the common case — most patches don't use it) we don't
    // even tick the env: "do not calculate if not required".  Still push a
    // target (env term + LFO term) so a just-disabled sweep ramps back to
    // base pitch rather than freezing.  Phase 3: the LFO pitch contribution
    // (already netted across both LFOs by SynthCore, spec Decision #3) is
    // summed in here rather than inside OscSection, which still receives
    // ONE total and stays a plain overwrite — no accumulator, no reset bugs.
    const float envTerm = (_pitchDepthSemis != 0.0f)
                              ? _pitchEnv.tickBlock() * _pitchDepthSemis
                              : 0.0f;

    // Glide slew (v1 OscillatorBlock::update(), spec §4.1).  Advanced ONCE PER
    // BLOCK — this block-rate application is v1's documented ~128× "quirk",
    // preserved so preset glide-times feel identical.  Constant-ratio slide:
    // move a fixed FRACTION of the remaining distance each block.  Costs a
    // couple of ops only while a glide is in flight; 0 when settled (the common
    // case), honouring "do not calculate if not required".
    float glideSemis = 0.0f;
    if (_glideActive) {
        const float delta = _glideTargetHz - _glideCurrentHz;
        if (delta < 0.1f && delta > -0.1f) {   // |delta| < 0.1 Hz: snap & stop
            _glideCurrentHz = _glideTargetHz;
            _glideActive    = false;
        } else {
            _glideCurrentHz += delta * _glideRate;
        }
        // Express the slide as a semitone offset the FM path applies on top of
        // the cores' true tuning (_baseHz): 12·log2(current/base), 0 when done.
        glideSemis = 12.0f * log2f(_glideCurrentHz / _baseHz);
    }

    // ONE total into the section (overwrite, no accumulator — spec §4.3 / the
    // Phase 3 LFO decision pattern): env + LFO + glide + bend, all semitones,
    // all summed in the same octave-space FM path v1 used.
    _oscs.setPitchModSemis(envTerm + _lfoPitchSemis + glideSemis + _bendSemis);

    _oscs.render(buf, n);          // writes the mixed oscillator section

    // Filter envelope: one block-rate tick, pushed to the section BEFORE it
    // filters so this block's cutoff reflects the current env level (v1 fed
    // the filter env through the cutoff mod bus at block rate — same cadence).
    // Unipolar 0..1; the section applies sign/depth via filter.env_amount.
    _filter.setEnvLevel(_filterEnv.tickBlock());
    _filter.process(buf, n);

    // Block-rate envelope, per-sample linear ramp of the COMPOSITE gain
    // (env × velocity — Voice.h §ENVELOPE APPLICATION).  Ramping the product
    // means a retrigger's new velocity is glided across the block exactly
    // like the env level, never stepped.  kVoiceGain is constant, so it is
    // folded into the ramp endpoints — the hot loop stays at one multiply
    // per sample for the gain (one FEWER than before this fix).
    const float gainEnd = _ampEnv.tickBlock() * _velGain;
    const float g0      = _lastGain * kVoiceGain;
    const float g1      = gainEnd   * kVoiceGain;
    _lastGain = gainEnd;
    const float step = (g1 - g0) / (float)n;

    float gain = g0;
    for (size_t i = 0; i < n; ++i) {
        gain += step;
        const float s = buf[i] * gain;
        mixL[i] += s;
        mixR[i] += s;
    }
}

} // namespace JT
