// VoiceBlock.cpp — JT-8000 voice implementation
// =============================================================================
// See VoiceBlock.h for architecture notes.
//
// VOICE ACTIVITY:
//   noteOn()  → triggers all envelopes; voice becomes audio-active.
//   noteOff() → tells envelopes to enter release; voice remains audio-active
//               until the amp envelope reaches zero.
//   isAudioActive() queries the amp envelope hardware — no boolean to maintain.
// =============================================================================

// Do NOT include "synth_waveform.h" directly — that loads the library version
// and bypasses the JT fork guard in Audio.h.  VoiceBlock.h already pulls in
// Audio.h which includes the local Synth_Waveform.h fork first.
#include "VoiceBlock.h"

VoiceBlock::VoiceBlock()
    : _osc1(true)    // OSC1: supersaw enabled
    , _osc2(false)   // OSC2: supersaw disabled (saves CPU)
{
    // =========================================================================
    // AUDIO CONNECTIONS — voice signal path
    // =========================================================================
    _patchCables[0]  = new AudioConnection(_osc1.output(), 0, _oscMixer, 0);
    _patchCables[1]  = new AudioConnection(_osc2.output(), 0, _oscMixer, 1);
    _patchCables[2]  = new AudioConnection(_osc1.output(), 0, _ring1, 0);
    _patchCables[3]  = new AudioConnection(_osc2.output(), 0, _ring1, 1);
    _patchCables[4]  = new AudioConnection(_osc1.output(), 0, _ring2, 0);
    _patchCables[5]  = new AudioConnection(_osc2.output(), 0, _ring2, 1);
    _patchCables[6]  = new AudioConnection(_ring1, 0, _oscMixer, 2);
    _patchCables[7]  = new AudioConnection(_ring2, 0, _oscMixer, 3);
    _patchCables[8]  = new AudioConnection(_oscMixer, 0, _voiceMixer, 0);
    _patchCables[9]  = new AudioConnection(_subOsc.output(), 0, _voiceMixer, 2);
    _patchCables[10] = new AudioConnection(_noise, 0, _voiceMixer, 3);
    _patchCables[11] = new AudioConnection(_voiceMixer, 0, _filter.input(), 0);
    _patchCables[12] = new AudioConnection(_filter.output(), 0, _ampEnvelope.input(), 0);
    _patchCables[13] = new AudioConnection(_filter.envmod(), 0, _filterEnvelope.input(), 0);
    _patchCables[14] = new AudioConnection(_filterEnvelope.output(), 0, _filter.modMixer(), 1);

    // Pitch envelope DC source → pitch envelope ADSR
    _pitchEnvDc.amplitude(0.0f);
    _patchCables[15] = new AudioConnection(_pitchEnvDc, 0, _pitchEnvelope.input(), 0);

    // =========================================================================
    // MIXER GAINS
    // =========================================================================
    _oscMixer.gain(0, _kMaxMixerGain);  // OSC1
    _oscMixer.gain(1, _kMaxMixerGain);  // OSC2
    _oscMixer.gain(2, 0.0f);            // Ring1
    _oscMixer.gain(3, 0.0f);            // Ring2

    _voiceMixer.gain(0, _kMaxMixerGain);  // Osc mixer
    _voiceMixer.gain(1, 0.0f);            // Unused
    _voiceMixer.gain(2, _kMaxMixerGain);  // Sub osc
    _voiceMixer.gain(3, 0.0f);            // Noise

    // =========================================================================
    // DEFAULT OSCILLATOR STATE
    // =========================================================================
    _subOsc.setWaveform(WAVEFORM_SINE);
    _subOsc.setAmplitude(0.0f);
    _subOsc.setFrequency(110.0f);
    _noise.amplitude(0.0f);

    _osc1.setWaveformType(WAVEFORM_SAWTOOTH);
    _osc2.setWaveformType(WAVEFORM_SAWTOOTH);
}

// =============================================================================
// NOTE ON
// =============================================================================

void VoiceBlock::noteOn(float frequency, float velocity) {
    _currentFreq   = frequency;
    _lastVelocity  = velocity;     // Cache for mono legato return

    // Velocity arrives already normalised 0.0–1.0 from the sketch.
    const float velNorm = velocity;

    // Velocity → amp level (0=flat, 1=full velocity tracking)
    const float velAmpScale = (1.0f - _velAmpSens) + (_velAmpSens * velNorm);

    // Velocity → filter cutoff offset (±3 octaves max)
    static constexpr float kVelFilterOctRange = 3.0f;
    const float cutoffOctOffset = _velFilterSens * (velNorm - 0.5f) * kVelFilterOctRange;
    _filter.setCutoff(_baseCutoff * powf(2.0f, cutoffOctOffset));

    // Velocity → filter env depth scaling
    const float envDepthScale = (1.0f - _velEnvSens) + (_velEnvSens * velNorm);
    _filter.setEnvModAmount(_baseFilterEnvAmount * envDepthScale);

    // Trigger oscillators with velocity-scaled amplitude
    _osc1.noteOn(frequency, velocity * velAmpScale);
    _osc2.noteOn(frequency, velocity * velAmpScale);
    _subOsc.setFrequency(frequency);

#if JT_OPT_OSC_SYNC
    // Forward note parameters to sync engine when active.
    // The sync engine maintains its own phase accumulators so it needs
    // the base frequency and amplitude independently of OscillatorBlock.
    if (_syncActive) {
        _syncEngine.setSlaveFrequency(frequency);
        _syncEngine.setMasterFrequency(frequency);
        _syncEngine.setSlaveAmplitude(velocity * velAmpScale);
        _syncEngine.setMasterAmplitude(velocity * velAmpScale);
    }
#endif

    // Trigger all envelopes — voice becomes audio-active immediately.
    // isAudioActive() will return true from this point until the amp
    // envelope completes its full release phase.
    _filterEnvelope.noteOn();
    _ampEnvelope.noteOn();
    _pitchEnvelope.noteOn();

    // Key tracking: compute filter cutoff modulation
    const float deltaOct   = log2f(frequency / 440.0f);
    const float octaveCtrl = _filter.getOctaveControl();
    float norm = (octaveCtrl > 0.0f) ? (deltaOct / octaveCtrl) : 0.0f;
    norm *= _filterKeyTrackAmount;
    norm = constrain(norm, -1.0f, 1.0f);
    _filter.setKeyTrackAmount(norm);
}

// =============================================================================
// NOTE OFF
// =============================================================================

void VoiceBlock::noteOff() {
    // Tell envelopes to begin their release phase.
    // The voice remains audio-active (isAudioActive() == true) until the amp
    // envelope output reaches zero.  Do NOT set any boolean here — the hardware
    // envelope is the single source of truth.
    _filterEnvelope.noteOff();
    _ampEnvelope.noteOff();
    _pitchEnvelope.noteOff();
}

// =============================================================================
// OSCILLATOR CONFIGURATION
// =============================================================================

void VoiceBlock::setOsc1Waveform(int waveform) {
    _osc1.setWaveformType(waveform);
#if JT_OPT_OSC_SYNC
    if (_syncActive) _syncEngine.setSlaveWaveform((short)waveform);
#endif
}
void VoiceBlock::setOsc2Waveform(int waveform) {
    _osc2.setWaveformType(waveform);
#if JT_OPT_OSC_SYNC
    if (_syncActive) _syncEngine.setMasterWaveform((short)waveform);
#endif
}

void VoiceBlock::setBaseFrequency(float frequency) {
    _osc1.noteOn(frequency, _osc1.getShapeDcAmp());  // Preserve current amp
    _osc2.noteOn(frequency, _osc2.getShapeDcAmp());
    _subOsc.setFrequency(frequency);
}

void VoiceBlock::setAmplitude(float amplitude) {
    _osc1.setAmplitude(amplitude);
    _osc2.setAmplitude(amplitude);
    _subOsc.setAmplitude(_subMix);
    _noise.amplitude(_noiseMix);
}

float VoiceBlock::_clampedLevel(float level) {
    return (level > _kMaxMixerGain) ? _kMaxMixerGain : level;
}

void VoiceBlock::setOscMix(float osc1Level, float osc2Level) {
    _osc1Level = osc1Level;
    _osc2Level = osc2Level;
    _oscMixer.gain(0, _clampedLevel(_osc1Level));
    _oscMixer.gain(1, _clampedLevel(_osc2Level));
}

void VoiceBlock::setOsc1Mix(float level) {
    _osc1Level = level;
    _oscMixer.gain(0, _clampedLevel(_osc1Level));
}

void VoiceBlock::setOsc2Mix(float level) {
    _osc2Level = level;
    _oscMixer.gain(1, _clampedLevel(_osc2Level));
}

void VoiceBlock::setRing1Mix(float level) {
    _ring1Level = level;
    _oscMixer.gain(2, _clampedLevel(_ring1Level));
}

void VoiceBlock::setRing2Mix(float level) {
    _ring2Level = level;
    _oscMixer.gain(3, _clampedLevel(_ring2Level));
}

void VoiceBlock::setSubMix(float level) {
    _subMix = level;
    _subOsc.setAmplitude(_subMix);
}

void VoiceBlock::setNoiseMix(float level) {
    _noiseMix = level;
    _noise.amplitude(_noiseMix);
    _voiceMixer.gain(3, _clampedLevel(_noiseMix));
}

// =========================================================================
// PITCH CONTROL — delegates to OscillatorBlock DC-based system
// =========================================================================

void VoiceBlock::setOsc1PitchOffset(float semitones) { _osc1.setPitchOffset(semitones); }
void VoiceBlock::setOsc2PitchOffset(float semitones) { _osc2.setPitchOffset(semitones); }
void VoiceBlock::setOsc1PitchBend(float semitones)   { _osc1.setPitchBend(semitones); }
void VoiceBlock::setOsc2PitchBend(float semitones)   { _osc2.setPitchBend(semitones); }
void VoiceBlock::setOsc1Detune(float semitones)       { _osc1.setDetune(semitones); }
void VoiceBlock::setOsc2Detune(float semitones)       { _osc2.setDetune(semitones); }
void VoiceBlock::setOsc1FineTune(float cents)         { _osc1.setFineTune(cents); }
void VoiceBlock::setOsc2FineTune(float cents)         { _osc2.setFineTune(cents); }

void VoiceBlock::setOsc1SupersawDetune(float amount) { _osc1.setSupersawDetune(amount); }
void VoiceBlock::setOsc2SupersawDetune(float amount) { _osc2.setSupersawDetune(amount); }
void VoiceBlock::setOsc1SupersawMix(float amount)    { _osc1.setSupersawMix(amount); }
void VoiceBlock::setOsc2SupersawMix(float amount)    { _osc2.setSupersawMix(amount); }

void VoiceBlock::setGlideEnabled(bool enabled) {
    _osc1.setGlideEnabled(enabled);
    _osc2.setGlideEnabled(enabled);
}

void VoiceBlock::setGlideTime(float milliseconds) {
    _osc1.setGlideTime(milliseconds);
    _osc2.setGlideTime(milliseconds);
}

void VoiceBlock::setOsc1ShapeDcAmp(float amplitude) { _osc1.setShapeDcAmp(amplitude); }
void VoiceBlock::setOsc2ShapeDcAmp(float amplitude) { _osc2.setShapeDcAmp(amplitude); }
void VoiceBlock::setOsc1FrequencyDcAmp(float amplitude) { _osc1.setExternalPitchDc(amplitude); }
void VoiceBlock::setOsc2FrequencyDcAmp(float amplitude) { _osc2.setExternalPitchDc(amplitude); }

void VoiceBlock::setSeqPitchOffset(float fmScaledOffset) {
    _osc1.setSeqPitchOffset(fmScaledOffset);
    _osc2.setSeqPitchOffset(fmScaledOffset);
}

// =========================================================================
// FILTER
// =========================================================================

void VoiceBlock::setFilterCutoff(float hertz) {
    _baseCutoff = hertz;
    _filter.setCutoff(hertz);
}

void VoiceBlock::setFilterResonance(float amount)   { _filter.setResonance(amount); }
void VoiceBlock::setFilterOctaveControl(float value) { _filter.setOctaveControl(value); }

void VoiceBlock::setFilterEnvAmount(float amount) {
    _filterEnvAmount     = amount;
    _baseFilterEnvAmount = amount;
    _filter.setEnvModAmount(amount);
}

void VoiceBlock::setFilterKeyTrackAmount(float amount) {
    _filterKeyTrackAmount = amount;
    if (_currentFreq > 0.0f) {
        const float deltaOct   = log2f(_currentFreq / 440.0f);
        const float octaveCtrl = _filter.getOctaveControl();
        float norm = (octaveCtrl > 0.0f) ? (deltaOct / octaveCtrl) : 0.0f;
        norm *= _filterKeyTrackAmount;
        norm = constrain(norm, -1.0f, 1.0f);
        _filter.setKeyTrackAmount(norm);
    }
}

void VoiceBlock::setMultimode(float amount)         { _multimode = amount; _filter.setMultimode(amount); }

void VoiceBlock::setSeqFilterOffset(float offset) {
    _filter.setSeqFilterOffset(offset);
}

void VoiceBlock::setTwoPole(bool enabled)           { _useTwoPole = enabled; _filter.setTwoPole(enabled); }
void VoiceBlock::setXpander4Pole(bool enabled)      { _xpander4Pole = enabled; _filter.setXpander4Pole(enabled); }
void VoiceBlock::setXpanderMode(uint8_t mode)       { _xpanderMode = mode; _filter.setXpanderMode(mode); }
void VoiceBlock::setBPBlend2Pole(bool enabled)      { _bpBlend2Pole = enabled; _filter.setBPBlend2Pole(enabled); }
void VoiceBlock::setPush2Pole(bool enabled)         { _push2Pole = enabled; _filter.setPush2Pole(enabled); }
void VoiceBlock::setResonanceModDepth(float depth)  { _resonanceModDepth = depth; _filter.setResonanceModDepth(depth); }
void VoiceBlock::setFilterEngine(uint8_t engine)    { _filter.setFilterEngine(engine); }
void VoiceBlock::setVAFilterType(VAFilterType type) { _filter.setVAFilterType(type); }

// =========================================================================
// ENVELOPES
// =========================================================================

void VoiceBlock::setAmpAttack(float ms)  { _ampEnvelope.setAttackTime(ms); }
void VoiceBlock::setAmpDecay(float ms)   { _ampEnvelope.setDecayTime(ms); }
void VoiceBlock::setAmpSustain(float s)  { _ampEnvelope.setSustainLevel(s); }
void VoiceBlock::setAmpRelease(float ms) { _ampEnvelope.setReleaseTime(ms); }
void VoiceBlock::setAmpADSR(float a, float d, float s, float r) {
    _ampEnvelope.setADSR(a, d, s, r);
}

void VoiceBlock::setFilterAttack(float ms)  { _filterEnvelope.setAttackTime(ms); }
void VoiceBlock::setFilterDecay(float ms)   { _filterEnvelope.setDecayTime(ms); }
void VoiceBlock::setFilterSustain(float s)  { _filterEnvelope.setSustainLevel(s); }
void VoiceBlock::setFilterRelease(float ms) { _filterEnvelope.setReleaseTime(ms); }
void VoiceBlock::setFilterADSR(float a, float d, float s, float r) {
    _filterEnvelope.setADSR(a, d, s, r);
}

// =========================================================================
// UPDATE — glide processing, runs while voice is audio-active
// =========================================================================

void VoiceBlock::update() {
    // Run oscillator updates (glide) while the voice is producing audio.
    // This covers the release phase too — glide may still be settling when
    // noteOff arrives.  isAudioActive() queries the amp envelope hardware.
    if (isAudioActive()) {
        _osc1.update();
        _osc2.update();
#if JT_OPT_OSC_SYNC
        // When sync is active, OscillatorBlock::update() still runs glide
        // on the internal oscillators (keeping their state current for when
        // sync is disabled).  Forward the glided base frequencies to the
        // sync engine so its phase accumulators track the same pitch.
        if (_syncActive) {
            _syncEngine.setSlaveFrequency(_osc1.getBaseFreq());
            _syncEngine.setMasterFrequency(_osc2.getBaseFreq());
        }
#endif
    }
}

// =========================================================================
// AUDIO OUTPUTS
// =========================================================================

AudioStream& VoiceBlock::output()              { return _ampEnvelope.output(); }
AudioMixer4& VoiceBlock::frequencyModMixerOsc1() { return _osc1.frequencyModMixer(); }
AudioMixer4& VoiceBlock::frequencyModMixerOsc2() { return _osc2.frequencyModMixer(); }
AudioMixer4& VoiceBlock::shapeModMixerOsc1()   { return _osc1.shapeModMixer(); }
AudioMixer4& VoiceBlock::shapeModMixerOsc2()   { return _osc2.shapeModMixer(); }
AudioMixer4& VoiceBlock::filterModMixer()      { return _filter.modMixer(); }

// =========================================================================
// PITCH ENVELOPE
// =========================================================================

void VoiceBlock::setPitchEnvAttack(float ms)     { _pitchEnvelope.setAttackTime(ms); }
void VoiceBlock::setPitchEnvDecay(float ms)      { _pitchEnvelope.setDecayTime(ms); }
void VoiceBlock::setPitchEnvSustain(float level) { _pitchEnvelope.setSustainLevel(level); }
void VoiceBlock::setPitchEnvRelease(float ms)    { _pitchEnvelope.setReleaseTime(ms); }

void VoiceBlock::setPitchEnvDepth(float semitones) {
    semitones = constrain(semitones, -24.0f, 24.0f);
    _pitchEnvDepth = semitones;
    // Encode depth into DC amplitude using FM scaling.
    // The envelope shapes this DC signal (0→1→sustain→0).
    // FM mixer slot 3 gain is 1.0 (pass-through).
    _pitchEnvDc.amplitude(semitones * FM_SEMITONE_SCALE);
}

AudioStream& VoiceBlock::pitchEnvOutput()       { return _pitchEnvelope.output(); }
AudioSynthWaveformDc& VoiceBlock::pitchEnvDcRef() { return _pitchEnvDc; }

// =========================================================================
// GETTERS
// =========================================================================

int   VoiceBlock::getOsc1Waveform()       const { return _osc1.getWaveform(); }
int   VoiceBlock::getOsc2Waveform()       const { return _osc2.getWaveform(); }
float VoiceBlock::getOsc1PitchOffset()    const { return _osc1.getPitchOffset(); }
float VoiceBlock::getOsc2PitchOffset()    const { return _osc2.getPitchOffset(); }
float VoiceBlock::getOsc1Detune()         const { return _osc1.getDetune(); }
float VoiceBlock::getOsc2Detune()         const { return _osc2.getDetune(); }
float VoiceBlock::getOsc1FineTune()       const { return _osc1.getFineTune(); }
float VoiceBlock::getOsc2FineTune()       const { return _osc2.getFineTune(); }
float VoiceBlock::getLastVelocity()       const { return _lastVelocity; }
float VoiceBlock::getOscMix1()            const { return _osc1Level; }
float VoiceBlock::getOscMix2()            const { return _osc2Level; }
float VoiceBlock::getSubMix()             const { return _subMix; }
float VoiceBlock::getNoiseMix()           const { return _noiseMix; }
float VoiceBlock::getOsc1SupersawDetune() const { return _osc1.getSupersawDetune(); }
float VoiceBlock::getOsc2SupersawDetune() const { return _osc2.getSupersawDetune(); }
float VoiceBlock::getOsc1SupersawMix()    const { return _osc1.getSupersawMix(); }
float VoiceBlock::getOsc2SupersawMix()    const { return _osc2.getSupersawMix(); }
float VoiceBlock::getOsc1ShapeDc()        const { return _osc1.getShapeDcAmp(); }
float VoiceBlock::getOsc2ShapeDc()        const { return _osc2.getShapeDcAmp(); }
float VoiceBlock::getOsc1FrequencyDc()    const { return _osc1.getExternalPitchDc(); }
float VoiceBlock::getOsc2FrequencyDc()    const { return _osc2.getExternalPitchDc(); }
bool  VoiceBlock::getGlideEnabled()       const { return _osc1.getGlideEnabled(); }
float VoiceBlock::getGlideTime()          const { return _osc1.getGlideTime(); }
float VoiceBlock::getRing1Mix()           const { return _ring1Level; }
float VoiceBlock::getRing2Mix()           const { return _ring2Level; }

float VoiceBlock::getFilterCutoff()       const { return _baseCutoff; }
float VoiceBlock::getFilterResonance()    const { return _filter.getResonance(); }
float VoiceBlock::getFilterOctaveControl() const { return _filter.getOctaveControl(); }
float VoiceBlock::getFilterEnvAmount()    const { return _filterEnvAmount; }
float VoiceBlock::getFilterKeyTrackAmount() const { return _filterKeyTrackAmount; }

float VoiceBlock::getAmpAttack()          const { return _ampEnvelope.getAttackTime(); }
float VoiceBlock::getAmpDecay()           const { return _ampEnvelope.getDecayTime(); }
float VoiceBlock::getAmpSustain()         const { return _ampEnvelope.getSustainLevel(); }
float VoiceBlock::getAmpRelease()         const { return _ampEnvelope.getReleaseTime(); }

float VoiceBlock::getFilterEnvAttack()    const { return _filterEnvelope.getAttackTime(); }
float VoiceBlock::getFilterEnvDecay()     const { return _filterEnvelope.getDecayTime(); }
float VoiceBlock::getFilterEnvSustain()   const { return _filterEnvelope.getSustainLevel(); }
float VoiceBlock::getFilterEnvRelease()   const { return _filterEnvelope.getReleaseTime(); }

// =========================================================================
// ARBITRARY WAVEFORM FORWARDING
// =========================================================================

void VoiceBlock::setOsc1ArbBank(ArbBank bank)    { _osc1.setArbBank(bank); }
void VoiceBlock::setOsc2ArbBank(ArbBank bank)    { _osc2.setArbBank(bank); }
void VoiceBlock::setOsc1ArbIndex(uint16_t index) { _osc1.setArbTableIndex(index); }
void VoiceBlock::setOsc2ArbIndex(uint16_t index) { _osc2.setArbTableIndex(index); }

// =========================================================================
// FEEDBACK OSCILLATION
// =========================================================================

void VoiceBlock::setOsc1FeedbackAmount(float amount) { _osc1.setFeedbackAmount(amount); }
void VoiceBlock::setOsc2FeedbackAmount(float amount) { _osc2.setFeedbackAmount(amount); }
void VoiceBlock::setOsc1FeedbackMix(float mix)       { _osc1.setFeedbackMix(mix); }
void VoiceBlock::setOsc2FeedbackMix(float mix)       { _osc2.setFeedbackMix(mix); }

float VoiceBlock::getOsc1FeedbackMix()    const { return _osc1.getFeedbackMix(); }
float VoiceBlock::getOsc2FeedbackMix()    const { return _osc2.getFeedbackMix(); }
float VoiceBlock::getOsc1FeedbackAmount() const { return _osc1.getFeedbackAmount(); }
float VoiceBlock::getOsc2FeedbackAmount() const { return _osc2.getFeedbackAmount(); }

// =========================================================================
// CROSS MODULATION & OSCILLATOR SYNC
// =========================================================================

void VoiceBlock::setCrossModDepth(float depth) {
    _crossModDepth = depth;
#if JT_OPT_OSC_SYNC
    if (_syncActive) {
        _syncEngine.setCrossModDepth(depth);
    }
    // When sync is off, cross-mod has no effect — it requires the sync
    // engine's per-sample FM injection.  Standalone cross-mod via a
    // pre-mixer on OscillatorBlock is a future enhancement.
#endif
}

float VoiceBlock::getCrossModDepth() const {
    return _crossModDepth;
}

bool VoiceBlock::getSyncEnabled() const {
#if JT_OPT_OSC_SYNC
    return _syncActive;
#else
    return false;
#endif
}

void VoiceBlock::setSyncEnabled(bool enabled) {
#if JT_OPT_OSC_SYNC
    if (enabled == _syncActive) return;   // No change — skip cable rebuild
    _syncActive = enabled;

    // ── Cable swap inside AudioNoInterrupts guard ───────────────────────
    // The Teensy audio scheduler traverses the connection graph in the ISR.
    // We must prevent ISR execution while deleting/creating AudioConnections
    // to avoid dangling pointer traversal.  The new/delete calls are fast
    // on Teensy 4.1 (<1 µs each) so the guard window is short.
    // ────────────────────────────────────────────────────────────────────
    AudioNoInterrupts();

    if (enabled) {
        // --- ENTERING SYNC MODE ---
        // The AudioSynthOscSync replaces both oscillator outputs.
        // Disconnect the normal osc → mixer and osc → ring mod paths,
        // then wire the sync engine's two outputs in their place.

        // 1. Delete normal osc → oscMixer connections
        delete _patchCables[0]; _patchCables[0] = nullptr;   // osc1 → oscMixer 0
        delete _patchCables[1]; _patchCables[1] = nullptr;   // osc2 → oscMixer 1

        // 2. Delete normal osc → ring mod connections
        delete _patchCables[2]; _patchCables[2] = nullptr;   // osc1 → ring1 in0
        delete _patchCables[3]; _patchCables[3] = nullptr;   // osc2 → ring1 in1
        delete _patchCables[4]; _patchCables[4] = nullptr;   // osc1 → ring2 in0
        delete _patchCables[5]; _patchCables[5] = nullptr;   // osc2 → ring2 in1

        // 3. Wire sync engine outputs → oscMixer
        _patchSyncSlaveToMix    = new AudioConnection(_syncEngine, 0, _oscMixer, 0);
        _patchSyncMasterToMix   = new AudioConnection(_syncEngine, 1, _oscMixer, 1);

        // 4. Wire sync engine outputs → ring modulators
        _patchSyncSlaveToRing1  = new AudioConnection(_syncEngine, 0, _ring1, 0);
        _patchSyncMasterToRing1 = new AudioConnection(_syncEngine, 1, _ring1, 1);
        _patchSyncSlaveToRing2  = new AudioConnection(_syncEngine, 0, _ring2, 0);
        _patchSyncMasterToRing2 = new AudioConnection(_syncEngine, 1, _ring2, 1);

        // 5. Wire FM modulation mixers → sync engine inputs
        //    The sync engine reads the same FM mixer outputs that the normal
        //    oscillators use, so LFO/pitch-env/bend all pass through.
        _patchSyncSlaveFM     = new AudioConnection(
            _osc1.frequencyModMixer(), 0, _syncEngine, 0);
        _patchSyncMasterFM    = new AudioConnection(
            _osc2.frequencyModMixer(), 0, _syncEngine, 1);

        // 6. Wire shape/PWM modulation mixers → sync engine inputs
        _patchSyncSlaveShape  = new AudioConnection(
            _osc1.shapeModMixer(), 0, _syncEngine, 2);
        _patchSyncMasterShape = new AudioConnection(
            _osc2.shapeModMixer(), 0, _syncEngine, 3);

        // 7. Configure sync engine with current oscillator state
        _syncEngine.setSyncEnabled(true);
        _syncEngine.setSlaveFrequency(_osc1.getBaseFreq());
        _syncEngine.setMasterFrequency(_osc2.getBaseFreq());
        _syncEngine.setSlaveAmplitude(1.0f);
        _syncEngine.setMasterAmplitude(1.0f);
        _syncEngine.setSlaveWaveform((short)_osc1.getWaveform());
        _syncEngine.setMasterWaveform((short)_osc2.getWaveform());
        _syncEngine.frequencyModulation(FM_OCTAVE_RANGE);
        _syncEngine.setCrossModDepth(_crossModDepth);

    } else {
        // --- LEAVING SYNC MODE ---
        // Delete all sync connections and restore the normal osc paths.

        // 1. Delete sync engine connections
        delete _patchSyncSlaveToMix;    _patchSyncSlaveToMix    = nullptr;
        delete _patchSyncMasterToMix;   _patchSyncMasterToMix   = nullptr;
        delete _patchSyncSlaveFM;       _patchSyncSlaveFM       = nullptr;
        delete _patchSyncMasterFM;      _patchSyncMasterFM      = nullptr;
        delete _patchSyncSlaveShape;    _patchSyncSlaveShape    = nullptr;
        delete _patchSyncMasterShape;   _patchSyncMasterShape   = nullptr;
        delete _patchSyncSlaveToRing1;  _patchSyncSlaveToRing1  = nullptr;
        delete _patchSyncMasterToRing1; _patchSyncMasterToRing1 = nullptr;
        delete _patchSyncSlaveToRing2;  _patchSyncSlaveToRing2  = nullptr;
        delete _patchSyncMasterToRing2; _patchSyncMasterToRing2 = nullptr;

        // 2. Restore normal osc → oscMixer connections
        _patchCables[0] = new AudioConnection(_osc1.output(), 0, _oscMixer, 0);
        _patchCables[1] = new AudioConnection(_osc2.output(), 0, _oscMixer, 1);

        // 3. Restore normal osc → ring mod connections
        _patchCables[2] = new AudioConnection(_osc1.output(), 0, _ring1, 0);
        _patchCables[3] = new AudioConnection(_osc2.output(), 0, _ring1, 1);
        _patchCables[4] = new AudioConnection(_osc1.output(), 0, _ring2, 0);
        _patchCables[5] = new AudioConnection(_osc2.output(), 0, _ring2, 1);

        _syncEngine.setSyncEnabled(false);
    }

    AudioInterrupts();
#endif // JT_OPT_OSC_SYNC
}
