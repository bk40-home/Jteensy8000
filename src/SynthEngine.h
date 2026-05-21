#pragma once
// SynthEngine.h — 8-voice polyphonic synthesizer engine
// Mixer topology: Voices 0-3 → MixerA, Voices 4-7 → MixerB, A+B → MixerFinal
// CPU target: < 80% @ 44.1 kHz on Teensy 4.1

#include <Arduino.h>
#include "VoiceBlock.h"
#include "LFOBlock.h"
#include "StepSequencer.h"
#include "FXChainBlock.h"
#include "Mapping.h"
#include "Waveforms.h"
#include "DebugTrace.h"
#include "AKWF_All.h"
#include "BPMClockManager.h"
#include "PatchState.h"

using namespace JT8000Map;

// ============================================================================
// FREQUENCY MODULATION SCALING — READ THIS BEFORE TOUCHING ANY PITCH GAINS
// ============================================================================
//
// OscillatorBlock wires a 4-channel AudioMixer4 into the FM input of every
// AudioSynthWaveformJT oscillator.  The call:
//
//     _mainOsc.frequencyModulation(FM_OCTAVE_RANGE)   // = 10
//
// means a ±1.0 signal on the FM input shifts pitch by ±FM_OCTAVE_RANGE octaves
// using EXPONENTIAL (musical) scaling:
//
//     output_freq = base_freq × 2^(fm_input × FM_OCTAVE_RANGE)
//
// To shift by S semitones:
//     fm_input = S / (FM_OCTAVE_RANGE × 12)  =  S × FM_SEMITONE_SCALE
//
// Examples:
//     ±1 semitone  → fm_input = ±0.00833
//     ±2 semitones → fm_input = ±0.01667
//     ±7 semitones → fm_input = ±0.05833
//     ±12 semitones → fm_input = ±0.10000
//     ±24 semitones → fm_input = ±0.20000
//
// ── FM MIXER SLOT ALLOCATION ──────────────────────────────────────────────
//   Slot 0: _frequencyDc     — static pitch offset (DC), gain 1.0, amplitude scaled
//   Slot 1: LFO1             — gain set by _applyLFO1Gains(), amplitude kept at eff1
//   Slot 2: LFO2             — gain set by _applyLFO2Gains(), amplitude kept at eff2
//   Slot 3: Pitch envelope   — gain 1.0, per-voice DC amplitude carries the depth
//
// ── LFO DESIGN RATIONALE ─────────────────────────────────────────────────
//   LFO amplitude is always kept at eff1 (0..1 from LFO1_DEPTH CC or auto-1.0).
//   The per-destination depth CC controls the FM MIXER GAIN for that slot — NOT
//   the LFO waveform amplitude.  This keeps the LFO waveform shape undistorted
//   and allows the same LFO to simultaneously modulate pitch at one depth and
//   filter at a different depth.
//
//   At full LFO1_DEPTH (eff1=1.0) and full LFO1_PITCH_DEPTH (depth=1.0):
//     mixer gain = 1.0 × 1.0 × (LFO_PITCH_MAX_SEMITONES × FM_SEMITONE_SCALE)
//               = 7 / 120 = 0.0583
//     FM input peak = ±0.0583  →  ±7 semitones of vibrato
//
//   With gain=1.0 (unscaled), ±1.0 LFO → ±10 octaves: clearly unusable.
//   With FM_SEMITONE_SCALE applied, the range is musical and controllable.
//
// ── PITCH BEND ────────────────────────────────────────────────────────────
//   Pitch bend is handled in SOFTWARE (OscillatorBlock::setPitchBend),
//   NOT through the FM mixer.  This gives exact semitone accuracy at all
//   base frequencies and avoids consuming an FM mixer slot.
//   The bend amount is applied to all active voices via SynthEngine::setPitchBend().
// ============================================================================

#define MAX_VOICES 8   // 8-voice polyphony

// FM_OCTAVE_RANGE and FM_SEMITONE_SCALE are defined in OscillatorBlock.h.
// See that header for the complete FM scaling documentation.

// Maximum LFO vibrato at full pitch depth CC (peak semitones, positive side).
// At LFO1_DEPTH=127 + LFO1_PITCH_DEPTH=127: ±LFO_PITCH_MAX_SEMITONES of vibrato.
// 7 semitones ≈ JP-8000 full vibrato range.
static constexpr float LFO_PITCH_MAX_SEMITONES = 7.0f;

// Maximum unipolar DC pitch offset (semitones).
// CC=0 → 0 semitones, CC=127 → +DC_PITCH_MAX_SEMITONES.
static constexpr float DC_PITCH_MAX_SEMITONES  = 24.0f;

// Maximum bipolar pitch envelope depth (semitones from centre).
// CC=64 → 0, CC=127 → +PITCH_ENV_MAX_SEMITONES, CC=0 → -PITCH_ENV_MAX_SEMITONES.
static constexpr float PITCH_ENV_MAX_SEMITONES = 24.0f;

// Default and maximum pitch bend range (semitones, applied ± symmetrically).
// Standard MIDI = ±2 semitones.  Set via CC::PITCH_BEND_RANGE.
static constexpr float PITCH_BEND_DEFAULT_SEMITONES = 2.0f;
static constexpr float PITCH_BEND_MAX_SEMITONES     = 24.0f;

// =============================================================================
// PolyMode — voice allocation and stacking mode
//
// POLY:   Standard 8-voice polyphony, last-note stealing.
// MONO:   Single voice, last-note priority. Glide always active (uses
//         VoiceBlock glide regardless of GLIDE_ENABLE CC).
// UNISON: All 8 voices triggered simultaneously on each note, detuned
//         by _unisonDetuneSemis spread evenly across voices.
//         Produces a fat stacked sound at the cost of all polyphony.
// =============================================================================
enum class PolyMode : uint8_t {
    POLY   = 0,
    MONO   = 1,
    UNISON = 2,
};

// Maximum unison detune spread (semitones total, ± this / 2 per voice).
static constexpr float UNISON_MAX_SPREAD_SEMITONES = 1.0f;

// =============================================================================
// MonoNoteStack — simple fixed-capacity stack for mono legato note tracking.
//
// Stores the MIDI note numbers of all currently-held keys so that releasing
// the most recent key returns to the previous pitch.  Last-note priority:
// the top of the stack is always the note that should be sounding.
//
// Capacity of 16 is generous — even aggressive playing rarely exceeds 10
// simultaneous held keys.  Zero dynamic allocation, no ISR cost.
// =============================================================================
struct MonoNoteStack {
    static constexpr int CAPACITY = 16;
    byte notes[CAPACITY] = {};
    int  count            = 0;

    // Push a note onto the stack.  If already present, move it to the top.
    void push(byte note) {
        remove(note);                              // Deduplicate first
        if (count < CAPACITY) notes[count++] = note;
    }

    // Remove a specific note (key released). Returns true if found.
    bool remove(byte note) {
        for (int i = 0; i < count; ++i) {
            if (notes[i] == note) {
                // Shift everything above down by one
                for (int j = i; j < count - 1; ++j) notes[j] = notes[j + 1];
                --count;
                return true;
            }
        }
        return false;
    }

    // Peek at the most recent note (top of stack). Only valid when count > 0.
    byte top()     const { return notes[count - 1]; }
    bool empty()   const { return count == 0; }

    void clear()         { count = 0; }
};

class SynthEngine {
public:
    // =========================================================================
    // Lifecycle
    // =========================================================================
    //
    // Construction is trivial — NO audio objects are touched, NO connections
    // made.  Every engine owns its own AudioGraph (LFOs, mixers, step seq)
    // and its own FXChainBlock; both are constructed as direct members with
    // default values.  The AudioGraph and FX chain self-register with the
    // Teensy Audio Library at global construction time, before setup() runs.
    //
    // Before calling begin(), you MUST call setVoicePool() so the engine
    // knows which physical VoiceBlock[] to address.  In the JT-8000 object
    // graph this is done by LayerManager which owns the single VoicePool
    // shared by Engine A and Engine B.
    //
    // begin() creates AudioConnections — that step requires AudioMemory()
    // to have been called already.  Failing to follow the order yields a
    // silent/broken graph with no runtime error.
    // =========================================================================
    SynthEngine();
    ~SynthEngine();

    // Point this engine at the shared voice pool.  MUST be called before
    // begin().  The pool pointer is stored, not copied — VoicePool must
    // outlive this SynthEngine.
    void setVoicePool(class VoicePool* pool);

    // Must be called from setup() AFTER AudioMemory() AND after setVoicePool().
    // Creates every AudioConnection for this engine's audio graph.
    void begin();

    void noteOn(byte note, float velocity);
    void noteOff(byte note);
    void update();

    static constexpr uint8_t VOICE_NONE = 255;  // Sentinel: no voice assigned

    // =========================================================================
    // CC state cache — raw MIDI 0-127 values
    // =========================================================================
    // Filled by handleControlChange(); lets the UI read back any CC value
    // without needing typed getters for every parameter.
    // Not valid until the first handleControlChange() call for that CC.

    // Returns the last raw CC value received (0-127), or 0 if never set.
    inline uint8_t getCC(uint8_t cc) const {
        // POLY_MODE and UNISON_DETUNE live above the MIDI range and use
        // dedicated backing fields — _patch.ccState is not written for these.
        if (cc == CC::POLY_MODE) {
            // Encode current poly mode back into a representative CC value
            switch ((PolyMode)_patch.polyMode) {
                case PolyMode::POLY:   return 21;   // midpoint of zone 0-42
                case PolyMode::MONO:   return 63;   // midpoint of zone 43-84
                case PolyMode::UNISON: return 106;  // midpoint of zone 85-127
                default:               return 0;
            }
        }
        if (cc == CC::UNISON_DETUNE) {
            // Encode normalised float back to 0-127 for the UI
            return (uint8_t)constrain((int)(_patch.unisonDetune * 127.0f), 0, 127);
        }
        // Envelope curve exponents — PatchState stores the native float; encode
        // back to CC byte for TFT knob display.
        // MUST use curve_to_cc() (Mapping.h) which is the exact inverse of
        // cc_to_curve() used in the CC dispatch path.  The old ExponentToCC()
        // used a different linear 0..5→0..127 scale that broke round-tripping.
        if (cc == CC::AMP_ATTACK_CURVE)     return JT8000Map::curve_to_cc(_patch.ampAttackCurve);
        if (cc == CC::AMP_DECAY_CURVE)      return JT8000Map::curve_to_cc(_patch.ampDecayCurve);
        if (cc == CC::AMP_RELEASE_CURVE)    return JT8000Map::curve_to_cc(_patch.ampReleaseCurve);
        if (cc == CC::FILTER_ATTACK_CURVE)  return JT8000Map::curve_to_cc(_patch.filterAttackCurve);
        if (cc == CC::FILTER_DECAY_CURVE)   return JT8000Map::curve_to_cc(_patch.filterDecayCurve);
        if (cc == CC::FILTER_RELEASE_CURVE) return JT8000Map::curve_to_cc(_patch.filterReleaseCurve);
        if (cc == CC::PITCH_ATTACK_CURVE)   return JT8000Map::curve_to_cc(_patch.pitchAttackCurve);
        if (cc == CC::PITCH_DECAY_CURVE)    return JT8000Map::curve_to_cc(_patch.pitchDecayCurve);
        if (cc == CC::PITCH_RELEASE_CURVE)  return JT8000Map::curve_to_cc(_patch.pitchReleaseCurve);
        return _patch.ccState[cc];  // covers 0-127 (MIDI) and 130+ (internal)
    }

    // Dispatches a CC as if received from MIDI. Also updates _patch.ccState.
    // Use this from the UI encoder/touch handlers.
    inline void setCC(uint8_t cc, uint8_t value) {
        handleControlChange(1, cc, value);
    }

    // =========================================================================
    // Oscillator control
    // =========================================================================
    void setOscWaveforms(int wave1, int wave2);
    void setOsc1Waveform(int wave);
    void setOsc2Waveform(int wave);
    void setOsc1PitchOffset(float semis);
    void setOsc2PitchOffset(float semis);
    void setOsc1Detune(float hz);
    void setOsc2Detune(float hz);
    void setOsc1FineTune(float cents);
    void setOsc2FineTune(float cents);

    // ---- Pitch bend ----
    // handlePitchBend()  — call from MIDI pitch bend callback.
    //   value   = raw 14-bit MIDI value (0..16383, centre = 8192).
    //   channel = MIDI channel (currently ignored; all channels bend equally).
    // Stores bend internally and applies to all active voices immediately.
    void handlePitchBend(uint8_t channel, int16_t value);

    // setPitchBendRange()  — set ±range in semitones (0..PITCH_BEND_MAX_SEMITONES).
    //   Default = PITCH_BEND_DEFAULT_SEMITONES (2).
    //   Applied on next handlePitchBend() call.
    void setPitchBendRange(float semitones);

    float getPitchBendRange()  const { return _patch.pitchBendRange; }
    float getPitchBendSemis()  const { return _patch.pitchBendSemis; }

    // -------------------------------------------------------------------------
    // POLY MODE — voice allocation mode (poly / mono / unison)
    // -------------------------------------------------------------------------
    void     setPolyMode(PolyMode mode);
    PolyMode getPolyMode()         const { return (PolyMode)_patch.polyMode; }

    // Unison detune spread — only audible in UNISON mode.
    // 0.0 = all voices in perfect unison;  1.0 = full UNISON_MAX_SPREAD_SEMITONES.
    void  setUnisonDetune(float amount);     // 0..1 normalised
    float getUnisonDetune() const           { return _patch.unisonDetune; }

    // -------------------------------------------------------------------------
    // VOICE RANGE — which slice of the shared 8-voice pool this engine owns.
    // Used by LayerManager to split voices between layers (hard-partitioned).
    //
    // Can be called at any time. Voices leaving the range have their notes
    // silenced and their modulation gate-slots zeroed so they contribute no
    // sound and pick up no modulation from this engine's LFOs / sequencer.
    // Voices entering the range are given the engine's current modulation
    // depths via _applyModRangeGains() (called from setVoiceRange).
    //
    // See _applyModRangeGains() for the gating model that replaces the old
    // "only wire in-range voices" approach.
    // -------------------------------------------------------------------------
    void setVoiceRange(uint8_t first, uint8_t count);
    uint8_t getFirstVoice() const { return _firstVoice; }
    uint8_t getVoiceCount() const { return _voiceCount; }

    void setOscMix(float osc1Level, float osc2Level);
    void setOsc1Mix(float oscLevel);
    void setOsc2Mix(float oscLevel);
    void setSubMix(float mix);
    void setNoiseMix(float mix);
    void setSupersawDetune(uint8_t oscIndex, float amount);
    void setSupersawMix(uint8_t oscIndex, float amount);
    void setOsc1FrequencyDcAmp(float amp);
    void setOsc2FrequencyDcAmp(float amp);
    void setOsc1ShapeDcAmp(float amp);
    void setOsc2ShapeDcAmp(float amp);
    void setRing1Mix(float level);
    void setRing2Mix(float level);
    void setOsc1FeedbackAmount(float amount);
    void setOsc2FeedbackAmount(float amount);
    void setOsc1FeedbackMix(float mix);
    void setOsc2FeedbackMix(float mix);

    float getOsc1FeedbackAmount()  const;
    float getOsc2FeedbackAmount()  const;
    float getOsc1FeedbackMix()     const;
    float getOsc2FeedbackMix()     const;

    // ── Cross Modulation & Oscillator Sync ───────────────────────────────
    void  setCrossModDepth(float depth);
    void  setSyncEnabled(bool enabled);
    float getCrossModDepth() const { return _patch.crossModDepth; }
    bool  getSyncEnabled()   const { return _patch.syncEnabled; }


    // Returns true if voice slot v is producing audio (including release tail).
    // Queries the amp envelope hardware — accurate even during release phase.
    // Used by the display for voice activity dots.
    inline bool isVoiceActive(uint8_t v) const {
        return (v < MAX_VOICES) && _voices[v].isAudioActive();
    }

    // Returns true if voice slot v has a MIDI note gate currently open.
    // A voice can have its gate closed but still be audio-active (release phase).
    // Used internally for voice allocation decisions.
    inline bool isGateOpen(uint8_t v) const {
        return (v < MAX_VOICES) && _gateOpen[v];
    }

    // =========================================================================
    // Arbitrary waveform (AKWF bank) selection
    // =========================================================================
    void setOsc1ArbBank(ArbBank b);
    void setOsc2ArbBank(ArbBank b);
    void setOsc1ArbIndex(uint16_t idx);
    void setOsc2ArbIndex(uint16_t idx);
    ArbBank  getOsc1ArbBank()  const { return _patch.osc1ArbBank; }
    ArbBank  getOsc2ArbBank()  const { return _patch.osc2ArbBank; }
    uint16_t getOsc1ArbIndex() const { return _patch.osc1ArbIndex; }
    uint16_t getOsc2ArbIndex() const { return _patch.osc2ArbIndex; }

    // =========================================================================
    // Amp modulation DC offset
    // =========================================================================
    void  SetAmpModFixedLevel(float level);
    float GetAmpModFixedLevel() const;
    float getAmpModFixedLevel() const;   // alias

    // =========================================================================
    // LFO 1
    // =========================================================================
    void   setLFO1Frequency(float hz);
    void   setLFO1Amount(float amt);
    void   setLFO1Waveform(int type);
    void   setLFO1Destination(LFODestination dest);
    float  getLFO1Frequency()    const;
    float  getLFO1Amount()       const;
    int    getLFO1Waveform()     const;
    LFODestination getLFO1Destination() const;
    const char* getLFO1WaveformName()    const;
    const char* getLFO1DestinationName() const;

    // =========================================================================
    // LFO 2
    // =========================================================================
    void   setLFO2Frequency(float hz);
    void   setLFO2Amount(float amt);
    void   setLFO2Waveform(int type);
    void   setLFO2Destination(LFODestination dest);
    float  getLFO2Frequency()    const;
    float  getLFO2Amount()       const;
    int    getLFO2Waveform()     const;
    LFODestination getLFO2Destination() const;
    const char* getLFO2WaveformName()    const;
    const char* getLFO2DestinationName() const;

    // =========================================================================
    // NEW: LFO per-destination depths (JP-8000 style)
    // Each destination has an independent depth (0..1). Final mixer gain =
    // masterAmount * perDestDepth, allowing simultaneous multi-target mod.
    // =========================================================================
    void  setLFO1PitchDepth(float d);   void  setLFO1FilterDepth(float d);
    void  setLFO1PWMDepth(float d);     void  setLFO1AmpDepth(float d);
    void  setLFO1Delay(float ms);       // Fade-in delay after noteOn
    float getLFO1PitchDepth()  const { return _patch.lfo1PitchDepth; }
    float getLFO1FilterDepth() const { return _patch.lfo1FilterDepth; }
    float getLFO1PWMDepth()    const { return _patch.lfo1PWMDepth; }
    float getLFO1AmpDepth()    const { return _patch.lfo1AmpDepth; }
    float getLFO1Delay()       const { return _patch.lfo1DelayMs; }

    void  setLFO2PitchDepth(float d);   void  setLFO2FilterDepth(float d);
    void  setLFO2PWMDepth(float d);     void  setLFO2AmpDepth(float d);
    void  setLFO2Delay(float ms);
    float getLFO2PitchDepth()  const { return _patch.lfo2PitchDepth; }
    float getLFO2FilterDepth() const { return _patch.lfo2FilterDepth; }
    float getLFO2PWMDepth()    const { return _patch.lfo2PWMDepth; }
    float getLFO2AmpDepth()    const { return _patch.lfo2AmpDepth; }
    float getLFO2Delay()       const { return _patch.lfo2DelayMs; }

    // =========================================================================
    // NEW: Pitch envelope — separate ADSR that modulates oscillator pitch.
    // Depth is in semitones (±24). Depth=0 skips triggering (CPU guard).
    // =========================================================================
    void setPitchEnvAttack(float ms);   void setPitchEnvDecay(float ms);
    void setPitchEnvSustain(float l);   void setPitchEnvRelease(float ms);
    void setPitchEnvDepth(float semitones);
    float getPitchEnvAttack()  const { return _patch.pitchEnvAttack; }
    float getPitchEnvDecay()   const { return _patch.pitchEnvDecay; }
    float getPitchEnvSustain() const { return _patch.pitchEnvSustain; }
    float getPitchEnvRelease() const { return _patch.pitchEnvRelease; }
    float getPitchEnvDepth()   const { return _patch.pitchEnvDepth; }

    // =========================================================================
    // NEW: Velocity sensitivity — three targets matching JP-8000
    // 0 = flat (velocity ignored), 1 = full velocity control.
    // Applied on each noteOn; does not modify stored base parameter values.
    // =========================================================================
    void  setVelocityAmpSens(float s);    // → VCA level scale
    void  setVelocityFilterSens(float s); // → filter cutoff offset (octaves)
    void  setVelocityEnvSens(float s);    // → filter env depth scale
    float getVelocityAmpSens()    const { return _patch.velAmpSens; }
    float getVelocityFilterSens() const { return _patch.velFilterSens; }
    float getVelocityEnvSens()    const { return _patch.velEnvSens; }

    // =========================================================================
    // Filter
    // =========================================================================
    void setFilterEnvAmount(float amt);
    void setFilterCutoff(float value);
    void setFilterResonance(float value);
    void setFilterKeyTrackAmount(float amt);
    void setFilterOctaveControl(float octaves);
    void setFilterMultimode(float multimode);

    // Single topology selector — encodes all OBXa bool flags into one value.
    // Mode constants defined by CC::FILTER_MODE_* in CCDefs.h.
    // Selecting a 4-pole or Xpander mode automatically clears 2-pole sub-flags.
    void setFilterMode(uint8_t mode);

    // Xpander sub-mode (0..14): only meaningful when mode == FILTER_MODE_XPANDER_M
    void setFilterXpanderMode(uint8_t mode);

    void setFilterResonanceModDepth(float depth01);

    // ── Filter engine switching ───────────────────────────────────────────────
    // 0 = OBXa (CC::FILTER_ENGINE_OBXA), 1 = VA bank (CC::FILTER_ENGINE_VA).
    // Applies to all 8 voices simultaneously.
    void setFilterEngine(uint8_t engine);

    // VA bank topology (0..FILTER_COUNT-1).  See VAFilterType enum.
    // Only meaningful when engine == FILTER_ENGINE_VA.
    void setVAFilterType(uint8_t vaType);

    float   getFilterCutoff()          const;
    float   getFilterResonance()       const;
    float   getFilterEnvAmount()       const;
    float   getFilterKeyTrackAmount()  const;
    float   getFilterOctaveControl()   const;
    float   getFilterMultimode()       const { return _patch.filterMultimode; }
    uint8_t getFilterMode()            const { return _patch.filterMode; }
    uint8_t getFilterEngine()          const { return _patch.filterEngine; }
    uint8_t getVAFilterType()          const { return _patch.vaFilterType; }
    // Low-level bool getters kept for OBXa core and SectionScreen display
    bool    getFilterTwoPole()         const { return _patch.filterUseTwoPole; }
    bool    getFilterXpander4Pole()    const { return _patch.filterXpander4Pole; }
    uint8_t getFilterXpanderMode()     const { return _patch.filterXpanderMode; }
    bool    getFilterBPBlend2Pole()    const { return _patch.filterBpBlend2Pole; }
    bool    getFilterPush2Pole()       const { return _patch.filterPush2Pole; }
    float   getFilterResonanceModDepth() const { return _patch.filterResonaceModDepth; }

    // =========================================================================
    // Envelopes
    // =========================================================================
    float getAmpAttack()         const;
    float getAmpDecay()          const;
    float getAmpSustain()        const;
    float getAmpRelease()        const;
    float getFilterEnvAttack()   const;
    float getFilterEnvDecay()    const;
    float getFilterEnvSustain()  const;
    float getFilterEnvRelease()  const;

    // ---- Amp envelope curve shaping -----------------------------------------
    // SysEx-only (no CC alias). PIDs 0x0504 / 0x0505 / 0x0506.
    void  setAmpAttackCurve(float exponent);
    void  setAmpDecayCurve(float exponent);
    void  setAmpReleaseCurve(float exponent);
    float getAmpAttackCurve()   const { return _patch.ampAttackCurve; }
    float getAmpDecayCurve()    const { return _patch.ampDecayCurve; }
    float getAmpReleaseCurve()  const { return _patch.ampReleaseCurve; }

    // ---- Filter envelope curve shaping --------------------------------------
    // SysEx-only (no CC alias). PIDs 0x0604 / 0x0605 / 0x0606.
    void  setFilterAttackCurve(float exponent);
    void  setFilterDecayCurve(float exponent);
    void  setFilterReleaseCurve(float exponent);
    float getFilterAttackCurve()   const { return _patch.filterAttackCurve; }
    float getFilterDecayCurve()    const { return _patch.filterDecayCurve; }
    float getFilterReleaseCurve()  const { return _patch.filterReleaseCurve; }

    // ---- Pitch envelope curve shaping ---------------------------------------
    // SysEx-only (no CC alias). PIDs 0x0705 / 0x0706 / 0x0707.
    void  setPitchEnvAttackCurve(float exponent);
    void  setPitchEnvDecayCurve(float exponent);
    void  setPitchEnvReleaseCurve(float exponent);
    float getPitchEnvAttackCurve()   const { return _patch.pitchAttackCurve; }
    float getPitchEnvDecayCurve()    const { return _patch.pitchDecayCurve; }
    float getPitchEnvReleaseCurve()  const { return _patch.pitchReleaseCurve; }

    // =========================================================================
    // JPFX Effects — Tone
    // =========================================================================
    void   setFXBassGain(float dB);
    void   setFXTrebleGain(float dB);
    float  getFXBassGain()   const;
    float  getFXTrebleGain() const;

    // =========================================================================
    // JPFX Effects — Modulation (chorus/flange/phase)
    // =========================================================================
    void   setFXModEffect(int8_t variation);
    void   setFXModMix(float mix);
    void   setFXModRate(float hz);
    void   setFXModFeedback(float fb);
    int8_t getFXModEffect()   const;
    float  getFXModMix()      const;
    float  getFXModRate()     const;
    float  getFXModFeedback() const;
    const char* getFXModEffectName() const;

    // =========================================================================
    // JPFX Effects — Delay
    // =========================================================================
    void   setFXDelayEffect(int8_t variation);
    void   setFXDelayMix(float mix);
    void   setFXDelayFeedback(float fb);
    void   setFXDelayTime(float ms);
    int8_t getFXDelayEffect()   const;
    float  getFXDelayMix()      const;
    float  getFXDelayFeedback() const;
    float  getFXDelayTime()     const;
    const char* getFXDelayEffectName() const;

    // =========================================================================
    // Reverb — MOVED to GlobalFX (Phase 3).
    // Access via LayerManager::getGlobalFX() for get/set of room size,
    // damping, shimmer, freeze, lowpass, hipass, bypass, and wet mix.
    // The engine no longer owns any reverb state.
    // =========================================================================

    // =========================================================================
    // Output mix levels (dry + JPFX — reverb mix now in GlobalFX)
    // =========================================================================
    void  setFXDryMix(float level);
    void  setFXJPFXMix(float left, float right);

    float getFXDryMix()     const;
    float getFXJPFXMixL()   const;
    float getFXJPFXMixR()   const;

    // =========================================================================
    // UI helpers — typed getters for display formatting
    // =========================================================================
    int  getOsc1Waveform() const;
    int  getOsc2Waveform() const;
    const char* getOsc1WaveformName() const;
    const char* getOsc2WaveformName() const;

    float getSupersawDetune(uint8_t oscIndex) const;
    float getSupersawMix(uint8_t oscIndex)    const;
    float getOsc1PitchOffset() const;
    float getOsc2PitchOffset() const;
    float getOsc1Detune()      const;
    float getOsc2Detune()      const;
    float getOsc1FineTune()    const;
    float getOsc2FineTune()    const;
    float getOscMix1()         const;
    float getOscMix2()         const;
    float getSubMix()          const;
    float getNoiseMix()        const;
    float getRing1Mix()        const;
    float getRing2Mix()        const;
    float getOsc1FrequencyDc() const;
    float getOsc2FrequencyDc() const;
    float getOsc1ShapeDc()     const;
    float getOsc2ShapeDc()     const;

    bool  getGlideEnabled() const;
    float getGlideTimeMs()  const;

    // =========================================================================
    // MIDI
    // =========================================================================
    void handleControlChange(byte channel, byte control, byte value);

    // Callback fired after every CC is processed; UI uses this to stay in sync
    using NotifyFn = void(*)(uint8_t cc, uint8_t val);
    void setNotifier(NotifyFn fn);

    // =========================================================================
    // Audio graph outputs — always valid, _audio is a direct member.
    // =========================================================================
    AudioMixer4& getVoiceMixer() { return _audio.voiceMixerFinal; }
    AudioMixer4& getFXOutL()     { return _fxChain.getOutputLeft(); }
    AudioMixer4& getFXOutR()     { return _fxChain.getOutputRight(); }

    // FX chain access — LayerManager uses this for setBypass() in SINGLE mode.
    FXChainBlock&       getFXChain()       { return _fxChain; }
    const FXChainBlock& getFXChain() const { return _fxChain; }

    // =========================================================================
    // BPM clock sync
    // =========================================================================
    void setBPMClock(BPMClockManager* clock);
    void updateBPMSync();   // Called from update() to refresh synced params

    void       setLFO1TimingMode(TimingMode mode);
    void       setLFO2TimingMode(TimingMode mode);
    TimingMode getLFO1TimingMode() const;
    TimingMode getLFO2TimingMode() const;

    void       setDelayTimingMode(TimingMode mode);
    TimingMode getDelayTimingMode() const;

    StepSequencer&       getSeq1()       { return _audio.seq1; }
    const StepSequencer& getSeq1() const { return _audio.seq1; }

private:
    // =========================================================================
    // 8-voice audio architecture
    //
    //   Voices 0-3 → _voiceMixerA  
    //                                → _voiceMixerFinal → FX chain
    //   Voices 4-7 → _voiceMixerB  
    //
    // Each voice contributes 1/8 of full scale.
    // CPU @ 44.1 kHz: ~30-40% for voices, leaves headroom for FX.
    // RAM: 8 × VoiceBlock (~8 KB each) = ~64 KB.
    // =========================================================================

    // =========================================================================
    // Voice pool access — engines DO NOT own voices. A single VoicePool
    // (owned by LayerManager) holds the MAX_VOICES VoiceBlock instances.
    // Each engine addresses the pool by _firstVoice/_voiceCount.
    //
    // The _voices pointer is set by setVoicePool() and points at index 0 of
    // the pool — not at index _firstVoice — so that existing loop patterns
    //   for (i = _firstVoice; i < _firstVoice + _voiceCount; ++i)
    //       _voices[i].something();
    // continue to work unchanged.
    //
    // Gating mechanism (Option R3 — permanent wiring, slot-gain gating):
    //   begin() wires this engine's LFOs / step sequencer / voice-mixer slots
    //   to ALL MAX_VOICES voices. Which of those voices actually hear this
    //   engine's modulation is controlled by the per-slot gain: in-range
    //   voices get the configured depth, out-of-range voices get 0. The
    //   depth-apply helpers (_applyLFO*Gains, _applySeqOutput) walk all 8
    //   voices and apply either the depth or zero based on range membership.
    //   This pattern lets setVoiceRange() change the range at runtime with
    //   no connection surgery — only gain writes.
    // =========================================================================
    VoiceBlock* _voices = nullptr;         // pointer into VoicePool::data()
    bool        _gateOpen[MAX_VOICES];
    byte        _noteToVoice[128];
    uint32_t    _noteTimestamps[MAX_VOICES];
    uint32_t    _clock = 0;

    uint8_t     _firstVoice  = 0;
    uint8_t     _voiceCount  = MAX_VOICES;

    // Helper: true if voice index v is currently owned by this engine.
    // Used by the depth-apply helpers to gate per-voice mixer slot gains.
    inline bool _voiceInRange(uint8_t v) const {
        return (v >= _firstVoice) && (v < (uint8_t)(_firstVoice + _voiceCount));
    }

    int _findFreeVoice();

    // =========================================================================
    // Patch state — one per layer (lightweight, no audio objects)
    // =========================================================================
    PatchState _patch;

    // =========================================================================
    // Shared audio infrastructure — every engine owns a full AudioGraph.
    //
    // Direct-member allocation (not heap): the AudioStream subclasses inside
    // AudioGraph register with the Audio Library at construction time, which
    // is before main() when SynthEngine is a global. No null-check needed at
    // any call site. Heap fragmentation avoided.
    //
    // RAM cost: roughly 1–2 KB of object state per AudioGraph plus the cost
    // of AudioConnection allocations done in begin(). Two AudioGraphs (Engine
    // A + Engine B) is well within RAM1 budget on Teensy 4.1.
    // =========================================================================
    struct AudioGraph {
        StepSequencer        seq1;
        uint8_t              seqPrevDestination = 0;
        uint8_t              seqSelectedStep    = 0;
        uint32_t             lastUpdateMicros   = 0;

        AudioSynthWaveformDc seqDc;
        LFOBlock             lfo1;
        LFOBlock             lfo2;

        AudioSynthWaveformDc ampModFixedDc;
        AudioSynthWaveformDc ampModLimitFixedDc;
        AudioEffectMultiply  ampMultiply;
        AudioMixer4          ampModMixer;
        AudioMixer4          ampModLimiterMixer;

        AudioMixer4          voiceMixerA;
        AudioMixer4          voiceMixerB;
        AudioMixer4          voiceMixerFinal;

        AudioConnection* voicePatch[MAX_VOICES]                  = {};
        AudioConnection* voicePatchLFO1ShapeOsc1[MAX_VOICES]     = {};
        AudioConnection* voicePatchLFO1ShapeOsc2[MAX_VOICES]     = {};
        AudioConnection* voicePatchLFO1FrequencyOsc1[MAX_VOICES] = {};
        AudioConnection* voicePatchLFO1FrequencyOsc2[MAX_VOICES] = {};
        AudioConnection* voicePatchLFO1Filter[MAX_VOICES]        = {};
        AudioConnection* voicePatchLFO2ShapeOsc1[MAX_VOICES]     = {};
        AudioConnection* voicePatchLFO2ShapeOsc2[MAX_VOICES]     = {};
        AudioConnection* voicePatchLFO2FrequencyOsc1[MAX_VOICES] = {};
        AudioConnection* voicePatchLFO2FrequencyOsc2[MAX_VOICES] = {};
        AudioConnection* voicePatchLFO2Filter[MAX_VOICES]        = {};

        AudioConnection* patchSeqDcToShapeOsc1[MAX_VOICES]       = {};
        AudioConnection* patchSeqDcToShapeOsc2[MAX_VOICES]       = {};
        AudioConnection* patchSeqDcToAmpModMixer      = nullptr;

        AudioConnection* patchAmpModFixedDcToAmpModMixer = nullptr;
        AudioConnection* patchLFO1ToAmpModMixer        = nullptr;
        AudioConnection* patchLFO2ToAmpModMixer        = nullptr;
        AudioConnection* patchAmpModMixerToAmpMultiply = nullptr;
        AudioConnection* patchVoiceMixerToAmpMultiply  = nullptr;
        AudioConnection* fxPatchInL                    = nullptr;
        AudioConnection* fxPatchDryL                   = nullptr;
        AudioConnection* fxPatchDryR                   = nullptr;

        AudioConnection* patchMixerAToFinal            = nullptr;
        AudioConnection* patchMixerBToFinal            = nullptr;
    };

    // Direct member — constructed automatically with default-initialised
    // AudioStream subclasses, ready for begin() to wire the connections.
    AudioGraph _audio;

    // =========================================================================
    // FX chain — independently owned per layer (each layer gets its own FX)
    // =========================================================================
    FXChainBlock _fxChain;

    // =========================================================================
    // BPM / timing
    // =========================================================================
    BPMClockManager* _bpmClock = nullptr;  // Pointer to global clock (not owned)

    // =========================================================================
    // UI notifier callback
    // =========================================================================
    NotifyFn _notify = nullptr;

    // =========================================================================
    // LFO delay ramp TRANSIENT state — not patch data, not saveable.
    // Tracks the in-progress fade-in after a noteOn.
    // =========================================================================
    float    _lfo1CurrentAmp = 0.0f, _lfo2CurrentAmp = 0.0f;
    uint32_t _lfo1NoteOnMs   = 0,    _lfo2NoteOnMs   = 0;
    bool     _lfo1Ramping    = false, _lfo2Ramping    = false;

    // =========================================================================
    // Poly mode RUNTIME state — not part of saveable patch
    // =========================================================================
    int           _unisonNote = -1;      // -1 = no note held
    MonoNoteStack _monoStack;

    // =========================================================================
    // Private helpers
    // =========================================================================
    void _applySeqOutput();
    void _applyLFO1Gains();     // Recompute all LFO1 destination mixer gains
    void _applyLFO2Gains();     // Recompute all LFO2 destination mixer gains
    void _applyUnisonDetune();  // Spread detune offsets across voices (UNISON mode)
    void _updateLFODelay();     // Called from update(): handle delay ramps

    // Voice-range gate refresh. Walks all MAX_VOICES voices; for each voice,
    // all gated mixer slots (voice→mixerA/B, LFO1/2→voice mod mixers, seqDc→
    // voice shape mixer slot 3, voice→mixerA/B amplitude) are set to their
    // configured depth if the voice is in this engine's range, or to 0 if it
    // is not. Called from begin() (baseline zero), from setVoiceRange() when
    // range changes, and from _applyLFO*Gains/_applySeqOutput which each do
    // the walk implicitly as part of writing depths.
    void _applyVoiceRangeGains();
};