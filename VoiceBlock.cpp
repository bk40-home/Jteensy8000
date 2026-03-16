// VoiceBlock.cpp — JT-8000 voice implementation
// =============================================================================
// See VoiceBlock.h for architecture notes.
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
    _voiceMixer.gain(2, _kMaxMixerGain);            // Sub osc
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
    _isActive      = true;
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

    // Trigger all envelopes
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
    _isActive = false;
    _filterEnvelope.noteOff();
    _ampEnvelope.noteOff();
    _pitchEnvelope.noteOff();
}

// =============================================================================
// OSCILLATOR CONFIGURATION
// =============================================================================

void VoiceBlock::setOsc1Waveform(int waveform) { _osc1.setWaveformType(waveform); }
void VoiceBlock::setOsc2Waveform(int waveform) { _osc2.setWaveformType(waveform); }

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
    //_voiceMixer.gain(2, _clampedLevel(_subMix));
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
void VoiceBlock::setOsc1Detune(float hertz)           { _osc1.setDetune(hertz); }
void VoiceBlock::setOsc2Detune(float hertz)           { _osc2.setDetune(hertz); }
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
// UPDATE — Only glide needs periodic processing
// =========================================================================

void VoiceBlock::update() {
    if (_isActive) {
        _osc1.update();  // Only does work if glide is active
        _osc2.update();
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
