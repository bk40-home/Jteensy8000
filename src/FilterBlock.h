/* Audio Library for Teensy
 * Copyright (c) 2025, Kris Bishop, bishopkris40@hotmail.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
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
//   VoiceBlock         FilterBlock internals                  VoiceBlock
//   (voiceMixer) ─►[_inputBuf]─┬─►[_filterOBXa]─►[_outputMix]ch0─►(ampEnv)
//                              └─►[_filterVA]  ─►[_outputMix]ch1
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
// The inactive engine is skipped via its filter's setActive(false) flag: that
// filter's update() drains its input and returns before any DSP (real CPU
// skip under JT_OPT_FILTER_ENGINE_SKIP).  This does NOT mute the audible
// output — that is still done at _outputMix.
//
// Engine switching: gates _outputMix channel gains (audible mute) and, when
// the skip opt is enabled, the per-engine input amp gains (CPU skip).  No
// cable changes at runtime — always ISR-safe.
//
// ─── External interface ──────────────────────────────────────────────────────
// VoiceBlock calls input()/output()/envmod()/modMixer() — identical
// regardless of active engine.  VoiceBlock.cpp needs no changes.
// =============================================================================

#include "Audio.h"
#include "AudioFilterOBXa_OBXf.h"
#include "AudioFilterVABank.h"
#include "ParamDefs.h"
#include "JT8000_OptFlags.h"   // JT_OPT_FILTER_ENGINE_SKIP

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

    // VA-only drive / saturation (reachable only when VA engine active).
    // Drive is stored as a 0..1 UI value and scaled to the VA setDrive()
    // range internally (see .cpp).  Saturation is a VASaturationType.
    void setVADrive(float norm01);
    float getVADrive() const { return _vaDrive01; }

    void setVASaturation(uint8_t satType);
    uint8_t getVASaturation() const { return (uint8_t)_vaSat; }

    // ── Engine-context shared CCs — VA-side decode only ───────────────────────
    // FilterBlock is per-voice and cannot own the OBXa mode→flags→all-voices
    // fan-out (that needs SynthEngine's _patch + voice loop).  So the OBXa
    // meaning of each shared CC is decoded in SynthEngine; these methods decode
    // the VA meaning only and SynthEngine calls them when VA is active.
    //
    //   CC 112 : (OBXa Filter Mode in SynthEngine) | VA Filter Type here
    //   CC 114 : (OBXa Xpander sub-mode "        ) | VA Drive (0..1) here
    //   CC 111 : (OBXa Multimode        "        ) | VA Saturation here
    //
    // Each takes the raw 7-bit CC value (0..127); bucket math matches the
    // SELECT decode used elsewhere so the display stays in agreement.
    void setContextCC112(uint8_t ccValue);   // VA Type
    void setContextCC114(uint8_t ccValue);   // VA Drive
    void setContextCC111(uint8_t ccValue);   // VA Saturation

    // ── Audio graph access (fixed entry/exit points) ──────────────────────────
    AudioStream& input();     // VoiceBlock wires voiceMixer here
    AudioStream& output();    // VoiceBlock wires ampEnvelope here
    AudioStream& envmod();    // DC source for filter envelope ADSR
    AudioMixer4& modMixer();  // Key track / env / LFO cutoff mod bus

private:
    // ── Audio components ──────────────────────────────────────────────────────

    // Fan-out buffer: VoiceBlock audio arrives here and is copied to both
    // filters (each filter skips its own DSP when inactive).
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

    // VA-only cached state (restored on engine switch via _applyParamsToVA)
    float            _vaDrive01 = 0.0f;       // 0..1 UI value
    VASaturationType _vaSat     = SAT_TANH;   // default Warm

    // ── Audio connections (fixed; built once in constructor) ──────────────────
    //  [0]  _inputBuf   → _filterOBXa 0          (audio fan-out)
    //  [1]  _inputBuf   → _filterVA   0          (audio fan-out)
    //  [2]  _modMixer   → _filterOBXa 1          (cutoff mod)
    //  [3]  _modMixer   → _filterVA   1          (cutoff mod)
    //  [4]  _filterOBXa → _outputMix  ch 0
    //  [5]  _filterVA   → _outputMix  ch 1
    //  [6]  _keyTrackDc → _modMixer   ch 0
    static constexpr int NUM_CABLES = 7;
    AudioConnection* _cables[NUM_CABLES];

    // ── Internal helpers ──────────────────────────────────────────────────────
    void _applyParamsToOBXa();
    void _applyParamsToVA();
};
