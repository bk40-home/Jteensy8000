#pragma once
// =============================================================================
// FilterBlock.h  –  Dual-engine filter wrapper
// =============================================================================
//
// Holds both AudioFilterOBXa and AudioFilterVABank permanently in the Teensy
// audio graph.  The active engine is selected at runtime by muting/unmuting
// mixer channels — no cable rebuilding, no runtime allocation.
//
// ─── Audio graph (built once in constructor, never changed) ─────────────────
//
//   VoiceBlock             FilterBlock internals              VoiceBlock
//   (voiceMixer) ──► [_inputBuf] ──┬──► [_filterOBXa] ──► [_outputMix] ch0 ──► (ampEnv)
//                                  └──► [_filterVA]   ──► [_outputMix] ch1
//
//   [modMixer] ──► [_filterOBXa] input1
//              └─► [_filterVA]   input1
//
//   [_keyTrackDc] ──► [_modMixer] ch0
//   [_envModDc]   ──► feeds filter envelope ADSR via envmod() accessor
//
// _inputBuf is an AudioAmplifier used purely as a 1-in/1-out fan point so
// both filters can be wired to the same upstream source.  Its gain is 1.0f.
//
// Engine switching: sets _inputBuf → OBXa/VA gain via _outputMix (not the
// input side) and gates the inactive engine's _outputMix channel to 0.
// Both engines always receive audio; the inactive one's output is silenced
// at the _outputMix level.  This avoids any graph-modification calls at
// runtime and is always ISR-safe.
//
// ─── External interface ──────────────────────────────────────────────────────
// VoiceBlock calls input()/output()/envmod()/modMixer() — identical
// regardless of active engine.  VoiceBlock.cpp needs no changes.
// =============================================================================

#include "Audio.h"
#include "AudioFilterOBXa_OBXf.h"
#include "AudioFilterVABank.h"
#include "CCDefs.h"

class FilterBlock
{
public:
    FilterBlock();

    // ── Core controls — applied to both engines; cached for engine-switch ─────
    void setCutoff(float freqHz);
    void setResonance(float amount);
    void setOctaveControl(float octaves);
    void setEnvModAmount(float amount);
    void setKeyTrackAmount(float amount);

    /**
     * @brief Set step sequencer filter modulation offset.
     *        Added to the key tracking DC amplitude (same mod bus, slot 0).
     *        Range: −1.0 … +1.0 (bipolar, same scale as key track).
     */
    void setSeqFilterOffset(float offset);
    void setResonanceModDepth(float depth01);

    float getCutoff()            const { return _cutoff; }
    float getResonance()         const { return _resonance; }
    float getOctaveControl()     const { return _octaveControl; }
    float getEnvModAmount()      const { return _envModAmount; }
    float getKeyTrackAmount()    const { return _keyTrackAmount; }
    float getResonanceModDepth() const { return _resonanceModDepth; }

    // Envelope modulation passed to both engines' setEnvModOctaves()
    void setEnvModOctaves(float oct);
    float getEnvModOctaves() const { return _envModAmount; }

    void setMidiNote(float note);
    float getMidiNote() const { return _midiNote; }

    void setEnvValue(float env01);
    float getEnvValue() const { return _envValue; }

    // ── Engine switching ──────────────────────────────────────────────────────
    // 0 = OBXa  (CC::FILTER_ENGINE_OBXA)
    // 1 = VA    (CC::FILTER_ENGINE_VA)
    // Glitch-free: only sets _outputMix gains; no cable changes.
    void setFilterEngine(uint8_t engine);
    uint8_t getFilterEngine() const { return _activeEngine; }

    // ── OBXa-specific topology ────────────────────────────────────────────────
    void setMultimode(float amount);
    float getMultimode() const { return _multimode; }

    void setTwoPole(bool enabled);
    bool getTwoPole() const { return _useTwoPole; }

    void setXpander4Pole(bool enabled);
    bool getXpander4Pole() const { return _xpander4Pole; }

    void setXpanderMode(uint8_t mode);
    uint8_t getXpanderMode() const { return _xpanderMode; }

    void setBPBlend2Pole(bool enabled);
    bool getBPBlend2Pole() const { return _bpBlend2Pole; }

    void setPush2Pole(bool enabled);
    bool getPush2Pole() const { return _push2Pole; }

    // ── VA bank ───────────────────────────────────────────────────────────────
    void setVAFilterType(VAFilterType type);
    VAFilterType getVAFilterType() const { return _vaType; }
    const char*  getVAFilterName() const { return _filterVA.getFilterName(); }

    // ── Audio graph access (fixed entry/exit points) ──────────────────────────
    AudioStream& input();     // VoiceBlock wires voiceMixer here
    AudioStream& output();    // VoiceBlock wires ampEnvelope here
    AudioStream& envmod();    // DC source for filter envelope ADSR
    AudioMixer4& modMixer();  // Key track / env / LFO cutoff mod bus

private:
    // ── Audio components ──────────────────────────────────────────────────────

    // Fan-out buffer: VoiceBlock audio arrives here and is copied to both filters
    AudioAmplifier _inputBuf;

    AudioFilterOBXa   _filterOBXa;
    AudioFilterVABank _filterVA;

    // Output mixer: ch0 = OBXa, ch1 = VA.  Engine switch sets gains.
    AudioMixer4 _outputMix;

    // Cutoff modulation bus: ch0 = key track DC, ch1 = filter env, ch2/3 = LFOs
    AudioMixer4 _modMixer;

    AudioSynthWaveformDc _envModDc;    // envelope modulation DC
    AudioSynthWaveformDc _keyTrackDc;  // key tracking DC

    // ── State ─────────────────────────────────────────────────────────────────
    uint8_t      _activeEngine = CC::FILTER_ENGINE_OBXA;
    VAFilterType _vaType       = FILTER_SVF_LP;

    // Cached parameters (applied to both engines; re-pushed on engine switch)
    float _cutoff            = 0.0f;
    float _resonance         = 0.0f;
    float _octaveControl     = 4.0f;
    float _envModAmount      = 0.0f;
    float _keyTrackAmount    = 0.0f;
    float _seqFilterOffset   = 0.0f;  // Step sequencer filter mod (bipolar)
    float _multimode         = 0.0f;
    float _resonanceModDepth = 0.0f;
    float _midiNote          = 60.0f;
    float _envValue          = 0.0f;

    bool    _useTwoPole   = false;
    bool    _xpander4Pole = false;
    uint8_t _xpanderMode  = 0;
    bool    _bpBlend2Pole = false;
    bool    _push2Pole    = false;

    // ── Audio connections (fixed; built once in constructor) ──────────────────
    //  [0]  _inputBuf   → _filterOBXa input 0   (audio)
    //  [1]  _inputBuf   → _filterVA   input 0   (audio)
    //  [2]  _modMixer   → _filterOBXa input 1   (cutoff mod)
    //  [3]  _modMixer   → _filterVA   input 1   (cutoff mod)
    //  [4]  _filterOBXa → _outputMix  ch 0
    //  [5]  _filterVA   → _outputMix  ch 1
    //  [6]  _keyTrackDc → _modMixer   ch 0
    static constexpr int NUM_CABLES = 7;
    AudioConnection* _cables[NUM_CABLES];

    // ── Internal helpers ──────────────────────────────────────────────────────
    void _applyParamsToOBXa();
    void _applyParamsToVA();
};
