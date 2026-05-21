#pragma once
// =============================================================================
// PatchState.h — Cached synthesis parameter snapshot
// =============================================================================
//
// PURPOSE:
//   Holds every typed parameter value that defines a patch (sound).
//   Extracted from SynthEngine's private member variables so that each
//   layer in a dual-layer performance can own independent state.
//
// DESIGN RULES:
//   - Pure data only.  No AudioStream objects, no AudioConnection pointers,
//     no DSP processing.  This struct is safe to copy, compare, and serialise.
//   - Default values match the existing SynthEngine constructor defaults
//     so that constructing a PatchState gives an identical starting point.
//   - The raw CC cache (_ccState[]) stays here because it is per-patch state
//     used by getCC() for UI readback.
//   - Voice allocation bookkeeping (_gateOpen, _noteToVoice, _noteTimestamps,
//     _clock, _monoStack, _unisonNote) is NOT here — that is voice-runtime
//     state owned by SynthEngine, not part of a saveable patch.
//   - LFO delay ramp state (_lfo1Ramping, _lfo1NoteOnMs, _lfo1CurrentAmp etc.)
//     is NOT here — that is transient runtime state, not patch data.
//   - BPM clock pointer, UI notifier callback, and audio graph pointers are
//     NOT here — those are engine infrastructure, not patch parameters.
//
// MIGRATION PATH:
//   Phase 0a: SynthEngine gets a PatchState _patch member.  Every reference
//             to e.g. _osc1Wave becomes _patch.osc1Wave.  No behaviour change.
//   Phase 1+: LayerManager gives each SynthEngine its own PatchState.
//             Crossfade/morph interpolates between two PatchState snapshots.
//
// =============================================================================

#include <Arduino.h>
#include "CCDefs.h"
#include "LFOBlock.h"       // LFODestination enum
#include "Waveforms.h"      // ArbBank enum

// Forward reference only — PatchState does not depend on filter internals.
// The uint8_t/float types used here mirror the SynthEngine cache, not the
// audio object APIs.

struct PatchState {

    // =========================================================================
    // Oscillators
    // =========================================================================
    int   osc1Wave          = 0;
    int   osc2Wave          = 0;
    float osc1PitchSemi     = 0.0f;
    float osc2PitchSemi     = 0.0f;
    float osc1DetuneHz      = 0.0f;
    float osc2DetuneHz      = 0.0f;
    float osc1FineCents     = 0.0f;
    float osc2FineCents     = 0.0f;
    float osc1Mix           = 1.0f;
    float osc2Mix           = 1.0f;
    float subMix            = 0.0f;
    float noiseMix          = 0.0f;
    float ring1Mix          = 0.0f;
    float ring2Mix          = 0.0f;
    float supersawDetune[2] = {0.0f, 0.0f};
    float supersawMix[2]    = {0.0f, 0.0f};
    float osc1FreqDc        = 0.0f;
    float osc2FreqDc        = 0.0f;
    float osc1ShapeDc       = 0.0f;
    float osc2ShapeDc       = 0.0f;
    float osc1FeedbackAmount = 0.0f;
    float osc2FeedbackAmount = 0.0f;
    float osc1FeedbackMix   = 0.0f;
    float osc2FeedbackMix   = 0.0f;
    float crossModDepth     = 0.0f;
    bool  syncEnabled       = false;

    // Arbitrary waveform bank/index selection
    ArbBank  osc1ArbBank    = ArbBank::BwBlended;
    ArbBank  osc2ArbBank    = ArbBank::BwBlended;
    uint16_t osc1ArbIndex   = 0;
    uint16_t osc2ArbIndex   = 0;

    // =========================================================================
    // Pitch bend (global, but stored per-patch for layer independence)
    // =========================================================================
    float pitchBendRange    = 2.0f;   // PITCH_BEND_DEFAULT_SEMITONES
    float pitchBendSemis    = 0.0f;   // Current bend in semitones

    // =========================================================================
    // Poly mode
    // =========================================================================
    //   PolyMode enum is declared in SynthEngine.h.  To avoid a circular
    //   include, we store it as uint8_t here and cast at the usage site.
    //   0 = POLY, 1 = MONO, 2 = UNISON.
    uint8_t polyMode        = 0;      // PolyMode::POLY
    float   unisonDetune    = 0.0f;   // 0..1 normalised

    // =========================================================================
    // Filter
    // =========================================================================
    float   filterCutoffHz  = 20000.0f;
    float   filterResonance = 0.0f;
    float   filterEnvAmount = 0.0f;
    float   filterKeyTrack  = 0.0f;
    float   filterOctaves   = 0.0f;
    float   filterMultimode = 0.0f;
    uint8_t filterMode      = 0;      // CC::FILTER_MODE_4POLE
    uint8_t filterEngine    = 0;      // CC::FILTER_ENGINE_OBXA
    uint8_t vaFilterType    = 0;      // FILTER_SVF_LP
    bool    filterUseTwoPole    = false;
    bool    filterXpander4Pole  = false;
    uint8_t filterXpanderMode   = 0;
    bool    filterBpBlend2Pole  = false;
    bool    filterPush2Pole     = false;
    float   filterResModDepth   = 0.0f;
    float   filterResonaceModDepth = 0.0f;  // intentional spelling kept for ABI

    // =========================================================================
    // Glide
    // =========================================================================
    bool  glideEnabled      = false;
    float glideTimeMs       = 0.0f;
    float lastNoteFreq      = 0.0f;

    // =========================================================================
    // LFO mirrors (per-patch so each layer can have independent modulation)
    // =========================================================================
    float lfo1Frequency     = 0.0f;
    float lfo2Frequency     = 0.0f;
    float lfo1Amount        = 0.0f;
    float lfo2Amount        = 0.0f;
    int   lfo1Type          = 0;
    int   lfo2Type          = 0;
    LFODestination lfo1Dest = (LFODestination)0;
    LFODestination lfo2Dest = (LFODestination)0;

    // Per-destination LFO depths (0..1 each)
    float lfo1PitchDepth    = 0.0f;
    float lfo1FilterDepth   = 0.0f;
    float lfo1PWMDepth      = 0.0f;
    float lfo1AmpDepth      = 0.0f;
    float lfo2PitchDepth    = 0.0f;
    float lfo2FilterDepth   = 0.0f;
    float lfo2PWMDepth      = 0.0f;
    float lfo2AmpDepth      = 0.0f;

    // LFO delay (fade-in time in ms)
    float lfo1DelayMs       = 0.0f;
    float lfo2DelayMs       = 0.0f;

    // =========================================================================
    // Pitch envelope
    // =========================================================================
    float pitchEnvAttack    = 1.0f;
    float pitchEnvDecay     = 80.0f;
    float pitchEnvSustain   = 0.0f;
    float pitchEnvRelease   = 50.0f;
    float pitchEnvDepth     = 0.0f;   // semitones, signed

    // =========================================================================
    // Envelope curve exponents (power-law, per timed stage)
    //
    // SysEx-only — no CC alias. Routed via ParamMap PIDs 0x0504..0x0506 (Amp),
    // 0x0604..0x0606 (Filter), 0x0705..0x0707 (Pitch).
    //
    // Default 1.0 = linear — identical to stock AudioEffectEnvelope.
    // Range:  0.2 – 5.0 (engine clamps to 0.05 – 10.0 internally).
    //   < 1.0 → fast start, slow finish  (logarithmic feel)
    //   = 1.0 → linear
    //   > 1.0 → slow start, fast finish  (exponential feel)
    //
    // All existing patches that predate this feature load with 1.0 and
    // behave identically — no backward-compatibility break.
    // =========================================================================
    float ampAttackCurve      = 1.0f;
    float ampDecayCurve       = 1.0f;
    float ampReleaseCurve     = 1.0f;

    float filterAttackCurve   = 1.0f;
    float filterDecayCurve    = 1.0f;
    float filterReleaseCurve  = 1.0f;

    float pitchAttackCurve    = 1.0f;
    float pitchDecayCurve     = 1.0f;
    float pitchReleaseCurve   = 1.0f;

    // =========================================================================
    // Velocity sensitivity (0..1)
    // =========================================================================
    float velAmpSens        = 0.0f;
    float velFilterSens     = 0.0f;
    float velEnvSens        = 0.0f;

    // =========================================================================
    // Amp modulation
    // =========================================================================
    float ampModFixedLevel  = 1.0f;

    // =========================================================================
    // FX parameters (cached values — actual audio objects live in SynthEngine)
    // =========================================================================
    float  fxBassGain       = 0.0f;
    float  fxTrebleGain     = 0.0f;
    int8_t fxModEffect      = -1;
    float  fxModMix         = 0.5f;
    float  fxModRate        = 0.0f;
    float  fxModFeedback    = -1.0f;
    int8_t fxDelayEffect    = -1;
    float  fxDelayMix       = 0.5f;
    float  fxDelayFeedback  = -1.0f;
    float  fxDelayTime      = 0.0f;
    float  fxDryMix         = 1.0f;
    float  fxJPFXMixL       = 0.0f;
    float  fxJPFXMixR       = 0.0f;

    // Reverb fields — MOVED to Performance (Phase 3). Reverb is now shared
    // between layers via GlobalFX, so its state is performance-scope, not
    // per-patch. Loading an old patch JSON with fxReverb* keys silently
    // drops them (breaking change, accepted).

    // =========================================================================
    // Step sequencer destination (the sequencer itself is runtime state, but
    // the destination selection is a patch parameter)
    // =========================================================================
    uint8_t seqDestination  = 0;      // LFO_DEST_NONE

    // =========================================================================
    // Raw CC state cache
    // =========================================================================
    //   Populated by handleControlChange().  getCC() reads from here.
    //   Size 160: MIDI CCs 0-127 + internal CCs (FX_DRIVE=130 etc.).
    //   POLY_MODE(128) and UNISON_DETUNE(129) use dedicated fields above.
    uint8_t ccState[160]    = {};
};
