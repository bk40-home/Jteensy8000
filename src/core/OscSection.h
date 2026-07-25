// =============================================================================
// OscSection.h — the complete oscillator section of one JT-8000 v2 voice
// =============================================================================
//
// ROLE (Pass 4, Step 3)
//   Everything before the filter: two oscillator units (each owning an
//   OscCore for the 13 basic waves AND a SupersawOsc), the sub oscillator,
//   the noise source, ring modulation, cross-modulation, hard sync, and the
//   section mixer.  Replaces Phase 1's single OscSaw inside Voice.
//
// V1 FIDELITY NOTES (each verified against the uploaded source)
//   * Sync: OSC2 is the MASTER, OSC1 the slave (AudioSynthOscSync wiring).
//     Supersaw cannot participate in sync — the combination did not exist
//     in v1 (the sync engine had its own plain cores); selecting supersaw
//     on a synced oscillator simply renders it unsynced.
//   * Cross-mod: OSC2 output exponentially FMs OSC1 at depth × 10 octaves
//     full scale — v1 routed the same signal into the ±10-octave FM mixer.
//   * Ring: v1's two AudioEffectMultiply units both computed osc1 × osc2
//     (verified: identical input cables).  v2 computes the product ONCE and
//     applies gain (ring1 + ring2) — mathematically identical output.
//   * Sub: SINE one octave below the note, level × 0.9 headroom.  (The v1
//     header comment says "square"; the v1 CODE says begin(WAVEFORM_SINE) —
//     the code is what every patch heard, so sine it is.)
//   * Units: detune ±12 st, fine ±100 cents, freq DC ±24 st (v1's CC was
//     unipolar 0..+24; the v2 table made the range bipolar — a superset
//     with an identical default), shape DC -1..1 -> pulse width 0..1.
//
// ONE DELIBERATE FIX (flagged, needs no patch migration)
//   v1's MIX_BALANCE wrote the SAME mixer gains as OSC1_MIX/OSC2_MIX —
//   whichever CC arrived last silently overwrote the other.  v2 composes:
//     gain1 = mixOsc1 × min(1, 1 − balance)
//     gain2 = mixOsc2 × min(1, 1 + balance)
//   Balance keeps its v1 linear crossfade law, mix levels keep theirs, and
//   at the default (balance = 0) the two are exactly v1-equivalent.
//
// CPU DISCIPLINE ("do not calculate if not required")
//   Per block, each source renders ONLY if audible or needed as a modulator:
//   osc2 runs when its mix, ring, cross-mod or sync demands it; sub, noise
//   and the ring product are skipped at zero level; pitch recomputes its
//   exp2f only when a pitch-affecting parameter actually changed.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "core/AudioConfig.h"
#include "core/dsp/OscCore.h"
#include "core/dsp/SupersawOsc.h"
#include "core/dsp/FeedbackComb.h"

namespace JT {

class OscSection {
public:
    // --- note events (control plane, called from Voice) ---
    // seedBase decorrelates the two units' and S&H/noise randomness per
    // voice while keeping the whole engine deterministic for tests/renders.
    void noteOn(float noteHz, uint32_t seedBase);

    // --- per-unit parameters (unit 0 = OSC1, unit 1 = OSC2) ---
    void setWave(int unit, int waveOption);            // osc_wave index
    void setPitchOffset(int unit, int option);         // {-24,-12,0,+12,+24}
    void setFineTuneCents(int unit, float cents);      // ±100
    void setDetuneSemis(int unit, float semis);        // ±12
    void setFreqDcSemis(int unit, float semis);        // ±24
    void setShapeDc(int unit, float dc);               // -1..1 -> width
    void setSupersawDetune(int unit, float v01);
    void setSupersawMix(int unit, float v01);
    void setFeedbackAmount(int unit, float v01);       // JP-8000 comb loop
    void setFeedbackMix(int unit, float v01);
    // One kDelaySamples slice of the platform's comb pool per unit —
    // see FeedbackComb::attachStorage for the DTCM/OCRAM rationale.
    void attachCombStorage(int unit, float* line);
    void setRingMix(int unit, float v01);              // gains SUM (see note)
    void setArbTable(int unit, const int16_t* data, uint16_t len);

    // --- section mixer & interactions ---
    void setMixOsc1(float v01)      { _mix1 = v01; }
    void setMixOsc2(float v01)      { _mix2 = v01; }
    void setMixSub(float v01)       { _subLevel = v01; }
    void setMixNoise(float v01)     { _noiseLevel = v01; }
    void setBalance(float bal)      { _balance = bal; }        // -1..1
    void setCrossMod(float depth01) { _xmodDepth = depth01; }
    void setSyncEnabled(bool on)    { _sync = on; }

    // --- Phase 3: PWM LFO (control plane, block boundaries) ---
    // This block's net LFO offset onto pulse width, split into TWO lanes
    // (G2, JP-8000 OSC Control 2 in SQR): the LFO1 part is scaled PER UNIT by
    // pwmScale (osc<n>.pwm_lfo1_depth) before it offsets that unit's width;
    // the common part (LFO2 + sequencer) is applied unscaled to both — the
    // JP-8000 only ever offered per-osc depth for LFO1.  With both scales at
    // their default 1.0 the sum equals the old single-lane value, so existing
    // patches and the render baseline are byte-identical.
    void setLfoPwm(float lfo1Part, float commonPart)
    {
        _lfoPwm1 = lfo1Part;
        _lfoPwmC = commonPart;
    }
    // osc<n>.pwm_lfo1_depth — how much of LFO1's PWM this unit receives (0..1).
    void setPwmLfo1Scale(int unit, float s) { _u[unit].pwmScale = s; }

    // --- Pass 7: pitch envelope (control plane, block boundaries) ---
    // This block's pitch-mod TARGET in semitones (the voice pushes env×depth
    // before render).  Stored as octaves; render() applies it on top of the
    // base pitch as a per-sample linear ramp through each unit's exponential-FM
    // input (2^(fmBuf·1.0)), so a fast pitch env shifts WITHOUT stepping the
    // frequency at block boundaries — the "non-zipped pitch" requirement.  v1
    // fed the pitch env through the audio-rate FM mixer; this is the same path.
    void setPitchModSemis(float semis) { _pitchModOct = semis * (1.0f / 12.0f); }

    // --- G1: JP-8000 "LFO1 & ENV Destination" (mix.pitch_mod_dest) ---
    // EXTRA pitch-mod target for OSC2 ALONE (semitones), on top of the common
    // term above.  The voice routes (LFO1-pitch + pitch-env) here when the
    // destination is "OSC2"; render() ramps it exactly like the common term
    // so OSC2-only vibrato is just as zipper-free.  0 (the default) costs one
    // compare per block — "do not calculate if not required".
    void setOsc2ExtraPitchSemis(float semis) { _pitchModOct2 = semis * (1.0f / 12.0f); }

    // Additive offset onto the cross-mod DEPTH (normalised, ±1 spans the
    // whole knob range).  The voice routes (LFO1-pitch + pitch-env)/7 st here
    // when the destination is "X-MOD" — the JP trick of sweeping FM depth
    // from the LFO/envelope.  Applied at block rate (a ±1-normalised step at
    // the LFO ceiling of 39 Hz moves ≪ 1 % of range per block, inaudible on
    // an FM DEPTH — unlike pitch, which is why pitch gets the ramp).
    void setXmodOffsetNorm(float x) { _xmodOffset = x; }

    // --- audio plane: WRITE one block of the mixed section ---
    void render(float* out, size_t n);

#ifdef JT_TESTING
    // This block's pitch-mod TARGET (semitones) and the offset actually applied
    // at the END of the previous block (octaves).  During a fast attack these
    // DIFFER — that gap IS the per-sample ramp that keeps pitch un-zipped.
    float debugPitchTargetSemis() const { return _pitchModOct * 12.0f; }
    float debugPitchAppliedOct()  const { return _pitchOctPrev; }
#endif

private:
    struct Unit {
        OscCore      core;
        SupersawOsc  ss;
        FeedbackComb comb;

        int   waveOption   = 1;        // table default: SAW
        float coarseSemis  = 0.0f;
        float fineCents    = 0.0f;
        float detuneSemis  = 0.0f;
        float freqDcSemis  = 0.0f;
        float ringGain     = 0.0f;
        float noteHz       = 440.0f;
        bool  pitchDirty   = true;
        // Phase 3: the knob's OWN shape DC, stored so the PWM LFO can offset
        // it each block without losing the base position (render() recombines
        // base + scaled LFO lanes; see setShapeDc/render()).
        float shapeDcBase  = 0.0f;
        // G2: this unit's share of LFO1's PWM (osc<n>.pwm_lfo1_depth).
        // 1.0 = the pre-split behaviour.
        float pwmScale     = 1.0f;

        bool isSupersaw() const { return waveOption == (int)Wave::Supersaw; }
        // exp2f ONCE per actual pitch change, fanned to both cores so a
        // wave switch mid-note lands on the identical frequency.
        void applyPitchIfDirty();
    };

    float nextNoise();                 // white noise for the noise source

    Unit  _u[2];

    // Member defaults mirror the TABLE defaults (v1 boot: BOTH oscillators
    // on at 0.787).  In the engine the store's boot dirty-application
    // overwrites these anyway; matching them keeps direct section use (host
    // tests, tools) consistent with a booted synth.
    float _mix1       = 0.787402f;
    float _mix2       = 0.787402f;
    float _subLevel   = 0.0f;
    float _noiseLevel = 0.0f;
    float _balance    = 0.0f;
    float _xmodDepth  = 0.0f;
    bool  _sync       = false;

    // --- Pass 7 pitch-envelope modulation ---
    // _pitchModOct: this block's target offset (octaves), set by the voice.
    // _pitchOctPrev: the offset applied at the END of the previous block, i.e.
    // the ramp's start point — render() ramps prev→target across the 128
    // samples and never STEPS the pitch.  Both zero ⇒ no work, default patch
    // byte-identical to before this pass.
    float _pitchModOct  = 0.0f;
    float _pitchOctPrev = 0.0f;

    // --- G1 routed pitch (OSC2-only lane) + X-MOD depth offset ---
    // Same target/prev ramp pair as the common term, but applied to unit 1
    // alone.  Both zero (destination "OSC1+2", the default) ⇒ no extra work.
    float _pitchModOct2  = 0.0f;
    float _pitch2OctPrev = 0.0f;
    // Block-rate additive offset onto _xmodDepth (see setXmodOffsetNorm).
    float _xmodOffset    = 0.0f;

    // --- Phase 3 PWM-LFO modulation (two lanes, see setLfoPwm) ---
    // Both 0.0 is the common case (no LFO wired to PWM), and render() skips
    // the recompute entirely then — default patch stays byte-identical.
    float _lfoPwm1 = 0.0f;     // LFO1's part, scaled per unit by pwmScale
    float _lfoPwmC = 0.0f;     // LFO2 + sequencer part, unscaled
    // D-PWM-stick fix: true while a lane was non-zero last block, so the
    // first all-zero block still recomputes once and restores base widths.
    bool  _pwmWasLive = false;

    // Sub oscillator: one phase + FastMath sine — too simple to justify a
    // third OscCore per voice.
    float _subPhase = 0.0f;
    float _subInc   = 0.0f;

    uint32_t _rng = 0x2F6E2B1u;        // noise source state
};

} // namespace JT
