// =============================================================================
// FilterBlock.cpp  –  Dual-engine filter wrapper implementation
// =============================================================================
//
// Both AudioFilterOBXa and AudioFilterVABank live permanently in the audio
// graph.  Engine switching uses _outputMix gain gating only — no cable
// changes at runtime.  This is fully ISR-safe.
//
// CPU note: both engines always process audio (each receives the same signal
// via _inputBuf).  The inactive engine's output is silenced at _outputMix,
// but its filter runs one block per audio interrupt.  On Teensy 4.1 @600MHz
// this is well within budget.  The ZDF filters converge toward zero when fed
// the same signal as the active engine, so their states remain warm and
// switching sounds seamless.
// =============================================================================

#include "DebugTrace.h"
#include "FilterBlock.h"

// ---------------------------------------------------------------------------
// Mixer gain constants
// ---------------------------------------------------------------------------
static constexpr float KEY_TRACK_GAIN  = 1.0f;
static constexpr float ENV_MOD_GAIN    = 1.0f;
static constexpr float LFO_GAIN_INIT   = 0.0f;
static constexpr float ENGINE_ACTIVE   = 1.0f;
static constexpr float ENGINE_MUTED    = 0.0f;
static constexpr float INPUT_BUF_GAIN  = 1.0f;   // unity passthrough

// ---------------------------------------------------------------------------
// Constructor  —  build the permanent audio graph
// ---------------------------------------------------------------------------
FilterBlock::FilterBlock()
{
    // ── Audio fan-out: _inputBuf fans the single audio input to both filters ──
    // VoiceBlock wires its voiceMixer to _inputBuf (via input() accessor).
    // _inputBuf has gain 1.0 and passes to OBXa input0 and VA input0.
    _cables[0] = new AudioConnection(_inputBuf, 0, _filterOBXa, 0);
    _cables[1] = new AudioConnection(_inputBuf, 0, _filterVA,   0);

    // Cutoff modulation bus → both engines' mod input (input 1)
    _cables[2] = new AudioConnection(_modMixer,  0, _filterOBXa, 1);
    _cables[3] = new AudioConnection(_modMixer,  0, _filterVA,   1);

    // Both engine outputs → output mixer (engine switch gates these gains)
    _cables[4] = new AudioConnection(_filterOBXa, 0, _outputMix, 0);
    _cables[5] = new AudioConnection(_filterVA,   0, _outputMix, 1);

    // Key tracking DC → modMixer ch0
    _cables[6] = new AudioConnection(_keyTrackDc, 0, _modMixer, 0);

    // ── Input buffer gain (unity passthrough) ─────────────────────────────────
    _inputBuf.gain(INPUT_BUF_GAIN);

    // ── Output mixer: OBXa active by default, VA muted ───────────────────────
    _outputMix.gain(0, ENGINE_ACTIVE);   // OBXa on
    _outputMix.gain(1, ENGINE_MUTED);    // VA off
    _outputMix.gain(2, 0.0f);
    _outputMix.gain(3, 0.0f);

    // ── Cutoff modulation mixer initial gains ─────────────────────────────────
    _modMixer.gain(0, KEY_TRACK_GAIN);   // ch0: key tracking DC
    _modMixer.gain(1, ENV_MOD_GAIN);     // ch1: filter envelope
    _modMixer.gain(2, LFO_GAIN_INIT);    // ch2: LFO1 (set by SynthEngine)
    _modMixer.gain(3, LFO_GAIN_INIT);    // ch3: LFO2 (set by SynthEngine)

    // ── DC source initialisation ──────────────────────────────────────────────
    _envModDc.amplitude(0.0f);
    _keyTrackDc.amplitude(0.0f);

    // ── Initial octave control applied to both engines ────────────────────────
    _filterOBXa.setCutoffModOctaves(_octaveControl);
    _filterVA.setCutoffModOctaves(_octaveControl);
}

// ---------------------------------------------------------------------------
// setFilterEngine  —  switch between OBXa (0) and VA bank (1)
//
// Only modifies _outputMix gains.  No cable changes; safe during audio.
// Re-applies all cached parameters to the newly active engine so its state
// is consistent with what the user last set — avoids jumps on switch.
// ---------------------------------------------------------------------------
void FilterBlock::setFilterEngine(uint8_t engine)
{
    if (engine >= CC::FILTER_ENGINE_COUNT) engine = CC::FILTER_ENGINE_OBXA;
    if (engine == _activeEngine) return;

    _activeEngine = engine;

    if (engine == CC::FILTER_ENGINE_OBXA)
    {
        _outputMix.gain(0, ENGINE_ACTIVE);
        _outputMix.gain(1, ENGINE_MUTED);
        _applyParamsToOBXa();
        JT_LOGF("[FLT] Engine → OBXa\n");
    }
    else
    {
        _outputMix.gain(0, ENGINE_MUTED);
        _outputMix.gain(1, ENGINE_ACTIVE);
        _applyParamsToVA();
        JT_LOGF("[FLT] Engine → VA (%s)\n", _filterVA.getFilterName());
    }
}

// ---------------------------------------------------------------------------
// setVAFilterType
// ---------------------------------------------------------------------------
void FilterBlock::setVAFilterType(VAFilterType type)
{
    _vaType = type;
    _filterVA.setFilterType(type);
    JT_LOGF("[FLT] VA type: %s\n", _filterVA.getFilterName());
}

// ---------------------------------------------------------------------------
// Core parameter setters — applied to BOTH engines always.
// Both engines are always running; keeping them in sync means switching
// sounds seamless (no parameter jump when the output crossfades).
// ---------------------------------------------------------------------------
void FilterBlock::setCutoff(float freqHz)
{
    if (freqHz == _cutoff) return;
    _cutoff = freqHz;
    _filterOBXa.frequency(freqHz);
    _filterVA.frequency(freqHz);
    JT_LOGF_RATE(200, "[FLT] Cutoff: %.2f Hz\n", freqHz);
}

void FilterBlock::setResonance(float amount)
{
    _resonance = amount;
    _filterOBXa.resonance(amount);
    _filterVA.resonance(amount);
    JT_LOGF("[FLT] Resonance: %.4f\n", amount);
}

void FilterBlock::setOctaveControl(float octaves)
{
    _octaveControl = octaves;
    _filterOBXa.setCutoffModOctaves(octaves);
    _filterVA.setCutoffModOctaves(octaves);
    JT_LOGF("[FLT] Octave Ctrl: %.2f\n", octaves);
}

void FilterBlock::setEnvModAmount(float amount)
{
    _envModAmount = amount;
    _envModDc.amplitude(amount);
    JT_LOGF("[FLT] Env Mod: %.2f\n", amount);
}

void FilterBlock::setKeyTrackAmount(float amount)
{
    _keyTrackAmount = amount;
    _keyTrackDc.amplitude(amount);
    JT_LOGF("[FLT] Key Track: %.2f\n", amount);
}

void FilterBlock::setResonanceModDepth(float amount)
{
    _resonanceModDepth = amount;
    _filterOBXa.setResonanceModDepth(amount);
    _filterVA.setResonanceModDepth(amount);
    JT_LOGF("[FLT] ResModDepth: %.2f\n", amount);
}

void FilterBlock::setEnvModOctaves(float oct)
{
    _envModAmount = oct;
    _filterOBXa.setEnvModOctaves(oct);
    _filterVA.setEnvModOctaves(oct);
}

void FilterBlock::setMidiNote(float note)
{
    _midiNote = note;
    _filterOBXa.setMidiNote(note);
    _filterVA.setMidiNote(note);
}

void FilterBlock::setEnvValue(float env01)
{
    _envValue = env01;
    _filterOBXa.setEnvValue(env01);
    _filterVA.setEnvValue(env01);
}

// ---------------------------------------------------------------------------
// OBXa-specific topology setters
// (Only applied to OBXa; VA bank uses its own topology select)
// ---------------------------------------------------------------------------
void FilterBlock::setMultimode(float amount)
{
    _multimode = amount;
    _filterOBXa.multimode(amount);
    JT_LOGF("[FLT] Multimode: %.2f\n", amount);
}

void FilterBlock::setTwoPole(bool enabled)
{
    _useTwoPole = enabled;
    _filterOBXa.setTwoPole(enabled);
    JT_LOGF("[FLT] TwoPole: %d\n", (int)enabled);
}

void FilterBlock::setXpander4Pole(bool enabled)
{
    _xpander4Pole = enabled;
    _filterOBXa.setXpander4Pole(enabled);
    JT_LOGF("[FLT] Xpander4P: %d\n", (int)enabled);
}

void FilterBlock::setXpanderMode(uint8_t amount)
{
    _xpanderMode = amount;
    _filterOBXa.setXpanderMode(amount);
    JT_LOGF("[FLT] XpanderMode: %u\n", (unsigned)amount);
}

void FilterBlock::setBPBlend2Pole(bool enabled)
{
    _bpBlend2Pole = enabled;
    _filterOBXa.setBPBlend2Pole(enabled);
    JT_LOGF("[FLT] BPBlend2P: %d\n", (int)enabled);
}

void FilterBlock::setPush2Pole(bool enabled)
{
    _push2Pole = enabled;
    _filterOBXa.setPush2Pole(enabled);
    JT_LOGF("[FLT] Push2P: %d\n", (int)enabled);
}

// ---------------------------------------------------------------------------
// Audio graph access
// ---------------------------------------------------------------------------
// input(): VoiceBlock wires voiceMixer → _inputBuf.
//          _inputBuf fans the signal to both filter inputs (cables [0] and [1]).
AudioStream& FilterBlock::input()  { return _inputBuf; }

// output(): VoiceBlock wires _outputMix → ampEnvelope.
//           Only the active engine's channel has non-zero gain here.
AudioStream& FilterBlock::output() { return _outputMix; }

// envmod(): envelope DC source — feeds the filter ADSR in VoiceBlock.
AudioStream& FilterBlock::envmod() { return _envModDc; }

// modMixer(): SynthEngine wires LFO1/LFO2 here (ch2/ch3).
AudioMixer4& FilterBlock::modMixer(){ return _modMixer; }

// ---------------------------------------------------------------------------
// _applyParamsToOBXa  —  sync OBXa to all cached state
// ---------------------------------------------------------------------------
void FilterBlock::_applyParamsToOBXa()
{
    _filterOBXa.frequency(_cutoff);
    _filterOBXa.resonance(_resonance);
    _filterOBXa.setCutoffModOctaves(_octaveControl);
    _filterOBXa.setResonanceModDepth(_resonanceModDepth);
    _filterOBXa.setMidiNote(_midiNote);
    _filterOBXa.setEnvValue(_envValue);
    _filterOBXa.multimode(_multimode);
    _filterOBXa.setTwoPole(_useTwoPole);
    _filterOBXa.setXpander4Pole(_xpander4Pole);
    _filterOBXa.setXpanderMode(_xpanderMode);
    _filterOBXa.setBPBlend2Pole(_bpBlend2Pole);
    _filterOBXa.setPush2Pole(_push2Pole);
}

// ---------------------------------------------------------------------------
// _applyParamsToVA  —  sync VA bank to all cached state
// ---------------------------------------------------------------------------
void FilterBlock::_applyParamsToVA()
{
    _filterVA.setFilterType(_vaType);
    _filterVA.frequency(_cutoff);
    _filterVA.resonance(_resonance);
    _filterVA.setCutoffModOctaves(_octaveControl);
    _filterVA.setResonanceModDepth(_resonanceModDepth);
    _filterVA.setMidiNote(_midiNote);
    _filterVA.setEnvValue(_envValue);
}
