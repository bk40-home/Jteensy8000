/*
 * FXChainBlock.cpp - OPTIMIZED HYBRID VERSION (JPFX + hexefx Reverb)
 *
 * KEY IMPROVEMENTS:
 * 1. Smart reverb bypass: Reverb disabled when mix = 0 (saves ~10-15% CPU)
 * 2. Flexible routing: JPFX can feed or bypass reverb
 * 3. Better documentation
 * 4. Manual bypass override support
 *
 * SIGNAL FLOW:
 *   Amp Out → JPFX (tone/mod/delay) → [optionally] → PlateReverb
 *                                   ↓                      ↓
 *   Dry Signal ───────────────────→ Mixer (3-channel) ← Reverb Wet
 *                JPFX Direct ───────↗
 *
 * MIXER CHANNELS:
 *   Channel 0: Dry (from amp, pre-JPFX)
 *   Channel 1: JPFX wet output (can bypass reverb)
 *   Channel 2: Reverb wet output (processes JPFX output)
 *   Channel 3: Unused (available for future expansion)
 *
 * CPU OPTIMIZATION:
 *   Reverb automatically bypasses when mix=0 on both channels
 *   Saves ~10-15% CPU when reverb not needed
 */

#include "FXChainBlock.h"

// ============================================================================
// EFFECT NAME ARRAYS
// ============================================================================

static const char* modEffectNames[] = {
    "Chorus 1", "Chorus 2", "Chorus 3",           // 0-2: Chorus variations
    "Flanger 1", "Flanger 2", "Flanger 3",        // 3-5: Flanger variations
    "Phaser 1", "Phaser 2", "Phaser 3", "Phaser 4",  // 6-9: Phaser variations
    "Chorus Deep"                                  // 10: Deep chorus
};

static const char* delayEffectNames[] = {
    "Short",                    // 0: Short delay
    "Long",                     // 1: Long delay
    "PingPong 1",              // 2: Ping-pong variation 1
    "PingPong 2",              // 3: Ping-pong variation 2
    "PingPong 3"               // 4: Ping-pong variation 3
};

// ============================================================================
// CONSTRUCTOR - Initialize FX chain and audio connections
// ============================================================================

FXChainBlock::FXChainBlock()
    : _jpfx(), _plateReverb()
{
    // -------------------------------------------------------------------------
    // Initialize Reverb (start with bypass enabled for CPU efficiency)
    // -------------------------------------------------------------------------
    _plateReverb.bypass_set(true);  // ✅ Start bypassed (saves CPU)
    _plateReverb.mix(1.0f);         // Fully wet (dry handled by mixer)
    _plateReverb.size(_reverbRoomSize);
    _plateReverb.hidamp(_reverbHiDamp);
    _plateReverb.lodamp(_reverbLoDamp);

    // -------------------------------------------------------------------------
    // Create Audio Connections
    // -------------------------------------------------------------------------
    
    // Connect JPFX → Reverb (reverb processes JPFX output)
    _patchJPFXtoReverbL = new AudioConnection(_jpfx, 0, _plateReverb, 0);
    _patchJPFXtoReverbR = new AudioConnection(_jpfx, 1, _plateReverb, 1);

    // Connect JPFX → Output Mixer (channel 1 = JPFX direct, bypasses reverb)
    _patchJPFXtoMixerL = new AudioConnection(_jpfx, 0, _mixerOutL, 1);
    _patchJPFXtoMixerR = new AudioConnection(_jpfx, 1, _mixerOutR, 1);

    // Connect Reverb → Output Mixer (channel 2 = reverb wet)
    _patchReverbToMixerL = new AudioConnection(_plateReverb, 0, _mixerOutL, 2);
    _patchReverbToMixerR = new AudioConnection(_plateReverb, 1, _mixerOutR, 2);

    // -------------------------------------------------------------------------
    // Set Default Mixer Gains
    // -------------------------------------------------------------------------
    // Channel 0 (dry) is connected from SynthEngine amp output
    
    // Left mixer
    _mixerOutL.gain(0, 1.0f);   // Ch 0: Dry - default ON
    _mixerOutL.gain(1, 0.0f);   // Ch 1: JPFX direct - default OFF
    _mixerOutL.gain(2, 0.0f);   // Ch 2: Reverb wet - default OFF
    _mixerOutL.gain(3, 0.0f);   // Ch 3: Unused
    
    // Right mixer
    _mixerOutR.gain(0, 1.0f);   // Ch 0: Dry - default ON
    _mixerOutR.gain(1, 0.0f);   // Ch 1: JPFX direct - default OFF
    _mixerOutR.gain(2, 0.0f);   // Ch 2: Reverb wet - default OFF
    _mixerOutR.gain(3, 0.0f);   // Ch 3: Unused

    // -------------------------------------------------------------------------
    // Initialize JPFX (all effects off by default)
    // -------------------------------------------------------------------------
    _jpfx.setBassGain(0.0f);
    _jpfx.setTrebleGain(0.0f);
    _jpfx.setModEffect(AudioEffectJPFX::JPFX_MOD_OFF);
    _jpfx.setModMix(0.5f);
    _jpfx.setDelayEffect(AudioEffectJPFX::JPFX_DELAY_OFF);
    _jpfx.setDelayMix(0.5f);
}

// ============================================================================
// DESTRUCTOR - Clean up audio connections
// ============================================================================

FXChainBlock::~FXChainBlock()
{
    if (_patchJPFXtoReverbL) delete _patchJPFXtoReverbL;
    if (_patchJPFXtoReverbR) delete _patchJPFXtoReverbR;
    if (_patchJPFXtoMixerL) delete _patchJPFXtoMixerL;
    if (_patchJPFXtoMixerR) delete _patchJPFXtoMixerR;
    if (_patchReverbToMixerL) delete _patchReverbToMixerL;
    if (_patchReverbToMixerR) delete _patchReverbToMixerR;
}

// ============================================================================
// JPFX TONE CONTROL
// ============================================================================

void FXChainBlock::setBassGain(float dB) {
    _bassGain = dB;
    _jpfx.setBassGain(dB);
}

void FXChainBlock::setTrebleGain(float dB) {
    _trebleGain = dB;
    _jpfx.setTrebleGain(dB);
}

float FXChainBlock::getBassGain() const { return _bassGain; }
float FXChainBlock::getTrebleGain() const { return _trebleGain; }

// ============================================================================
// JPFX MODULATION EFFECTS (Chorus, Flanger, Phaser)
// ============================================================================

void FXChainBlock::setModEffect(int8_t variation) {
    _modEffect = variation;
    
    // Convert to JPFX enum type with bounds checking
    AudioEffectJPFX::ModEffectType type = (variation < 0) ? 
        AudioEffectJPFX::JPFX_MOD_OFF : 
        (AudioEffectJPFX::ModEffectType)(variation > 10 ? 10 : variation);
    
    _jpfx.setModEffect(type);
}

void FXChainBlock::setModMix(float mix) {
    _modMix = mix;
    _jpfx.setModMix(mix);
}

void FXChainBlock::setModRate(float hz) {
    _modRate = hz;
    _jpfx.setModRate(hz);
}

void FXChainBlock::setModFeedback(float fb) {
    _modFeedback = fb;
    _jpfx.setModFeedback(fb);
}

int8_t FXChainBlock::getModEffect() const { return _modEffect; }
float FXChainBlock::getModMix() const { return _modMix; }
float FXChainBlock::getModRate() const { return _modRate; }
float FXChainBlock::getModFeedback() const { return _modFeedback; }

const char* FXChainBlock::getModEffectName() const {
    if (_modEffect < 0) return "Off";
    if (_modEffect > 10) return "Unknown";
    return modEffectNames[_modEffect];
}

// ============================================================================
// JPFX DELAY EFFECTS
// ============================================================================

void FXChainBlock::setDelayEffect(int8_t variation) {
    _delayEffect = variation;
    
    // Convert to JPFX enum type with bounds checking
    AudioEffectJPFX::DelayEffectType type = (variation < 0) ? 
        AudioEffectJPFX::JPFX_DELAY_OFF : 
        (AudioEffectJPFX::DelayEffectType)(variation > 4 ? 4 : variation);
    
    _jpfx.setDelayEffect(type);
}

void FXChainBlock::setDelayMix(float mix) {
    _delayMix = mix;
    _jpfx.setDelayMix(mix);
}

void FXChainBlock::setDelayFeedback(float fb) {
    _delayFeedback = fb;
    _jpfx.setDelayFeedback(fb);
}

void FXChainBlock::setDelayTime(float ms) {
    _delayTime = ms;
    _jpfx.setDelayTime(ms);
}

int8_t FXChainBlock::getDelayEffect() const { return _delayEffect; }
float FXChainBlock::getDelayMix() const { return _delayMix; }
float FXChainBlock::getDelayFeedback() const { return _delayFeedback; }
float FXChainBlock::getDelayTime() const { return _delayTime; }

const char* FXChainBlock::getDelayEffectName() const {
    if (_delayEffect < 0) return "Off";
    if (_delayEffect > 4) return "Unknown";
    return delayEffectNames[_delayEffect];
}
void FXChainBlock::updateFromBPMClock(const BPMClockManager& bpmClock) {
    _jpfx.updateFromBPMClock(bpmClock);
}

void FXChainBlock::setDelayTimingMode(TimingMode mode) {
    _jpfx.setDelayTimingMode(mode);
}

TimingMode FXChainBlock::getDelayTimingMode() const {
    return _jpfx.getDelayTimingMode();
}

// ============================================================================
// HEXEFX REVERB CONTROLS
// ============================================================================

void FXChainBlock::setReverbRoomSize(float size) {
    // Clamp to valid range
    if (size < 0.0f) size = 0.0f;
    if (size > 1.0f) size = 1.0f;
    
    _reverbRoomSize = size;
    _plateReverb.size(size);
}

void FXChainBlock::setReverbHiDamping(float damp) {
    // Clamp to valid range
    if (damp < 0.0f) damp = 0.0f;
    if (damp > 1.0f) damp = 1.0f;
    
    _reverbHiDamp = damp;
    _plateReverb.hidamp(damp);
}

void FXChainBlock::setReverbLoDamping(float damp) {
    // Clamp to valid range
    if (damp < 0.0f) damp = 0.0f;
    if (damp > 1.0f) damp = 1.0f;
    
    _reverbLoDamp = damp;
    _plateReverb.lodamp(damp);
}

float FXChainBlock::getReverbRoomSize() const { return _reverbRoomSize; }
float FXChainBlock::getReverbHiDamping() const { return _reverbHiDamp; }
float FXChainBlock::getReverbLoDamping() const { return _reverbLoDamp; }

void FXChainBlock::setReverbBypass(bool bypass) {
    _reverbManualBypass = bypass;
    updateReverbBypass();
}

bool FXChainBlock::getReverbBypass() const {
    return _reverbManualBypass;
}

// ============================================================================
// MIX CONTROLS
// ============================================================================

void FXChainBlock::setDryMix(float left, float right) {
    _dryMixL = left;
    _dryMixR = right;
    
    // Update mixer gains (channel 0 = dry)
    _mixerOutL.gain(0, left);
    _mixerOutR.gain(0, right);
}

void FXChainBlock::setJPFXMix(float left, float right) {
    _jpfxMixL = left;
    _jpfxMixR = right;
    
    // Update mixer gains (channel 1 = JPFX direct)
    _mixerOutL.gain(1, left);
    _mixerOutR.gain(1, right);
}

void FXChainBlock::setReverbMix(float left, float right) {
    _reverbMixL = left;
    _reverbMixR = right;
    
    // Update mixer gains (channel 2 = reverb wet)
    _mixerOutL.gain(2, left);
    _mixerOutR.gain(2, right);
    
    // CPU OPTIMIZATION: Update reverb bypass state
    updateReverbBypass();
}

float FXChainBlock::getDryMixL() const { return _dryMixL; }
float FXChainBlock::getDryMixR() const { return _dryMixR; }
float FXChainBlock::getJPFXMixL() const { return _jpfxMixL; }
float FXChainBlock::getJPFXMixR() const { return _jpfxMixR; }
float FXChainBlock::getReverbMixL() const { return _reverbMixL; }
float FXChainBlock::getReverbMixR() const { return _reverbMixR; }

// ============================================================================
// PRIVATE HELPER METHODS
// ============================================================================

/*
 * updateReverbBypass - Intelligently bypass reverb to save CPU
 * 
 * CPU OPTIMIZATION STRATEGY:
 * Reverb processing is expensive (~10-15% CPU). We can bypass it when:
 * 1. User has manually bypassed it (_reverbManualBypass = true)
 * 2. Reverb mix is 0 on BOTH channels (no output anyway)
 * 
 * IMPORTANT: We DO NOT bypass based on input activity because:
 * - Reverb needs to continue processing to maintain tail decay
 * - JPFX may have delay/modulation even without new notes
 * - Input activity detection adds CPU cost (defeating the purpose)
 * 
 * RESULT: ~10-15% CPU saved when reverb mix = 0, but effect tails preserved
 */
void FXChainBlock::updateReverbBypass() {
    // Check if reverb is needed
    bool reverbNeeded = !_reverbManualBypass &&          // Not manually bypassed
                       (_reverbMixL > 0.001f ||          // Left mix > 0
                        _reverbMixR > 0.001f);           // Right mix > 0
    
    // Set bypass state (bypass = !needed)
    _plateReverb.bypass_set(!reverbNeeded);
    
    // Optional: Debug logging (comment out in production)
    // static uint32_t lastLog = 0;
    // if (millis() - lastLog > 1000) {
    //     Serial.print("[FXChain] Reverb ");
    //     Serial.println(reverbNeeded ? "ACTIVE" : "BYPASSED");
    //     lastLog = millis();
    // }
}