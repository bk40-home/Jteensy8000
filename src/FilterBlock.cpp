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
#include "FilterShape.h"   // per-filter fcMin/fcMax for Hz↔norm conversion
#include <math.h>          // logf, powf

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
// Hz → normalised (0..1) for the VA bank's setCutoffNorm() API.
//
// The VA bank's norm API expects a 0..1 knob value and applies the per-filter
// FilterShape exponential curve  Hz = fcMin·(fcMax/fcMin)^c  internally. The
// synth plumbs cutoff as Hz (SynthEngine maps CC→Hz, VoiceBlock scales by
// velocity in Hz), so we invert that curve here using the SAME per-filter
// range, giving the exact normalised position the bank will re-expand. Result
// is clamped to 0..1 so out-of-band Hz (after velocity offset) lands cleanly
// at the rails instead of overshooting.
//
//   c = log(Hz/fcMin) / log(fcMax/fcMin)
//
// Done at control rate only (per setCutoff call), so the logf cost is trivial.
// ---------------------------------------------------------------------------
static inline float hzToNormForType(float hz, VAFilterType type)
{
    const FilterShape& s = kFilterShape[type];
    if (hz <= s.fcMinHz) return 0.0f;
    if (hz >= s.fcMaxHz) return 1.0f;
    return logf(hz / s.fcMinHz) / logf(s.fcMaxHz / s.fcMinHz);
}

// ---------------------------------------------------------------------------
// Constructor  —  build the permanent audio graph
// ---------------------------------------------------------------------------
FilterBlock::FilterBlock()
{
    // ── Audio fan-out: _inputBuf fans the single audio input to both filters ──
    // VoiceBlock wires its voiceMixer to _inputBuf (via input() accessor).
    // _inputBuf has gain 1.0 and passes to OBXa input0 and VA input0.  The
    // inactive engine is skipped via its setActive(false) flag (real DSP skip),
    // not by gating audio — so no per-engine input amps are needed.
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

    // ── Engine-skip gate: OBXa active by default ──────────────────────────────
    _filterOBXa.setActive(true);
    _filterVA.setActive(false);

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
        _filterOBXa.setActive(true);    // CPU skip gate: OBXa runs
        _filterVA.setActive(false);     // VA drains & skips
        _applyParamsToOBXa();
        JT_LOGF("[FLT] Engine → OBXa\n");
    }
    else
    {
        _outputMix.gain(0, ENGINE_MUTED);
        _outputMix.gain(1, ENGINE_ACTIVE);
        _filterOBXa.setActive(false);   // OBXa drains & skips
        _filterVA.setActive(true);      // CPU skip gate: VA runs
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
    // The Hz→norm inversion depends on the filter's FilterShape range, so the
    // same cached cutoff Hz maps to a different norm under a new type. Re-push
    // it so the cutoff stays at the intended Hz across a type change.
    _filterVA.setCutoffNorm(hzToNormForType(_cutoff, _vaType));
    JT_LOGF("[FLT] VA type: %s\n", _filterVA.getFilterName());
}

// ---------------------------------------------------------------------------
// VA drive / saturation
//
// Drive is cached as a 0..1 UI value here. The VA bank's setDriveNorm() does
// the 0..1 → ×1..4 scale AND block-rate slews it, so CC drive sweeps are
// click-free. Caching the un-scaled value keeps getVADrive()/PatchState in UI
// units and the scale lives inside the bank (single source of truth).
// ---------------------------------------------------------------------------
void FilterBlock::setVADrive(float norm01)
{
    _vaDrive01 = constrain(norm01, 0.0f, 1.0f);
    _filterVA.setDriveNorm(_vaDrive01);             // bank scales 0..1 → ×1..4
    JT_LOGF("[FLT] VA drive: %.3f (×%.2f)\n", _vaDrive01, 1.0f + _vaDrive01 * 3.0f);
}

void FilterBlock::setVASaturation(uint8_t satType)
{
    if (satType > SAT_TANH) satType = SAT_TANH;     // clamp to valid enum range
    _vaSat = (VASaturationType)satType;
    _filterVA.setSaturation(_vaSat);
    JT_LOGF("[FLT] VA sat: %u\n", (unsigned)_vaSat);
}

// ---------------------------------------------------------------------------
// Engine-context shared CCs — VA-side decode only.
//
// FilterBlock is per-voice, so it cannot own the OBXa mode→flags→all-voices
// fan-out (that needs SynthEngine's _patch + voice loop).  By design the
// OBXa branch is therefore handled in SynthEngine; these methods decode the
// VA meaning only and are called by SynthEngine when the VA engine is active.
// The bucket math mirrors the SELECT decode used everywhere else
// (value * count / 128) so display and firmware agree.
// ---------------------------------------------------------------------------
void FilterBlock::setContextCC112(uint8_t ccValue)   // VA: Filter Type
{
    const uint8_t vt = (uint8_t)constrain(
        (int)ccValue * (int)FILTER_COUNT / 128, 0, (int)FILTER_COUNT - 1);
    setVAFilterType((VAFilterType)vt);
}

void FilterBlock::setContextCC114(uint8_t ccValue)   // VA: Drive (0..1)
{
    setVADrive((float)ccValue * (1.0f / 127.0f));
}

void FilterBlock::setContextCC111(uint8_t ccValue)   // VA: Saturation (3)
{
    // 3 options: None / Soft(Fast) / Warm(Tanh) — same bucket decode as display
    const uint8_t s = (uint8_t)constrain((int)ccValue * 3 / 128, 0, 2);
    setVASaturation(s);
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
    // VA bank: feed the normalised API so cutoff sweeps are slewed and the
    // per-filter FilterShape curve applies. Hz is inverted under the ACTIVE VA
    // filter's range (the bank re-expands it with the same range internally).
    _filterVA.setCutoffNorm(hzToNormForType(freqHz, _vaType));
    JT_LOGF_RATE(200, "[FLT] Cutoff: %.2f Hz\n", freqHz);
}

void FilterBlock::setResonance(float amount)
{
    _resonance = amount;
    _filterOBXa.resonance(amount);
    // VA bank: normalised API applies per-filter γ + slewing (vs raw 0..1).
    _filterVA.setResonanceNorm(amount);
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
    // DC amplitude = key tracking + sequencer offset (both share mod bus slot 0)
    _keyTrackDc.amplitude(constrain(_keyTrackAmount + _seqFilterOffset, -1.0f, 1.0f));
    JT_LOGF("[FLT] Key Track: %.2f\n", amount);
}

void FilterBlock::setSeqFilterOffset(float offset)
{
    _seqFilterOffset = offset;
    // Re-push the combined DC — same slot, additive
    _keyTrackDc.amplitude(constrain(_keyTrackAmount + _seqFilterOffset, -1.0f, 1.0f));
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
//
// reset() is called first: the incoming engine starts from clean state rather
// than relying on having stayed "warm" while inactive (the engine-skip opt
// can leave it cold).  This is the reset-on-switch contract.
// ---------------------------------------------------------------------------
void FilterBlock::_applyParamsToOBXa()
{
    _filterOBXa.reset();
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
//
// reset() (incoming-engine contract) — note setFilterType() already resets on
// a genuine type change, but an explicit reset here covers the case where the
// type is unchanged across the switch.  Drive/saturation are re-pushed too.
// ---------------------------------------------------------------------------
void FilterBlock::_applyParamsToVA()
{
    _filterVA.reset();
    _filterVA.setFilterType(_vaType);
    // Normalised API: cutoff Hz inverted under the active filter's range, res
    // and drive as 0..1. setFilterType() above already cleared DSP state.
    _filterVA.setCutoffNorm(hzToNormForType(_cutoff, _vaType));
    _filterVA.setResonanceNorm(_resonance);
    _filterVA.setCutoffModOctaves(_octaveControl);
    _filterVA.setResonanceModDepth(_resonanceModDepth);
    _filterVA.setMidiNote(_midiNote);
    _filterVA.setEnvValue(_envValue);
    _filterVA.setDriveNorm(_vaDrive01);             // bank scales 0..1 → ×1..4
    _filterVA.setSaturation(_vaSat);
    // Settle all slews to the just-pushed targets so switching to VA does not
    // glide in from stale slew state — the engine starts exactly at the cached
    // patch values (matches the OBXa branch's instant apply).
    _filterVA.snap();
}
