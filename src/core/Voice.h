// =============================================================================
// Voice.h — one synthesis voice for JT-8000 v2 (Phase 1 topology)
// =============================================================================
//
// TOPOLOGY (Pass 4/6):  OscSection -> FilterSection -> amp EnvGen * velocity -> mix bus
//   OscSection carries the full v1 source set (dual osc with all waves and
//   supersaw, sub, noise, ring, cross-mod, hard sync).  A SECOND EnvGen (the
//   filter envelope) does NOT touch the audio gain — it feeds FilterSection's
//   block-rate cutoff modulation (Pass 6), summed there with key-tracking.
//   Same EnvGen class as the amp env (v1 used one EnvelopeBlock class for all
//   envelopes), different destination.  A THIRD EnvGen (the pitch envelope,
//   Pass 7) also modulates neither gain nor cutoff: scaled by a ±24 st depth it
//   feeds OscSection::setPitchModSemis, which applies it as a per-sample ramp
//   through the oscillators' FM input (non-zipped) — v1 summed the pitch env
//   into the audio-rate FM mixer, so this is the same signal path.
//
// RENDER CONTRACT (brief §6.1)
//   render() ADDS this voice into the shared mix buffers.  All work happens
//   in one DTCM scratch buffer with plain function calls — no audio-object
//   graph, no per-module block copies.  An idle voice costs exactly one
//   branch: SynthCore skips it entirely (v1's every-voice-always-runs
//   lesson, designed out).
//
// ENVELOPE APPLICATION
//   The amp envelope ticks once per block; its output is applied as a
//   per-sample LINEAR RAMP from the previous block's level to the new one.
//   That keeps even a 0 ms attack click-free while costing one multiply-add
//   per sample.  The ramp carries the COMPOSITE gain (env level × velocity
//   gain), not the env level alone — so a velocity change on a retrigger is
//   ramped too, never stepped (envelope-click fix, this delivery).
//
// STEAL FADE (envelope-click fix, this delivery)
//   noteOn() on a voice whose applied gain is still audible does NOT restart
//   immediately: restarting resets oscillator phases, the sub phase and the
//   filter state — hard discontinuities that click when multiplied by a
//   non-zero envelope (the from-current-level attack keeps the ENVELOPE
//   continuous, but not the signal underneath it).  Instead the strike is
//   stashed, the amp env is quickFade()d (one-block linear ramp to silence
//   through the existing per-sample ramp), and render() fires the real
//   restart on the next block, when every reset is inaudible.  Cost: <= one
//   block (2.9 ms) of extra onset latency on stolen / retriggered notes
//   only; a clean strike on a silent voice is unchanged and pays nothing.
//   A noteOff arriving during the fade is remembered and re-applied after
//   the restart, so ultra-short re-strikes still speak then release.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>

#include "core/AudioConfig.h"
#include "core/dsp/EnvGen.h"
#include "core/OscSection.h"
#include "core/FilterSection.h"

namespace JT {

class Voice {
public:
    // --- note events (control plane) ---
    // 'phaseRandom01' comes from the allocator's RNG and seeds the section's
    // per-voice randomness (phases, S&H, noise) deterministically.
    void noteOn(uint8_t note, uint8_t velocity, float phaseRandom01);
    void noteOff();
    void hardKill();                       // steal fallback: instant silence

    // --- per-voice parameter targets (engine dirty-application) ---
    // Shared parameters fan out to all voices; each voice keeps its own
    // DSP state so per-voice modulation (Phase 3) needs no rework.
    OscSection&    oscSection() { return _oscs; }
    FilterSection& filter()     { return _filter; }
    EnvGen&        ampEnv()     { return _ampEnv; }
    EnvGen&        filterEnv()  { return _filterEnv; }
    EnvGen&        pitchEnv()   { return _pitchEnv; }
    // Pitch-env depth in semitones (ENV_PITCH_DEPTH × 24); multiplies the
    // unipolar 0..1 env each block before it reaches the oscillators.
    void setPitchEnvDepthSemis(float s) { _pitchDepthSemis = s; }

    // Phase 3 / G1: this block's LFO pitch contribution, in semitones, now in
    // TWO lanes (SynthCore computes both; see docs/PHASE3_LFO_SPEC.md §4):
    //   common — LFO2 pitch + sequencer pitch.  Always both oscs, never
    //            routed (JP-8000 manual p.112: only "LFO1 & ENV" route).
    //   routed — LFO1 pitch alone; render() sums it with the pitch env and
    //            steers the pair by _pitchDest (mix.pitch_mod_dest).
    // Each lane is still an overwrite-with-total into the section — no
    // accumulator, no reset bugs (spec Decision #3 unchanged).
    void setLfoPitchSemis(float semis)       { _lfoPitchSemis = semis; }
    void setRoutedLfoPitchSemis(float semis) { _routedLfoSemis = semis; }

    // G1: JP-8000 "LFO1 & ENV Destination" — 0 OSC1+2 (default, the
    // pre-routing behaviour), 1 OSC2 only, 2 X-MOD depth.  Option index from
    // the table (mix.pitch_mod_dest), pushed by SynthCore on change.
    void setPitchModDest(uint8_t dest) { _pitchDest = dest; }

    // --- Phase 4: glide / portamento (per-voice pitch slew) ---------------
    // Ported from v1 OscillatorBlock (glide lived per-oscillator there; here a
    // single per-voice slew feeds the SAME octave-space FM path as env/LFO, so
    // net pitch is identical and glide sums cleanly with them — spec §4.1).
    //   setGlideEnabled  — arms/disarms; disarming cancels an in-flight glide.
    //   setGlideRate     — v1's 1/samples fraction (see SynthCore for the ms map).
    //                      NOTE: applied ONCE PER BLOCK, reproducing v1's
    //                      documented ~128× "quirk" so preset glide-times match.
    void setGlideEnabled(bool on)  { _glideEnabled = on; if (!on) _glideActive = false; }
    void setGlideRate(float rate)  { _glideRate = rate; }

    // --- Phase 4: pitch bend (engine-shared semitone offset) --------------
    // v1 wrote the wheel as a DC into every voice's FM pre-mixer; v2 sums it
    // into the same setPitchModSemis total (spec §4.3).  0 = centred wheel.
    void setBendSemis(float semis) { _bendSemis = semis; }

    // Current sounding base frequency (Hz), for the allocator's poly glide
    // pre-seed: a stolen voice should glide FROM the synth's last note, not the
    // stale pitch it happens to hold (v1 SynthEngine setGlideFromFreq, §1.1).
    float baseHz() const { return _baseHz; }
    // Pre-seed the glide start pitch (v1 setGlideFromFreq): only meaningful with
    // glide armed; harmless otherwise.  >20 Hz guard mirrors v1.
    void setGlideFromHz(float hz) { if (hz > 20.0f) _baseHz = hz; }

    // Velocity sensitivity knobs (Pass 8).  Stored here and consumed at the NEXT
    // noteOn, exactly as v1's VoiceBlock held them and applied them in noteOn:
    // amp-sens shapes _velGain locally; filter- and env-sens derive per-note DC
    // that is pushed into FilterSection (see Voice.cpp).  0..1 each.
    void setVelAmpSens(float s)    { _velAmpSens    = s; }
    void setVelFilterSens(float s) { _velFilterSens = s; }
    void setVelEnvSens(float s)    { _velEnvSens    = s; }

    // --- audio plane ---
    // Adds one block into mixL/mixR.  Caller guarantees n == kBlockSize.
    void render(float* mixL, float* mixR, size_t n);

    // --- allocator queries ---
    // A voice with a stashed restart counts as active even while its amp env
    // sits Idle for the one block between fade and restart — SynthCore must
    // keep calling render() so the pending note can fire (steal fade above).
    bool    isActive() const     { return _ampEnv.isActive() || _pendingRestart; }
    bool    isReleasing() const  { return _ampEnv.isReleasing(); }
    uint8_t note() const         { return _note; }
    uint32_t age() const         { return _age; }
    void    setAge(uint32_t a)   { _age = a; }

private:
    // The immediate restart body — everything noteOn() used to do.  Called
    // directly for a silent voice; deferred one block behind quickFade() for
    // an audible one (STEAL FADE above).
    void startNote(uint8_t note, uint8_t velocity, float phaseRandom01);

    OscSection    _oscs;
    FilterSection _filter;
    EnvGen        _ampEnv;
    EnvGen        _filterEnv;    // cutoff modulator (not a gain) — see topology
    EnvGen        _pitchEnv;     // oscillator pitch modulator — see topology

    uint8_t _note     = 0;
    float   _velGain  = 0.0f;      // velocity -> gain (× amp-sens curve), set at noteOn
    // Previous block's APPLIED gain (env level × _velGain) — the ramp start.
    // Composite, so both env moves AND velocity changes are ramped; also the
    // audibility test for the steal fade (is this voice still heard?).
    float   _lastGain = 0.0f;

    // --- steal-fade pending restart (see header) --------------------------
    bool    _pendingRestart = false;  // strike stashed, waiting for silence
    bool    _pendReleased   = false;  // noteOff arrived during the fade
    uint8_t _pendNote       = 0;
    uint8_t _pendVel        = 0;
    float   _pendPhase01    = 0.0f;
    float   _pitchDepthSemis = 0.0f;  // ENV_PITCH_DEPTH × 24, ±24 st full scale
    float   _lfoPitchSemis  = 0.0f;   // Phase 3: common lane (LFO2 + seq)
    float   _routedLfoSemis = 0.0f;   // G1: LFO1 lane, steered by _pitchDest
    uint8_t _pitchDest      = 0;      // G1: 0 OSC1+2, 1 OSC2, 2 X-MOD

    // Phase 4 glide (spec §4.1).  All Hz; glideSemis is derived per block only
    // while _glideActive, then summed into the FM total.  Defaults are the
    // no-op values (glide off, rate 0) so a default patch is byte-identical.
    bool    _glideEnabled  = false;
    bool    _glideActive   = false;
    float   _glideRate     = 0.0f;    // v1 1/samples fraction; block-rate applied
    float   _glideCurrentHz = 440.0f; // where the slide is now
    float   _glideTargetHz  = 440.0f; // where it's heading (this note's pitch)
    float   _baseHz         = 440.0f; // this note's true frequency (glide start ref)

    // Phase 4 pitch bend (spec §4.3).  0 = centred wheel → no shift → default
    // patch unaffected until a bend message arrives.
    float   _bendSemis      = 0.0f;
    // Velocity sensitivity (Pass 8), 0..1.  Defaults are the v1 power-on values
    // (VoiceBlock.h) AND the no-op values: amp-sens 0 = linear velocity, the
    // filter/env terms 0 = velocity has no effect — so a default patch is
    // byte-identical to Pass 7.  See Voice.cpp noteOn for the v1 formulae.
    float   _velAmpSens    = 0.0f;
    float   _velFilterSens = 0.0f;
    float   _velEnvSens    = 0.0f;
    uint32_t _age     = 0;         // allocator timestamp for steal ordering
};

} // namespace JT
