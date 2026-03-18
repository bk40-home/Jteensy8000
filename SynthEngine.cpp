// SynthEngine.cpp
#include "SynthEngine.h"
#include "Mapping.h"
#include "CCDefs.h"
#include "Waveforms.h"   // ensure waveformFromCC + names are available
 

using namespace CC;

/*
 * SynthEngine.cpp - 8 VOICE CONSTRUCTOR (CORRECTED)
 *
 * CRITICAL FIXES:
 * 1. Use output() not getOutput()
 * 2. Use shapeModMixerOsc1() not getShapeModOsc1()
 * 3. Use frequencyModMixerOsc1() not getFreqModOsc1()
 * 4. Use filterModMixer() not getFilterMod()
 * 5. Removed setDryMix (JPFX doesn't have this - mixing is internal)
 */

SynthEngine::SynthEngine()
    : _lfo1(), _lfo2(), _fxChain()
{
    // =========================================================================
    // CONSTRUCTOR: C++ member initialisation ONLY.
    // No AudioConnection objects are created here.
    //
    // REASON: SynthEngine is a global object — its constructor runs before
    // setup(), which means before AudioMemory() is called.  The Teensy Audio
    // Library requires AudioMemory() to be allocated before any AudioConnection
    // is created; connections built before that point are silently broken and
    // produce no audio.
    //
    // All AudioConnection creation is deferred to begin(), which must be called
    // from setup() AFTER AudioMemory().
    // =========================================================================

    // Voice bookkeeping
    for (int i = 0; i < MAX_VOICES; i++) {
        _activeNotes[i]    = false;
        _noteTimestamps[i] = 0;
    }
    for (int i = 0; i < 128; i++) {
        _noteToVoice[i] = VOICE_NONE;
    }

    // Null all patch cord pointers so begin() can detect double-init
    for (int i = 0; i < MAX_VOICES; i++) {
        _voicePatch[i]                  = nullptr;
        _voicePatchLFO1ShapeOsc1[i]     = nullptr;
        _voicePatchLFO1ShapeOsc2[i]     = nullptr;
        _voicePatchLFO1FrequencyOsc1[i] = nullptr;
        _voicePatchLFO1FrequencyOsc2[i] = nullptr;
        _voicePatchLFO1Filter[i]        = nullptr;
        _voicePatchLFO2ShapeOsc1[i]     = nullptr;
        _voicePatchLFO2ShapeOsc2[i]     = nullptr;
        _voicePatchLFO2FrequencyOsc1[i] = nullptr;
        _voicePatchLFO2FrequencyOsc2[i] = nullptr;
        _voicePatchLFO2Filter[i]        = nullptr;
    }
    _patchMixerAToFinal            = nullptr;
    _patchMixerBToFinal            = nullptr;
    _patchAmpModFixedDcToAmpModMixer = nullptr;
    _patchLFO1ToAmpModMixer        = nullptr;
    _patchLFO2ToAmpModMixer        = nullptr;
    _patchAmpModMixerToAmpMultiply = nullptr;
    _patchVoiceMixerToAmpMultiply  = nullptr;
    _fxPatchInL  = nullptr;
    _fxPatchInR  = nullptr;
    _fxPatchDryL = nullptr;
    _fxPatchDryR = nullptr;
}

// =============================================================================
// begin() — call from setup() AFTER AudioMemory()
//
// Creates every AudioConnection in the engine graph.  This is separated from
// the constructor because SynthEngine is a global object and its constructor
// runs before AudioMemory() is called in setup().  Any AudioConnection built
// before AudioMemory() is silently broken and passes no audio.
// =============================================================================
void SynthEngine::begin()
{
    // =========================================================================
    // MIXER GAINS
    // Must be set before audio starts, but safe to do in begin() since they
    // are simple register writes with no memory dependency.
    // =========================================================================

    // Amp mod DC level and limiter
    _ampModFixedDc.amplitude(_ampModFixedLevel);
    _ampModLimitFixedDc.amplitude(1.0f);

    // Amp mod signal mixer: slot 0 = fixed DC, 1/2 = LFO tremolo, 3 = unused
    _ampModMixer.gain(0, 1.0f);
    _ampModMixer.gain(1, 0.0f);
    _ampModMixer.gain(2, 0.0f);
    _ampModMixer.gain(3, 0.0f);

    // Step sequencer DC — fixed amplitude 1.0, value carried by mixer gain
    _seqDc.amplitude(1.0f);

    _ampModLimiterMixer.gain(0, 1.0f);
    _ampModLimiterMixer.gain(1, 0.0f);
    _ampModLimiterMixer.gain(2, 0.0f);
    _ampModLimiterMixer.gain(3, 0.0f);

    // Voice sub-mixer gains — 8 voices across two AudioMixer4 objects
    // Each sub-mixer receives 4 voices; final mixer combines both.
    // Gain 1.0 per voice: clipping is prevented by MAX_VOICE_VELOCITY clamping
    // and the 0.1 final-mixer attenuation below.
    for (int i = 0; i < 4; i++) {
        _voiceMixerA.gain(i, 1.0f);
        _voiceMixerB.gain(i, 1.0f);
    }

    // Final mixer: 0.1 per sub-mixer = −20 dB, giving 8-voice headroom before
    // the amp-multiply stage.  Raise if output is too quiet.
    _voiceMixerFinal.gain(0, 0.1f);  // Sub-mixer A (voices 0–3)
    _voiceMixerFinal.gain(1, 0.1f);  // Sub-mixer B (voices 4–7)
    _voiceMixerFinal.gain(2, 0.0f);  // Unused
    _voiceMixerFinal.gain(3, 0.0f);  // Unused

    // =========================================================================
    // AUDIO CONNECTIONS — VOICES → MIXERS
    // =========================================================================

    for (int i = 0; i < 4; i++) {
        _voicePatch[i] = new AudioConnection(_voices[i].output(), 0, _voiceMixerA, i);
    }
    for (int i = 4; i < MAX_VOICES; i++) {
        _voicePatch[i] = new AudioConnection(_voices[i].output(), 0, _voiceMixerB, i - 4);
    }

    _patchMixerAToFinal = new AudioConnection(_voiceMixerA, 0, _voiceMixerFinal, 0);
    _patchMixerBToFinal = new AudioConnection(_voiceMixerB, 0, _voiceMixerFinal, 1);

    // =========================================================================
    // AUDIO CONNECTIONS — LFO → ALL VOICES
    // =========================================================================

    for (int i = 0; i < MAX_VOICES; i++) {
        // LFO1: shape mod (PWM), frequency mod (vibrato), filter mod (wah)
        _voicePatchLFO1ShapeOsc1[i]     = new AudioConnection(_lfo1.output(), 0, _voices[i].shapeModMixerOsc1(),     1);
        _voicePatchLFO1ShapeOsc2[i]     = new AudioConnection(_lfo1.output(), 0, _voices[i].shapeModMixerOsc2(),     1);
        _voicePatchLFO1FrequencyOsc1[i] = new AudioConnection(_lfo1.output(), 0, _voices[i].frequencyModMixerOsc1(), 1);
        _voicePatchLFO1FrequencyOsc2[i] = new AudioConnection(_lfo1.output(), 0, _voices[i].frequencyModMixerOsc2(), 1);
        _voicePatchLFO1Filter[i]        = new AudioConnection(_lfo1.output(), 0, _voices[i].filterModMixer(),         2);

        // LFO2: same destinations, separate depth control
        _voicePatchLFO2ShapeOsc1[i]     = new AudioConnection(_lfo2.output(), 0, _voices[i].shapeModMixerOsc1(),     2);
        _voicePatchLFO2ShapeOsc2[i]     = new AudioConnection(_lfo2.output(), 0, _voices[i].shapeModMixerOsc2(),     2);
        _voicePatchLFO2FrequencyOsc1[i] = new AudioConnection(_lfo2.output(), 0, _voices[i].frequencyModMixerOsc1(), 2);
        _voicePatchLFO2FrequencyOsc2[i] = new AudioConnection(_lfo2.output(), 0, _voices[i].frequencyModMixerOsc2(), 2);
        _voicePatchLFO2Filter[i]        = new AudioConnection(_lfo2.output(), 0, _voices[i].filterModMixer(),         3);
    }

    // =========================================================================
    // AUDIO CONNECTIONS — PITCH ENVELOPE → FM MIXERS
    //
    // frequencyModMixer slot allocation (per OscillatorBlock internal wiring):
    //   0 = _frequencyDc   (internal DC, wired inside OscillatorBlock — DO NOT use)
    //   1 = LFO1           (wired above)
    //   2 = LFO2           (wired above)
    //   3 = Pitch envelope (this section)
    //
    // Depth is encoded in _pitchEnvDc amplitude as semitones/12 so that
    // FM_SEMITONE_SCALE produces the correct Hz shift.  Mixer gain stays 1.0.
    // =========================================================================

    for (int i = 0; i < MAX_VOICES; ++i) {
        _voices[i]._pitchEnvPatch1 = new AudioConnection(
            _voices[i].pitchEnvOutput(), 0, _voices[i].frequencyModMixerOsc1(), 3);
        _voices[i]._pitchEnvPatch2 = new AudioConnection(
            _voices[i].pitchEnvOutput(), 0, _voices[i].frequencyModMixerOsc2(), 3);

        _voices[i].frequencyModMixerOsc1().gain(3, 1.0f);
        _voices[i].frequencyModMixerOsc2().gain(3, 1.0f);
    }

    // =========================================================================
    // AUDIO CONNECTIONS — AMP MOD MULTIPLY → FX CHAIN
    //
    // Signal flow:
    //   _ampModMixer (DC + LFO tremolo)
    //     ↘
    //      _ampMultiply  ← _voiceMixerFinal (all voices summed)
    //     ↓
    //   _fxChain (reverb/chorus/delay)
    //     ↓
    //   getFXOutL() / getFXOutR()  → I2S DAC
    // =========================================================================

    _patchAmpModFixedDcToAmpModMixer = new AudioConnection(_ampModFixedDc,    0, _ampModMixer,   0);
    _patchLFO1ToAmpModMixer          = new AudioConnection(_lfo1.output(),    0, _ampModMixer,   1);
    _patchLFO2ToAmpModMixer          = new AudioConnection(_lfo2.output(),    0, _ampModMixer,   2);
    _patchAmpModMixerToAmpMultiply   = new AudioConnection(_ampModMixer,      0, _ampMultiply,   0);
    _patchVoiceMixerToAmpMultiply    = new AudioConnection(_voiceMixerFinal,  0, _ampMultiply,   1);

    // Step sequencer → per-voice shape (PWM) mixer slot 3
    for (int i = 0; i < MAX_VOICES; ++i) {
        _patchSeqDcToShapeOsc1[i] = new AudioConnection(
            _seqDc, 0, _voices[i].shapeModMixerOsc1(), 3);
        _patchSeqDcToShapeOsc2[i] = new AudioConnection(
            _seqDc, 0, _voices[i].shapeModMixerOsc2(), 3);
    }
    // Step sequencer → amp mod mixer slot 3
    _patchSeqDcToAmpModMixer = new AudioConnection(_seqDc, 0, _ampModMixer, 3);

    // Wet path: amp → JPFX stereo input
    _fxPatchInL  = new AudioConnection(_ampMultiply, 0, _fxChain.getJPFXInput(),   0);
    _fxPatchInR  = new AudioConnection(_ampMultiply, 0, _fxChain.getJPFXInput(),   1);

    // Dry path: amp → output mixers directly (bypass FX)
    _fxPatchDryL = new AudioConnection(_ampMultiply, 0, _fxChain.getOutputLeft(),  0);
    _fxPatchDryR = new AudioConnection(_ampMultiply, 0, _fxChain.getOutputRight(), 0);
}

static inline float CCtoTime(uint8_t cc) { return JT8000Map::cc_to_time_ms(cc); }

// Safe CC-name lookup for logs (avoids nullptr)
static inline const char* ccname(uint8_t cc) {
  const char* n = CC::name(cc);
  return n ? n : "?";
}

void SynthEngine::setNotifier(NotifyFn fn) { _notify = fn; }

// ============================================================================
// BPM CLOCK MANAGEMENT & TIMING SYNC
// ============================================================================

void SynthEngine::setBPMClock(BPMClockManager* clock) {
    _bpmClock = clock;
}

void SynthEngine::updateBPMSync() {
    // Called from update() to refresh BPM-synced parameters
    if (!_bpmClock) return;
    
    // Update LFO1 if synced
    TimingMode lfo1Mode = _lfo1.getTimingMode();
    if (lfo1Mode != TimingMode::TIMING_FREE) {
        float hz = _bpmClock->getFrequencyForMode(lfo1Mode);
        _lfo1.setFrequency(hz);
    }

    _seq1.updateFromBPMClock(*_bpmClock);
    
    // Update LFO2 if synced
    TimingMode lfo2Mode = _lfo2.getTimingMode();
    if (lfo2Mode != TimingMode::TIMING_FREE) {
        float hz = _bpmClock->getFrequencyForMode(lfo2Mode);
        _lfo2.setFrequency(hz);
    }
    
    // Update delay if synced
    TimingMode delayMode = _fxChain.getDelayTimingMode();
    if (delayMode != TimingMode::TIMING_FREE) {
        float ms = _bpmClock->getTimeForMode(delayMode);
        _fxChain.setDelayTime(ms);
    }
}

// ============================================================================
// LFO TIMING MODE CONTROLS
// ============================================================================

void SynthEngine::setLFO1TimingMode(TimingMode mode) {
    _lfo1.setTimingMode(mode);
    
    if (mode == TimingMode::TIMING_FREE) {
        // Restore manual frequency control
        _lfo1.setFrequency(_lfo1Frequency);
    } else if (_bpmClock) {
        // Lock to BPM
        float hz = _bpmClock->getFrequencyForMode(mode);
        _lfo1.setFrequency(hz);
    }
}

void SynthEngine::setLFO2TimingMode(TimingMode mode) {
    _lfo2.setTimingMode(mode);
    
    if (mode == TimingMode::TIMING_FREE) {
        // Restore manual frequency control
        _lfo2.setFrequency(_lfo2Frequency);
    } else if (_bpmClock) {
        // Lock to BPM
        float hz = _bpmClock->getFrequencyForMode(mode);
        _lfo2.setFrequency(hz);
    }
}

TimingMode SynthEngine::getLFO1TimingMode() const {
    return _lfo1.getTimingMode();
}

TimingMode SynthEngine::getLFO2TimingMode() const {
    return _lfo2.getTimingMode();
}

// ============================================================================
// DELAY TIMING MODE CONTROLS
// ============================================================================

void SynthEngine::setDelayTimingMode(TimingMode mode) {
    _fxChain.setDelayTimingMode(mode);
    
    if (mode == TimingMode::TIMING_FREE) {
        // Restore manual delay time control
        _fxChain.setDelayTime(_fxDelayTime);
    } else if (_bpmClock) {
        // Lock to BPM
        float ms = _bpmClock->getTimeForMode(mode);
        _fxChain.setDelayTime(ms);
    }
}

TimingMode SynthEngine::getDelayTimingMode() const {
    return _fxChain.getDelayTimingMode();
}



void SynthEngine::noteOn(byte note, float velocity) {
    float freq = 440.0f * powf(2.0f, (note - 69) / 12.0f);
    _lastNoteFreq = freq;

    // Step sequencer retrigger
    if (_seq1.getRetrigger()) _seq1.reset();

    // Restart LFO delay ramps on any noteOn (standard JP-8000 retrigger behaviour)
    if (_lfo1DelayMs > 0.0f) { _lfo1NoteOnMs = millis(); _lfo1Ramping = true; _lfo1CurrentAmp = 0.0f; }
    if (_lfo2DelayMs > 0.0f) { _lfo2NoteOnMs = millis(); _lfo2Ramping = true; _lfo2CurrentAmp = 0.0f; }

    // Limit per-voice amplitude to 0.95 — leaves headroom when multiple
    // voices sound simultaneously.
    static constexpr float MAX_VOICE_VELOCITY = 0.95f;
    if (velocity > MAX_VOICE_VELOCITY) velocity = MAX_VOICE_VELOCITY;

    // =========================================================================
    // MONO mode — single voice, last-note priority with legato note stack.
    //
    // All held keys are tracked in _monoStack.  The top of the stack is the
    // note that should be sounding.  When a new key is pressed it becomes
    // the top and voice 0 glides to it.  When a key is released:
    //   - If it was the top note AND other keys are still held, voice 0
    //     glides back to the new top note (legato return).
    //   - If no keys remain, voice 0 enters its release envelope.
    // This matches classic mono synth behaviour (e.g. Minimoog, JP-8000).
    // =========================================================================
    if (_polyMode == PolyMode::MONO) {
        _monoStack.push(note);

        // Clear any stale note→voice mappings — only voice 0 is used.
        for (int n = 0; n < 128; ++n) {
            if (_noteToVoice[n] != VOICE_NONE) { _noteToVoice[n] = VOICE_NONE; }
        }
        _noteToVoice[note] = 0;

        _voices[0].noteOn(freq, velocity);
        _activeNotes[0] = true;
        _noteTimestamps[0] = _clock++;
        return;
    }

    // =========================================================================
    // UNISON mode — all 8 voices play the same note, detuned
    // =========================================================================
    if (_polyMode == PolyMode::UNISON) {
        // Release the previous unison note if a different note comes in
        if (_unisonNote >= 0 && _unisonNote != (int)note) {
            for (int i = 0; i < MAX_VOICES; ++i) {
                _voices[i].noteOff();
                _activeNotes[i] = false;
            }
            _noteToVoice[_unisonNote] = VOICE_NONE;
        }
        _unisonNote = (int)note;
        _noteToVoice[note] = 0;  // Sentinel: voice 0 represents the note
        for (int i = 0; i < MAX_VOICES; ++i) {
            _voices[i].noteOn(freq, velocity);
            _activeNotes[i] = true;
            _noteTimestamps[i] = _clock++;
        }
        return;
    }

    // =========================================================================
    // POLY mode — standard 8-voice last-note-priority polyphony
    // =========================================================================

    // Retrigger: same note already playing on a voice
    if (_noteToVoice[note] != VOICE_NONE) {
        int v = _noteToVoice[note];
        _voices[v].noteOn(freq, velocity);
        _noteTimestamps[v] = _clock++;
        return;
    }
    // Free voice available
    for (int i = 0; i < MAX_VOICES; ++i) {
        if (!_activeNotes[i]) {
            _voices[i].noteOn(freq, velocity);
            _activeNotes[i] = true;
            _noteToVoice[note] = i;
            _noteTimestamps[i] = _clock++;
            return;
        }
    }
    // Voice steal — take the oldest note
    int oldest = 0;
    for (int i = 1; i < MAX_VOICES; ++i)
        if (_noteTimestamps[i] < _noteTimestamps[oldest]) oldest = i;

    for (int n = 0; n < 128; ++n)
        if (_noteToVoice[n] == oldest) { _noteToVoice[n] = VOICE_NONE; break; }

    _voices[oldest].noteOn(freq, velocity);
    _activeNotes[oldest] = true;
    _noteToVoice[note] = oldest;
    _noteTimestamps[oldest] = _clock++;
}

void SynthEngine::noteOff(byte note) {

    // =========================================================================
    // MONO mode — legato note-stack return.
    //
    // Remove the released key from the stack.  If other keys are still held,
    // glide voice 0 to the new top note instead of releasing it.  If no keys
    // remain, do a normal release so the amp envelope closes naturally.
    // =========================================================================
    if (_polyMode == PolyMode::MONO) {
        _monoStack.remove(note);
        _noteToVoice[note] = VOICE_NONE;

        if (!_monoStack.empty()) {
            // Return to the most recently held key that is still down.
            byte returnNote = _monoStack.top();
            float returnFreq = 440.0f * powf(2.0f, (returnNote - 69) / 12.0f);
            _lastNoteFreq = returnFreq;

            // Clear stale mappings, map the return note to voice 0.
            for (int n = 0; n < 128; ++n) _noteToVoice[n] = VOICE_NONE;
            _noteToVoice[returnNote] = 0;

            // noteOn will glide from the current pitch to the return pitch
            // because voice 0 is already active and glide is implicit in MONO.
            _voices[0].noteOn(returnFreq, _voices[0].getLastVelocity());
            _noteTimestamps[0] = _clock++;
        } else {
            // No held keys — release voice 0 normally.
            _voices[0].noteOff();
            _activeNotes[0] = false;
        }
        return;
    }

    if (_noteToVoice[note] == VOICE_NONE) return;

    if (_polyMode == PolyMode::UNISON) {
        // Release all voices when the held unison note lifts
        if ((int)note == _unisonNote) {
            for (int i = 0; i < MAX_VOICES; ++i) {
                _voices[i].noteOff();
                _activeNotes[i] = false;
            }
            _noteToVoice[note] = VOICE_NONE;
            _unisonNote = -1;
        }
        return;
    }

    // POLY: release the single assigned voice
    int v = _noteToVoice[note];
    _voices[v].noteOff();
    _activeNotes[v] = false;
    _noteToVoice[note] = VOICE_NONE;
}

void SynthEngine::update() {
    
// ── Step sequencer tick ─────────────────────────────────────────
  {
      uint32_t now = micros();
      float deltaMs = (now - _lastUpdateMicros) * 0.001f;
      _lastUpdateMicros = now;
      // Clamp to avoid huge jumps on first call or overflow
      if (deltaMs > 50.0f) deltaMs = 50.0f;
      _seq1.tick(deltaMs);
      _applySeqOutput();
  }
    
    // Update BPM-synced parameters
    if (_bpmClock) {
        updateBPMSync();
    }

    // Update LFO delay ramps (must run every loop iteration)
    _updateLFODelay();

    // Update LFO enabled state
    _lfo1.update();
    _lfo2.update();

    // Update only voices that are actively sounding.
    // IMPORTANT: do NOT add a "|| v == 0" exception here — that forces voice 0
    // to run its oscillator/glide update on every loop even when silent, which
    // costs the equivalent of one full voice of CPU at idle.  In MONO mode the
    // voice-0-only path is handled correctly by _activeNotes[0] being set.
    for (uint8_t v = 0; v < MAX_VOICES; v++) {
        if (_activeNotes[v]) {
            _voices[v].update();
        }
    }

}

// ---- Filter / Env ----
void SynthEngine::setFilterCutoff(float value) {
    // Validate range
    value = constrain(value, CUTOFF_MIN_HZ, CUTOFF_MAX_HZ);
    _filterCutoffHz = value;
    for (int i = 0; i < MAX_VOICES; ++i) _voices[i].setFilterCutoff(value);
}
void SynthEngine::setFilterResonance(float value) {
    _filterResonance = value;
    for (int i = 0; i < MAX_VOICES; ++i) _voices[i].setFilterResonance(value);
}
void SynthEngine::setFilterEnvAmount(float amt) {
    _filterEnvAmount = amt;
    for (int i = 0; i < MAX_VOICES; ++i) _voices[i].setFilterEnvAmount(amt);
}
void SynthEngine::setFilterKeyTrackAmount(float amt) {
    _filterKeyTrack = amt;
    for (int i = 0; i < MAX_VOICES; ++i) _voices[i].setFilterKeyTrackAmount(amt);
}
void SynthEngine::setFilterOctaveControl(float octaves) {
    _filterOctaves = octaves;
    for (int i = 0; i < MAX_VOICES; ++i) _voices[i].setFilterOctaveControl(octaves);
}

void SynthEngine::setFilterMultimode(float amount) {
    _filterMultimode = amount;
    for (int i = 0; i < MAX_VOICES; ++i) _voices[i].setMultimode(amount);
}

// ---------------------------------------------------------------------------
// setFilterMode  —  single entry-point for all OBXa topology switching.
//
// This replaces the four individual bool setters (setFilterTwoPole,
// setFilterXpander4Pole, setFilterBPBlend2Pole, setFilterPush2Pole).
// Each mode resets conflicting flags so the OBXa core never sees an
// inconsistent combination (e.g. both TwoPole and Xpander active).
//
// Mode constants are CC::FILTER_MODE_* defined in CCDefs.h.
// ---------------------------------------------------------------------------
void SynthEngine::setFilterMode(uint8_t mode) {
    if (mode >= CC::FILTER_MODE_COUNT) mode = CC::FILTER_MODE_4POLE;

    _filterMode = mode;

    // Decode into the bool flags that FilterBlock/AudioFilterOBXa consume.
    // All flags default false; only the active mode sets its flag(s).
    _filterUseTwoPole   = false;
    _filterXpander4Pole = false;
    _filterBpBlend2Pole = false;
    _filterPush2Pole    = false;

    switch (mode) {
        case CC::FILTER_MODE_4POLE:
            // All flags already cleared above — standard 4-pole LP
            break;
        case CC::FILTER_MODE_2POLE:
            _filterUseTwoPole = true;
            break;
        case CC::FILTER_MODE_2POLE_BP:
            _filterUseTwoPole   = true;
            _filterBpBlend2Pole = true;
            break;
        case CC::FILTER_MODE_2POLE_PUSH:
            _filterUseTwoPole = true;
            _filterPush2Pole  = true;
            break;
        case CC::FILTER_MODE_XPANDER:
        case CC::FILTER_MODE_XPANDER_M:
            _filterXpander4Pole = true;
            break;
        default:
            break;
    }

    // Push decoded flags to all voices
    for (int i = 0; i < MAX_VOICES; ++i) {
        _voices[i].setTwoPole(_filterUseTwoPole);
        _voices[i].setXpander4Pole(_filterXpander4Pole);
        _voices[i].setBPBlend2Pole(_filterBpBlend2Pole);
        _voices[i].setPush2Pole(_filterPush2Pole);
    }
}

void SynthEngine::setFilterXpanderMode(uint8_t amount) {
    _filterXpanderMode = amount;
    for (int i = 0; i < MAX_VOICES; ++i) _voices[i].setXpanderMode(amount);
}

void SynthEngine::setFilterEngine(uint8_t engine) {
    _filterEngine = engine;
    for (int i = 0; i < MAX_VOICES; ++i) _voices[i].setFilterEngine(engine);
    JT_LOGF("[SE] FilterEngine = %u\n", engine);
}

void SynthEngine::setVAFilterType(uint8_t vaType) {
    // Clamp to valid VAFilterType range
    if (vaType >= (uint8_t)FILTER_COUNT) vaType = (uint8_t)FILTER_SVF_LP;
    _vaFilterType = vaType;
    for (int i = 0; i < MAX_VOICES; ++i)
        _voices[i].setVAFilterType((VAFilterType)vaType);
    JT_LOGF("[SE] VAFilterType = %u\n", vaType);
}

void SynthEngine::setFilterResonanceModDepth(float amount) {
    _filterResonaceModDepth = amount;
    for (int i = 0; i < MAX_VOICES; ++i) _voices[i].setResonanceModDepth(amount);
}

float SynthEngine::getFilterCutoff() const         { return _filterCutoffHz; }
float SynthEngine::getFilterResonance() const      { return _filterResonance; }
float SynthEngine::getFilterEnvAmount() const      { return _filterEnvAmount; }
float SynthEngine::getFilterKeyTrackAmount() const { return _filterKeyTrack; }
float SynthEngine::getFilterOctaveControl() const  { return _filterOctaves; }

// ---- Envelopes (read-through to voices; uses voice 0 as representative) ----
float SynthEngine::getAmpAttack()  const { return MAX_VOICES ? _voices[0].getAmpAttack()  : 0.0f; }
float SynthEngine::getAmpDecay()   const { return MAX_VOICES ? _voices[0].getAmpDecay()   : 0.0f; }
float SynthEngine::getAmpSustain() const { return MAX_VOICES ? _voices[0].getAmpSustain() : 0.0f; }
float SynthEngine::getAmpRelease() const { return MAX_VOICES ? _voices[0].getAmpRelease() : 0.0f; }

float SynthEngine::getFilterEnvAttack()  const { return MAX_VOICES ? _voices[0].getFilterEnvAttack()  : 0.0f; }
float SynthEngine::getFilterEnvDecay()   const { return MAX_VOICES ? _voices[0].getFilterEnvDecay()   : 0.0f; }
float SynthEngine::getFilterEnvSustain() const { return MAX_VOICES ? _voices[0].getFilterEnvSustain() : 0.0f; }
float SynthEngine::getFilterEnvRelease() const { return MAX_VOICES ? _voices[0].getFilterEnvRelease() : 0.0f; }

// ---- Oscillators / mixes ----
void SynthEngine::setOscWaveforms(int wave1, int wave2) { setOsc1Waveform(wave1); setOsc2Waveform(wave2); }
void SynthEngine::setOsc1Waveform(int wave) { _osc1Wave = wave; for (int i=0;i<MAX_VOICES;++i) _voices[i].setOsc1Waveform(wave); }
void SynthEngine::setOsc2Waveform(int wave) { _osc2Wave = wave; for (int i=0;i<MAX_VOICES;++i) _voices[i].setOsc2Waveform(wave); }

void SynthEngine::setOsc1PitchOffset(float semis) { _osc1PitchSemi = semis; for (int i=0;i<MAX_VOICES;++i) _voices[i].setOsc1PitchOffset(semis); }
void SynthEngine::setOsc2PitchOffset(float semis) { _osc2PitchSemi = semis; for (int i=0;i<MAX_VOICES;++i) _voices[i].setOsc2PitchOffset(semis); }

// ============================================================================
// PITCH BEND
// ============================================================================
// MIDI pitch bend uses a 14-bit value (0..16383, centre = 8192).
// Converted to ±_pitchBendRange semitones and routed to every voice via
// OscillatorBlock::setPitchBend(), which writes a DC value into the FM
// pre-mixer. The AudioSynthWaveformJT handles the actual pitch shift
// via its frequency modulation input — no software pitch calculation needed.
//
// This gives exact semitone accuracy at ALL base frequencies because the
// FM input uses exponential (musical) scaling internally.
// ============================================================================

void SynthEngine::setPitchBendRange(float semitones) {
    // Clamp to sensible range.  Zero range means the wheel is silent.
    if (semitones < 0.0f)                        semitones = 0.0f;
    if (semitones > PITCH_BEND_MAX_SEMITONES)     semitones = PITCH_BEND_MAX_SEMITONES;
    _pitchBendRange = semitones;

    // Re-apply the current bend at the new range so pitch is consistent
    // immediately without requiring another wheel movement.
    // Preserve current normalised wheel position, re-scale to new range.
    const float oldRange = (_pitchBendRange > 0.0f) ? _pitchBendRange : PITCH_BEND_DEFAULT_SEMITONES;
    const float normPos  = (_pitchBendRange > 0.0f) ? (_pitchBendSemis / oldRange) : 0.0f;
    _pitchBendSemis = normPos * semitones;
    for (int i = 0; i < MAX_VOICES; ++i) {
        _voices[i].setOsc1PitchBend(_pitchBendSemis);
        _voices[i].setOsc2PitchBend(_pitchBendSemis);
    }
}

// =============================================================================
// POLY MODE
// =============================================================================

void SynthEngine::setPolyMode(PolyMode mode) {
    if (_polyMode == mode) return;
    _polyMode = mode;

    // Kill all active voices when switching modes to prevent stuck notes.
    for (int i = 0; i < MAX_VOICES; ++i) {
        _voices[i].noteOff();
        _activeNotes[i] = false;
    }
    for (int n = 0; n < 128; ++n) _noteToVoice[n] = VOICE_NONE;
    _unisonNote = -1;
    _monoStack.clear();

    // In UNISON mode, apply spread detune across voices immediately.
    if (mode == PolyMode::UNISON) _applyUnisonDetune();

    JT_LOGF("[SynthEngine] PolyMode → %s\n",
        mode == PolyMode::POLY   ? "POLY" :
        mode == PolyMode::MONO   ? "MONO" : "UNISON");
}

void SynthEngine::setUnisonDetune(float amount) {
    if (amount < 0.0f) amount = 0.0f;
    if (amount > 1.0f) amount = 1.0f;
    _unisonDetune = amount;
    if (_polyMode == PolyMode::UNISON) _applyUnisonDetune();
}

// _applyUnisonDetune() — spread voices evenly across ±spread/2 semitones.
// Voice 0 gets the most negative offset, voice N-1 the most positive.
// With 8 voices and spread=1.0 semitone: offsets = -0.5, -0.357, ..., +0.5 semi.
void SynthEngine::_applyUnisonDetune() {
    const float spread = _unisonDetune * UNISON_MAX_SPREAD_SEMITONES;
    for (int i = 0; i < MAX_VOICES; ++i) {
        // Linear spacing from -spread/2 to +spread/2 across MAX_VOICES.
        // With 1 voice: offset = 0 (avoids divide by zero via the guard).
        const float offset = (MAX_VOICES > 1)
            ? (-spread * 0.5f + spread * (float)i / (float)(MAX_VOICES - 1))
            : 0.0f;
        _voices[i].setOsc1Detune(offset);
        _voices[i].setOsc2Detune(offset);
    }
}

void SynthEngine::handlePitchBend(uint8_t /*channel*/, int16_t value) {
    // MIDI pitch bend: 0..16383, centre = 8192.
    // Map to -1.0 .. +1.0:
    //   value=0     → -1.0 (full down)
    //   value=8192  →  0.0 (centre / no bend)
    //   value=16383 → +1.0 (full up, almost — correct enough)
    //
    // Multiply by current bend range to get semitones.
    const float normalised  = (float)(value - 8192) / 8192.0f;
    _pitchBendSemis         = normalised * _pitchBendRange;

    // Apply to all voices — both oscillators share the same bend offset.
    for (int i = 0; i < MAX_VOICES; ++i) {
        _voices[i].setOsc1PitchBend(_pitchBendSemis);
        _voices[i].setOsc2PitchBend(_pitchBendSemis);
    }
}

void SynthEngine::setOsc1Detune(float hz) { _osc1DetuneHz = hz; for (int i=0;i<MAX_VOICES;++i) _voices[i].setOsc1Detune(hz); }
void SynthEngine::setOsc2Detune(float hz) { _osc2DetuneHz = hz; for (int i=0;i<MAX_VOICES;++i) _voices[i].setOsc2Detune(hz); }

void SynthEngine::setOsc1FineTune(float cents) { _osc1FineCents = cents; for (int i=0;i<MAX_VOICES;++i) _voices[i].setOsc1FineTune(cents); }
void SynthEngine::setOsc2FineTune(float cents) { _osc2FineCents = cents; for (int i=0;i<MAX_VOICES;++i) _voices[i].setOsc2FineTune(cents); }

void SynthEngine::setOscMix(float osc1Level, float osc2Level) {
    _osc1Mix = osc1Level; _osc2Mix = osc2Level;
    for (int i=0;i<MAX_VOICES;++i) _voices[i].setOscMix(osc1Level, osc2Level);
}
void SynthEngine::setOsc1Mix(float oscLevel) { _osc1Mix = oscLevel; for (int i=0;i<MAX_VOICES;++i) _voices[i].setOsc1Mix(oscLevel); }
void SynthEngine::setOsc2Mix(float oscLevel) { _osc2Mix = oscLevel; for (int i=0;i<MAX_VOICES;++i) _voices[i].setOsc2Mix(oscLevel); }
void SynthEngine::setSubMix(float mix)  { _subMix = mix;  for (int i=0;i<MAX_VOICES;++i) _voices[i].setSubMix(mix); }
void SynthEngine::setNoiseMix(float mix){ _noiseMix = mix;for (int i=0;i<MAX_VOICES;++i) _voices[i].setNoiseMix(mix); }

void SynthEngine::setSupersawDetune(uint8_t oscIndex, float amount) {
    if (oscIndex > 1) return;
    for (int i = 0; i < MAX_VOICES; ++i) {
        if (oscIndex == 0) _voices[i].setOsc1SupersawDetune(amount);
        else                _voices[i].setOsc2SupersawDetune(amount);
    }
}

void SynthEngine::setSupersawMix(uint8_t oscIndex, float amount) {
    if (oscIndex > 1) return;
    for (int i = 0; i < MAX_VOICES; ++i) {
        if (oscIndex == 0) _voices[i].setOsc1SupersawMix(amount);
        else                _voices[i].setOsc2SupersawMix(amount);
    }
}

void SynthEngine::setOsc1FrequencyDcAmp(float amp) { _osc1FreqDc = amp; for (int i=0;i<MAX_VOICES;++i) _voices[i].setOsc1FrequencyDcAmp(amp); }
void SynthEngine::setOsc2FrequencyDcAmp(float amp) { _osc2FreqDc = amp; for (int i=0;i<MAX_VOICES;++i) _voices[i].setOsc2FrequencyDcAmp(amp); }
void SynthEngine::setOsc1ShapeDcAmp(float amp)     { _osc1ShapeDc = amp; for (int i=0;i<MAX_VOICES;++i) _voices[i].setOsc1ShapeDcAmp(amp); }
void SynthEngine::setOsc2ShapeDcAmp(float amp)     { _osc2ShapeDc = amp; for (int i=0;i<MAX_VOICES;++i) _voices[i].setOsc2ShapeDcAmp(amp); }

void SynthEngine::setRing1Mix(float level) { _ring1Mix = level; for (int i=0;i<MAX_VOICES;++i) _voices[i].setRing1Mix(level); }
void SynthEngine::setRing2Mix(float level) { _ring2Mix = level; for (int i=0;i<MAX_VOICES;++i) _voices[i].setRing2Mix(level); }

void SynthEngine::setOsc1FeedbackAmount(float amount) { _osc1FeedbackAmount = amount; for (int i=0;i<MAX_VOICES;++i) _voices[i].setOsc1FeedbackAmount(amount); }
void SynthEngine::setOsc2FeedbackAmount(float amount) { _osc2FeedbackAmount = amount; for (int i=0;i<MAX_VOICES;++i) _voices[i].setOsc2FeedbackAmount(amount); }

void SynthEngine::setOsc1FeedbackMix(float mix) { _osc1FeedbackMix = mix; for (int i=0;i<MAX_VOICES;++i) _voices[i].setOsc1FeedbackMix(mix); }
void SynthEngine::setOsc2FeedbackMix(float mix) { _osc2FeedbackMix = mix; for (int i=0;i<MAX_VOICES;++i) _voices[i].setOsc2FeedbackMix(mix); }



// ---- Arbitrary waveform bank/index selection ----
void SynthEngine::setOsc1ArbBank(ArbBank b) {
    _osc1ArbBank = b;
    // Clamp current index against the new bank count
    uint16_t count = akwf_bankCount(b);
    if (count > 0 && _osc1ArbIndex >= count) _osc1ArbIndex = count - 1;
    for (int i = 0; i < MAX_VOICES; ++i) {
        _voices[i].setOsc1ArbBank(b);
        // also update index on voice since setArbBank may clamp index internally
        _voices[i].setOsc1ArbIndex(_osc1ArbIndex);
    }
}

void SynthEngine::setOsc2ArbBank(ArbBank b) {
    _osc2ArbBank = b;
    uint16_t count = akwf_bankCount(b);
    if (count > 0 && _osc2ArbIndex >= count) _osc2ArbIndex = count - 1;
    for (int i = 0; i < MAX_VOICES; ++i) {
        _voices[i].setOsc2ArbBank(b);
        _voices[i].setOsc2ArbIndex(_osc2ArbIndex);
    }
}

void SynthEngine::setOsc1ArbIndex(uint16_t idx) {
    // Clamp index by current bank
    uint16_t count = akwf_bankCount(_osc1ArbBank);
    if (count == 0) {
        _osc1ArbIndex = 0;
    } else {
        if (idx >= count) idx = count - 1;
        _osc1ArbIndex = idx;
    }
    for (int i = 0; i < MAX_VOICES; ++i) _voices[i].setOsc1ArbIndex(_osc1ArbIndex);
}

void SynthEngine::setOsc2ArbIndex(uint16_t idx) {
    uint16_t count = akwf_bankCount(_osc2ArbBank);
    if (count == 0) {
        _osc2ArbIndex = 0;
    } else {
        if (idx >= count) idx = count - 1;
        _osc2ArbIndex = idx;
    }
    for (int i = 0; i < MAX_VOICES; ++i) _voices[i].setOsc2ArbIndex(_osc2ArbIndex);
}

// ---- Amp mod DC ----
void SynthEngine::SetAmpModFixedLevel(float level) {
    _ampModFixedLevel = level;
    _ampModFixedDc.amplitude(level);
}
float SynthEngine::GetAmpModFixedLevel() const { return _ampModFixedLevel; }
float SynthEngine::getAmpModFixedLevel() const { return _ampModFixedLevel; }

// ---- LFOs ----
void SynthEngine::setLFO1Frequency(float hz) { _lfo1Frequency = hz; _lfo1.setFrequency(hz); }
void SynthEngine::setLFO2Frequency(float hz) { _lfo2Frequency = hz; _lfo2.setFrequency(hz); }
void SynthEngine::setLFO1Amount(float amt) {
    _lfo1Amount = amt;
    _lfo1.setAmplitude(amt);
    if (!_lfo1Ramping) _applyLFO1Gains(); // only if not mid-delay ramp
}
void SynthEngine::setLFO2Amount(float amt) {
    _lfo2Amount = amt;
    _lfo2.setAmplitude(amt);
    if (!_lfo2Ramping) _applyLFO2Gains();
}

void SynthEngine::setLFO1Waveform(int type) { _lfo1Type = type; _lfo1.setWaveformType(type); }
void SynthEngine::setLFO2Waveform(int type) { _lfo2Type = type; _lfo2.setWaveformType(type); }

void SynthEngine::setLFO1Destination(LFODestination dest) {
    // Legacy single-destination setter — zeros all per-dest depths then sets one to 1.0.
    // Existing presets that use a single destination continue to work unchanged.
    // For multi-target modulation use setLFO1PitchDepth/FilterDepth/PWMDepth/AmpDepth.
    _lfo1Dest = dest;
    _lfo1.setDestination(dest);
    _lfo1PitchDepth  = 0.0f;
    _lfo1FilterDepth = 0.0f;
    _lfo1PWMDepth    = 0.0f;
    _lfo1AmpDepth    = 0.0f;
    switch (dest) {
        case LFO_DEST_PITCH:  _lfo1PitchDepth  = 1.0f; break;
        case LFO_DEST_FILTER: _lfo1FilterDepth = 1.0f; break;
        case LFO_DEST_PWM:    _lfo1PWMDepth    = 1.0f; break;
        case LFO_DEST_AMP:    _lfo1AmpDepth    = 1.0f; break;
        default: break;
    }
    _applyLFO1Gains();
}

void SynthEngine::setLFO2Destination(LFODestination dest) {
    // Legacy single-destination setter — backward compatible with Microsphere presets.
    _lfo2Dest = dest;
    _lfo2.setDestination(dest);
    _lfo2PitchDepth  = 0.0f;
    _lfo2FilterDepth = 0.0f;
    _lfo2PWMDepth    = 0.0f;
    _lfo2AmpDepth    = 0.0f;
    switch (dest) {
        case LFO_DEST_PITCH:  _lfo2PitchDepth  = 1.0f; break;
        case LFO_DEST_FILTER: _lfo2FilterDepth = 1.0f; break;
        case LFO_DEST_PWM:    _lfo2PWMDepth    = 1.0f; break;
        case LFO_DEST_AMP:    _lfo2AmpDepth    = 1.0f; break;
        default: break;
    }
    _applyLFO2Gains();
}

float SynthEngine::getLFO1Frequency() const { return _lfo1Frequency; }
float SynthEngine::getLFO2Frequency() const { return _lfo2Frequency; }
float SynthEngine::getLFO1Amount() const    { return _lfo1Amount; }
float SynthEngine::getLFO2Amount() const    { return _lfo2Amount; }
int   SynthEngine::getLFO1Waveform() const  { return _lfo1Type; }
int   SynthEngine::getLFO2Waveform() const  { return _lfo2Type; }
LFODestination SynthEngine::getLFO1Destination() const { return _lfo1Dest; }
LFODestination SynthEngine::getLFO2Destination() const { return _lfo2Dest; }

const char* SynthEngine::getLFO1WaveformName() const {
    return waveformShortName((WaveformType)_lfo1Type);
}
const char* SynthEngine::getLFO2WaveformName() const {
    return waveformShortName((WaveformType)_lfo2Type);
}
const char* SynthEngine::getLFO1DestinationName() const {
    // Show "Multi" when more than one per-destination depth is active
    int active = (_lfo1PitchDepth > 0) + (_lfo1FilterDepth > 0)
               + (_lfo1PWMDepth   > 0) + (_lfo1AmpDepth   > 0);
    if (active > 1)  return "Multi";
    if (active == 0) return LFODestNames[LFO_DEST_NONE];
    if (_lfo1PitchDepth  > 0) return LFODestNames[LFO_DEST_PITCH];
    if (_lfo1FilterDepth > 0) return LFODestNames[LFO_DEST_FILTER];
    if (_lfo1PWMDepth    > 0) return LFODestNames[LFO_DEST_PWM];
    if (_lfo1AmpDepth    > 0) return LFODestNames[LFO_DEST_AMP];
    return LFODestNames[LFO_DEST_NONE];
}
const char* SynthEngine::getLFO2DestinationName() const {
    int active = (_lfo2PitchDepth > 0) + (_lfo2FilterDepth > 0)
               + (_lfo2PWMDepth   > 0) + (_lfo2AmpDepth   > 0);
    if (active > 1)  return "Multi";
    if (active == 0) return LFODestNames[LFO_DEST_NONE];
    if (_lfo2PitchDepth  > 0) return LFODestNames[LFO_DEST_PITCH];
    if (_lfo2FilterDepth > 0) return LFODestNames[LFO_DEST_FILTER];
    if (_lfo2PWMDepth    > 0) return LFODestNames[LFO_DEST_PWM];
    if (_lfo2AmpDepth    > 0) return LFODestNames[LFO_DEST_AMP];
    return LFODestNames[LFO_DEST_NONE];
}


// ============================================================================
// NEW: LFO PER-DESTINATION GAINS
// Final gain on each mixer input = masterAmount * destDepth
// ============================================================================

void SynthEngine::_applyLFO1Gains() {
    // -------------------------------------------------------------------------
    // Effective LFO amplitude:
    //   If LFO1_DEPTH CC is explicitly set, use it as a master scale.
    //   If no depth CC was received but any per-dest depth is non-zero,
    //   auto-raise to 1.0 so destinations work without needing LFO1_DEPTH.
    //   This mirrors the JP-8000 where LFO Rate + Depth are independent of dest.
    // -------------------------------------------------------------------------
    const float eff1 = (_lfo1Amount > 0.0f) ? _lfo1Amount : (
        (_lfo1PitchDepth > 0.0f || _lfo1FilterDepth > 0.0f ||
         _lfo1PWMDepth   > 0.0f || _lfo1AmpDepth   > 0.0f) ? 1.0f : 0.0f);

    // Drive the DSP waveform amplitude — only write if changed (avoid audio glitch)
    if (eff1 != _lfo1.getAmplitude()) _lfo1.setAmplitude(eff1);

    // -------------------------------------------------------------------------
    // PITCH gain:
    //   _lfo1PitchDepth (0..1 from CC) represents the fraction of max vibrato.
    //   LFO_PITCH_MAX_SEMITONES × FM_SEMITONE_SCALE converts the desired semitone
    //   range into the correct FM-mixer input amplitude (see SynthEngine.h).
    //   Without FM_SEMITONE_SCALE, full depth would try to shift ±10 octaves!
    // -------------------------------------------------------------------------
    const float pitchScale = LFO_PITCH_MAX_SEMITONES * FM_SEMITONE_SCALE;  // = 7/120 ≈ 0.0583
    const float pitchG  = eff1 * _lfo1PitchDepth * pitchScale;

    // Filter, PWM and amp gains are already dimensionless (0..1 into their respective
    // mod mixers) — no additional scale needed for those paths.
    const float filterG = eff1 * _lfo1FilterDepth;
    const float pwmG    = eff1 * _lfo1PWMDepth;
    const float ampG    = eff1 * _lfo1AmpDepth;

    for (int i = 0; i < MAX_VOICES; ++i) {
        _voices[i].frequencyModMixerOsc1().gain(1, pitchG);
        _voices[i].frequencyModMixerOsc2().gain(1, pitchG);
        _voices[i].filterModMixer().gain(2, filterG);
        _voices[i].shapeModMixerOsc1().gain(1, pwmG);
        _voices[i].shapeModMixerOsc2().gain(1, pwmG);
    }
    _ampModMixer.gain(1, ampG);
}

void SynthEngine::_applyLFO2Gains() {
    // Same structure as _applyLFO1Gains — see comments there for explanation.
    const float eff2 = (_lfo2Amount > 0.0f) ? _lfo2Amount : (
        (_lfo2PitchDepth > 0.0f || _lfo2FilterDepth > 0.0f ||
         _lfo2PWMDepth   > 0.0f || _lfo2AmpDepth   > 0.0f) ? 1.0f : 0.0f);
    if (eff2 != _lfo2.getAmplitude()) _lfo2.setAmplitude(eff2);

    // Pitch: scale depth (0..1) to FM mod-input units via semitone conversion
    const float pitchScale = LFO_PITCH_MAX_SEMITONES * FM_SEMITONE_SCALE;  // ≈ 0.0583
    const float pitchG  = eff2 * _lfo2PitchDepth * pitchScale;
    const float filterG = eff2 * _lfo2FilterDepth;
    const float pwmG    = eff2 * _lfo2PWMDepth;
    const float ampG    = eff2 * _lfo2AmpDepth;
    for (int i = 0; i < MAX_VOICES; ++i) {
        _voices[i].frequencyModMixerOsc1().gain(2, pitchG);
        _voices[i].frequencyModMixerOsc2().gain(2, pitchG);
        _voices[i].filterModMixer().gain(3, filterG);
        _voices[i].shapeModMixerOsc1().gain(2, pwmG);
        _voices[i].shapeModMixerOsc2().gain(2, pwmG);
    }
    _ampModMixer.gain(2, ampG);
}

void SynthEngine::_applySeqOutput() {
    const float val = _seq1.getOutput();  // −depth … +depth (0 when disabled)
    const uint8_t dest = _seqDestination;

    // ── Destination changed: zero out the OLD destination ─────────────
    if (dest != _seqPrevDestination) {
        switch (_seqPrevDestination) {
        case LFO_DEST_PITCH:
            for (int i = 0; i < MAX_VOICES; ++i)
                _voices[i].setSeqPitchOffset(0.0f);
            break;
        case LFO_DEST_FILTER:
            for (int i = 0; i < MAX_VOICES; ++i)
                _voices[i].setSeqFilterOffset(0.0f);
            break;
        case LFO_DEST_PWM:
            for (int i = 0; i < MAX_VOICES; ++i) {
                _voices[i].shapeModMixerOsc1().gain(3, 0.0f);
                _voices[i].shapeModMixerOsc2().gain(3, 0.0f);
            }
            break;
        case LFO_DEST_AMP:
            _ampModMixer.gain(3, 0.0f);
            break;
        }
        _seqPrevDestination = dest;
    }

    // ── Apply to current destination ─────────────────────────────────
    switch (dest) {
    case LFO_DEST_PITCH: {
        // Convert bipolar output to FM-scaled semitones.
        // Same scaling as LFO pitch: max semitones × FM_SEMITONE_SCALE.
        const float pitchFm = val * LFO_PITCH_MAX_SEMITONES * FM_SEMITONE_SCALE;
        for (int i = 0; i < MAX_VOICES; ++i)
            _voices[i].setSeqPitchOffset(pitchFm);
        break;
    }
    case LFO_DEST_FILTER:
        // Added to key tracking DC on mod bus slot 0
        for (int i = 0; i < MAX_VOICES; ++i)
            _voices[i].setSeqFilterOffset(val);
        break;

    case LFO_DEST_PWM:
        // Shape mixer slot 3 gain — _seqDc outputs 1.0, gain carries value
        for (int i = 0; i < MAX_VOICES; ++i) {
            _voices[i].shapeModMixerOsc1().gain(3, val);
            _voices[i].shapeModMixerOsc2().gain(3, val);
        }
        break;

    case LFO_DEST_AMP:
        // Amp mod mixer slot 3 gain — _seqDc outputs 1.0, gain carries value
        _ampModMixer.gain(3, val);
        break;

    default:
        // LFO_DEST_NONE — nothing to do (output is 0.0 anyway)
        break;
    }
}
void SynthEngine::setLFO1PitchDepth(float d)  { _lfo1PitchDepth  = d; _applyLFO1Gains(); }
void SynthEngine::setLFO1FilterDepth(float d) { _lfo1FilterDepth = d; _applyLFO1Gains(); }
void SynthEngine::setLFO1PWMDepth(float d)    { _lfo1PWMDepth    = d; _applyLFO1Gains(); }
void SynthEngine::setLFO1AmpDepth(float d)    { _lfo1AmpDepth    = d; _applyLFO1Gains(); }
void SynthEngine::setLFO1Delay(float ms)      { _lfo1DelayMs     = ms; }

void SynthEngine::setLFO2PitchDepth(float d)  { _lfo2PitchDepth  = d; _applyLFO2Gains(); }
void SynthEngine::setLFO2FilterDepth(float d) { _lfo2FilterDepth = d; _applyLFO2Gains(); }
void SynthEngine::setLFO2PWMDepth(float d)    { _lfo2PWMDepth    = d; _applyLFO2Gains(); }
void SynthEngine::setLFO2AmpDepth(float d)    { _lfo2AmpDepth    = d; _applyLFO2Gains(); }
void SynthEngine::setLFO2Delay(float ms)      { _lfo2DelayMs     = ms; }

// ============================================================================
// LFO DELAY RAMP — called from update() every Arduino loop iteration.
//
// JP-8000 LFO delay: modulation fades in linearly from 0 to the target
// depth over the user-set delay time after each note-on.
//
// TWO things must be ramped in parallel:
//   1. The FM/filter/PWM/amp MIXER GAINS — these scale how much of the LFO
//      waveform reaches each destination.
//   2. The LFO OSCILLATOR AMPLITUDE — this is what the LFO audio block
//      actually outputs.  Without ramping this, the waveform is full
//      amplitude from the start and the gain ramp has no effect because
//      the mixer gain is zeroed by _applyLFO1Gains at startup.
//
// Bug that was here before: when the ramp completed (_lfo1Ramping = false)
// the final gain values were never written because the if-block was exited
// before the for-loop.  This left gains at the last intermediate step
// rather than fully applied, meaning the LFO never reached its target depth.
// Fix: write gains first, clear _lfoRamping afterwards, then hand off to
// _applyLFO1Gains for the final fully-applied state.
// ============================================================================
void SynthEngine::_updateLFODelay() {
    const uint32_t now = millis();

    // -------------------------------------------------------------------------
    // LFO1 delay ramp
    // -------------------------------------------------------------------------
    if (_lfo1Ramping && _lfo1DelayMs > 0.0f) {
        const float elapsed = (float)(now - _lfo1NoteOnMs);

        // t: normalised ramp position 0..1.  Clamp to 1 at end of delay window.
        const float t = (elapsed >= _lfo1DelayMs) ? 1.0f : (elapsed / _lfo1DelayMs);
        _lfo1CurrentAmp = _lfo1Amount * t;

        // Ramp the LFO oscillator amplitude so the waveform itself fades in.
        // Without this, the audio block outputs full amplitude from note-on and
        // only the gain ramp is effective — the intended fade-in still works but
        // the LFO oscillator wastes CPU running at full amplitude from the start.
        _lfo1.setAmplitude(_lfo1CurrentAmp);

        // Apply ramped mixer gains — same formula as _applyLFO1Gains.
        const float pitchScale = LFO_PITCH_MAX_SEMITONES * FM_SEMITONE_SCALE;
        const float pitchG  = _lfo1CurrentAmp * _lfo1PitchDepth * pitchScale;
        const float filterG = _lfo1CurrentAmp * _lfo1FilterDepth;
        const float pwmG    = _lfo1CurrentAmp * _lfo1PWMDepth;
        const float ampG    = _lfo1CurrentAmp * _lfo1AmpDepth;
        for (int i = 0; i < MAX_VOICES; ++i) {
            _voices[i].frequencyModMixerOsc1().gain(1, pitchG);
            _voices[i].frequencyModMixerOsc2().gain(1, pitchG);
            _voices[i].filterModMixer().gain(2, filterG);
            _voices[i].shapeModMixerOsc1().gain(1, pwmG);
            _voices[i].shapeModMixerOsc2().gain(1, pwmG);
        }
        _ampModMixer.gain(1, ampG);

        // End of ramp: clear flag AFTER writing gains so the final t=1 step
        // is always applied.  _applyLFO1Gains() will now own the gain state.
        if (t >= 1.0f) {
            _lfo1Ramping = false;
            _applyLFO1Gains();  // Snap to final fully-accurate values
        }
    }

    // -------------------------------------------------------------------------
    // LFO2 delay ramp (same logic as LFO1)
    // -------------------------------------------------------------------------
    if (_lfo2Ramping && _lfo2DelayMs > 0.0f) {
        const float elapsed = (float)(now - _lfo2NoteOnMs);
        const float t = (elapsed >= _lfo2DelayMs) ? 1.0f : (elapsed / _lfo2DelayMs);
        _lfo2CurrentAmp = _lfo2Amount * t;

        _lfo2.setAmplitude(_lfo2CurrentAmp);

        const float pitchScale = LFO_PITCH_MAX_SEMITONES * FM_SEMITONE_SCALE;
        const float pitchG  = _lfo2CurrentAmp * _lfo2PitchDepth * pitchScale;
        const float filterG = _lfo2CurrentAmp * _lfo2FilterDepth;
        const float pwmG    = _lfo2CurrentAmp * _lfo2PWMDepth;
        const float ampG    = _lfo2CurrentAmp * _lfo2AmpDepth;
        for (int i = 0; i < MAX_VOICES; ++i) {
            _voices[i].frequencyModMixerOsc1().gain(2, pitchG);
            _voices[i].frequencyModMixerOsc2().gain(2, pitchG);
            _voices[i].filterModMixer().gain(3, filterG);
            _voices[i].shapeModMixerOsc1().gain(2, pwmG);
            _voices[i].shapeModMixerOsc2().gain(2, pwmG);
        }
        _ampModMixer.gain(2, ampG);

        if (t >= 1.0f) {
            _lfo2Ramping = false;
            _applyLFO2Gains();
        }
    }
}

// ============================================================================
// NEW: PITCH ENVELOPE
// ============================================================================

void SynthEngine::setPitchEnvAttack(float ms) {
    _pitchEnvAttack = ms;
    for (int i = 0; i < MAX_VOICES; ++i) _voices[i].setPitchEnvAttack(ms);
}
void SynthEngine::setPitchEnvDecay(float ms) {
    _pitchEnvDecay = ms;
    for (int i = 0; i < MAX_VOICES; ++i) _voices[i].setPitchEnvDecay(ms);
}
void SynthEngine::setPitchEnvSustain(float l) {
    _pitchEnvSustain = l;
    for (int i = 0; i < MAX_VOICES; ++i) _voices[i].setPitchEnvSustain(l);
}
void SynthEngine::setPitchEnvRelease(float ms) {
    _pitchEnvRelease = ms;
    for (int i = 0; i < MAX_VOICES; ++i) _voices[i].setPitchEnvRelease(ms);
}
void SynthEngine::setPitchEnvDepth(float semitones) {
    semitones = constrain(semitones, -24.0f, 24.0f);
    _pitchEnvDepth = semitones;
    // VoiceBlock::setPitchEnvDepth writes amplitude = semitones / 12.0 to _pitchEnvDc.
    // Range: -1.0 (full down) to +1.0 (full up), 0 = no pitch shift.
    // freqModMixer gain(3) = 1.0. Depth is encoded in the _pitchEnvDc amplitude — do not change here.
    for (int i = 0; i < MAX_VOICES; ++i) _voices[i].setPitchEnvDepth(semitones);
}

// ============================================================================
// NEW: VELOCITY SENSITIVITY
// ============================================================================

void SynthEngine::setVelocityAmpSens(float s) {
    _velAmpSens = s;
    for (int i = 0; i < MAX_VOICES; ++i) _voices[i].setVelocityAmpSens(s);
}
void SynthEngine::setVelocityFilterSens(float s) {
    _velFilterSens = s;
    for (int i = 0; i < MAX_VOICES; ++i) _voices[i].setVelocityFilterSens(s);
}
void SynthEngine::setVelocityEnvSens(float s) {
    _velEnvSens = s;
    for (int i = 0; i < MAX_VOICES; ++i) _voices[i].setVelocityEnvSens(s);
}

// ============================================================================
// JPFX TONE CONTROL
// ============================================================================

void SynthEngine::setFXBassGain(float dB) {
    _fxBassGain = dB;
    _fxChain.setBassGain(dB);
}

void SynthEngine::setFXTrebleGain(float dB) {
    _fxTrebleGain = dB;
    _fxChain.setTrebleGain(dB);
}

float SynthEngine::getFXBassGain() const {
    return _fxBassGain;
}

float SynthEngine::getFXTrebleGain() const {
    return _fxTrebleGain;
}

// ============================================================================
// JPFX MODULATION EFFECTS
// ============================================================================

void SynthEngine::setFXModEffect(int8_t variation) {
    _fxModEffect = variation;
    _fxChain.setModEffect(variation);
}

void SynthEngine::setFXModMix(float mix) {
    _fxModMix = mix;
    _fxChain.setModMix(mix);
}

void SynthEngine::setFXModRate(float hz) {
    _fxModRate = hz;
    _fxChain.setModRate(hz);
}

void SynthEngine::setFXModFeedback(float fb) {
    _fxModFeedback = fb;
    _fxChain.setModFeedback(fb);
}

int8_t SynthEngine::getFXModEffect() const {
    return _fxModEffect;
}

float SynthEngine::getFXModMix() const {
    return _fxModMix;
}

float SynthEngine::getFXModRate() const {
    return _fxModRate;
}

float SynthEngine::getFXModFeedback() const {
    return _fxModFeedback;
}

const char* SynthEngine::getFXModEffectName() const {
    return _fxChain.getModEffectName();
}

// ============================================================================
// JPFX DELAY EFFECTS
// ============================================================================

void SynthEngine::setFXDelayEffect(int8_t variation) {
    _fxDelayEffect = variation;
    _fxChain.setDelayEffect(variation);
}

void SynthEngine::setFXDelayMix(float mix) {
    _fxDelayMix = mix;
    _fxChain.setDelayMix(mix);
}

void SynthEngine::setFXDelayFeedback(float fb) {
    _fxDelayFeedback = fb;
    _fxChain.setDelayFeedback(fb);
}

void SynthEngine::setFXDelayTime(float ms) {
    _fxDelayTime = ms;
    _fxChain.setDelayTime(ms);
}

int8_t SynthEngine::getFXDelayEffect() const {
    return _fxDelayEffect;
}

float SynthEngine::getFXDelayMix() const {
    return _fxDelayMix;
}

float SynthEngine::getFXDelayFeedback() const {
    return _fxDelayFeedback;
}

float SynthEngine::getFXDelayTime() const {
    return _fxDelayTime;
}

const char* SynthEngine::getFXDelayEffectName() const {
    return _fxChain.getDelayEffectName();
}

// ============================================================================
// JPFX DRY MIX
// ============================================================================

void SynthEngine::setFXDryMix(float level) {
    _fxDryMix = level;
    _fxChain.setDryMix(level, level); // Stereo
}

float SynthEngine::getFXDryMix() const {
    return _fxDryMix;
}

void SynthEngine::setFXReverbRoomSize(float size) {
    _fxReverbRoomSize = size;
    _fxChain.setReverbRoomSize(size);
}

void SynthEngine::setFXReverbHiDamping(float damp) {
    _fxReverbHiDamp = damp;
    _fxChain.setReverbHiDamping(damp);
}

void SynthEngine::setFXReverbLoDamping(float damp) {
    _fxReverbLoDamp = damp;
    _fxChain.setReverbLoDamping(damp);
}

void SynthEngine::setFXJPFXMix(float left, float right) {
    _fxJPFXMixL = left;
    _fxJPFXMixR = right;
    _fxChain.setJPFXMix(left, right);
}

void SynthEngine::setFXReverbMix(float left, float right) {
    _fxReverbMixL = left;
    _fxReverbMixR = right;
    _fxChain.setReverbMix(left, right);
}

// Getters
float SynthEngine::getFXReverbRoomSize() const { return _fxReverbRoomSize; }
float SynthEngine::getFXReverbHiDamping() const { return _fxReverbHiDamp; }
float SynthEngine::getFXReverbLoDamping() const { return _fxReverbLoDamp; }
float SynthEngine::getFXJPFXMixL() const { return _fxJPFXMixL; }
float SynthEngine::getFXJPFXMixR() const { return _fxJPFXMixR; }
float SynthEngine::getFXReverbMixL() const { return _fxReverbMixL; }
float SynthEngine::getFXReverbMixR() const { return _fxReverbMixR; }

// ============================================================================
// FX REVERB BYPASS CONTROL (NEW - CPU Optimization)
// ============================================================================

void SynthEngine::setFXReverbBypass(bool bypass) {
    _fxChain.setReverbBypass(bypass);
}

bool SynthEngine::getFXReverbBypass() const {
    return _fxChain.getReverbBypass();
}



// ---- UI helper getters ----
int SynthEngine::getOsc1Waveform() const { return _osc1Wave; }
int SynthEngine::getOsc2Waveform() const { return _osc2Wave; }
const char* SynthEngine::getOsc1WaveformName() const {
    return waveformShortName((WaveformType)_osc1Wave);
}
const char* SynthEngine::getOsc2WaveformName() const {
    return waveformShortName((WaveformType)_osc2Wave);
}


float SynthEngine::getSupersawDetune(uint8_t osc) const { return (osc<2)?_supersawDetune[osc]:0.0f; }
float SynthEngine::getSupersawMix(uint8_t osc)    const { return (osc<2)?_supersawMix[osc]:0.0f; }
float SynthEngine::getOsc1PitchOffset() const { return _osc1PitchSemi; }
float SynthEngine::getOsc2PitchOffset() const { return _osc2PitchSemi; }
float SynthEngine::getOsc1Detune() const { return _osc1DetuneHz; }
float SynthEngine::getOsc2Detune() const { return _osc2DetuneHz; }
float SynthEngine::getOsc1FineTune() const { return _osc1FineCents; }
float SynthEngine::getOsc2FineTune() const { return _osc2FineCents; }
float SynthEngine::getOscMix1() const { return _osc1Mix; }
float SynthEngine::getOscMix2() const { return _osc2Mix; }
float SynthEngine::getSubMix() const { return _subMix; }
float SynthEngine::getNoiseMix() const { return _noiseMix; }
float SynthEngine::getRing1Mix() const { return _ring1Mix; }
float SynthEngine::getRing2Mix() const { return _ring2Mix; }
float SynthEngine::getOsc1FrequencyDc() const { return _osc1FreqDc; }
float SynthEngine::getOsc2FrequencyDc() const { return _osc2FreqDc; }
float SynthEngine::getOsc1ShapeDc() const     { return _osc1ShapeDc; }
float SynthEngine::getOsc2ShapeDc() const     { return _osc2ShapeDc; }

float SynthEngine::getOsc1FeedbackAmount( ) const {return _osc1FeedbackAmount;}
float SynthEngine::getOsc2FeedbackAmount( ) const {return _osc2FeedbackAmount;}

float SynthEngine::getOsc1FeedbackMix( ) const {return _osc1FeedbackMix;}
float SynthEngine::getOsc2FeedbackMix( ) const {return _osc2FeedbackMix;}

bool  SynthEngine::getGlideEnabled() const { return _glideEnabled; }
float SynthEngine::getGlideTimeMs()  const { return _glideTimeMs; }



// ---- MIDI CC dispatcher with JT_LOGF tracing --------------------------------
// ---- MIDI CC dispatcher: now using CCDefs.h names consistently ----
void SynthEngine::handleControlChange(byte /*channel*/, byte control, byte value) {
    // Human-readable CC name for logs
    const char* ccName = CC::name(control);
    if (!ccName) ccName = "?";

    const float norm = value / 127.0f;

    switch (control) {
        // ------------------- OSC waveforms -------------------
        case CC::OSC1_WAVE: {
            WaveformType t = waveformFromCC(value);
            setOsc1Waveform((int)t);
            JT_LOGF("[CC %u:%s] OSC1 Waveform -> %s (%d)\n", control, ccName, waveformShortName(t), (int)t);
        } break;

        case CC::OSC2_WAVE: {
            WaveformType t = waveformFromCC(value);
            setOsc2Waveform((int)t);
            JT_LOGF("[CC %u:%s] OSC2 Waveform -> %s (%d)\n", control, ccName, waveformShortName(t), (int)t);
        } break;

        // ------------------- Mod Wheel (example: LFO1 frequency) -------------------
        case 1: { // MIDI ModWheel
            float hz = JT8000Map::cc_to_lfo_hz(value);
            setLFO1Frequency(hz);
            JT_LOGF("[CC %u:ModWheel] LFO1 Freq = %.4f Hz\n", control, hz);
        } break;

        // ------------------- Filter main -------------------
        case CC::FILTER_CUTOFF: {
            float hz = JT8000Map::cc_to_obxa_cutoff_hz(value);
            hz = fminf(fmaxf(hz, CUTOFF_MIN_HZ), CUTOFF_MAX_HZ);
            setFilterCutoff(hz);
            JT_LOGF("[CC %u:%s] Cutoff = %.2f Hz\n", control, ccName, hz);
        } break;

        case CC::FILTER_RESONANCE: {
            // Route through the engine-aware helper so the OBXa safety ceiling
            // (OBXA_RES_MAX) is applied when the OBXa engine is active, while
            // the VA engine receives the full 0..1 range.
            float r = JT8000Map::cc_to_resonance(value, _filterEngine);
            setFilterResonance(r);
            JT_LOGF("[CC %u:%s] Resonance = %.4f (engine %u)\n", control, ccName, r, _filterEngine);
        } break;

        // ------------------- Amp envelope -------------------
        case CC::AMP_ATTACK: {
            float ms = CCtoTime(value);
            for (int i=0; i<MAX_VOICES; ++i) _voices[i].setAmpAttack(ms);
            JT_LOGF("[CC %u:%s] Amp Attack = %.2f ms\n", control, ccName, ms);
        } break;

        case CC::AMP_DECAY: {
            float ms = CCtoTime(value);
            for (int i=0; i<MAX_VOICES; ++i) _voices[i].setAmpDecay(ms);
            JT_LOGF("[CC %u:%s] Amp Decay = %.2f ms\n", control, ccName, ms);
        } break;

        case CC::AMP_SUSTAIN: {
            for (int i=0; i<MAX_VOICES; ++i) _voices[i].setAmpSustain(norm);
            JT_LOGF("[CC %u:%s] Amp Sustain = %.3f\n", control, ccName, norm);
        } break;

        case CC::AMP_RELEASE: {
            float ms = CCtoTime(value);
            for (int i=0; i<MAX_VOICES; ++i) _voices[i].setAmpRelease(ms);
            JT_LOGF("[CC %u:%s] Amp Release = %.2f ms\n", control, ccName, ms);
        } break;

        // ------------------- Filter envelope -------------------
        case CC::FILTER_ENV_ATTACK: {
            float ms = CCtoTime(value);
            for (int i=0; i<MAX_VOICES; ++i) _voices[i].setFilterAttack(ms);
            JT_LOGF("[CC %u:%s] Filt Env Attack = %.2f ms\n", control, ccName, ms);
        } break;

        case CC::FILTER_ENV_DECAY: {
            float ms = CCtoTime(value);
            for (int i=0; i<MAX_VOICES; ++i) _voices[i].setFilterDecay(ms);
            JT_LOGF("[CC %u:%s] Filt Env Decay = %.2f ms\n", control, ccName, ms);
        } break;

        case CC::FILTER_ENV_SUSTAIN: {
            for (int i=0; i<MAX_VOICES; ++i) _voices[i].setFilterSustain(norm);
            JT_LOGF("[CC %u:%s] Filt Env Sustain = %.3f\n", control, ccName, norm);
        } break;

        case CC::FILTER_ENV_RELEASE: {
            float ms = CCtoTime(value);
            for (int i=0; i<MAX_VOICES; ++i) _voices[i].setFilterRelease(ms);
            JT_LOGF("[CC %u:%s] Filt Env Release = %.2f ms\n", control, ccName, ms);
        } break;

        // ------------------- Coarse pitch (stepped) -------------------
        case CC::OSC1_PITCH_OFFSET: {
            float semis;
            if (value <= 25)       semis = -24.0f;
            else if (value <= 51)  semis = -12.0f;
            else if (value <= 76)  semis =   0.0f;
            else if (value <= 101) semis = +12.0f;
            else                   semis = +24.0f;
            setOsc1PitchOffset(semis);
            JT_LOGF("[CC %u:%s] OSC1 Coarse = %.1f semitones\n", control, ccName, semis);
        } break;

        case CC::OSC2_PITCH_OFFSET: {
            float semis;
            if (value <= 25)       semis = -24.0f;
            else if (value <= 51)  semis = -12.0f;
            else if (value <= 76)  semis =   0.0f;
            else if (value <= 101) semis = +12.0f;
            else                   semis = +24.0f;
            setOsc2PitchOffset(semis);
            JT_LOGF("[CC %u:%s] OSC2 Coarse = %.1f semitones\n", control, ccName, semis);
        } break;

        // ------------------- Detune / Fine -------------------
        // Dead-zone: CC 64 forces exact zero. Without this, the odd 0-127 range
        // means no single CC value maps to precisely 0.0 through norm*2-1.
        case CC::OSC1_DETUNE: {
            float d = (value == 64) ? 0.0f : (norm * 2.0f - 1.0f) * 12.0f;
            setOsc1Detune(d);
            JT_LOGF("[CC %u:%s] OSC1 Detune = %.2f Hz\n", control, ccName, d);
        } break;
        case CC::OSC2_DETUNE: {
            float d = (value == 64) ? 0.0f : (norm * 2.0f - 1.0f) * 12.0f;
            setOsc2Detune(d);
            JT_LOGF("[CC %u:%s] OSC2 Detune = %.2f Hz\n", control, ccName, d);
        } break;
        case CC::OSC1_FINE_TUNE: {
            float c = (value == 64) ? 0.0f : norm * 200.0f - 100.0f;
            setOsc1FineTune(c);
            JT_LOGF("[CC %u:%s] OSC1 Fine = %.1f cents\n", control, ccName, c);
        } break;
        case CC::OSC2_FINE_TUNE: {
            float c = (value == 64) ? 0.0f : norm * 200.0f - 100.0f;
            setOsc2FineTune(c);
            JT_LOGF("[CC %u:%s] OSC2 Fine = %.1f cents\n", control, ccName, c);
        } break;

        // ------------------- Osc mix + taps -------------------
        case CC::OSC1_FEEDBACK_AMOUNT: {
            float a = norm;
            setOsc1FeedbackAmount(norm);
            JT_LOGF("[CC %u:%s] Osc1 feedback amount = %.3f \n", control, ccName, a);
        } break;

        case CC::OSC2_FEEDBACK_AMOUNT: {
            float a = norm;
            setOsc2FeedbackAmount(norm);
            JT_LOGF("[CC %u:%s] Osc2 feedback amount = %.3f \n", control, ccName, a);
        } break;

        case CC::OSC1_FEEDBACK_MIX: {
            float a = norm;
            setOsc1FeedbackMix(norm);
            JT_LOGF("[CC %u:%s] Osc1 feedback mix = %.3f \n", control, ccName, a);
        } break;

         case CC::OSC2_FEEDBACK_MIX: {
            float a = norm;
            setOsc2FeedbackMix(norm);
            JT_LOGF("[CC %u:%s] Osc2 feedback mix = %.3f \n", control, ccName, a);
        } break;

        // ------------------- Osc mix + taps -------------------
        case CC::OSC_MIX_BALANCE: {
            float l = 1.0f - norm, r = norm;
            setOscMix(l, r);
            JT_LOGF("[CC %u:%s] Osc Mix balance L=%.3f R=%.3f\n", control, ccName, l, r);
        } break;

        case CC::OSC1_MIX: {
            for (int i=0; i<MAX_VOICES; ++i) _voices[i].setOsc1Mix(norm);
            _osc1Mix = norm;
            JT_LOGF("[CC %u:%s] OSC1 Mix = %.3f\n", control, ccName, norm);
        } break;

        case CC::OSC2_MIX: {
            for (int i=0; i<MAX_VOICES; ++i) _voices[i].setOsc2Mix(norm);
            _osc2Mix = norm;
            JT_LOGF("[CC %u:%s] OSC2 Mix = %.3f\n", control, ccName, norm);
        } break;

        case CC::SUB_MIX:   { setSubMix(norm);   JT_LOGF("[CC %u:%s] Sub Mix   = %.3f\n", control, ccName, norm); } break;
        case CC::NOISE_MIX: { setNoiseMix(norm); JT_LOGF("[CC %u:%s] Noise Mix = %.3f\n", control, ccName, norm); } break;

        // ------------------- Filter modulation -------------------
        case CC::FILTER_ENV_AMOUNT: {
            float a = norm * 2.0f - 1.0f;
            setFilterEnvAmount(a);
            JT_LOGF("[CC %u:%s] Filt Env Amount = %.3f\n", control, ccName, a);
        } break;

        case CC::FILTER_KEY_TRACK: {
            float k = norm * 2.0f - 1.0f;
            setFilterKeyTrackAmount(k);
            JT_LOGF("[CC %u:%s] KeyTrack = %.3f\n", control, ccName, k);
        } break;

        case CC::FILTER_OCTAVE_CONTROL: {
            float o = norm * 10.0f;
            setFilterOctaveControl(o);
            JT_LOGF("[CC %u:%s] Filter Octave = %.3f\n", control, ccName, o);
        } break;

        // --- OBXa Filter Extended Controls ---

        // Multimode blend: CC 0-127 → 0.0-1.0 (LP4 → HP via pole mixing)
        case CC::FILTER_OBXA_MULTIMODE: {
            setFilterMultimode(norm);
            JT_LOGF("[CC %u:%s] Multimode = %.3f\n", control, ccName, norm);
        } break;

        // Single topology selector — clears conflicting flags automatically.
        // CC value 0-127 mapped into FILTER_MODE_COUNT equal buckets.
        case CC::FILTER_MODE: {
            const uint8_t mode = (uint8_t)constrain(
                (int)value * (int)CC::FILTER_MODE_COUNT / 128, 0,
                (int)CC::FILTER_MODE_COUNT - 1);
            setFilterMode(mode);
            JT_LOGF("[CC %u:%s] FilterMode = %u\n", control, ccName, mode);
        } break;

        // Filter engine select: 0 = OBXa, 1 = VA bank.
        // CC 0-63 = OBXa, CC 64-127 = VA bank (2 equal buckets).
        case CC::FILTER_ENGINE: {
            const uint8_t eng = (value >= 64) ? CC::FILTER_ENGINE_VA
                                              : CC::FILTER_ENGINE_OBXA;
            setFilterEngine(eng);
            JT_LOGF("[CC %u:%s] FilterEngine = %u\n", control, ccName, eng);
        } break;

        // VA bank topology: CC 0-127 mapped into FILTER_COUNT equal buckets.
        case CC::VA_FILTER_TYPE: {
            const uint8_t vt = (uint8_t)constrain(
                (int)value * (int)FILTER_COUNT / 128, 0, (int)FILTER_COUNT - 1);
            setVAFilterType(vt);
            JT_LOGF("[CC %u:%s] VAFilterType = %u\n", control, ccName, vt);
        } break;

        // Xpander sub-mode (0..14): only meaningful when FilterMode == XPANDER_M
        case CC::FILTER_OBXA_XPANDER_MODE: {
            const uint8_t mode = (uint8_t)constrain((int)value * 15 / 128, 0, 14);
            setFilterXpanderMode(mode);
            JT_LOGF("[CC %u:%s] XpanderMode = %u\n", control, ccName, mode);
        } break;

        // Resonance modulation depth: CC 0-127 → 0.0-1.0
        case CC::FILTER_OBXA_RES_MOD_DEPTH: {
            setFilterResonanceModDepth(norm);
            JT_LOGF("[CC %u:%s] ResModDepth = %.3f\n", control, ccName, norm);
        } break;

        // ------------------- LFO1 -------------------
        case CC::LFO1_FREQ:        { float hz = JT8000Map::cc_to_lfo_hz(value); setLFO1Frequency(hz); JT_LOGF("[CC %u:%s] LFO1 Freq = %.4f Hz\n", control, ccName, hz); } break;
        case CC::LFO1_DEPTH:       { setLFO1Amount(norm); JT_LOGF("[CC %u:%s] LFO1 Depth = %.3f\n", control, ccName, norm); } break;
        case CC::LFO1_DESTINATION: { int d = JT8000Map::lfoDestFromCC(value); setLFO1Destination((LFODestination)d); JT_LOGF("[CC %u:%s] LFO1 Dest = %d\n", control, ccName, d); } break;
        case CC::LFO1_WAVEFORM:    { WaveformType t = waveformFromCC(value); setLFO1Waveform((int)t); JT_LOGF("[CC %u:%s] LFO1 Wave -> %s (%d)\n", control, ccName, waveformShortName(t), (int)t); } break;

        // ------------------- LFO2 -------------------
        case CC::LFO2_FREQ:        { float hz = JT8000Map::cc_to_lfo_hz(value); setLFO2Frequency(hz); JT_LOGF("[CC %u:%s] LFO2 Freq = %.4f Hz\n", control, ccName, hz); } break;
        case CC::LFO2_DEPTH:       { setLFO2Amount(norm); JT_LOGF("[CC %u:%s] LFO2 Depth = %.3f\n", control, ccName, norm); } break;
        case CC::LFO2_DESTINATION: { int d = JT8000Map::lfoDestFromCC(value); setLFO2Destination((LFODestination)d); JT_LOGF("[CC %u:%s] LFO2 Dest = %d\n", control, ccName, d); } break;
        case CC::LFO2_WAVEFORM:    { WaveformType t = waveformFromCC(value); setLFO2Waveform((int)t); JT_LOGF("[CC %u:%s] LFO2 Wave -> %s (%d)\n", control, ccName, waveformShortName(t), (int)t); } break;

        
        // ============================================================================
        // JPFX CC HANDLERS (add to handleControlChange switch)
        // ============================================================================


        // --- JPFX Tone Control ---
        case CC::FX_BASS_GAIN: {
            float dB = (norm * 24.0f) - 12.0f; // 0..1 → -12..+12 dB
            setFXBassGain(dB);
            JT_LOGF("[CC %u:%s] Bass = %.1f dB\n", control, ccName, dB);
        } break;

        case CC::FX_TREBLE_GAIN: {
            float dB = (norm * 24.0f) - 12.0f; // 0..1 → -12..+12 dB
            setFXTrebleGain(dB);
            JT_LOGF("[CC %u:%s] Treble = %.1f dB\n", control, ccName, dB);
        } break;

        // --- JPFX Modulation Effects ---
        case CC::FX_MOD_EFFECT: {
            // Map CC 0..127 to -1..10 (off + 11 variations)
            int8_t variation = -1;
            if (value > 0) {
                // Map 1..127 evenly across 0..10
                variation = ((uint16_t)(value - 1) * 11) / 127;
                if (variation > 10) variation = 10;
            }
            setFXModEffect(variation);
            JT_LOGF("[CC %u:%s] Mod Effect = %d (%s)\n", 
                    control, ccName, variation, getFXModEffectName());
        } break;

        case CC::FX_MOD_MIX: {
            setFXModMix(norm);
            JT_LOGF("[CC %u:%s] Mod Mix = %.3f\n", control, ccName, norm);
        } break;

        case CC::FX_MOD_RATE: {
            float hz = norm * 20.0f; // 0..1 → 0..20 Hz
            setFXModRate(hz);
            JT_LOGF("[CC %u:%s] Mod Rate = %.2f Hz\n", control, ccName, hz);
        } break;

        case CC::FX_MOD_FEEDBACK: {
            // Map CC 0..127 to -1..0.99 (0 = use preset)
            float fb = -1.0f;
            if (value > 0) {
                fb = ((value - 1) / 126.0f) * 0.99f;
            }
            setFXModFeedback(fb);
            JT_LOGF("[CC %u:%s] Mod FB = %.3f\n", control, ccName, fb);
        } break;

        // --- JPFX Delay Effects ---
        case CC::FX_JPFX_DELAY_EFFECT: {
            // Map CC 0..127 to -1..4 (off + 5 variations)
            int8_t variation = -1;
            if (value > 0) {
                variation = ((uint16_t)(value - 1) * 5) / 127;
                if (variation > 4) variation = 4;
            }
            setFXDelayEffect(variation);
            JT_LOGF("[CC %u:%s] Delay Effect = %d (%s)\n", 
                    control, ccName, variation, getFXDelayEffectName());
        } break;

        case CC::FX_JPFX_DELAY_MIX: {
            setFXDelayMix(norm);
            JT_LOGF("[CC %u:%s] Delay Mix = %.3f\n", control, ccName, norm);
        } break;

        case CC::FX_JPFX_DELAY_FEEDBACK: {
            // Map CC 0..127 to -1..0.99 (0 = use preset)
            float fb = -1.0f;
            if (value > 0) {
                fb = ((value - 1) / 126.0f) * 0.99f;
            }
            setFXDelayFeedback(fb);
            JT_LOGF("[CC %u:%s] Delay FB = %.3f\n", control, ccName, fb);
        } break;

        case CC::FX_JPFX_DELAY_TIME: {
            float ms = norm * 1500.0f; // 0..1 → 0..1500 ms
            setFXDelayTime(ms);
            JT_LOGF("[CC %u:%s] Delay Time = %.1f ms\n", control, ccName, ms);
        } break;

        // --- JPFX Dry Mix ---
        case CC::FX_DRY_MIX: {
            setFXDryMix(norm);
            JT_LOGF("[CC %u:%s] Dry Mix = %.3f\n", control, ccName, norm);
        } break;
        case CC::FX_REVERB_SIZE: {
    setFXReverbRoomSize(norm);
    JT_LOGF("[CC %u:%s] Reverb Size = %.3f\n", control, ccName, norm);
} break;

case CC::FX_REVERB_DAMP: {
    setFXReverbHiDamping(norm);
    JT_LOGF("[CC %u:%s] Reverb HiDamp = %.3f\n", control, ccName, norm);
} break;

case CC::FX_REVERB_LODAMP: {
    setFXReverbLoDamping(norm);
    JT_LOGF("[CC %u:%s] Reverb LoDamp = %.3f\n", control, ccName, norm);
} break;

case CC::FX_REVERB_MIX: {
    setFXReverbMix(norm, norm);  // Stereo
    JT_LOGF("[CC %u:%s] Reverb Mix = %.3f\n", control, ccName, norm);
} break;

// ============================================================================
// NEW CC HANDLERS FOR FX MIX CONTROLS
// ============================================================================

case CC::FX_JPFX_MIX: {
    // FX_JPFX_MIX controls the JPFX output level (bypasses reverb)
    // This is the "JPFX Mix" control on Page 19
    float mix = norm;  // norm is already calculated from CC value
    setFXJPFXMix(mix, mix);  // Set both L and R channels
    JT_LOGF("[CC %u:%s] JPFX Mix = %.3f\n", control, ccName, mix);
    if (_notify) _notify(control, value);
} break;

case CC::FX_REVERB_BYPASS: {
    // FX_REVERB_BYPASS toggles reverb on/off for CPU savings
    // Values > 63 = bypassed, <= 63 = active
    bool bypass = (value > 63);
    setFXReverbBypass(bypass);
    JT_LOGF("[CC %u:%s] Reverb Bypass = %s\n", control, ccName, bypass ? "ON" : "OFF");
    if (_notify) _notify(control, value);
} break;


        // ------------------- Supersaw / DC / Ring -------------------
        case CC::SUPERSAW1_DETUNE: { setSupersawDetune(0, norm); JT_LOGF("[CC %u:%s] Supersaw1 Detune = %.3f\n", control, ccName, norm); } break;
        case CC::SUPERSAW1_MIX:    { setSupersawMix(0, norm);    JT_LOGF("[CC %u:%s] Supersaw1 Mix    = %.3f\n", control, ccName, norm); } break;
        case CC::SUPERSAW2_DETUNE: { setSupersawDetune(1, norm); JT_LOGF("[CC %u:%s] Supersaw2 Detune = %.3f\n", control, ccName, norm); } break;
        case CC::SUPERSAW2_MIX:    { setSupersawMix(1, norm);    JT_LOGF("[CC %u:%s] Supersaw2 Mix    = %.3f\n", control, ccName, norm); } break;

        // OSC1/2 FREQ DC — static pitch offset injected into the FM mixer.
        // Unipolar: CC=0 → no shift, CC=127 → +24 semitones (2 octaves up).
        //
        // PROBLEM with old code (setOsc1FrequencyDcAmp(norm)):
        //   norm=1.0 → FM input=1.0 → shift = 2^(1.0 × 10) = 1024× freq = 10 octaves
        //   (well beyond audio range — inaudible; hence "no effect")
        //   norm=0.0 → no shift — also "no effect"
        //
        // FIX: scale so CC=127 → +24 semitones using FM_SEMITONE_SCALE.
        //   amp = norm × DC_PITCH_MAX_SEMITONES × FM_SEMITONE_SCALE
        //       = norm × 24 × (1/120) = norm × 0.2
        //
        // Presets with CC=0 (default/unset) still play in tune — no regression.
        case CC::OSC1_FREQ_DC: {
            const float dcAmp = norm * DC_PITCH_MAX_SEMITONES * FM_SEMITONE_SCALE;
            setOsc1FrequencyDcAmp(dcAmp);
            JT_LOGF("[CC %u:%s] Osc1 Freq DC %.0f semitones (amp %.4f)\n",
                    control, ccName, norm * DC_PITCH_MAX_SEMITONES, dcAmp);
        } break;
        case CC::OSC1_SHAPE_DC: { setOsc1ShapeDcAmp(norm); JT_LOGF("[CC %u:%s] Osc1 Shape DC = %.3f\n", control, ccName, norm); } break;
        case CC::OSC2_FREQ_DC: {
            const float dcAmp = norm * DC_PITCH_MAX_SEMITONES * FM_SEMITONE_SCALE;
            setOsc2FrequencyDcAmp(dcAmp);
            JT_LOGF("[CC %u:%s] Osc2 Freq DC %.0f semitones (amp %.4f)\n",
                    control, ccName, norm * DC_PITCH_MAX_SEMITONES, dcAmp);
        } break;
        case CC::OSC2_SHAPE_DC: { setOsc2ShapeDcAmp(norm); JT_LOGF("[CC %u:%s] Osc2 Shape DC = %.3f\n", control, ccName, norm); } break;

        case CC::RING1_MIX: { setRing1Mix(norm); JT_LOGF("[CC %u:%s] Ring1 Mix = %.3f\n", control, ccName, norm); } break;
        case CC::RING2_MIX: { setRing2Mix(norm); JT_LOGF("[CC %u:%s] Ring2 Mix = %.3f\n", control, ccName, norm); } break;

        // ------------------- Arbitrary waveform bank selection -------------------
        case CC::OSC1_ARB_BANK: {
            // Map CC value (0..127) evenly across number of banks
            const uint8_t numBanks = static_cast<uint8_t>(ArbBank::BwTri) + 1;
            uint8_t bankIdx = (static_cast<uint16_t>(value) * numBanks) / 128;
            if (bankIdx >= numBanks) bankIdx = numBanks - 1;
            ArbBank bank = static_cast<ArbBank>(bankIdx);
            setOsc1ArbBank(bank);
            JT_LOGF("[CC %u:%s] OSC1 Bank -> %s (%u)\n", control, ccName, akwf_bankName(bank), bankIdx);
        } break;

        case CC::OSC2_ARB_BANK: {
            const uint8_t numBanks = static_cast<uint8_t>(ArbBank::BwTri) + 1;
            uint8_t bankIdx = (static_cast<uint16_t>(value) * numBanks) / 128;
            if (bankIdx >= numBanks) bankIdx = numBanks - 1;
            ArbBank bank = static_cast<ArbBank>(bankIdx);
            setOsc2ArbBank(bank);
            JT_LOGF("[CC %u:%s] OSC2 Bank -> %s (%u)\n", control, ccName, akwf_bankName(bank), bankIdx);
        } break;

        // ------------------- Arbitrary waveform table index ----------------------
        case CC::OSC1_ARB_INDEX: {
            uint16_t count = akwf_bankCount(_osc1ArbBank);
            uint16_t idx = 0;
            if (count > 0) {
                idx = (static_cast<uint16_t>(value) * count) / 128;
                if (idx >= count) idx = count - 1;
            }
            setOsc1ArbIndex(idx);
            JT_LOGF("[CC %u:%s] OSC1 Table -> %u/%u\n", control, ccName, idx, count);
        } break;

        case CC::OSC2_ARB_INDEX: {
            uint16_t count = akwf_bankCount(_osc2ArbBank);
            uint16_t idx = 0;
            if (count > 0) {
                idx = (static_cast<uint16_t>(value) * count) / 128;
                if (idx >= count) idx = count - 1;
            }
            setOsc2ArbIndex(idx);
            JT_LOGF("[CC %u:%s] OSC2 Table -> %u/%u\n", control, ccName, idx, count);
        } break;

        // ------------------- Glide -------------------
        case CC::GLIDE_ENABLE: {
            _glideEnabled = (value >= 1);
            for (int i=0; i<MAX_VOICES; ++i) _voices[i].setGlideEnabled(_glideEnabled);
            JT_LOGF("[CC %u:%s] Glide Enabled = %d\n", control, ccName, (int)_glideEnabled);
        } break;

        case CC::GLIDE_TIME: {
            float ms = CCtoTime(value);
            _glideTimeMs = ms;
            for (int i=0; i<MAX_VOICES; ++i) _voices[i].setGlideTime(ms);
            JT_LOGF("[CC %u:%s] Glide Time = %.2f ms\n", control, ccName, ms);
        } break;

        //AMP_MOD_FIXED_LEVEL
        case CC::AMP_MOD_FIXED_LEVEL: { SetAmpModFixedLevel(norm); JT_LOGF("[CC %u:%s] Amp mod fixed level = %.3f\n", control, ccName, norm); } break;

case CC::BPM_CLOCK_SOURCE: {
    // 0-63 = Internal, 64-127 = External
    bool useExternal = (value >= 64);
    if (_bpmClock) {
        _bpmClock->setClockSource(useExternal ? 
            ClockSource::CLOCK_EXTERNAL_MIDI : 
            ClockSource::CLOCK_INTERNAL);
        JT_LOGF("[CC %u:%s] Clock Source = %s\n", 
                control, ccName, useExternal ? "EXTERNAL" : "INTERNAL");
    }
    
    break;
}

case CC::BPM_INTERNAL_TEMPO: {
    // 0-127 → 40-300 BPM
    float bpm = 40.0f + (value / 127.0f) * (300.0f - 40.0f);
    if (_bpmClock) {
        _bpmClock->setInternalBPM(bpm);
        JT_LOGF("[CC %u:%s] Internal BPM = %.1f\n", control, ccName, bpm);
    }
    break;
}

case CC::LFO1_TIMING_MODE: {
    // Map value 0-127 to 12 timing modes
    TimingMode mode = TimingMode::TIMING_FREE;
    if (value >= 0 && value <= 10)       mode = TimingMode::TIMING_FREE;
    else if (value >= 11 && value <= 21) mode = TimingMode::TIMING_4_BARS;
    else if (value >= 22 && value <= 32) mode = TimingMode::TIMING_2_BARS;
    else if (value >= 33 && value <= 43) mode = TimingMode::TIMING_1_BAR;
    else if (value >= 44 && value <= 54) mode = TimingMode::TIMING_1_2;
    else if (value >= 55 && value <= 65) mode = TimingMode::TIMING_1_4;
    else if (value >= 66 && value <= 76) mode = TimingMode::TIMING_1_8;
    else if (value >= 77 && value <= 87) mode = TimingMode::TIMING_1_16;
    else if (value >= 88 && value <= 98) mode = TimingMode::TIMING_1_32;
    else if (value >= 99 && value <= 109) mode = TimingMode::TIMING_1_4T;
    else if (value >= 110 && value <= 120) mode = TimingMode::TIMING_1_8T;
    else if (value >= 121 && value <= 127) mode = TimingMode::TIMING_1_16T;
    
    setLFO1TimingMode(mode);
    JT_LOGF("[CC %u:%s] LFO1 Timing = %s\n", 
            control, ccName, TimingModeNames[(int)mode]);
    break;
}

case CC::LFO2_TIMING_MODE: {
    // Same mapping as LFO1
    TimingMode mode = TimingMode::TIMING_FREE;
    if (value >= 0 && value <= 10)       mode = TimingMode::TIMING_FREE;
    else if (value >= 11 && value <= 21) mode = TimingMode::TIMING_4_BARS;
    else if (value >= 22 && value <= 32) mode = TimingMode::TIMING_2_BARS;
    else if (value >= 33 && value <= 43) mode = TimingMode::TIMING_1_BAR;
    else if (value >= 44 && value <= 54) mode = TimingMode::TIMING_1_2;
    else if (value >= 55 && value <= 65) mode = TimingMode::TIMING_1_4;
    else if (value >= 66 && value <= 76) mode = TimingMode::TIMING_1_8;
    else if (value >= 77 && value <= 87) mode = TimingMode::TIMING_1_16;
    else if (value >= 88 && value <= 98) mode = TimingMode::TIMING_1_32;
    else if (value >= 99 && value <= 109) mode = TimingMode::TIMING_1_4T;
    else if (value >= 110 && value <= 120) mode = TimingMode::TIMING_1_8T;
    else if (value >= 121 && value <= 127) mode = TimingMode::TIMING_1_16T;
    
    setLFO2TimingMode(mode);
    JT_LOGF("[CC %u:%s] LFO2 Timing = %s\n", 
            control, ccName, TimingModeNames[(int)mode]);
    break;
}

case CC::DELAY_TIMING_MODE: {
    // Same mapping as LFOs
    TimingMode mode = TimingMode::TIMING_FREE;
    if (value >= 0 && value <= 10)       mode = TimingMode::TIMING_FREE;
    else if (value >= 11 && value <= 21) mode = TimingMode::TIMING_4_BARS;
    else if (value >= 22 && value <= 32) mode = TimingMode::TIMING_2_BARS;
    else if (value >= 33 && value <= 43) mode = TimingMode::TIMING_1_BAR;
    else if (value >= 44 && value <= 54) mode = TimingMode::TIMING_1_2;
    else if (value >= 55 && value <= 65) mode = TimingMode::TIMING_1_4;
    else if (value >= 66 && value <= 76) mode = TimingMode::TIMING_1_8;
    else if (value >= 77 && value <= 87) mode = TimingMode::TIMING_1_16;
    else if (value >= 88 && value <= 98) mode = TimingMode::TIMING_1_32;
    else if (value >= 99 && value <= 109) mode = TimingMode::TIMING_1_4T;
    else if (value >= 110 && value <= 120) mode = TimingMode::TIMING_1_8T;
    else if (value >= 121 && value <= 127) mode = TimingMode::TIMING_1_16T;
    
    setDelayTimingMode(mode);
    JT_LOGF("[CC %u:%s] Delay Timing = %s\n", 
            control, ccName, TimingModeNames[(int)mode]);
    break;
}



        // =================== NEW: LFO per-destination depths ===================
        // Each CC maps to a 0..1 depth for a specific LFO→destination lane.
        // Final mixer gain = masterAmount * depthScalar.

        case CC::LFO1_PITCH_DEPTH:  { setLFO1PitchDepth(norm);  JT_LOGF("[CC %u] LFO1 Pitch depth %.3f\n",  control, norm); } break;
        case CC::LFO1_FILTER_DEPTH: { setLFO1FilterDepth(norm); JT_LOGF("[CC %u] LFO1 Filter depth %.3f\n", control, norm); } break;
        case CC::LFO1_PWM_DEPTH:    { setLFO1PWMDepth(norm);    JT_LOGF("[CC %u] LFO1 PWM depth %.3f\n",    control, norm); } break;
        case CC::LFO1_AMP_DEPTH:    { setLFO1AmpDepth(norm);    JT_LOGF("[CC %u] LFO1 Amp depth %.3f\n",    control, norm); } break;
        case CC::LFO1_DELAY:
        {   // CC 0-127 → 0-4000 ms delay before LFO reaches full depth
            const float ms = norm * 4000.0f;
            setLFO1Delay(ms);
            JT_LOGF("[CC %u] LFO1 Delay %.0f ms\n", control, ms);
        } break;

        case CC::LFO2_PITCH_DEPTH:  { setLFO2PitchDepth(norm);  JT_LOGF("[CC %u] LFO2 Pitch depth %.3f\n",  control, norm); } break;
        case CC::LFO2_FILTER_DEPTH: { setLFO2FilterDepth(norm); JT_LOGF("[CC %u] LFO2 Filter depth %.3f\n", control, norm); } break;
        case CC::LFO2_PWM_DEPTH:    { setLFO2PWMDepth(norm);    JT_LOGF("[CC %u] LFO2 PWM depth %.3f\n",    control, norm); } break;
        case CC::LFO2_AMP_DEPTH:    { setLFO2AmpDepth(norm);    JT_LOGF("[CC %u] LFO2 Amp depth %.3f\n",    control, norm); } break;
        case CC::LFO2_DELAY:
        {   const float ms = norm * 4000.0f;
            setLFO2Delay(ms);
            JT_LOGF("[CC %u] LFO2 Delay %.0f ms\n", control, ms);
        } break;

        // =================== NEW: Pitch envelope ===================
        // ADSR times share the same cc_to_time_ms() mapping as amp/filter envs.
        // DEPTH is bipolar: CC64 = 0 semitones; 0 = -24; 127 = +24.

        case CC::PITCH_ENV_ATTACK:  { setPitchEnvAttack(CCtoTime(value));  JT_LOGF("[CC %u] PEnv Attack %.1f ms\n",  control, CCtoTime(value)); } break;
        case CC::PITCH_ENV_DECAY:   { setPitchEnvDecay(CCtoTime(value));   JT_LOGF("[CC %u] PEnv Decay %.1f ms\n",   control, CCtoTime(value)); } break;
        case CC::PITCH_ENV_SUSTAIN: { setPitchEnvSustain(norm);            JT_LOGF("[CC %u] PEnv Sustain %.3f\n",    control, norm);            } break;
        case CC::PITCH_ENV_RELEASE: { setPitchEnvRelease(CCtoTime(value)); JT_LOGF("[CC %u] PEnv Release %.1f ms\n", control, CCtoTime(value)); } break;
        case CC::PITCH_ENV_DEPTH:
        {   // Bipolar: CC 64 = 0 semis, 0 = -24, 127 = +24
            const float semis = ((float)value - 64.0f) * (24.0f / 64.0f);
            setPitchEnvDepth(semis);
            JT_LOGF("[CC %u] PEnv Depth %.1f semitones\n", control, semis);
        } break;

        // =================== NEW: Velocity sensitivity ===================
        case CC::VELOCITY_AMP_SENS:    { setVelocityAmpSens(norm);    JT_LOGF("[CC %u] Vel Amp Sens %.3f\n",    control, norm); } break;
        case CC::VELOCITY_FILTER_SENS: { setVelocityFilterSens(norm); JT_LOGF("[CC %u] Vel Filter Sens %.3f\n", control, norm); } break;
        case CC::VELOCITY_ENV_SENS:    { setVelocityEnvSens(norm);    JT_LOGF("[CC %u] Vel Env Sens %.3f\n",    control, norm); } break;

        // PITCH_BEND_RANGE: CC 0..127 → 0..PITCH_BEND_MAX_SEMITONES (24).
        // Default = 2 semitones (standard MIDI keyboard).
        // Setting to 12 gives a full-octave wheel; 24 gives 2-octave.
        case CC::PITCH_BEND_RANGE: {
            const float rangeSemis = norm * PITCH_BEND_MAX_SEMITONES;
            setPitchBendRange(rangeSemis);
            JT_LOGF("[CC %u:%s] Bend range = ±%.1f semitones\n", control, ccName, rangeSemis);
        } break;

        // ------------------- Poly / Mono / Unison -------------------
        // CC 0..42   = POLY,  43..84 = MONO,  85..127 = UNISON
        case CC::POLY_MODE: {
            PolyMode pm = (value <= 42) ? PolyMode::POLY :
                          (value <= 84) ? PolyMode::MONO : PolyMode::UNISON;
            setPolyMode(pm);
            JT_LOGF("[CC %u:%s] → %s\n", control, ccName,
                pm == PolyMode::POLY ? "POLY" : pm == PolyMode::MONO ? "MONO" : "UNISON");
        } break;

        case CC::UNISON_DETUNE: {
            setUnisonDetune(norm);
            JT_LOGF("[CC %u:%s] Unison detune = %.3f\n", control, ccName, norm);
        } break;

        // ------------------- Drive / Saturation -------------------
        // Maps 0-127 to drive amount. 0=bypass, 1-63=soft clip, 64-127=hard clip.
        // Passed directly to AudioEffectJPFX which interprets the mode internally.
        case CC::FX_DRIVE: {
            const float driveNorm = norm;  // 0..1
            _fxChain.setDrive(driveNorm);
            JT_LOGF("[CC %u:%s] Drive = %.3f\n", control, ccName, driveNorm);
        } break;


 // ─────────────────────────────────────────────────────────────
        // Step Sequencer
        // ─────────────────────────────────────────────────────────────
        case CC::SEQ_ENABLE: {
            _seq1.setEnabled(value >= 64);
            JT_LOGF("[CC %u] Seq enable = %s\n", control, value >= 64 ? "ON" : "OFF");
        } break;

        case CC::SEQ_STEPS: {
            int steps = 1 + (value * 15 / 127);  // 0-127 → 1-16
            _seq1.setStepCount(steps);
            JT_LOGF("[CC %u] Seq steps = %d\n", control, steps);
        } break;

        case CC::SEQ_GATE_LENGTH: {
            _seq1.setGateLength(norm);
            JT_LOGF("[CC %u] Seq gate = %.0f%%\n", control, norm * 100.0f);
        } break;

        case CC::SEQ_SLIDE: {
            _seq1.setSlide(norm);
            JT_LOGF("[CC %u] Seq slide = %.0f%%\n", control, norm * 100.0f);
        } break;

        case CC::SEQ_DIRECTION: {
            int dir = (value * 3) / 127;  // 0-127 → 0-3
            _seq1.setDirection(static_cast<SeqDirection>(dir));
            JT_LOGF("[CC %u] Seq dir = %d (%s)\n", control, dir, SeqDirectionNames[dir]);
        } break;

        case CC::SEQ_RATE: {
            // Exponential mapping: 0.1 Hz to 20 Hz
            float hz = 0.1f * powf(200.0f, norm);
            _seq1.setRate(hz);
            JT_LOGF("[CC %u] Seq rate = %.2f Hz\n", control, hz);
        } break;

        case CC::SEQ_DEPTH: {
            _seq1.setDepth(norm);
            JT_LOGF("[CC %u] Seq depth = %.3f\n", control, norm);
        } break;

        case CC::SEQ_DESTINATION: {
            int dest = (value * 4) / 127;  // 0-127 → 0-4 (None/Pitch/Filter/PWM/Amp)
            dest = constrain(dest, 0, NUM_LFO_DESTS - 1);
            _seqDestination = dest;
            JT_LOGF("[CC %u] Seq dest = %d (%s)\n", control, dest, LFODestNames[dest]);
        } break;

        case CC::SEQ_RETRIGGER: {
            _seq1.setRetrigger(value >= 64);
            JT_LOGF("[CC %u] Seq retrigger = %s\n", control, value >= 64 ? "ON" : "OFF");
        } break;

        case CC::SEQ_STEP_SELECT: {
            _seqSelectedStep = constrain(value, 0, SEQ_MAX_STEPS - 1);
            JT_LOGF("[CC %u] Seq step select = %d\n", control, _seqSelectedStep);
        } break;

        case CC::SEQ_STEP_VALUE: {
            _seq1.setStepValue(_seqSelectedStep, value);
            JT_LOGF("[CC %u] Seq step[%d] = %d\n", control, _seqSelectedStep, value);
        } break;

        case CC::SEQ_TIMING_MODE: {
            int mode = constrain(value, 0, NUM_TIMING_MODES - 1);
            _seq1.setTimingMode(static_cast<TimingMode>(mode));
            JT_LOGF("[CC %u] Seq timing = %d (%s)\n", control, mode, TimingModeNames[mode]);
        } break;       

        // ------------------- Fallback -------------------
        default:
            JT_LOGF("[CC %u:%s] Unmapped value=%u\n", control, ccName, value);
            break;
    }

    // Keep raw CC cache in sync — lets the UI read back any value via getCC().
    // POLY_MODE(128) and UNISON_DETUNE(129) are handled by dedicated backing
    // fields; getCC() encodes them on demand. Do not write _ccState for those.
    // FX_DRIVE(130) and future internal CCs above 127 use _ccState directly.
    if (control < 128 || (control >= 130 && control < 160)) {
        _ccState[control] = value;
    }
}