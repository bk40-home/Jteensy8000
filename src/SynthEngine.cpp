// SynthEngine.cpp
#include "SynthEngine.h"
#include "VoicePool.h"
#include "Mapping.h"
#include "ParamDefs.h"
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
    : _fxChain()
{
    // =========================================================================
    // Construction is deliberately trivial: _audio is a direct member so its
    // AudioStream children (LFOs, mixers, step seq, amp-mod chain) register
    // themselves with the Teensy Audio Library during their own constructors.
    // _fxChain does the same for its internal audio objects.
    //
    // NO audio connections are created here — AudioMemory() has not run yet
    // when this executes (SynthEngine is a global), so any AudioConnection
    // built at this point would be silently broken. All wiring happens in
    // begin() which must be called from setup() after AudioMemory().
    //
    // Voice ownership: this engine does NOT own a VoiceBlock[] array. A
    // single shared VoicePool is owned by LayerManager. setVoicePool() must
    // be called before begin() so _voices points at the pool's data().
    // =========================================================================

    // Voice bookkeeping — sized for the full pool so the engine can safely
    // use absolute voice indices across its range.
    for (int i = 0; i < MAX_VOICES; i++) {
        _gateOpen[i]       = false;
        _noteTimestamps[i] = 0;
    }
    for (int i = 0; i < 128; i++) {
        _noteToVoice[i] = VOICE_NONE;
    }
}

SynthEngine::~SynthEngine() {
    // Nothing to free: _audio is a direct member, _fxChain is a direct member,
    // and _voices points into a VoicePool owned by LayerManager. Audio
    // connections created in begin() are owned by the _audio struct and
    // would need explicit cleanup — but SynthEngine instances are globals
    // that live for the whole program, so we skip that here. If instance
    // lifetime ever becomes dynamic, add: for each AudioConnection* in
    // _audio, delete it; then delete the pitch-env connections held inside
    // each VoiceBlock. For now this destructor is a no-op.
    _voices = nullptr;
}

// ---------------------------------------------------------------------------
// setVoicePool() — MUST be called before begin().
// ---------------------------------------------------------------------------
void SynthEngine::setVoicePool(VoicePool* pool) {
    if (pool) {
        _voices = pool->data();
    } else {
        _voices = nullptr;
    }
}



// This replaces the old flat scan of _activeNotes[] which treated "gate closed"
// and "envelope idle" as the same thing, causing release tails to be cut and
// releasing voices to be treated as equivalent to truly silent ones.

int SynthEngine::_findFreeVoice() {
    // Engines with a 0-voice range (e.g. Layer B in SINGLE mode) cannot
    // allocate — caller must handle VOICE_NONE. Without this guard, the
    // steal-loop below would return _firstVoice (a voice that may belong
    // to a different engine) and we'd trample it.
    if (_voiceCount == 0) return VOICE_NONE;

    const uint8_t last = _firstVoice + _voiceCount;

    // --- Priority 1: Find a truly idle voice (envelope finished) ---
    for (uint8_t i = _firstVoice; i < last; ++i) {
        if (!_gateOpen[i] && !_voices[i].isAudioActive()) {
            return i;
        }
    }

    // --- Priority 2: Find the oldest releasing voice (gate closed, still sounding) ---
    int oldestReleasing = -1;
    uint32_t oldestRelTime = UINT32_MAX;
    for (uint8_t i = _firstVoice; i < last; ++i) {
        if (!_gateOpen[i] && _noteTimestamps[i] < oldestRelTime) {
            oldestReleasing = i;
            oldestRelTime   = _noteTimestamps[i];
        }
    }
    if (oldestReleasing >= 0) return oldestReleasing;

    // --- Priority 3: Steal the oldest held note (gate still open) ---
    int oldest = _firstVoice;
    for (uint8_t i = _firstVoice + 1; i < last; ++i) {
        if (_noteTimestamps[i] < _noteTimestamps[oldest]) oldest = i;
    }
    return oldest;
}

// =============================================================================
// begin() — call from setup() AFTER AudioMemory() AND AFTER setVoicePool()
//
// Creates every AudioConnection in the engine graph. Connections are wired
// to ALL MAX_VOICES voices regardless of the engine's current voice range;
// range gating is done by per-voice mixer slot gain (Option R3). This lets
// setVoiceRange() change the range at runtime without touching connections.
//
// After wiring, every gated mixer slot is explicitly zeroed. The depth-apply
// helpers (_applyLFO*Gains, _applySeqOutput, _applyVoiceRangeGains) will set
// correct gains when the engine's patch state is populated.
// =============================================================================
void SynthEngine::begin()
{
    // Voice pool must be set before begin(). If LayerManager forgets to
    // call setVoicePool, fail loud: no connections are wired, no audio
    // produced, and a diagnostic goes to the serial log.
    if (!_voices) {
        JT_LOGF("[SynthEngine] begin() called with no VoicePool — audio graph NOT wired\n");
        return;
    }

    // =========================================================================
    // MIXER GAINS — simple register writes with no memory dependency
    // =========================================================================

    // Amp mod DC level and limiter
    _audio.ampModFixedDc.amplitude(_patch.ampModFixedLevel);
    _audio.ampModLimitFixedDc.amplitude(1.0f);

    // Amp mod signal mixer: slot 0 = fixed DC, 1/2 = LFO tremolo, 3 = seq
    _audio.ampModMixer.gain(0, 1.0f);
    _audio.ampModMixer.gain(1, 0.0f);
    _audio.ampModMixer.gain(2, 0.0f);
    _audio.ampModMixer.gain(3, 0.0f);

    // Step sequencer DC — fixed amplitude 1.0, value carried by mixer gain
    _audio.seqDc.amplitude(1.0f);

    _audio.ampModLimiterMixer.gain(0, 1.0f);
    _audio.ampModLimiterMixer.gain(1, 0.0f);
    _audio.ampModLimiterMixer.gain(2, 0.0f);
    _audio.ampModLimiterMixer.gain(3, 0.0f);

    // Voice sub-mixer gains — zeroed here and set by _applyVoiceRangeGains().
    // Each engine wires voices 0..3 to voiceMixerA and 4..7 to voiceMixerB,
    // but only in-range voices get a non-zero mixer slot gain, so the two
    // engines' sub-mixers never double-count a voice's audio.
    for (int i = 0; i < 4; i++) {
        _audio.voiceMixerA.gain(i, 0.0f);
        _audio.voiceMixerB.gain(i, 0.0f);
    }

    // Final mixer makeup gain.
    //
    // Headroom is now budgeted at the SUB-mixer stage (_applyVoiceRangeGains
    // sets each in-range voice's slot to 0.25, so 4 voices in phase = ±1.0
    // max at voiceMixerA/B output). The final mixer therefore only needs
    // enough headroom to sum two sub-mixers each at ±1.0 → 0.5 each.
    //
    // Previously 0.1 here gave global headroom but did NOT protect the
    // sub-mixer's internal int16 saturation: with slot gain 1.0 at A/B,
    // 3+ voices in phase clipped at the sub-mixer output before the 0.1
    // attenuated the (already-clipped) signal — audible distortion with
    // a clean-looking output meter.
    _audio.voiceMixerFinal.gain(0, 0.5f);  // Sub-mixer A (voices 0–3)
    _audio.voiceMixerFinal.gain(1, 0.5f);  // Sub-mixer B (voices 4–7)
    _audio.voiceMixerFinal.gain(2, 0.0f);  // Unused
    _audio.voiceMixerFinal.gain(3, 0.0f);  // Unused

    // =========================================================================
    // AUDIO CONNECTIONS — VOICES → VOICE MIXERS
    // Wired for ALL 8 voices; per-voice mixer slot gain does the range gating.
    // =========================================================================

    for (int i = 0; i < 4; i++) {
        _audio.voicePatch[i] = new AudioConnection(_voices[i].output(), 0, _audio.voiceMixerA, i);
    }
    for (int i = 4; i < MAX_VOICES; i++) {
        _audio.voicePatch[i] = new AudioConnection(_voices[i].output(), 0, _audio.voiceMixerB, i - 4);
    }

    _audio.patchMixerAToFinal = new AudioConnection(_audio.voiceMixerA, 0, _audio.voiceMixerFinal, 0);
    _audio.patchMixerBToFinal = new AudioConnection(_audio.voiceMixerB, 0, _audio.voiceMixerFinal, 1);

    // =========================================================================
    // AUDIO CONNECTIONS — LFO → ALL VOICES
    //
    // Both engines wire their own LFOs to all 8 voices. Each voice's shape/
    // freq/filter mixer slots therefore have connections from TWO LFO
    // sources (one per engine). Only one source is audible at a time because
    // the per-voice mixer slot gains are set to the owning engine's depth
    // and zeroed by the other engine.
    // =========================================================================

    for (int i = 0; i < MAX_VOICES; i++) {
        // LFO1: shape mod (PWM), frequency mod (vibrato), filter mod (wah)
        _audio.voicePatchLFO1ShapeOsc1[i]     = new AudioConnection(_audio.lfo1.output(), 0, _voices[i].shapeModMixerOsc1(),     1);
        _audio.voicePatchLFO1ShapeOsc2[i]     = new AudioConnection(_audio.lfo1.output(), 0, _voices[i].shapeModMixerOsc2(),     1);
        _audio.voicePatchLFO1FrequencyOsc1[i] = new AudioConnection(_audio.lfo1.output(), 0, _voices[i].frequencyModMixerOsc1(), 1);
        _audio.voicePatchLFO1FrequencyOsc2[i] = new AudioConnection(_audio.lfo1.output(), 0, _voices[i].frequencyModMixerOsc2(), 1);
        _audio.voicePatchLFO1Filter[i]        = new AudioConnection(_audio.lfo1.output(), 0, _voices[i].filterModMixer(),         2);

        // LFO2: same destinations, separate depth control
        _audio.voicePatchLFO2ShapeOsc1[i]     = new AudioConnection(_audio.lfo2.output(), 0, _voices[i].shapeModMixerOsc1(),     2);
        _audio.voicePatchLFO2ShapeOsc2[i]     = new AudioConnection(_audio.lfo2.output(), 0, _voices[i].shapeModMixerOsc2(),     2);
        _audio.voicePatchLFO2FrequencyOsc1[i] = new AudioConnection(_audio.lfo2.output(), 0, _voices[i].frequencyModMixerOsc1(), 2);
        _audio.voicePatchLFO2FrequencyOsc2[i] = new AudioConnection(_audio.lfo2.output(), 0, _voices[i].frequencyModMixerOsc2(), 2);
        _audio.voicePatchLFO2Filter[i]        = new AudioConnection(_audio.lfo2.output(), 0, _voices[i].filterModMixer(),         3);
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
    // FM_SEMITONE_SCALE produces the correct Hz shift. Mixer gain stays 1.0.
    //
    // Pitch envelope is INTERNAL to a voice (voice N's env → voice N's pitch),
    // so it needs no range gating. Both engines wiring it for all 8 voices
    // is fine because voice N's env only outputs when voice N plays, and
    // only one engine will noteOn() any given voice at a time.
    //
    // However, we DO skip re-wiring a voice's pitch-env if it's already set
    // (Engine A may have already wired it). This prevents leaking a second
    // AudioConnection pointer on the second engine.
    // =========================================================================

    for (int i = 0; i < MAX_VOICES; ++i) {
        if (_voices[i]._pitchEnvPatch1 == nullptr) {
            _voices[i]._pitchEnvPatch1 = new AudioConnection(
                _voices[i].pitchEnvOutput(), 0, _voices[i].frequencyModMixerOsc1(), 3);
        }
        if (_voices[i]._pitchEnvPatch2 == nullptr) {
            _voices[i]._pitchEnvPatch2 = new AudioConnection(
                _voices[i].pitchEnvOutput(), 0, _voices[i].frequencyModMixerOsc2(), 3);
        }

        _voices[i].frequencyModMixerOsc1().gain(3, 1.0f);
        _voices[i].frequencyModMixerOsc2().gain(3, 1.0f);
    }

    // =========================================================================
    // AUDIO CONNECTIONS — AMP MOD MULTIPLY → FX CHAIN
    //
    // Signal flow:
    //   _audio.ampModMixer (DC + LFO tremolo)
    //     ↘
    //      _audio.ampMultiply  ← _audio.voiceMixerFinal (all voices summed)
    //     ↓
    //   _fxChain (reverb/chorus/delay)
    //     ↓
    //   getFXOutL() / getFXOutR()  → I2S DAC
    // =========================================================================

    _audio.patchAmpModFixedDcToAmpModMixer = new AudioConnection(_audio.ampModFixedDc,    0, _audio.ampModMixer,   0);
    _audio.patchLFO1ToAmpModMixer          = new AudioConnection(_audio.lfo1.output(),    0, _audio.ampModMixer,   1);
    _audio.patchLFO2ToAmpModMixer          = new AudioConnection(_audio.lfo2.output(),    0, _audio.ampModMixer,   2);
    _audio.patchAmpModMixerToAmpMultiply   = new AudioConnection(_audio.ampModMixer,      0, _audio.ampMultiply,   0);
    _audio.patchVoiceMixerToAmpMultiply    = new AudioConnection(_audio.voiceMixerFinal,  0, _audio.ampMultiply,   1);

    // Step sequencer → per-voice shape (PWM) mixer slot 3.
    // Gated via shape-mixer slot 3 gain in _applySeqOutput(); zeroed below.
    for (int i = 0; i < MAX_VOICES; ++i) {
        _audio.patchSeqDcToShapeOsc1[i] = new AudioConnection(
            _audio.seqDc, 0, _voices[i].shapeModMixerOsc1(), 3);
        _audio.patchSeqDcToShapeOsc2[i] = new AudioConnection(
            _audio.seqDc, 0, _voices[i].shapeModMixerOsc2(), 3);
    }
    // Step sequencer → amp mod mixer slot 3
    _audio.patchSeqDcToAmpModMixer = new AudioConnection(_audio.seqDc, 0, _audio.ampModMixer, 3);

    // Wet path: amp → JPFX mono input (JPFX fans mono→stereo internally).
    // _fxChain.getJPFX() returns AudioEffectJPFX&, which upcasts implicitly
    // to the AudioStream& the AudioConnection ctor expects.
    _audio.fxPatchInL  = new AudioConnection(_audio.ampMultiply, 0, _fxChain.getJPFX(),   0);
    // NOTE: _fxPatchInR removed — JPFX is AudioStream(1, ...) with only
    // input 0.  Input 1 does not exist; the connection was silently ignored
    // by the Teensy Audio Library.  JPFX creates stereo spread internally
    // via its modulation and delay processing stages.

    // Dry path: amp → output mixers directly (bypass FX)
    _audio.fxPatchDryL = new AudioConnection(_audio.ampMultiply, 0, _fxChain.getOutputLeft(),  0);
    _audio.fxPatchDryR = new AudioConnection(_audio.ampMultiply, 0, _fxChain.getOutputRight(), 0);

    // =========================================================================
    // INITIAL GAIN STATE — zero every gated mixer slot on every voice so this
    // engine contributes nothing before its patch state is applied.
    // Slots zeroed here:
    //   shapeModMixerOsc1/Osc2 slots 1, 2, 3  (LFO1 PWM, LFO2 PWM, seq PWM)
    //   frequencyModMixerOsc1/Osc2 slots 1, 2 (LFO1 pitch, LFO2 pitch)
    //   filterModMixer slots 2, 3             (LFO1 filter, LFO2 filter)
    // The pitch-env slot (3) on freqMod stays at 1.0 (voice-internal).
    //
    // After this, _applyVoiceRangeGains() lights up the in-range voices to
    // the depths set in _patch (all zero at boot — correct for silent start).
    // =========================================================================
    for (uint8_t i = 0; i < MAX_VOICES; ++i) {
        _voices[i].shapeModMixerOsc1().gain(1, 0.0f);
        _voices[i].shapeModMixerOsc1().gain(2, 0.0f);
        _voices[i].shapeModMixerOsc1().gain(3, 0.0f);
        _voices[i].shapeModMixerOsc2().gain(1, 0.0f);
        _voices[i].shapeModMixerOsc2().gain(2, 0.0f);
        _voices[i].shapeModMixerOsc2().gain(3, 0.0f);
        _voices[i].frequencyModMixerOsc1().gain(1, 0.0f);
        _voices[i].frequencyModMixerOsc1().gain(2, 0.0f);
        _voices[i].frequencyModMixerOsc2().gain(1, 0.0f);
        _voices[i].frequencyModMixerOsc2().gain(2, 0.0f);
        _voices[i].filterModMixer().gain(2, 0.0f);
        _voices[i].filterModMixer().gain(3, 0.0f);

        // Initial pitch bend application — safe across all 8 voices
        _voices[i].setOsc1PitchBend(_patch.pitchBendSemis);
        _voices[i].setOsc2PitchBend(_patch.pitchBendSemis);
    }

    // Apply current range to voice-mixer A/B slot gains (0 or 1 per voice).
    _applyVoiceRangeGains();
}


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
    TimingMode lfo1Mode = _audio.lfo1.getTimingMode();
    if (lfo1Mode != TimingMode::TIMING_FREE) {
        float hz = _bpmClock->getFrequencyForMode(lfo1Mode);
        _audio.lfo1.setFrequency(hz);
    }

    _audio.seq1.updateFromBPMClock(*_bpmClock);
    
    // Update LFO2 if synced
    TimingMode lfo2Mode = _audio.lfo2.getTimingMode();
    if (lfo2Mode != TimingMode::TIMING_FREE) {
        float hz = _bpmClock->getFrequencyForMode(lfo2Mode);
        _audio.lfo2.setFrequency(hz);
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
    _audio.lfo1.setTimingMode(mode);
    
    if (mode == TimingMode::TIMING_FREE) {
        // Restore manual frequency control
        _audio.lfo1.setFrequency(_patch.lfo1Frequency);
    } else if (_bpmClock) {
        // Lock to BPM
        float hz = _bpmClock->getFrequencyForMode(mode);
        _audio.lfo1.setFrequency(hz);
    }
}

void SynthEngine::setLFO2TimingMode(TimingMode mode) {
    _audio.lfo2.setTimingMode(mode);
    
    if (mode == TimingMode::TIMING_FREE) {
        // Restore manual frequency control
        _audio.lfo2.setFrequency(_patch.lfo2Frequency);
    } else if (_bpmClock) {
        // Lock to BPM
        float hz = _bpmClock->getFrequencyForMode(mode);
        _audio.lfo2.setFrequency(hz);
    }
}

TimingMode SynthEngine::getLFO1TimingMode() const {
    return _audio.lfo1.getTimingMode();
}

TimingMode SynthEngine::getLFO2TimingMode() const {
    return _audio.lfo2.getTimingMode();
}

// ============================================================================
// DELAY TIMING MODE CONTROLS
// ============================================================================

void SynthEngine::setDelayTimingMode(TimingMode mode) {
    _fxChain.setDelayTimingMode(mode);
    
    if (mode == TimingMode::TIMING_FREE) {
        // Restore manual delay time control
        _fxChain.setDelayTime(_patch.fxDelayTime);
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
    // TEMP diagnostic — Phase 3, SINGLE-mode fan-out investigation.
    // Prints one line per incoming note with the three quantities that
    // distinguish the candidate causes (polyMode / voice-range). Revert
    // after the bug is diagnosed.
    JT_LOGF("[noteOn] note=%u polyMode=%u firstV=%u count=%u\n",
            (unsigned)note, (unsigned)_patch.polyMode,
            (unsigned)_firstVoice, (unsigned)_voiceCount);

    // Zero-voice engine (e.g. Layer B in SINGLE mode) — silently drop.
    // LayerManager should already have filtered notes to the active engine(s),
    // so hitting this path indicates either a misrouted note or a race during
    // perf-mode transitions. Either way, doing nothing is correct.
    if (_voiceCount == 0) return;

    float freq = 440.0f * powf(2.0f, (note - 69) / 12.0f);

    // Capture the PREVIOUS note's frequency before overwriting — this is
    // the pitch that portamento should slide FROM in poly mode.
    const float prevNoteFreq = _patch.lastNoteFreq;
    _patch.lastNoteFreq = freq;

    // Step sequencer retrigger — _audio is a direct member so no null check.
    if (_audio.seq1.getRetrigger()) _audio.seq1.reset();

    // Restart LFO delay ramps on any noteOn (standard JP-8000 retrigger behaviour)
    if (_patch.lfo1DelayMs > 0.0f) { _lfo1NoteOnMs = millis(); _lfo1Ramping = true; _lfo1CurrentAmp = 0.0f; }
    if (_patch.lfo2DelayMs > 0.0f) { _lfo2NoteOnMs = millis(); _lfo2Ramping = true; _lfo2CurrentAmp = 0.0f; }

    // Limit per-voice amplitude to 0.95 — leaves headroom when multiple
    // voices sound simultaneously.
    static constexpr float MAX_VOICE_VELOCITY = 0.95f;
    if (velocity > MAX_VOICE_VELOCITY) velocity = MAX_VOICE_VELOCITY;

    // =========================================================================
    // MONO mode — single voice, last-note priority with legato note stack.
    // =========================================================================
    if ((PolyMode)_patch.polyMode == PolyMode::MONO) {
        _monoStack.push(note);

        // Clear any stale note→voice mappings — only _firstVoice is used.
        for (int n = 0; n < 128; ++n) {
            if (_noteToVoice[n] != VOICE_NONE) { _noteToVoice[n] = VOICE_NONE; }
        }
        _noteToVoice[note] = _firstVoice;

        _voices[_firstVoice].noteOn(freq, velocity);
        _gateOpen[_firstVoice] = true;
        _noteTimestamps[_firstVoice] = _clock++;
        return;
    }

    // =========================================================================
    // UNISON mode — all owned voices play the same note, detuned
    // =========================================================================
    if ((PolyMode)_patch.polyMode == PolyMode::UNISON) {
        const uint8_t last = _firstVoice + _voiceCount;
        // Release the previous unison note if a different note comes in
        if (_unisonNote >= 0 && _unisonNote != (int)note) {
            for (uint8_t i = _firstVoice; i < last; ++i) {
                _voices[i].noteOff();
                _gateOpen[i] = false;
            }
            _noteToVoice[_unisonNote] = VOICE_NONE;
        }
        _unisonNote = (int)note;
        _noteToVoice[note] = _firstVoice;  // Sentinel: first voice represents the note
        for (uint8_t i = _firstVoice; i < last; ++i) {
            _voices[i].noteOn(freq, velocity);
            _gateOpen[i] = true;
            _noteTimestamps[i] = _clock++;
        }
        return;
    }

    // =========================================================================
    // POLY mode — standard polyphony with envelope-aware voice allocation
    // =========================================================================

    // Retrigger: same note already playing on a voice — reuse that voice.
    // The gate is already open for this note, just retrigger the envelopes.
    if (_noteToVoice[note] != VOICE_NONE) {
        int v = _noteToVoice[note];
        _voices[v].noteOn(freq, velocity);
        _gateOpen[v] = true;   // Ensure gate is marked open (may have been releasing)
        _noteTimestamps[v] = _clock++;
        return;
    }

    // Find the best available voice using three-tier priority:
    //   1. Envelope idle (truly silent)     — zero cost
    //   2. Gate closed, releasing           — cuts release tail
    //   3. Oldest held note                 — voice stealing
    int v = _findFreeVoice();
    if (v == VOICE_NONE) return;  // defensive: can only happen if _voiceCount == 0,
                                  // already filtered at top of function

    // Clear any stale note→voice mapping for the stolen voice
    for (int n = 0; n < 128; ++n) {
        if (_noteToVoice[n] == v) { _noteToVoice[n] = VOICE_NONE; break; }
    }

    // Pre-seed the glide start frequency so portamento slides from the
    // last note the *synth* played, not from whatever stale pitch this
    // voice happens to hold.  Without this, idle/releasing voices glide
    // from their previous (unrelated) note or the 440 Hz default.
    if (_patch.glideEnabled && prevNoteFreq > 20.0f) {
        _voices[v].setGlideFromFreq(prevNoteFreq);
    }

    _voices[v].noteOn(freq, velocity);
    _gateOpen[v] = true;
    _noteToVoice[note] = v;
    _noteTimestamps[v] = _clock++;
}

void SynthEngine::noteOff(byte note) {
    // Zero-voice engine — no state to tear down. Mirror of noteOn guard.
    if (_voiceCount == 0) return;

    // =========================================================================
    // MONO mode — legato note-stack return.
    // =========================================================================
    if ((PolyMode)_patch.polyMode == PolyMode::MONO) {
        _monoStack.remove(note);
        _noteToVoice[note] = VOICE_NONE;

        if (!_monoStack.empty()) {
            // Return to the most recently held key that is still down.
            byte returnNote = _monoStack.top();
            float returnFreq = 440.0f * powf(2.0f, (returnNote - 69) / 12.0f);
            _patch.lastNoteFreq = returnFreq;

            // Clear stale mappings, map the return note to _firstVoice.
            for (int n = 0; n < 128; ++n) _noteToVoice[n] = VOICE_NONE;
            _noteToVoice[returnNote] = _firstVoice;

            _voices[_firstVoice].noteOn(returnFreq, _voices[_firstVoice].getLastVelocity());
            _noteTimestamps[_firstVoice] = _clock++;
        } else {
            _voices[_firstVoice].noteOff();
            _gateOpen[_firstVoice] = false;
        }
        return;
    }

    if (_noteToVoice[note] == VOICE_NONE) return;

    if ((PolyMode)_patch.polyMode == PolyMode::UNISON) {
        // Release all owned voices when the held unison note lifts
        if ((int)note == _unisonNote) {
            const uint8_t last = _firstVoice + _voiceCount;
            for (uint8_t i = _firstVoice; i < last; ++i) {
                _voices[i].noteOff();
                _gateOpen[i] = false;
            }
            _noteToVoice[note] = VOICE_NONE;
            _unisonNote = -1;
        }
        return;
    }

    // POLY: close the gate on the assigned voice, let envelope release naturally.
    int v = _noteToVoice[note];
    _voices[v].noteOff();
    _gateOpen[v] = false;
    _noteToVoice[note] = VOICE_NONE;
    // Voice remains audio-active during release — isVoiceActive() still returns true.
    // The display dot stays lit and the update loop keeps running the voice.
}

void SynthEngine::update() {

// ── Step sequencer tick ─────────────────────────────────────────
  {
      uint32_t now = micros();
      float deltaMs = (now - _audio.lastUpdateMicros) * 0.001f;
      _audio.lastUpdateMicros = now;
      // Clamp to avoid huge jumps on first call or overflow
      if (deltaMs > 50.0f) deltaMs = 50.0f;
      _audio.seq1.tick(deltaMs);
      _applySeqOutput();
  }

    // Update BPM-synced parameters
    if (_bpmClock) {
        updateBPMSync();
    }

    // Update LFO delay ramps (must run every loop iteration)
    _updateLFODelay();

    // Update LFO enabled state
    _audio.lfo1.update();
    _audio.lfo2.update();

    // Update voices that are producing audio — including those in release phase.
    // isVoiceActive() queries the amp envelope hardware, so it returns true
    // during Attack, Decay, Sustain, AND Release.  Only truly idle voices
    // (envelope output at zero) are skipped.
    //
    // This means:
    //   - Glide completes even if noteOff arrives mid-glide
    //   - Display dots stay lit through the release tail
    //   - CPU is freed only when the voice is genuinely silent
    for (uint8_t v = _firstVoice; v < _firstVoice + _voiceCount; v++) {
        if (isVoiceActive(v)) {
            _voices[v].update();
        }
    }
}// ---- Filter / Env ----
void SynthEngine::setFilterCutoff(float value) {
    // Validate range
    value = constrain(value, CUTOFF_MIN_HZ, CUTOFF_MAX_HZ);
    _patch.filterCutoffHz = value;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setFilterCutoff(value);
}
void SynthEngine::setFilterResonance(float value) {
    _patch.filterResonance = value;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setFilterResonance(value);
}
void SynthEngine::setFilterEnvAmount(float amt) {
    _patch.filterEnvAmount = amt;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setFilterEnvAmount(amt);
}
void SynthEngine::setFilterKeyTrackAmount(float amt) {
    _patch.filterKeyTrack = amt;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setFilterKeyTrackAmount(amt);
}
void SynthEngine::setFilterOctaveControl(float octaves) {
    _patch.filterOctaves = octaves;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setFilterOctaveControl(octaves);
}

void SynthEngine::setFilterMultimode(float amount) {
    _patch.filterMultimode = amount;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setMultimode(amount);
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

    _patch.filterMode = mode;

    // Decode into the bool flags that FilterBlock/AudioFilterOBXa consume.
    // All flags default false; only the active mode sets its flag(s).
    _patch.filterUseTwoPole   = false;
    _patch.filterXpander4Pole = false;
    _patch.filterBpBlend2Pole = false;
    _patch.filterPush2Pole    = false;

    switch (mode) {
        case CC::FILTER_MODE_4POLE:
            // All flags already cleared above — standard 4-pole LP
            break;
        case CC::FILTER_MODE_2POLE:
            _patch.filterUseTwoPole = true;
            break;
        case CC::FILTER_MODE_2POLE_BP:
            _patch.filterUseTwoPole   = true;
            _patch.filterBpBlend2Pole = true;
            break;
        case CC::FILTER_MODE_2POLE_PUSH:
            _patch.filterUseTwoPole = true;
            _patch.filterPush2Pole  = true;
            break;
        case CC::FILTER_MODE_XPANDER:
        case CC::FILTER_MODE_XPANDER_M:
            _patch.filterXpander4Pole = true;
            break;
        default:
            break;
    }

    // Push decoded flags to all voices
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) {
        _voices[i].setTwoPole(_patch.filterUseTwoPole);
        _voices[i].setXpander4Pole(_patch.filterXpander4Pole);
        _voices[i].setBPBlend2Pole(_patch.filterBpBlend2Pole);
        _voices[i].setPush2Pole(_patch.filterPush2Pole);
    }
}

void SynthEngine::setFilterXpanderMode(uint8_t amount) {
    _patch.filterXpanderMode = amount;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setXpanderMode(amount);
}

void SynthEngine::setFilterEngine(uint8_t engine) {
    _patch.filterEngine = engine;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setFilterEngine(engine);
    JT_LOGF("[SE] FilterEngine = %u\n", engine);
}

void SynthEngine::setVAFilterType(uint8_t vaType) {
    // Clamp to valid VAFilterType range
    if (vaType >= (uint8_t)FILTER_COUNT) vaType = (uint8_t)FILTER_SVF_LP;
    _patch.vaFilterType = vaType;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i)
        _voices[i].setVAFilterType((VAFilterType)vaType);
    JT_LOGF("[SE] VAFilterType = %u\n", vaType);
}

void SynthEngine::setVADrive(float amount01) {
    _patch.vaDrive = amount01;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i)
        _voices[i].setVADrive(amount01);
    JT_LOGF("[SE] VADrive = %.3f\n", amount01);
}

void SynthEngine::setVASaturation(uint8_t satType) {
    _patch.vaSat = satType;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i)
        _voices[i].setVASaturation(satType);
    JT_LOGF("[SE] VASat = %u\n", satType);
}

void SynthEngine::setFilterResonanceModDepth(float amount) {
    _patch.filterResonaceModDepth = amount;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setResonanceModDepth(amount);
}

float SynthEngine::getFilterCutoff() const         { return _patch.filterCutoffHz; }
float SynthEngine::getFilterResonance() const      { return _patch.filterResonance; }
float SynthEngine::getFilterEnvAmount() const      { return _patch.filterEnvAmount; }
float SynthEngine::getFilterKeyTrackAmount() const { return _patch.filterKeyTrack; }
float SynthEngine::getFilterOctaveControl() const  { return _patch.filterOctaves; }

// ---- Envelopes (read-through to voices; uses voice 0 as representative) ----
float SynthEngine::getAmpAttack()  const { return _voiceCount ? _voices[_firstVoice].getAmpAttack()  : 0.0f; }
float SynthEngine::getAmpDecay()   const { return _voiceCount ? _voices[_firstVoice].getAmpDecay()   : 0.0f; }
float SynthEngine::getAmpSustain() const { return _voiceCount ? _voices[_firstVoice].getAmpSustain() : 0.0f; }
float SynthEngine::getAmpRelease() const { return _voiceCount ? _voices[_firstVoice].getAmpRelease() : 0.0f; }

float SynthEngine::getFilterEnvAttack()  const { return _voiceCount ? _voices[_firstVoice].getFilterEnvAttack()  : 0.0f; }
float SynthEngine::getFilterEnvDecay()   const { return _voiceCount ? _voices[_firstVoice].getFilterEnvDecay()   : 0.0f; }
float SynthEngine::getFilterEnvSustain() const { return _voiceCount ? _voices[_firstVoice].getFilterEnvSustain() : 0.0f; }
float SynthEngine::getFilterEnvRelease() const { return _voiceCount ? _voices[_firstVoice].getFilterEnvRelease() : 0.0f; }

// ---- Envelope curve shaping setters ----------------------------------------
// SysEx-only path — no CC alias. Received via SysExAdapter::_handleSyxOnlySet.
// Fan-out pattern mirrors the existing ADSR setters above.
// PatchState is updated so BANK_DUMP and GET_PARAM can read the current value.

void SynthEngine::setAmpAttackCurve(float exponent) {
    _patch.ampAttackCurve = exponent;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i)
        _voices[i].setAmpAttackCurve(exponent);
}

void SynthEngine::setAmpDecayCurve(float exponent) {
    _patch.ampDecayCurve = exponent;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i)
        _voices[i].setAmpDecayCurve(exponent);
}

void SynthEngine::setAmpReleaseCurve(float exponent) {
    _patch.ampReleaseCurve = exponent;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i)
        _voices[i].setAmpReleaseCurve(exponent);
}

void SynthEngine::setFilterAttackCurve(float exponent) {
    _patch.filterAttackCurve = exponent;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i)
        _voices[i].setFilterAttackCurve(exponent);
}

void SynthEngine::setFilterDecayCurve(float exponent) {
    _patch.filterDecayCurve = exponent;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i)
        _voices[i].setFilterDecayCurve(exponent);
}

void SynthEngine::setFilterReleaseCurve(float exponent) {
    _patch.filterReleaseCurve = exponent;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i)
        _voices[i].setFilterReleaseCurve(exponent);
}

void SynthEngine::setPitchEnvAttackCurve(float exponent) {
    _patch.pitchAttackCurve = exponent;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i)
        _voices[i].setPitchEnvAttackCurve(exponent);
}

void SynthEngine::setPitchEnvDecayCurve(float exponent) {
    _patch.pitchDecayCurve = exponent;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i)
        _voices[i].setPitchEnvDecayCurve(exponent);
}

void SynthEngine::setPitchEnvReleaseCurve(float exponent) {
    _patch.pitchReleaseCurve = exponent;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i)
        _voices[i].setPitchEnvReleaseCurve(exponent);
}
void SynthEngine::setOscWaveforms(int wave1, int wave2) { setOsc1Waveform(wave1); setOsc2Waveform(wave2); }
void SynthEngine::setOsc1Waveform(int wave) { _patch.osc1Wave = wave; for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setOsc1Waveform(wave); }
void SynthEngine::setOsc2Waveform(int wave) { _patch.osc2Wave = wave; for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setOsc2Waveform(wave); }

void SynthEngine::setOsc1PitchOffset(float semis) { _patch.osc1PitchSemi = semis; for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setOsc1PitchOffset(semis); }
void SynthEngine::setOsc2PitchOffset(float semis) { _patch.osc2PitchSemi = semis; for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setOsc2PitchOffset(semis); }

// ============================================================================
// PITCH BEND
// ============================================================================
// MIDI pitch bend uses a 14-bit value (0..16383, centre = 8192).
// Converted to ±_patch.pitchBendRange semitones and routed to every voice via
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
    _patch.pitchBendRange = semitones;
}

// =============================================================================
// POLY MODE
// =============================================================================

void SynthEngine::setPolyMode(PolyMode mode) {
    if ((PolyMode)_patch.polyMode == mode) return;
    _patch.polyMode = (uint8_t)mode;

    // Kill all owned voices when switching modes to prevent stuck notes.
    const uint8_t last = _firstVoice + _voiceCount;
    for (uint8_t i = _firstVoice; i < last; ++i) {
        _voices[i].noteOff();
        _gateOpen[i] = false;
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
    _patch.unisonDetune = amount;
    if ((PolyMode)_patch.polyMode == PolyMode::UNISON) _applyUnisonDetune();
}

// =============================================================================
// VOICE RANGE — set which slice of the shared voice pool this engine owns.
//
// Uses the Option R3 gating model: audio connections for LFOs, step sequencer
// and voice-mixer inputs are wired to ALL MAX_VOICES voices at begin(). Range
// membership is enforced by per-voice mixer slot gain — in-range voices get
// the engine's configured depth, out-of-range voices get 0.
//
// Behaviour:
//   1. Clamp first/count to the pool size.
//   2. Kill notes on voices that are LEAVING the old range (the new range
//      may still contain some of the old voices — don't kill those).
//   3. Commit _firstVoice / _voiceCount.
//   4. Refresh mixer slot gains via _applyVoiceRangeGains() plus the three
//      depth-apply helpers so both engines' slots end up at the correct
//      in-range-depth / out-of-range-zero state.
// =============================================================================
void SynthEngine::setVoiceRange(uint8_t first, uint8_t count) {
    // Clamp to valid range. count == 0 IS permitted: an engine with zero
    // voices never allocates a note, produces no audio, and its depth-apply
    // helpers write 0.0 to every gated slot (because _voiceInRange() is
    // false for every voice). LayerManager uses (0, 0) on Engine B in
    // SINGLE mode so Engine B does literally no voice-DSP work.
    if (first >= MAX_VOICES)           first = 0;
    if (first + count > MAX_VOICES)    count = MAX_VOICES - first;

    const uint8_t oldFirst = _firstVoice;
    const uint8_t oldCount = _voiceCount;
    const uint8_t newLast  = first    + count;
    const uint8_t oldLast  = oldFirst + oldCount;

    // Kill notes only on voices that are leaving the range. A voice at
    // index v was owned if v ∈ [oldFirst, oldLast) and is no longer owned
    // if v ∉ [first, newLast).
    for (uint8_t v = oldFirst; v < oldLast; ++v) {
        const bool stillOwned = (v >= first) && (v < newLast);
        if (!stillOwned) {
            _voices[v].noteOff();
            _gateOpen[v] = false;
            _noteTimestamps[v] = 0;
        }
    }

    // Clear note→voice mappings that pointed into voices we just released.
    for (int n = 0; n < 128; ++n) {
        const uint8_t v = _noteToVoice[n];
        if (v == VOICE_NONE) continue;
        const bool stillOwned = (v >= first) && (v < newLast);
        if (!stillOwned) _noteToVoice[n] = VOICE_NONE;
    }

    // Mono/unison transient state is safe to clear on range change.
    _unisonNote = -1;
    _monoStack.clear();

    _firstVoice = first;
    _voiceCount = count;

    // Re-apply unison detune if active (spread recalculated for new count).
    if ((PolyMode)_patch.polyMode == PolyMode::UNISON) _applyUnisonDetune();

    // Refresh every gated mixer slot: voice-mixer A/B inputs first, then
    // the three modulation sources (LFO1, LFO2, step seq). Each helper
    // knows to write depth for in-range voices and 0 for out-of-range.
    _applyVoiceRangeGains();
    _applyLFO1Gains();
    _applyLFO2Gains();
    _applySeqOutput();

    JT_LOGF("[SynthEngine] VoiceRange → first=%u count=%u\n",
            (unsigned)_firstVoice, (unsigned)_voiceCount);
}

// =============================================================================
// _applyVoiceRangeGains() — set voiceMixer A/B slot gains from current range.
//
// Voice-mixer slot gain acts as the engine's "own or disown" gate for that
// voice. Voices 0..3 route into voiceMixerA slot 0..3; voices 4..7 route
// into voiceMixerB slot 0..3. The owning engine sets that slot to 1.0;
// the non-owning engine sets it to 0.0. With both engines wired to all 8
// voices, the slot-gain gate is what stops a voice's audio appearing in
// both engines' outputs.
//
// Does NOT touch LFO / seq gains — those live in the per-voice mod mixers
// and are set by _applyLFO*Gains / _applySeqOutput which each walk all 8
// voices with their own range-aware depth math.
// =============================================================================
void SynthEngine::_applyVoiceRangeGains() {
    // Per-voice slot gain: 0.25 = 1/4, the maximum gain that keeps the
    // 4-input sub-mixer sum clip-free when all four in-range voices peak
    // in phase. Pairs with voiceMixerFinal slots at 0.5 to give 8-voice
    // clean headroom while preserving overall loudness.
    //
    // Was 1.0 previously — that allowed 3+ in-phase voices to saturate
    // the sub-mixer's int16 output stage (audible distortion with a
    // clean-looking master meter, because voiceMixerFinal then attenuated
    // the already-clipped signal).
    static constexpr float kPerVoiceSlotGain = 0.25f;

    for (uint8_t v = 0; v < MAX_VOICES; ++v) {
        const float g = _voiceInRange(v) ? kPerVoiceSlotGain : 0.0f;
        if (v < 4) {
            _audio.voiceMixerA.gain(v, g);
        } else {
            _audio.voiceMixerB.gain(v - 4, g);
        }
    }
}

// _applyUnisonDetune() — spread voices evenly across ±spread/2 semitones.
// Uses _firstVoice/_voiceCount so each layer gets its own even spread.
void SynthEngine::_applyUnisonDetune() {
    const float spread = _patch.unisonDetune * UNISON_MAX_SPREAD_SEMITONES;
    for (uint8_t i = 0; i < _voiceCount; ++i) {
        const uint8_t v = _firstVoice + i;
        // Linear spacing from -spread/2 to +spread/2 across _voiceCount.
        // With 1 voice: offset = 0 (avoids divide by zero).
        const float offset = (_voiceCount > 1)
            ? (-spread * 0.5f + spread * (float)i / (float)(_voiceCount - 1))
            : 0.0f;
        _voices[v].setOsc1Detune(offset);
        _voices[v].setOsc2Detune(offset);
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
    const float normalised  = (float)(value) / 8192.0f;
    _patch.pitchBendSemis         = normalised * _patch.pitchBendRange;
    
    JT_LOGF("[normalised %.2f: semis %.2f, value = %.2f],  \n", normalised, _patch.pitchBendSemis, (float)value);
    // Apply to all voices — both oscillators share the same bend offset.
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) {
        _voices[i].setOsc1PitchBend(_patch.pitchBendSemis);
        _voices[i].setOsc2PitchBend(_patch.pitchBendSemis);
        
    }
}

void SynthEngine::setOsc1Detune(float hz) { _patch.osc1DetuneHz = hz; for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setOsc1Detune(hz); }
void SynthEngine::setOsc2Detune(float hz) { _patch.osc2DetuneHz = hz; for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setOsc2Detune(hz); }

void SynthEngine::setOsc1FineTune(float cents) { _patch.osc1FineCents = cents; for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setOsc1FineTune(cents); }
void SynthEngine::setOsc2FineTune(float cents) { _patch.osc2FineCents = cents; for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setOsc2FineTune(cents); }

void SynthEngine::setOscMix(float osc1Level, float osc2Level) {
    _patch.osc1Mix = osc1Level; _patch.osc2Mix = osc2Level;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setOscMix(osc1Level, osc2Level);
}
void SynthEngine::setOsc1Mix(float oscLevel) { _patch.osc1Mix = oscLevel; for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setOsc1Mix(oscLevel); }
void SynthEngine::setOsc2Mix(float oscLevel) { _patch.osc2Mix = oscLevel; for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setOsc2Mix(oscLevel); }
void SynthEngine::setSubMix(float mix)  { _patch.subMix = mix;  for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setSubMix(mix); }
void SynthEngine::setNoiseMix(float mix){ _patch.noiseMix = mix;for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setNoiseMix(mix); }

void SynthEngine::setSupersawDetune(uint8_t oscIndex, float amount) {
    if (oscIndex > 1) return;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) {
        if (oscIndex == 0) _voices[i].setOsc1SupersawDetune(amount);
        else                _voices[i].setOsc2SupersawDetune(amount);
    }
}

void SynthEngine::setSupersawMix(uint8_t oscIndex, float amount) {
    if (oscIndex > 1) return;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) {
        if (oscIndex == 0) _voices[i].setOsc1SupersawMix(amount);
        else                _voices[i].setOsc2SupersawMix(amount);
    }
}

void SynthEngine::setOsc1FrequencyDcAmp(float amp) { _patch.osc1FreqDc = amp; for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setOsc1FrequencyDcAmp(amp); }
void SynthEngine::setOsc2FrequencyDcAmp(float amp) { _patch.osc2FreqDc = amp; for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setOsc2FrequencyDcAmp(amp); }
void SynthEngine::setOsc1ShapeDcAmp(float amp)     { _patch.osc1ShapeDc = amp; for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setOsc1ShapeDcAmp(amp); }
void SynthEngine::setOsc2ShapeDcAmp(float amp)     { _patch.osc2ShapeDc = amp; for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setOsc2ShapeDcAmp(amp); }

void SynthEngine::setRing1Mix(float level) { _patch.ring1Mix = level; for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setRing1Mix(level); }
void SynthEngine::setRing2Mix(float level) { _patch.ring2Mix = level; for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setRing2Mix(level); }

void SynthEngine::setOsc1FeedbackAmount(float amount) { _patch.osc1FeedbackAmount = amount; for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setOsc1FeedbackAmount(amount); }
void SynthEngine::setOsc2FeedbackAmount(float amount) { _patch.osc2FeedbackAmount = amount; for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setOsc2FeedbackAmount(amount); }

void SynthEngine::setOsc1FeedbackMix(float mix) { _patch.osc1FeedbackMix = mix; for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setOsc1FeedbackMix(mix); }
void SynthEngine::setOsc2FeedbackMix(float mix) { _patch.osc2FeedbackMix = mix; for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setOsc2FeedbackMix(mix); }

// ── Cross Modulation & Oscillator Sync ───────────────────────────────────
void SynthEngine::setCrossModDepth(float depth) {
    _patch.crossModDepth = depth;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setCrossModDepth(depth);
}

void SynthEngine::setSyncEnabled(bool enabled) {
    _patch.syncEnabled = enabled;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setSyncEnabled(enabled);
}


// ---- Arbitrary waveform bank/index selection ----
void SynthEngine::setOsc1ArbBank(ArbBank b) {
    _patch.osc1ArbBank = b;
    // Clamp current index against the new bank count
    uint16_t count = akwf_bankCount(b);
    if (count > 0 && _patch.osc1ArbIndex >= count) _patch.osc1ArbIndex = count - 1;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) {
        _voices[i].setOsc1ArbBank(b);
        // also update index on voice since setArbBank may clamp index internally
        _voices[i].setOsc1ArbIndex(_patch.osc1ArbIndex);
    }
}

void SynthEngine::setOsc2ArbBank(ArbBank b) {
    _patch.osc2ArbBank = b;
    uint16_t count = akwf_bankCount(b);
    if (count > 0 && _patch.osc2ArbIndex >= count) _patch.osc2ArbIndex = count - 1;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) {
        _voices[i].setOsc2ArbBank(b);
        _voices[i].setOsc2ArbIndex(_patch.osc2ArbIndex);
    }
}

void SynthEngine::setOsc1ArbIndex(uint16_t idx) {
    // Clamp index by current bank
    uint16_t count = akwf_bankCount(_patch.osc1ArbBank);
    if (count == 0) {
        _patch.osc1ArbIndex = 0;
    } else {
        if (idx >= count) idx = count - 1;
        _patch.osc1ArbIndex = idx;
    }
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setOsc1ArbIndex(_patch.osc1ArbIndex);
}

void SynthEngine::setOsc2ArbIndex(uint16_t idx) {
    uint16_t count = akwf_bankCount(_patch.osc2ArbBank);
    if (count == 0) {
        _patch.osc2ArbIndex = 0;
    } else {
        if (idx >= count) idx = count - 1;
        _patch.osc2ArbIndex = idx;
    }
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setOsc2ArbIndex(_patch.osc2ArbIndex);
}

// ---- Amp mod DC ----
void SynthEngine::SetAmpModFixedLevel(float level) {
    _patch.ampModFixedLevel = level;
    _audio.ampModFixedDc.amplitude(level);
}
float SynthEngine::GetAmpModFixedLevel() const { return _patch.ampModFixedLevel; }
float SynthEngine::getAmpModFixedLevel() const { return _patch.ampModFixedLevel; }

// ---- LFOs ----
void SynthEngine::setLFO1Frequency(float hz) { _patch.lfo1Frequency = hz; _audio.lfo1.setFrequency(hz); }
void SynthEngine::setLFO2Frequency(float hz) { _patch.lfo2Frequency = hz; _audio.lfo2.setFrequency(hz); }
void SynthEngine::setLFO1Amount(float amt) {
    _patch.lfo1Amount = amt;
    _audio.lfo1.setAmplitude(amt);
    if (!_lfo1Ramping) _applyLFO1Gains(); // only if not mid-delay ramp
}
void SynthEngine::setLFO2Amount(float amt) {
    _patch.lfo2Amount = amt;
    _audio.lfo2.setAmplitude(amt);
    if (!_lfo2Ramping) _applyLFO2Gains();
}

void SynthEngine::setLFO1Waveform(int type) { _patch.lfo1Type = type; _audio.lfo1.setWaveformType(type); }
void SynthEngine::setLFO2Waveform(int type) { _patch.lfo2Type = type; _audio.lfo2.setWaveformType(type); }

void SynthEngine::setLFO1Destination(LFODestination dest) {
    // Legacy single-destination setter — zeros all per-dest depths then sets one to 1.0.
    // Existing presets that use a single destination continue to work unchanged.
    // For multi-target modulation use setLFO1PitchDepth/FilterDepth/PWMDepth/AmpDepth.
    _patch.lfo1Dest = dest;
    _audio.lfo1.setDestination(dest);
    _patch.lfo1PitchDepth  = 0.0f;
    _patch.lfo1FilterDepth = 0.0f;
    _patch.lfo1PWMDepth    = 0.0f;
    _patch.lfo1AmpDepth    = 0.0f;
    switch (dest) {
        case LFO_DEST_PITCH:  _patch.lfo1PitchDepth  = 1.0f; break;
        case LFO_DEST_FILTER: _patch.lfo1FilterDepth = 1.0f; break;
        case LFO_DEST_PWM:    _patch.lfo1PWMDepth    = 1.0f; break;
        case LFO_DEST_AMP:    _patch.lfo1AmpDepth    = 1.0f; break;
        default: break;
    }
    _applyLFO1Gains();
}

void SynthEngine::setLFO2Destination(LFODestination dest) {
    // Legacy single-destination setter — backward compatible with Microsphere presets.
    _patch.lfo2Dest = dest;
    _audio.lfo2.setDestination(dest);
    _patch.lfo2PitchDepth  = 0.0f;
    _patch.lfo2FilterDepth = 0.0f;
    _patch.lfo2PWMDepth    = 0.0f;
    _patch.lfo2AmpDepth    = 0.0f;
    switch (dest) {
        case LFO_DEST_PITCH:  _patch.lfo2PitchDepth  = 1.0f; break;
        case LFO_DEST_FILTER: _patch.lfo2FilterDepth = 1.0f; break;
        case LFO_DEST_PWM:    _patch.lfo2PWMDepth    = 1.0f; break;
        case LFO_DEST_AMP:    _patch.lfo2AmpDepth    = 1.0f; break;
        default: break;
    }
    _applyLFO2Gains();
}

float SynthEngine::getLFO1Frequency() const { return _patch.lfo1Frequency; }
float SynthEngine::getLFO2Frequency() const { return _patch.lfo2Frequency; }
float SynthEngine::getLFO1Amount() const    { return _patch.lfo1Amount; }
float SynthEngine::getLFO2Amount() const    { return _patch.lfo2Amount; }
int   SynthEngine::getLFO1Waveform() const  { return _patch.lfo1Type; }
int   SynthEngine::getLFO2Waveform() const  { return _patch.lfo2Type; }
LFODestination SynthEngine::getLFO1Destination() const { return _patch.lfo1Dest; }
LFODestination SynthEngine::getLFO2Destination() const { return _patch.lfo2Dest; }

const char* SynthEngine::getLFO1WaveformName() const {
    return waveformShortName((WaveformType)_patch.lfo1Type);
}
const char* SynthEngine::getLFO2WaveformName() const {
    return waveformShortName((WaveformType)_patch.lfo2Type);
}
const char* SynthEngine::getLFO1DestinationName() const {
    // Show "Multi" when more than one per-destination depth is active
    int active = (_patch.lfo1PitchDepth > 0) + (_patch.lfo1FilterDepth > 0)
               + (_patch.lfo1PWMDepth   > 0) + (_patch.lfo1AmpDepth   > 0);
    if (active > 1)  return "Multi";
    if (active == 0) return LFODestNames[LFO_DEST_NONE];
    if (_patch.lfo1PitchDepth  > 0) return LFODestNames[LFO_DEST_PITCH];
    if (_patch.lfo1FilterDepth > 0) return LFODestNames[LFO_DEST_FILTER];
    if (_patch.lfo1PWMDepth    > 0) return LFODestNames[LFO_DEST_PWM];
    if (_patch.lfo1AmpDepth    > 0) return LFODestNames[LFO_DEST_AMP];
    return LFODestNames[LFO_DEST_NONE];
}
const char* SynthEngine::getLFO2DestinationName() const {
    int active = (_patch.lfo2PitchDepth > 0) + (_patch.lfo2FilterDepth > 0)
               + (_patch.lfo2PWMDepth   > 0) + (_patch.lfo2AmpDepth   > 0);
    if (active > 1)  return "Multi";
    if (active == 0) return LFODestNames[LFO_DEST_NONE];
    if (_patch.lfo2PitchDepth  > 0) return LFODestNames[LFO_DEST_PITCH];
    if (_patch.lfo2FilterDepth > 0) return LFODestNames[LFO_DEST_FILTER];
    if (_patch.lfo2PWMDepth    > 0) return LFODestNames[LFO_DEST_PWM];
    if (_patch.lfo2AmpDepth    > 0) return LFODestNames[LFO_DEST_AMP];
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
    const float eff1 = (_patch.lfo1Amount > 0.0f) ? _patch.lfo1Amount : (
        (_patch.lfo1PitchDepth > 0.0f || _patch.lfo1FilterDepth > 0.0f ||
         _patch.lfo1PWMDepth   > 0.0f || _patch.lfo1AmpDepth   > 0.0f) ? 1.0f : 0.0f);

    // Drive the DSP waveform amplitude — only write if changed (avoid audio glitch)
    if (eff1 != _audio.lfo1.getAmplitude()) _audio.lfo1.setAmplitude(eff1);

    // -------------------------------------------------------------------------
    // PITCH gain:
    //   _patch.lfo1PitchDepth (0..1 from CC) represents the fraction of max vibrato.
    //   LFO_PITCH_MAX_SEMITONES × FM_SEMITONE_SCALE converts the desired semitone
    //   range into the correct FM-mixer input amplitude (see SynthEngine.h).
    //   Without FM_SEMITONE_SCALE, full depth would try to shift ±10 octaves!
    // -------------------------------------------------------------------------
    const float pitchScale = LFO_PITCH_MAX_SEMITONES * FM_SEMITONE_SCALE;  // = 7/120 ≈ 0.0583
    const float pitchG  = eff1 * _patch.lfo1PitchDepth * pitchScale;

    // Filter, PWM and amp gains are already dimensionless (0..1 into their respective
    // mod mixers) — no additional scale needed for those paths.
    const float filterG = eff1 * _patch.lfo1FilterDepth;
    const float pwmG    = eff1 * _patch.lfo1PWMDepth;
    const float ampG    = eff1 * _patch.lfo1AmpDepth;

    // Walk ALL voices. In-range voices get the computed depth; out-of-range
    // voices get 0 so this engine's LFO1 doesn't leak into the other engine's
    // voices via the permanently-wired AudioConnections set up in begin().
    for (uint8_t v = 0; v < MAX_VOICES; ++v) {
        const bool in = _voiceInRange(v);
        _voices[v].frequencyModMixerOsc1().gain(1, in ? pitchG  : 0.0f);
        _voices[v].frequencyModMixerOsc2().gain(1, in ? pitchG  : 0.0f);
        _voices[v].filterModMixer()       .gain(2, in ? filterG : 0.0f);
        _voices[v].shapeModMixerOsc1()    .gain(1, in ? pwmG    : 0.0f);
        _voices[v].shapeModMixerOsc2()    .gain(1, in ? pwmG    : 0.0f);
    }
    // Amp-mod mixer is per-engine (not per-voice), so no range gate here.
    _audio.ampModMixer.gain(1, ampG);
}

void SynthEngine::_applyLFO2Gains() {
    // Same structure as _applyLFO1Gains — see comments there for explanation.
    const float eff2 = (_patch.lfo2Amount > 0.0f) ? _patch.lfo2Amount : (
        (_patch.lfo2PitchDepth > 0.0f || _patch.lfo2FilterDepth > 0.0f ||
         _patch.lfo2PWMDepth   > 0.0f || _patch.lfo2AmpDepth   > 0.0f) ? 1.0f : 0.0f);
    if (eff2 != _audio.lfo2.getAmplitude()) _audio.lfo2.setAmplitude(eff2);

    // Pitch: scale depth (0..1) to FM mod-input units via semitone conversion
    const float pitchScale = LFO_PITCH_MAX_SEMITONES * FM_SEMITONE_SCALE;  // ≈ 0.0583
    const float pitchG  = eff2 * _patch.lfo2PitchDepth * pitchScale;
    const float filterG = eff2 * _patch.lfo2FilterDepth;
    const float pwmG    = eff2 * _patch.lfo2PWMDepth;
    const float ampG    = eff2 * _patch.lfo2AmpDepth;

    // Range-gated walk over all MAX_VOICES voices (see _applyLFO1Gains).
    for (uint8_t v = 0; v < MAX_VOICES; ++v) {
        const bool in = _voiceInRange(v);
        _voices[v].frequencyModMixerOsc1().gain(2, in ? pitchG  : 0.0f);
        _voices[v].frequencyModMixerOsc2().gain(2, in ? pitchG  : 0.0f);
        _voices[v].filterModMixer()       .gain(3, in ? filterG : 0.0f);
        _voices[v].shapeModMixerOsc1()    .gain(2, in ? pwmG    : 0.0f);
        _voices[v].shapeModMixerOsc2()    .gain(2, in ? pwmG    : 0.0f);
    }
    _audio.ampModMixer.gain(2, ampG);
}

void SynthEngine::_applySeqOutput() {
    const float val = _audio.seq1.getOutput();  // −depth … +depth (0 when disabled)
    const uint8_t dest = _patch.seqDestination;

    // ── Destination changed: zero out the OLD destination on ALL voices ──
    // We walk every voice, not just in-range, so that out-of-range voices
    // don't retain stale gains from before a recent setVoiceRange().
    if (dest != _audio.seqPrevDestination) {
        switch (_audio.seqPrevDestination) {
        case LFO_DEST_PITCH:
            for (uint8_t v = 0; v < MAX_VOICES; ++v)
                _voices[v].setSeqPitchOffset(0.0f);
            break;
        case LFO_DEST_FILTER:
            for (uint8_t v = 0; v < MAX_VOICES; ++v)
                _voices[v].setSeqFilterOffset(0.0f);
            break;
        case LFO_DEST_PWM:
            for (uint8_t v = 0; v < MAX_VOICES; ++v) {
                _voices[v].shapeModMixerOsc1().gain(3, 0.0f);
                _voices[v].shapeModMixerOsc2().gain(3, 0.0f);
            }
            break;
        case LFO_DEST_AMP:
            _audio.ampModMixer.gain(3, 0.0f);
            break;
        }
        _audio.seqPrevDestination = dest;
    }

    // ── Apply to current destination, range-gated where appropriate ──────
    // For per-voice destinations, in-range voices get `val`, out-of-range
    // get 0 — keeps the other engine's seq out of our voices (and vice
    // versa, since the seqDc is permanently wired to all 8 voices).
    switch (dest) {
    case LFO_DEST_PITCH: {
        // Convert bipolar output to FM-scaled semitones.
        // Same scaling as LFO pitch: max semitones × FM_SEMITONE_SCALE.
        const float pitchFm = val * LFO_PITCH_MAX_SEMITONES * FM_SEMITONE_SCALE;
        for (uint8_t v = 0; v < MAX_VOICES; ++v)
            _voices[v].setSeqPitchOffset(_voiceInRange(v) ? pitchFm : 0.0f);
        break;
    }
    case LFO_DEST_FILTER:
        // Added to key tracking DC on mod bus slot 0
        for (uint8_t v = 0; v < MAX_VOICES; ++v)
            _voices[v].setSeqFilterOffset(_voiceInRange(v) ? val : 0.0f);
        break;

    case LFO_DEST_PWM:
        // Shape mixer slot 3 gain — _audio.seqDc outputs 1.0, gain carries value
        for (uint8_t v = 0; v < MAX_VOICES; ++v) {
            const float g = _voiceInRange(v) ? val : 0.0f;
            _voices[v].shapeModMixerOsc1().gain(3, g);
            _voices[v].shapeModMixerOsc2().gain(3, g);
        }
        break;

    case LFO_DEST_AMP:
        // Amp mod mixer slot 3 gain — _audio.seqDc outputs 1.0, gain carries value.
        // Amp-mod mixer is per-engine (not per-voice), so no range gate here.
        _audio.ampModMixer.gain(3, val);
        break;

    default:
        // LFO_DEST_NONE — nothing to do (output is 0.0 anyway)
        break;
    }
}
void SynthEngine::setLFO1PitchDepth(float d)  { _patch.lfo1PitchDepth  = d; _applyLFO1Gains(); }
void SynthEngine::setLFO1FilterDepth(float d) { _patch.lfo1FilterDepth = d; _applyLFO1Gains(); }
void SynthEngine::setLFO1PWMDepth(float d)    { _patch.lfo1PWMDepth    = d; _applyLFO1Gains(); }
void SynthEngine::setLFO1AmpDepth(float d)    { _patch.lfo1AmpDepth    = d; _applyLFO1Gains(); }
void SynthEngine::setLFO1Delay(float ms)      { _patch.lfo1DelayMs     = ms; }

void SynthEngine::setLFO2PitchDepth(float d)  { _patch.lfo2PitchDepth  = d; _applyLFO2Gains(); }
void SynthEngine::setLFO2FilterDepth(float d) { _patch.lfo2FilterDepth = d; _applyLFO2Gains(); }
void SynthEngine::setLFO2PWMDepth(float d)    { _patch.lfo2PWMDepth    = d; _applyLFO2Gains(); }
void SynthEngine::setLFO2AmpDepth(float d)    { _patch.lfo2AmpDepth    = d; _applyLFO2Gains(); }
void SynthEngine::setLFO2Delay(float ms)      { _patch.lfo2DelayMs     = ms; }

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
    if (_lfo1Ramping && _patch.lfo1DelayMs > 0.0f) {
        const float elapsed = (float)(now - _lfo1NoteOnMs);

        // t: normalised ramp position 0..1.  Clamp to 1 at end of delay window.
        const float t = (elapsed >= _patch.lfo1DelayMs) ? 1.0f : (elapsed / _patch.lfo1DelayMs);
        _lfo1CurrentAmp = _patch.lfo1Amount * t;

        // Ramp the LFO oscillator amplitude so the waveform itself fades in.
        // Without this, the audio block outputs full amplitude from note-on and
        // only the gain ramp is effective — the intended fade-in still works but
        // the LFO oscillator wastes CPU running at full amplitude from the start.
        _audio.lfo1.setAmplitude(_lfo1CurrentAmp);

        // Apply ramped mixer gains — same formula as _applyLFO1Gains, range-gated.
        const float pitchScale = LFO_PITCH_MAX_SEMITONES * FM_SEMITONE_SCALE;
        const float pitchG  = _lfo1CurrentAmp * _patch.lfo1PitchDepth * pitchScale;
        const float filterG = _lfo1CurrentAmp * _patch.lfo1FilterDepth;
        const float pwmG    = _lfo1CurrentAmp * _patch.lfo1PWMDepth;
        const float ampG    = _lfo1CurrentAmp * _patch.lfo1AmpDepth;
        for (uint8_t v = 0; v < MAX_VOICES; ++v) {
            const bool in = _voiceInRange(v);
            _voices[v].frequencyModMixerOsc1().gain(1, in ? pitchG  : 0.0f);
            _voices[v].frequencyModMixerOsc2().gain(1, in ? pitchG  : 0.0f);
            _voices[v].filterModMixer()       .gain(2, in ? filterG : 0.0f);
            _voices[v].shapeModMixerOsc1()    .gain(1, in ? pwmG    : 0.0f);
            _voices[v].shapeModMixerOsc2()    .gain(1, in ? pwmG    : 0.0f);
        }
        _audio.ampModMixer.gain(1, ampG);

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
    if (_lfo2Ramping && _patch.lfo2DelayMs > 0.0f) {
        const float elapsed = (float)(now - _lfo2NoteOnMs);
        const float t = (elapsed >= _patch.lfo2DelayMs) ? 1.0f : (elapsed / _patch.lfo2DelayMs);
        _lfo2CurrentAmp = _patch.lfo2Amount * t;

        _audio.lfo2.setAmplitude(_lfo2CurrentAmp);

        const float pitchScale = LFO_PITCH_MAX_SEMITONES * FM_SEMITONE_SCALE;
        const float pitchG  = _lfo2CurrentAmp * _patch.lfo2PitchDepth * pitchScale;
        const float filterG = _lfo2CurrentAmp * _patch.lfo2FilterDepth;
        const float pwmG    = _lfo2CurrentAmp * _patch.lfo2PWMDepth;
        const float ampG    = _lfo2CurrentAmp * _patch.lfo2AmpDepth;
        for (uint8_t v = 0; v < MAX_VOICES; ++v) {
            const bool in = _voiceInRange(v);
            _voices[v].frequencyModMixerOsc1().gain(2, in ? pitchG  : 0.0f);
            _voices[v].frequencyModMixerOsc2().gain(2, in ? pitchG  : 0.0f);
            _voices[v].filterModMixer()       .gain(3, in ? filterG : 0.0f);
            _voices[v].shapeModMixerOsc1()    .gain(2, in ? pwmG    : 0.0f);
            _voices[v].shapeModMixerOsc2()    .gain(2, in ? pwmG    : 0.0f);
        }
        _audio.ampModMixer.gain(2, ampG);

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
    _patch.pitchEnvAttack = ms;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setPitchEnvAttack(ms);
}
void SynthEngine::setPitchEnvDecay(float ms) {
    _patch.pitchEnvDecay = ms;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setPitchEnvDecay(ms);
}
void SynthEngine::setPitchEnvSustain(float l) {
    _patch.pitchEnvSustain = l;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setPitchEnvSustain(l);
}
void SynthEngine::setPitchEnvRelease(float ms) {
    _patch.pitchEnvRelease = ms;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setPitchEnvRelease(ms);
}
void SynthEngine::setPitchEnvDepth(float semitones) {
    semitones = constrain(semitones, -24.0f, 24.0f);
    _patch.pitchEnvDepth = semitones;
    // VoiceBlock::setPitchEnvDepth writes amplitude = semitones / 12.0 to _pitchEnvDc.
    // Range: -1.0 (full down) to +1.0 (full up), 0 = no pitch shift.
    // freqModMixer gain(3) = 1.0. Depth is encoded in the _pitchEnvDc amplitude — do not change here.
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setPitchEnvDepth(semitones);
}

// ============================================================================
// NEW: VELOCITY SENSITIVITY
// ============================================================================

void SynthEngine::setVelocityAmpSens(float s) {
    _patch.velAmpSens = s;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setVelocityAmpSens(s);
}
void SynthEngine::setVelocityFilterSens(float s) {
    _patch.velFilterSens = s;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setVelocityFilterSens(s);
}
void SynthEngine::setVelocityEnvSens(float s) {
    _patch.velEnvSens = s;
    for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setVelocityEnvSens(s);
}

// ============================================================================
// JPFX TONE CONTROL
// ============================================================================

void SynthEngine::setFXBassGain(float dB) {
    _patch.fxBassGain = dB;
    _fxChain.setBassGain(dB);
}

void SynthEngine::setFXTrebleGain(float dB) {
    _patch.fxTrebleGain = dB;
    _fxChain.setTrebleGain(dB);
}

float SynthEngine::getFXBassGain() const {
    return _patch.fxBassGain;
}

float SynthEngine::getFXTrebleGain() const {
    return _patch.fxTrebleGain;
}

// ============================================================================
// JPFX MODULATION EFFECTS
// ============================================================================

void SynthEngine::setFXModEffect(int8_t variation) {
    _patch.fxModEffect = variation;
    _fxChain.setModEffect(variation);
}

void SynthEngine::setFXModMix(float mix) {
    _patch.fxModMix = mix;
    _fxChain.setModMix(mix);
}

void SynthEngine::setFXModRate(float hz) {
    _patch.fxModRate = hz;
    _fxChain.setModRate(hz);
}

void SynthEngine::setFXModFeedback(float fb) {
    _patch.fxModFeedback = fb;
    _fxChain.setModFeedback(fb);
}

int8_t SynthEngine::getFXModEffect() const {
    return _patch.fxModEffect;
}

float SynthEngine::getFXModMix() const {
    return _patch.fxModMix;
}

float SynthEngine::getFXModRate() const {
    return _patch.fxModRate;
}

float SynthEngine::getFXModFeedback() const {
    return _patch.fxModFeedback;
}

const char* SynthEngine::getFXModEffectName() const {
    return _fxChain.getModEffectName();
}

// ============================================================================
// JPFX DELAY EFFECTS
// ============================================================================

void SynthEngine::setFXDelayEffect(int8_t variation) {
    _patch.fxDelayEffect = variation;
    _fxChain.setDelayEffect(variation);
}

void SynthEngine::setFXDelayMix(float mix) {
    _patch.fxDelayMix = mix;
    _fxChain.setDelayMix(mix);
}

void SynthEngine::setFXDelayFeedback(float fb) {
    _patch.fxDelayFeedback = fb;
    _fxChain.setDelayFeedback(fb);
}

void SynthEngine::setFXDelayTime(float ms) {
    _patch.fxDelayTime = ms;
    _fxChain.setDelayTime(ms);
}

int8_t SynthEngine::getFXDelayEffect() const {
    return _patch.fxDelayEffect;
}

float SynthEngine::getFXDelayMix() const {
    return _patch.fxDelayMix;
}

float SynthEngine::getFXDelayFeedback() const {
    return _patch.fxDelayFeedback;
}

float SynthEngine::getFXDelayTime() const {
    return _patch.fxDelayTime;
}

const char* SynthEngine::getFXDelayEffectName() const {
    return _fxChain.getDelayEffectName();
}

// ============================================================================
// JPFX DRY MIX
// ============================================================================

void SynthEngine::setFXDryMix(float level) {
    _patch.fxDryMix = level;
    _fxChain.setDryMix(level, level); // Stereo
}

float SynthEngine::getFXDryMix() const {
    return _patch.fxDryMix;
}

void SynthEngine::setFXJPFXMix(float left, float right) {
    _patch.fxJPFXMixL = left;
    _patch.fxJPFXMixR = right;
    _fxChain.setJPFXMix(left, right);
}

float SynthEngine::getFXJPFXMixL() const { return _patch.fxJPFXMixL; }
float SynthEngine::getFXJPFXMixR() const { return _patch.fxJPFXMixR; }

// ============================================================================
// FX REVERB — moved to GlobalFX (Phase 3). See GlobalFX.h / LayerManager.h.
// All per-engine reverb setters/getters are gone; callers should go through
// LayerManager::getGlobalFX() instead. Reverb state is also no longer
// captured in PatchState — it lives in Performance now since it's shared.
// ============================================================================


// ---- UI helper getters ----
int SynthEngine::getOsc1Waveform() const { return _patch.osc1Wave; }
int SynthEngine::getOsc2Waveform() const { return _patch.osc2Wave; }
const char* SynthEngine::getOsc1WaveformName() const {
    return waveformShortName((WaveformType)_patch.osc1Wave);
}
const char* SynthEngine::getOsc2WaveformName() const {
    return waveformShortName((WaveformType)_patch.osc2Wave);
}


float SynthEngine::getSupersawDetune(uint8_t osc) const { return (osc<2)?_patch.supersawDetune[osc]:0.0f; }
float SynthEngine::getSupersawMix(uint8_t osc)    const { return (osc<2)?_patch.supersawMix[osc]:0.0f; }
float SynthEngine::getOsc1PitchOffset() const { return _patch.osc1PitchSemi; }
float SynthEngine::getOsc2PitchOffset() const { return _patch.osc2PitchSemi; }
float SynthEngine::getOsc1Detune() const { return _patch.osc1DetuneHz; }
float SynthEngine::getOsc2Detune() const { return _patch.osc2DetuneHz; }
float SynthEngine::getOsc1FineTune() const { return _patch.osc1FineCents; }
float SynthEngine::getOsc2FineTune() const { return _patch.osc2FineCents; }
float SynthEngine::getOscMix1() const { return _patch.osc1Mix; }
float SynthEngine::getOscMix2() const { return _patch.osc2Mix; }
float SynthEngine::getSubMix() const { return _patch.subMix; }
float SynthEngine::getNoiseMix() const { return _patch.noiseMix; }
float SynthEngine::getRing1Mix() const { return _patch.ring1Mix; }
float SynthEngine::getRing2Mix() const { return _patch.ring2Mix; }
float SynthEngine::getOsc1FrequencyDc() const { return _patch.osc1FreqDc; }
float SynthEngine::getOsc2FrequencyDc() const { return _patch.osc2FreqDc; }
float SynthEngine::getOsc1ShapeDc() const     { return _patch.osc1ShapeDc; }
float SynthEngine::getOsc2ShapeDc() const     { return _patch.osc2ShapeDc; }

float SynthEngine::getOsc1FeedbackAmount( ) const {return _patch.osc1FeedbackAmount;}
float SynthEngine::getOsc2FeedbackAmount( ) const {return _patch.osc2FeedbackAmount;}

float SynthEngine::getOsc1FeedbackMix( ) const {return _patch.osc1FeedbackMix;}
float SynthEngine::getOsc2FeedbackMix( ) const {return _patch.osc2FeedbackMix;}

bool  SynthEngine::getGlideEnabled() const { return _patch.glideEnabled; }
float SynthEngine::getGlideTimeMs()  const { return _patch.glideTimeMs; }



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
            JT_CC_LOG("[CC %u:%s] OSC1 Waveform -> %s (%d)\n", control, ccName, waveformShortName(t), (int)t);
        } break;

        case CC::OSC2_WAVE: {
            WaveformType t = waveformFromCC(value);
            setOsc2Waveform((int)t);
            JT_CC_LOG("[CC %u:%s] OSC2 Waveform -> %s (%d)\n", control, ccName, waveformShortName(t), (int)t);
        } break;

        // ------------------- Mod Wheel (example: LFO1 frequency) -------------------
        case 1: { // MIDI ModWheel
            float hz = JT8000Map::cc_to_lfo_hz(value);
            setLFO1Frequency(hz);
            JT_CC_LOG("[CC %u:ModWheel] LFO1 Freq = %.4f Hz\n", control, hz);
        } break;

        // ------------------- Filter main -------------------
        case CC::FILTER_CUTOFF: {
            float hz = JT8000Map::cc_to_obxa_cutoff_hz(value);
            hz = fminf(fmaxf(hz, CUTOFF_MIN_HZ), CUTOFF_MAX_HZ);
            setFilterCutoff(hz);
            JT_CC_LOG("[CC %u:%s] Cutoff = %.2f Hz\n", control, ccName, hz);
        } break;

        case CC::FILTER_RESONANCE: {
            // Route through the engine-aware helper so the OBXa safety ceiling
            // (OBXA_RES_MAX) is applied when the OBXa engine is active, while
            // the VA engine receives the full 0..1 range.
            float r = JT8000Map::cc_to_resonance(value, _patch.filterEngine);
            setFilterResonance(r);
            JT_CC_LOG("[CC %u:%s] Resonance = %.4f (engine %u)\n", control, ccName, r, _patch.filterEngine);
        } break;

        // ------------------- Amp envelope -------------------
        case CC::AMP_ATTACK: {
            float ms = JT8000Map::cc_to_time_ms(value);
            for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setAmpAttack(ms);
            JT_CC_LOG("[CC %u:%s] Amp Attack = %.2f ms\n", control, ccName, ms);
        } break;

        case CC::AMP_DECAY: {
            float ms = JT8000Map::cc_to_time_ms(value);
            for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setAmpDecay(ms);
            JT_CC_LOG("[CC %u:%s] Amp Decay = %.2f ms\n", control, ccName, ms);
        } break;

        case CC::AMP_SUSTAIN: {
            for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setAmpSustain(norm);
            JT_CC_LOG("[CC %u:%s] Amp Sustain = %.3f\n", control, ccName, norm);
        } break;

        case CC::AMP_RELEASE: {
            float ms = JT8000Map::cc_to_time_ms(value);
            for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setAmpRelease(ms);
            JT_CC_LOG("[CC %u:%s] Amp Release = %.2f ms\n", control, ccName, ms);
        } break;

        // ------------------- Filter envelope -------------------
        case CC::FILTER_ENV_ATTACK: {
            float ms = JT8000Map::cc_to_time_ms(value);
            for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setFilterAttack(ms);
            JT_CC_LOG("[CC %u:%s] Filt Env Attack = %.2f ms\n", control, ccName, ms);
        } break;

        case CC::FILTER_ENV_DECAY: {
            float ms = JT8000Map::cc_to_time_ms(value);
            for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setFilterDecay(ms);
            JT_CC_LOG("[CC %u:%s] Filt Env Decay = %.2f ms\n", control, ccName, ms);
        } break;

        case CC::FILTER_ENV_SUSTAIN: {
            for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setFilterSustain(norm);
            JT_CC_LOG("[CC %u:%s] Filt Env Sustain = %.3f\n", control, ccName, norm);
        } break;

        case CC::FILTER_ENV_RELEASE: {
            float ms = JT8000Map::cc_to_time_ms(value);
            for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setFilterRelease(ms);
            JT_CC_LOG("[CC %u:%s] Filt Env Release = %.2f ms\n", control, ccName, ms);
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
            JT_CC_LOG("[CC %u:%s] OSC1 Coarse = %.1f semitones\n", control, ccName, semis);
        } break;

        case CC::OSC2_PITCH_OFFSET: {
            float semis;
            if (value <= 25)       semis = -24.0f;
            else if (value <= 51)  semis = -12.0f;
            else if (value <= 76)  semis =   0.0f;
            else if (value <= 101) semis = +12.0f;
            else                   semis = +24.0f;
            setOsc2PitchOffset(semis);
            JT_CC_LOG("[CC %u:%s] OSC2 Coarse = %.1f semitones\n", control, ccName, semis);
        } break;

        // ------------------- Detune / Fine -------------------
        // Dead-zone: CC 64 forces exact zero. Without this, the odd 0-127 range
        // means no single CC value maps to precisely 0.0 through norm*2-1.
        case CC::OSC1_DETUNE: {
            float d = (value == 64) ? 0.0f : (norm * 2.0f - 1.0f) * 12.0f;
            setOsc1Detune(d);
            JT_CC_LOG("[CC %u:%s] OSC1 Detune = %.2f Hz\n", control, ccName, d);
        } break;
        case CC::OSC2_DETUNE: {
            float d = (value == 64) ? 0.0f : (norm * 2.0f - 1.0f) * 12.0f;
            setOsc2Detune(d);
            JT_CC_LOG("[CC %u:%s] OSC2 Detune = %.2f Hz\n", control, ccName, d);
        } break;
        case CC::OSC1_FINE_TUNE: {
            float c = (value == 64) ? 0.0f : norm * 200.0f - 100.0f;
            setOsc1FineTune(c);
            JT_CC_LOG("[CC %u:%s] OSC1 Fine = %.1f cents\n", control, ccName, c);
        } break;
        case CC::OSC2_FINE_TUNE: {
            float c = (value == 64) ? 0.0f : norm * 200.0f - 100.0f;
            setOsc2FineTune(c);
            JT_CC_LOG("[CC %u:%s] OSC2 Fine = %.1f cents\n", control, ccName, c);
        } break;

        // ------------------- Osc mix + taps -------------------
        case CC::OSC1_FEEDBACK_AMOUNT: {
            setOsc1FeedbackAmount(norm);
            JT_CC_LOG("[CC %u:%s] Osc1 feedback amount = %.3f \n", control, ccName, norm);
        } break;

        case CC::OSC2_FEEDBACK_AMOUNT: {
            setOsc2FeedbackAmount(norm);
            JT_CC_LOG("[CC %u:%s] Osc2 feedback amount = %.3f \n", control, ccName, norm);
        } break;

        case CC::OSC1_FEEDBACK_MIX: {
            setOsc1FeedbackMix(norm);
            JT_CC_LOG("[CC %u:%s] Osc1 feedback mix = %.3f \n", control, ccName, norm);
        } break;

         case CC::OSC2_FEEDBACK_MIX: {
            setOsc2FeedbackMix(norm);
            JT_CC_LOG("[CC %u:%s] Osc2 feedback mix = %.3f \n", control, ccName, norm);
        } break;

        // ------------------- Osc mix + taps -------------------
        case CC::OSC_MIX_BALANCE: {
            float l = 1.0f - norm, r = norm;
            setOscMix(l, r);
            JT_CC_LOG("[CC %u:%s] Osc Mix balance L=%.3f R=%.3f\n", control, ccName, l, r);
        } break;

        case CC::OSC1_MIX: {
            for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setOsc1Mix(norm);
            _patch.osc1Mix = norm;
            JT_CC_LOG("[CC %u:%s] OSC1 Mix = %.3f\n", control, ccName, norm);
        } break;

        case CC::OSC2_MIX: {
            for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setOsc2Mix(norm);
            _patch.osc2Mix = norm;
            JT_CC_LOG("[CC %u:%s] OSC2 Mix = %.3f\n", control, ccName, norm);
        } break;

        case CC::SUB_MIX:   { setSubMix(norm);   JT_CC_LOG("[CC %u:%s] Sub Mix   = %.3f\n", control, ccName, norm); } break;
        case CC::NOISE_MIX: { setNoiseMix(norm); JT_CC_LOG("[CC %u:%s] Noise Mix = %.3f\n", control, ccName, norm); } break;

        // ------------------- Filter modulation -------------------
        case CC::FILTER_ENV_AMOUNT: {
            float a = norm * 2.0f - 1.0f;
            setFilterEnvAmount(a);
            JT_CC_LOG("[CC %u:%s] Filt Env Amount = %.3f\n", control, ccName, a);
        } break;

        case CC::FILTER_KEY_TRACK: {
            float k = norm * 2.0f - 1.0f;
            setFilterKeyTrackAmount(k);
            JT_CC_LOG("[CC %u:%s] KeyTrack = %.3f\n", control, ccName, k);
        } break;

        case CC::FILTER_OCTAVE_CONTROL: {
            float o = norm * 10.0f;
            setFilterOctaveControl(o);
            JT_CC_LOG("[CC %u:%s] Filter Octave = %.3f\n", control, ccName, o);
        } break;

        // --- Filter topology — engine-context-dependent CCs ---
        //
        // Three CCs change meaning with the active engine so the same physical
        // control / display slot serves both engines (halving the filter CC
        // count vs one-CC-per-param-per-engine):
        //
        //   CC 111 (Multimode/Sat) : OBXa → multimode blend (continuous)
        //                            VA   → saturation type (3-way select)
        //   CC 112 (Type)          : OBXa → filter mode (6-way select)
        //                            VA   → filter type (FILTER_COUNT select)
        //   CC 114 (Variant)       : OBXa → Xpander sub-mode (0..14 select)
        //                            VA   → drive amount (continuous)
        //
        // The active engine is read from _patch.filterEngine (kept in sync by
        // setFilterEngine()). FilterBlock caches each engine's values
        // independently and re-applies them on switch, so a control that is
        // inert for the current engine still preserves the other engine's value.

        // CC 111 — OBXa multimode blend  /  VA saturation type
        case CC::FILTER_OBXA_MULTIMODE: {
            if (_patch.filterEngine == CC::FILTER_ENGINE_VA) {
                // 3 saturation buckets: SAT_NONE / SAT_FAST / SAT_TANH
                const uint8_t sat = (uint8_t)constrain((int)value * 3 / 128, 0, 2);
                setVASaturation(sat);
                JT_CC_LOG("[CC %u:%s] VA Sat = %u\n", control, ccName, sat);
            } else {
                setFilterMultimode(norm);
                JT_CC_LOG("[CC %u:%s] Multimode = %.3f\n", control, ccName, norm);
            }
        } break;

        // CC 112 — OBXa filter mode  /  VA filter type
        case CC::FILTER_MODE: {
            if (_patch.filterEngine == CC::FILTER_ENGINE_VA) {
                const uint8_t vt = (uint8_t)constrain(
                    (int)value * (int)FILTER_COUNT / 128, 0, (int)FILTER_COUNT - 1);
                setVAFilterType(vt);
                JT_CC_LOG("[CC %u:%s] VAFilterType = %u\n", control, ccName, vt);
            } else {
                const uint8_t mode = (uint8_t)constrain(
                    (int)value * (int)CC::FILTER_MODE_COUNT / 128, 0,
                    (int)CC::FILTER_MODE_COUNT - 1);
                setFilterMode(mode);
                JT_CC_LOG("[CC %u:%s] FilterMode = %u\n", control, ccName, mode);
            }
        } break;

        // Filter engine select: 0 = OBXa, 1 = VA bank.
        // CC 0-63 = OBXa, CC 64-127 = VA bank (2 equal buckets).
        case CC::FILTER_ENGINE: {
            const uint8_t eng = (value >= 64) ? CC::FILTER_ENGINE_VA
                                              : CC::FILTER_ENGINE_OBXA;
            setFilterEngine(eng);
            JT_CC_LOG("[CC %u:%s] FilterEngine = %u\n", control, ccName, eng);
        } break;

        // CC 114 — OBXa Xpander sub-mode (0..14)  /  VA drive (continuous)
        case CC::FILTER_OBXA_XPANDER_MODE: {
            if (_patch.filterEngine == CC::FILTER_ENGINE_VA) {
                setVADrive(norm);
                JT_CC_LOG("[CC %u:%s] VA Drive = %.3f\n", control, ccName, norm);
            } else {
                const uint8_t mode = (uint8_t)constrain((int)value * 15 / 128, 0, 14);
                setFilterXpanderMode(mode);
                JT_CC_LOG("[CC %u:%s] XpanderMode = %u\n", control, ccName, mode);
            }
        } break;

        // Resonance modulation depth: CC 0-127 → 0.0-1.0
        case CC::FILTER_OBXA_RES_MOD_DEPTH: {
            setFilterResonanceModDepth(norm);
            JT_CC_LOG("[CC %u:%s] ResModDepth = %.3f\n", control, ccName, norm);
        } break;

        // ------------------- LFO1 -------------------
        case CC::LFO1_FREQ:        { float hz = JT8000Map::cc_to_lfo_hz(value); setLFO1Frequency(hz); JT_CC_LOG("[CC %u:%s] LFO1 Freq = %.4f Hz\n", control, ccName, hz); } break;
        case CC::LFO1_DEPTH:       { setLFO1Amount(norm); JT_CC_LOG("[CC %u:%s] LFO1 Depth = %.3f\n", control, ccName, norm); } break;
        case CC::LFO1_DESTINATION: { int d = JT8000Map::lfoDestFromCC(value); setLFO1Destination((LFODestination)d); JT_CC_LOG("[CC %u:%s] LFO1 Dest = %d\n", control, ccName, d); } break;
        case CC::LFO1_WAVEFORM:    { WaveformType t = waveformFromCC(value); setLFO1Waveform((int)t); JT_CC_LOG("[CC %u:%s] LFO1 Wave -> %s (%d)\n", control, ccName, waveformShortName(t), (int)t); } break;

        // ------------------- LFO2 -------------------
        case CC::LFO2_FREQ:        { float hz = JT8000Map::cc_to_lfo_hz(value); setLFO2Frequency(hz); JT_CC_LOG("[CC %u:%s] LFO2 Freq = %.4f Hz\n", control, ccName, hz); } break;
        case CC::LFO2_DEPTH:       { setLFO2Amount(norm); JT_CC_LOG("[CC %u:%s] LFO2 Depth = %.3f\n", control, ccName, norm); } break;
        case CC::LFO2_DESTINATION: { int d = JT8000Map::lfoDestFromCC(value); setLFO2Destination((LFODestination)d); JT_CC_LOG("[CC %u:%s] LFO2 Dest = %d\n", control, ccName, d); } break;
        case CC::LFO2_WAVEFORM:    { WaveformType t = waveformFromCC(value); setLFO2Waveform((int)t); JT_CC_LOG("[CC %u:%s] LFO2 Wave -> %s (%d)\n", control, ccName, waveformShortName(t), (int)t); } break;

        
        // ============================================================================
        // JPFX CC HANDLERS (add to handleControlChange switch)
        // ============================================================================


        // --- JPFX Tone Control ---
        case CC::FX_BASS_GAIN: {
            float dB = (norm * 24.0f) - 12.0f; // 0..1 → -12..+12 dB
            setFXBassGain(dB);
            JT_CC_LOG("[CC %u:%s] Bass = %.1f dB\n", control, ccName, dB);
        } break;

        case CC::FX_TREBLE_GAIN: {
            float dB = (norm * 24.0f) - 12.0f; // 0..1 → -12..+12 dB
            setFXTrebleGain(dB);
            JT_CC_LOG("[CC %u:%s] Treble = %.1f dB\n", control, ccName, dB);
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
            JT_CC_LOG("[CC %u:%s] Mod Effect = %d (%s)\n", 
                    control, ccName, variation, getFXModEffectName());
        } break;

        case CC::FX_MOD_MIX: {
            setFXModMix(norm);
            JT_CC_LOG("[CC %u:%s] Mod Mix = %.3f\n", control, ccName, norm);
        } break;

        case CC::FX_MOD_RATE: {
            float hz = norm * 20.0f; // 0..1 → 0..20 Hz
            setFXModRate(hz);
            JT_CC_LOG("[CC %u:%s] Mod Rate = %.2f Hz\n", control, ccName, hz);
        } break;

        case CC::FX_MOD_FEEDBACK: {
            // Map CC 0..127 to -1..0.99 (0 = use preset)
            float fb = -1.0f;
            if (value > 0) {
                fb = ((value - 1) / 126.0f) * 0.99f;
            }
            setFXModFeedback(fb);
            JT_CC_LOG("[CC %u:%s] Mod FB = %.3f\n", control, ccName, fb);
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
            JT_CC_LOG("[CC %u:%s] Delay Effect = %d (%s)\n", 
                    control, ccName, variation, getFXDelayEffectName());
        } break;

        case CC::FX_JPFX_DELAY_MIX: {
            setFXDelayMix(norm);
            JT_CC_LOG("[CC %u:%s] Delay Mix = %.3f\n", control, ccName, norm);
        } break;

        case CC::FX_JPFX_DELAY_FEEDBACK: {
            // Map CC 0..127 to -1..0.99 (0 = use preset)
            float fb = -1.0f;
            if (value > 0) {
                fb = ((value - 1) / 126.0f) * 0.99f;
            }
            setFXDelayFeedback(fb);
            JT_CC_LOG("[CC %u:%s] Delay FB = %.3f\n", control, ccName, fb);
        } break;

        case CC::FX_JPFX_DELAY_TIME: {
            float ms = norm * 1500.0f; // 0..1 → 0..1500 ms
            setFXDelayTime(ms);
            JT_CC_LOG("[CC %u:%s] Delay Time = %.1f ms\n", control, ccName, ms);
        } break;

        // --- JPFX Dry Mix ---
        case CC::FX_DRY_MIX: {
            setFXDryMix(norm);
            JT_CC_LOG("[CC %u:%s] Dry Mix = %.3f\n", control, ccName, norm);
        } break;

        // ======================================================================
        // REVERB CCs — moved to GlobalFX (Phase 3).
        //
        // LayerManager::handleControlChange intercepts these before they
        // ever reach SynthEngine and routes them to the shared GlobalFX
        // instance. If execution somehow gets here (e.g. a preset loader
        // calls engine.handleControlChange() directly bypassing LayerManager),
        // these CCs are silently ignored — the engine has no reverb to set.
        //
        // Preset loaders that want to restore reverb state should push it
        // into LayerManager::getGlobalFX() directly. See Performance::applyTo.
        // ======================================================================
        case CC::FX_REVERB_SIZE:
        case CC::FX_REVERB_DAMP:
        case CC::FX_REVERB_LODAMP:
        case CC::FX_REVERB_MIX:
        case CC::FX_REVERB_BYPASS:
        case CC::FX_REVERB_SHIMMER:
        case CC::FX_REVERB_FREEZE:
        case CC::FX_REVERB_LOWPASS:
        case CC::FX_REVERB_HIPASS:
            // no-op — handled by LayerManager / GlobalFX
            break;

        case CC::FX_JPFX_MIX: {
            // FX_JPFX_MIX controls the JPFX output level
            float mix = norm;
            setFXJPFXMix(mix, mix);
            JT_CC_LOG("[CC %u:%s] JPFX Mix = %.3f\n", control, ccName, mix);
        } break;


        // ------------------- Supersaw / DC / Ring -------------------
        case CC::SUPERSAW1_DETUNE: { setSupersawDetune(0, norm); JT_CC_LOG("[CC %u:%s] Supersaw1 Detune = %.3f\n", control, ccName, norm); } break;
        case CC::SUPERSAW1_MIX:    { setSupersawMix(0, norm);    JT_CC_LOG("[CC %u:%s] Supersaw1 Mix    = %.3f\n", control, ccName, norm); } break;
        case CC::SUPERSAW2_DETUNE: { setSupersawDetune(1, norm); JT_CC_LOG("[CC %u:%s] Supersaw2 Detune = %.3f\n", control, ccName, norm); } break;
        case CC::SUPERSAW2_MIX:    { setSupersawMix(1, norm);    JT_CC_LOG("[CC %u:%s] Supersaw2 Mix    = %.3f\n", control, ccName, norm); } break;

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
            JT_CC_LOG("[CC %u:%s] Osc1 Freq DC %.0f semitones (amp %.4f)\n",
                    control, ccName, norm * DC_PITCH_MAX_SEMITONES, dcAmp);
        } break;
        case CC::OSC1_SHAPE_DC: { setOsc1ShapeDcAmp(norm); JT_CC_LOG("[CC %u:%s] Osc1 Shape DC = %.3f\n", control, ccName, norm); } break;
        case CC::OSC2_FREQ_DC: {
            const float dcAmp = norm * DC_PITCH_MAX_SEMITONES * FM_SEMITONE_SCALE;
            setOsc2FrequencyDcAmp(dcAmp);
            JT_CC_LOG("[CC %u:%s] Osc2 Freq DC %.0f semitones (amp %.4f)\n",
                    control, ccName, norm * DC_PITCH_MAX_SEMITONES, dcAmp);
        } break;
        case CC::OSC2_SHAPE_DC: { setOsc2ShapeDcAmp(norm); JT_CC_LOG("[CC %u:%s] Osc2 Shape DC = %.3f\n", control, ccName, norm); } break;

        case CC::RING1_MIX: { setRing1Mix(norm); JT_CC_LOG("[CC %u:%s] Ring1 Mix = %.3f\n", control, ccName, norm); } break;
        case CC::RING2_MIX: { setRing2Mix(norm); JT_CC_LOG("[CC %u:%s] Ring2 Mix = %.3f\n", control, ccName, norm); } break;

        // ------------------- Cross Modulation & Oscillator Sync -------------------
        case CC::OSC_CROSS_MOD_DEPTH: {
#if JT_OPT_OSC_SYNC
            const float depth = crossModDepthFromCC(value);
            setCrossModDepth(depth);
            JT_CC_LOG("[CC %u:%s] XMod Depth = %.4f (CC %u)\n", control, ccName, depth, value);
#endif
        } break;

        case CC::OSC_SYNC_ENABLE: {
#if JT_OPT_OSC_SYNC
            const bool enabled = (value > 0);
            setSyncEnabled(enabled);
            JT_CC_LOG("[CC %u:%s] Osc Sync = %s\n", control, ccName, enabled ? "ON" : "OFF");
#endif
        } break;

        // ------------------- Arbitrary waveform bank selection -------------------
        case CC::OSC1_ARB_BANK: {
            // Map CC value (0..127) evenly across number of banks
            const uint8_t numBanks = static_cast<uint8_t>(ArbBank::BwTri) + 1;
            uint8_t bankIdx = (static_cast<uint16_t>(value) * numBanks) / 128;
            if (bankIdx >= numBanks) bankIdx = numBanks - 1;
            ArbBank bank = static_cast<ArbBank>(bankIdx);
            setOsc1ArbBank(bank);
            JT_CC_LOG("[CC %u:%s] OSC1 Bank -> %s (%u)\n", control, ccName, akwf_bankName(bank), bankIdx);
        } break;

        case CC::OSC2_ARB_BANK: {
            const uint8_t numBanks = static_cast<uint8_t>(ArbBank::BwTri) + 1;
            uint8_t bankIdx = (static_cast<uint16_t>(value) * numBanks) / 128;
            if (bankIdx >= numBanks) bankIdx = numBanks - 1;
            ArbBank bank = static_cast<ArbBank>(bankIdx);
            setOsc2ArbBank(bank);
            JT_CC_LOG("[CC %u:%s] OSC2 Bank -> %s (%u)\n", control, ccName, akwf_bankName(bank), bankIdx);
        } break;

        // ------------------- Arbitrary waveform table index ----------------------
        case CC::OSC1_ARB_INDEX: {
            uint16_t count = akwf_bankCount(_patch.osc1ArbBank);
            uint16_t idx = 0;
            if (count > 0) {
                idx = (static_cast<uint16_t>(value) * count) / 128;
                if (idx >= count) idx = count - 1;
            }
            setOsc1ArbIndex(idx);
            JT_CC_LOG("[CC %u:%s] OSC1 Table -> %u/%u\n", control, ccName, idx, count);
        } break;

        case CC::OSC2_ARB_INDEX: {
            uint16_t count = akwf_bankCount(_patch.osc2ArbBank);
            uint16_t idx = 0;
            if (count > 0) {
                idx = (static_cast<uint16_t>(value) * count) / 128;
                if (idx >= count) idx = count - 1;
            }
            setOsc2ArbIndex(idx);
            JT_CC_LOG("[CC %u:%s] OSC2 Table -> %u/%u\n", control, ccName, idx, count);
        } break;

        // ------------------- Glide -------------------
        case CC::GLIDE_ENABLE: {
            _patch.glideEnabled = (value >= 1);
            for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setGlideEnabled(_patch.glideEnabled);
            JT_CC_LOG("[CC %u:%s] Glide Enabled = %d\n", control, ccName, (int)_patch.glideEnabled);
        } break;

        case CC::GLIDE_TIME: {
            float ms = JT8000Map::cc_to_time_ms(value);
            _patch.glideTimeMs = ms;
            for (uint8_t i = _firstVoice; i < _firstVoice + _voiceCount; ++i) _voices[i].setGlideTime(ms);
            JT_CC_LOG("[CC %u:%s] Glide Time = %.2f ms\n", control, ccName, ms);
        } break;

        //AMP_MOD_FIXED_LEVEL
        case CC::AMP_MOD_FIXED_LEVEL: { SetAmpModFixedLevel(norm); JT_CC_LOG("[CC %u:%s] Amp mod fixed level = %.3f\n", control, ccName, norm); } break;

case CC::BPM_CLOCK_SOURCE: {
    // 0-63 = Internal, 64-127 = External
    bool useExternal = (value >= 64);
    if (_bpmClock) {
        _bpmClock->setClockSource(useExternal ? 
            ClockSource::CLOCK_EXTERNAL_MIDI : 
            ClockSource::CLOCK_INTERNAL);
        JT_CC_LOG("[CC %u:%s] Clock Source = %s\n", 
                control, ccName, useExternal ? "EXTERNAL" : "INTERNAL");
    }
    
    break;
}

case CC::BPM_INTERNAL_TEMPO: {
    // 0-127 → 40-300 BPM
    float bpm = 40.0f + (value / 127.0f) * (300.0f - 40.0f);
    if (_bpmClock) {
        _bpmClock->setInternalBPM(bpm);
        JT_CC_LOG("[CC %u:%s] Internal BPM = %.1f\n", control, ccName, bpm);
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
    JT_CC_LOG("[CC %u:%s] LFO1 Timing = %s\n", 
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
    JT_CC_LOG("[CC %u:%s] LFO2 Timing = %s\n", 
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
    JT_CC_LOG("[CC %u:%s] Delay Timing = %s\n", 
            control, ccName, TimingModeNames[(int)mode]);
    break;
}



        // =================== NEW: LFO per-destination depths ===================
        // Each CC maps to a 0..1 depth for a specific LFO→destination lane.
        // Final mixer gain = masterAmount * depthScalar.

        case CC::LFO1_PITCH_DEPTH:  { setLFO1PitchDepth(norm);  JT_CC_LOG("[CC %u] LFO1 Pitch depth %.3f\n",  control, norm); } break;
        case CC::LFO1_FILTER_DEPTH: { setLFO1FilterDepth(norm); JT_CC_LOG("[CC %u] LFO1 Filter depth %.3f\n", control, norm); } break;
        case CC::LFO1_PWM_DEPTH:    { setLFO1PWMDepth(norm);    JT_CC_LOG("[CC %u] LFO1 PWM depth %.3f\n",    control, norm); } break;
        case CC::LFO1_AMP_DEPTH:    { setLFO1AmpDepth(norm);    JT_CC_LOG("[CC %u] LFO1 Amp depth %.3f\n",    control, norm); } break;
        case CC::LFO1_DELAY:
        {   // CC 0-127 → 0-4000 ms delay before LFO reaches full depth
            const float ms = norm * 4000.0f;
            setLFO1Delay(ms);
            JT_CC_LOG("[CC %u] LFO1 Delay %.0f ms\n", control, ms);
        } break;

        case CC::LFO2_PITCH_DEPTH:  { setLFO2PitchDepth(norm);  JT_CC_LOG("[CC %u] LFO2 Pitch depth %.3f\n",  control, norm); } break;
        case CC::LFO2_FILTER_DEPTH: { setLFO2FilterDepth(norm); JT_CC_LOG("[CC %u] LFO2 Filter depth %.3f\n", control, norm); } break;
        case CC::LFO2_PWM_DEPTH:    { setLFO2PWMDepth(norm);    JT_CC_LOG("[CC %u] LFO2 PWM depth %.3f\n",    control, norm); } break;
        case CC::LFO2_AMP_DEPTH:    { setLFO2AmpDepth(norm);    JT_CC_LOG("[CC %u] LFO2 Amp depth %.3f\n",    control, norm); } break;
        case CC::LFO2_DELAY:
        {   const float ms = norm * 4000.0f;
            setLFO2Delay(ms);
            JT_CC_LOG("[CC %u] LFO2 Delay %.0f ms\n", control, ms);
        } break;

        // =================== NEW: Pitch envelope ===================
        // ADSR times share the same JT8000Map::cc_to_time_ms_ms() mapping as amp/filter envs.
        // DEPTH is bipolar: CC64 = 0 semitones; 0 = -24; 127 = +24.

        case CC::PITCH_ENV_ATTACK:  { setPitchEnvAttack(JT8000Map::cc_to_time_ms(value));  JT_CC_LOG("[CC %u] PEnv Attack %.1f ms\n",  control, JT8000Map::cc_to_time_ms(value)); } break;
        case CC::PITCH_ENV_DECAY:   { setPitchEnvDecay(JT8000Map::cc_to_time_ms(value));   JT_CC_LOG("[CC %u] PEnv Decay %.1f ms\n",   control, JT8000Map::cc_to_time_ms(value)); } break;
        case CC::PITCH_ENV_SUSTAIN: { setPitchEnvSustain(norm);            JT_CC_LOG("[CC %u] PEnv Sustain %.3f\n",    control, norm);            } break;
        case CC::PITCH_ENV_RELEASE: { setPitchEnvRelease(JT8000Map::cc_to_time_ms(value)); JT_CC_LOG("[CC %u] PEnv Release %.1f ms\n", control, JT8000Map::cc_to_time_ms(value)); } break;
        case CC::PITCH_ENV_DEPTH:
        {   // Bipolar: CC 64 = 0 semis, 0 = -24, 127 = +24
            const float semis = ((float)value - 64.0f) * (24.0f / 64.0f);
            setPitchEnvDepth(semis);
            JT_CC_LOG("[CC %u] PEnv Depth %.1f semitones\n", control, semis);
        } break;

        // =================== Envelope curve exponents ===================
        // Internal CCs (147–155) — never transmitted on MIDI wire.
        // Arrive from TFT knob via setCC(); SysEx path uses direct float
        // setters via SysExAdapter::_handleSyxOnlySet (no CC quantisation).

        case CC::AMP_ATTACK_CURVE: {
            setAmpAttackCurve(JT8000Map::cc_to_curve(value));
            JT_CC_LOG("[CC %u] Amp Atk Curve = %.2f\n", control, JT8000Map::cc_to_curve(value));
        } break;
        case CC::AMP_DECAY_CURVE: {
            setAmpDecayCurve(JT8000Map::cc_to_curve(value));
            JT_CC_LOG("[CC %u] Amp Dec Curve = %.2f\n", control, JT8000Map::cc_to_curve(value));
        } break;
        case CC::AMP_RELEASE_CURVE: {
            setAmpReleaseCurve(JT8000Map::cc_to_curve(value));
            JT_CC_LOG("[CC %u] Amp Rel Curve = %.2f\n", control, cc_to_curve(value));
        } break;
        case CC::FILTER_ATTACK_CURVE: {
            setFilterAttackCurve(JT8000Map::cc_to_curve(value));
            JT_CC_LOG("[CC %u] Flt Atk Curve = %.2f\n", control, JT8000Map::cc_to_curve(value));
        } break;
        case CC::FILTER_DECAY_CURVE: {
            setFilterDecayCurve(JT8000Map::cc_to_curve(value));
            JT_CC_LOG("[CC %u] Flt Dec Curve = %.2f\n", control, JT8000Map::cc_to_curve(value));
        } break;
        case CC::FILTER_RELEASE_CURVE: {
            setFilterReleaseCurve(JT8000Map::cc_to_curve(value));
            JT_CC_LOG("[CC %u] Flt Rel Curve = %.2f\n", control, JT8000Map::cc_to_curve(value));
        } break;
        case CC::PITCH_ATTACK_CURVE: {
            setPitchEnvAttackCurve(JT8000Map::cc_to_curve(value));
            JT_CC_LOG("[CC %u] Pit Atk Curve = %.2f\n", control, JT8000Map::cc_to_curve(value));
        } break;
        case CC::PITCH_DECAY_CURVE: {
            setPitchEnvDecayCurve(JT8000Map::cc_to_curve(value));
            JT_CC_LOG("[CC %u] Pit Dec Curve = %.2f\n", control, JT8000Map::cc_to_curve(value));
        } break;
        case CC::PITCH_RELEASE_CURVE: {
            setPitchEnvReleaseCurve(JT8000Map::cc_to_curve(value));
            JT_CC_LOG("[CC %u] Pit Rel Curve = %.2f\n", control, JT8000Map::cc_to_curve(value));
        } break;

        // =================== NEW: Velocity sensitivity ===================
        case CC::VELOCITY_AMP_SENS:    { setVelocityAmpSens(norm);    JT_CC_LOG("[CC %u] Vel Amp Sens %.3f\n",    control, norm); } break;
        case CC::VELOCITY_FILTER_SENS: { setVelocityFilterSens(norm); JT_CC_LOG("[CC %u] Vel Filter Sens %.3f\n", control, norm); } break;
        case CC::VELOCITY_ENV_SENS:    { setVelocityEnvSens(norm);    JT_CC_LOG("[CC %u] Vel Env Sens %.3f\n",    control, norm); } break;

        // PITCH_BEND_RANGE: CC 0..127 → 0..PITCH_BEND_MAX_SEMITONES (24).
        // Default = 2 semitones (standard MIDI keyboard).
        // Setting to 12 gives a full-octave wheel; 24 gives 2-octave.
        case CC::PITCH_BEND_RANGE: {
            const float rangeSemis = norm * PITCH_BEND_MAX_SEMITONES;
            setPitchBendRange(rangeSemis);
            JT_CC_LOG("[CC %u:%s] Bend range = ±%.1f semitones\n", control, ccName, rangeSemis);
        } break;

        // ------------------- Poly / Mono / Unison -------------------
        // CC 0..42   = POLY,  43..84 = MONO,  85..127 = UNISON
        case CC::POLY_MODE: {
            PolyMode pm = (value <= 42) ? PolyMode::POLY :
                          (value <= 84) ? PolyMode::MONO : PolyMode::UNISON;
            setPolyMode(pm);
            JT_CC_LOG("[CC %u:%s] → %s\n", control, ccName,
                pm == PolyMode::POLY ? "POLY" : pm == PolyMode::MONO ? "MONO" : "UNISON");
        } break;

        case CC::UNISON_DETUNE: {
            setUnisonDetune(norm);
            JT_CC_LOG("[CC %u:%s] Unison detune = %.3f\n", control, ccName, norm);
        } break;

        // ------------------- Drive / Saturation -------------------
        // Maps 0-127 to drive amount. 0=bypass, 1-63=soft clip, 64-127=hard clip.
        // Passed directly to AudioEffectJPFX which interprets the mode internally.
        case CC::FX_DRIVE: {
            const float driveNorm = norm;  // 0..1
            _fxChain.setDrive(driveNorm);
            JT_CC_LOG("[CC %u:%s] Drive = %.3f\n", control, ccName, driveNorm);
        } break;


 // ─────────────────────────────────────────────────────────────
        // Step Sequencer
        // ─────────────────────────────────────────────────────────────
        case CC::SEQ_ENABLE: {
            _audio.seq1.setEnabled(value >= 64);
            JT_CC_LOG("[CC %u] Seq enable = %s\n", control, value >= 64 ? "ON" : "OFF");
        } break;

        case CC::SEQ_STEPS: {
            int steps = 1 + (value * 15 / 127);  // 0-127 → 1-16
            _audio.seq1.setStepCount(steps);
            JT_CC_LOG("[CC %u] Seq steps = %d\n", control, steps);
        } break;

        case CC::SEQ_GATE_LENGTH: {
            _audio.seq1.setGateLength(norm);
            JT_CC_LOG("[CC %u] Seq gate = %.0f%%\n", control, norm * 100.0f);
        } break;

        case CC::SEQ_SLIDE: {
            _audio.seq1.setSlide(norm);
            JT_CC_LOG("[CC %u] Seq slide = %.0f%%\n", control, norm * 100.0f);
        } break;

        case CC::SEQ_DIRECTION: {
            int dir = (value * 3) / 127;  // 0-127 → 0-3
            _audio.seq1.setDirection(static_cast<SeqDirection>(dir));
            JT_CC_LOG("[CC %u] Seq dir = %d (%s)\n", control, dir, SeqDirectionNames[dir]);
        } break;

        case CC::SEQ_RATE: {
            // Exponential mapping: 0.1 Hz to 20 Hz
            float hz = 0.1f * powf(200.0f, norm);
            _audio.seq1.setRate(hz);
            JT_CC_LOG("[CC %u] Seq rate = %.2f Hz\n", control, hz);
        } break;

        case CC::SEQ_DEPTH: {
            // Bipolar depth: CC 0 = -1.0, CC 64 = 0.0, CC 127 = +1.0
            // Dead-zone at CC 64 to ensure exact zero (avoids float midpoint error)
            float bipolar;
            if (value == 64) {
                bipolar = 0.0f;
            } else {
                bipolar = (static_cast<float>(value) / 127.0f) * 2.0f - 1.0f;
            }
            _audio.seq1.setDepth(bipolar);
            JT_CC_LOG("[CC %u] Seq depth = %.3f (bipolar)\n", control, bipolar);
        } break;

        case CC::SEQ_DESTINATION: {
            int dest = (value * 4) / 127;  // 0-127 → 0-4 (None/Pitch/Filter/PWM/Amp)
            dest = constrain(dest, 0, NUM_LFO_DESTS - 1);
            _patch.seqDestination = dest;
            JT_CC_LOG("[CC %u] Seq dest = %d (%s)\n", control, dest, LFODestNames[dest]);
        } break;

        case CC::SEQ_RETRIGGER: {
            _audio.seq1.setRetrigger(value >= 64);
            JT_CC_LOG("[CC %u] Seq retrigger = %s\n", control, value >= 64 ? "ON" : "OFF");
        } break;

        case CC::SEQ_STEP_SELECT: {
            _audio.seqSelectedStep = constrain(value, 0, SEQ_MAX_STEPS - 1);
            JT_CC_LOG("[CC %u] Seq step select = %d\n", control, _audio.seqSelectedStep);
        } break;

        case CC::SEQ_STEP_VALUE: {
            _audio.seq1.setStepValue(_audio.seqSelectedStep, value);
            JT_CC_LOG("[CC %u] Seq step[%d] = %d\n", control, _audio.seqSelectedStep, value);
        } break;

case CC::SEQ_TIMING_MODE: {
    // Divide CC 0-127 into 12 equal bands (~10-11 values each).
    // This matches the JUCE bucket-midpoint encoding AND the HTML editor
    // direct-index encoding for values 0-11. Must mirror LFO/Delay handling.
    TimingMode mode;
    if      (value <=  10) mode = TimingMode::TIMING_FREE;
    else if (value <=  21) mode = TimingMode::TIMING_4_BARS;
    else if (value <=  32) mode = TimingMode::TIMING_2_BARS;
    else if (value <=  43) mode = TimingMode::TIMING_1_BAR;
    else if (value <=  54) mode = TimingMode::TIMING_1_2;
    else if (value <=  65) mode = TimingMode::TIMING_1_4;
    else if (value <=  76) mode = TimingMode::TIMING_1_8;
    else if (value <=  87) mode = TimingMode::TIMING_1_16;
    else if (value <=  98) mode = TimingMode::TIMING_1_32;
    else if (value <= 109) mode = TimingMode::TIMING_1_4T;
    else if (value <= 120) mode = TimingMode::TIMING_1_8T;
    else                   mode = TimingMode::TIMING_1_16T;

    _audio.seq1.setTimingMode(mode);

    // When switching INTO a sync mode, apply the current BPM duration
    // immediately rather than waiting for the next updateBPMSync() cycle.
    if (mode != TimingMode::TIMING_FREE && _bpmClock) {
        _audio.seq1.updateFromBPMClock(*_bpmClock);
    }

    JT_CC_LOG("[CC %u] Seq timing = %s\n", control, TimingModeNames[(int)mode]);
} break;      

        // ------------------- Fallback -------------------
        default:
            JT_CC_LOG("[CC %u:%s] Unmapped value=%u\n", control, ccName, value);
            break;
    }

    // Keep raw CC cache in sync — lets the UI read back any value via getCC().
    // POLY_MODE(128) and UNISON_DETUNE(129) are handled by dedicated backing
    // fields; getCC() encodes them on demand. Do not write _patch.ccState for those.
    // FX_DRIVE(130) and future internal CCs above 127 use _patch.ccState directly.
    if (control < 128 || (control >= 130 && control < 160)) {
        _patch.ccState[control] = value;
    }

    // Notify listener (USB MIDI echo + TFT dirty flag) for EVERY CC change.
    if (_notify) _notify(control, value);
}
