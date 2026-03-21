#include "Presets.h"
#include "Mapping.h"
#include "CCDefs.h"
#include "Presets_Microsphere.h"
#include "TUS_Presets.h"

namespace {
inline void sendCC(SynthEngine& synth, uint8_t cc, uint8_t val, uint8_t ch = 1) {
    synth.handleControlChange(ch, cc, val);
}
}

namespace Presets {

static const int kTEMPLATE_COUNT    = 9;
static const int kMICROSPHERE_START = kTEMPLATE_COUNT;                              // offset 9
static const int kTUS_START         = kMICROSPHERE_START + JT8000_PRESET_COUNT;     // offset 41

const char* templateName(uint8_t idx) {
    static const char* names[9] = {
        "Init Wave 0","Init Wave 1","Init Wave 2","Init Wave 3","Init Wave 4",
        "Init Wave 5","Init Wave 6","Init Wave 7","Init Wave 8"
    };
    return (idx < 9) ? names[idx] : "Init";
}

int presets_templateCount() { return kTEMPLATE_COUNT; }
int presets_totalCount()    { return kTEMPLATE_COUNT + JT8000_PRESET_COUNT + kTUS_COUNT; }

const char* presets_nameByGlobalIndex(int idx) {
    if (idx < kTEMPLATE_COUNT) return templateName((uint8_t)idx);
    if (idx < kTUS_START) {
        int bankIdx = idx - kMICROSPHERE_START;
        return (bankIdx >= 0 && bankIdx < JT8000_PRESET_COUNT) ? JT8000_Presets[bankIdx].name : "—";
    }
    int tusIdx = idx - kTUS_START;
    return (tusIdx >= 0 && tusIdx < kTUS_COUNT) ? kTUS_Patches[tusIdx].name : "—";
}

void presets_loadByGlobalIndex(SynthEngine& synth, int globalIdx, uint8_t midiCh) {
    int total = presets_totalCount(); if (total <= 0) return;
    while (globalIdx < 0) globalIdx += total;
    while (globalIdx >= total) globalIdx -= total;

    if (globalIdx < kTEMPLATE_COUNT) {
        loadInitTemplateByWave(synth, (uint8_t)globalIdx);
    } else if (globalIdx < kTUS_START) {
        loadMicrospherePreset(synth, globalIdx - kMICROSPHERE_START, midiCh);
    } else {
        loadTUSPreset(synth, globalIdx - kTUS_START);
    }
}

void loadInitTemplateByWave(SynthEngine &synth, uint8_t waveIndex) {
    // Constrain to the number of defined waveforms (sine, saw, square, supersaw, etc.).
    waveIndex %= numWaveformsAll;

    // Find the WaveformType corresponding to the template index.
    WaveformType wfType = waveformListAll[waveIndex];

    // Convert the waveform to the appropriate CC value for OSC1_WAVE/OSC2_WAVE.
    uint8_t waveCC = ccFromWaveform(wfType);

    // Now send CC values using the full 0–127 range.
    sendCC(synth, CC::OSC1_WAVE, waveCC);
    sendCC(synth, CC::OSC2_WAVE, waveCC);

    sendCC(synth, CC::OSC_MIX_BALANCE, 0);
    //sendCC(synth, CC::OSC2_MIX, 0);
    sendCC(synth, CC::SUB_MIX, 0);
    sendCC(synth, CC::NOISE_MIX, 0);

    sendCC(synth, CC::OSC1_PITCH_OFFSET, 64);
    sendCC(synth, CC::OSC1_FINE_TUNE, 64);
    sendCC(synth, CC::OSC1_DETUNE, 64);
    sendCC(synth, CC::OSC2_PITCH_OFFSET, 64);
    sendCC(synth, CC::OSC2_FINE_TUNE, 64);
    sendCC(synth, CC::OSC2_DETUNE, 64);

    sendCC(synth, CC::FILTER_CUTOFF,    127);
    sendCC(synth, CC::FILTER_RESONANCE, 0);
    sendCC(synth, CC::FILTER_ENV_AMOUNT, 65);
    sendCC(synth, CC::FILTER_KEY_TRACK,  65);
    sendCC(synth, CC::FILTER_OCTAVE_CONTROL, 0);

    sendCC(synth, CC::AMP_ATTACK,  0);
    sendCC(synth, CC::AMP_DECAY,   0);
    sendCC(synth, CC::AMP_SUSTAIN, 127);
    sendCC(synth, CC::AMP_RELEASE, 0);

    sendCC(synth, CC::FILTER_ENV_ATTACK,  0);
    sendCC(synth, CC::FILTER_ENV_DECAY,   0);
    sendCC(synth, CC::FILTER_ENV_SUSTAIN, 127);
    sendCC(synth, CC::FILTER_ENV_RELEASE, 0);

    sendCC(synth, CC::LFO1_DEPTH, 0);
    sendCC(synth, CC::LFO2_DEPTH, 0);
    // LFO per-destination depths — all off at init
    sendCC(synth, CC::LFO1_PITCH_DEPTH,  0);
    sendCC(synth, CC::LFO1_FILTER_DEPTH, 0);
    sendCC(synth, CC::LFO1_PWM_DEPTH,    0);
    sendCC(synth, CC::LFO1_AMP_DEPTH,    0);
    sendCC(synth, CC::LFO2_PITCH_DEPTH,  0);
    sendCC(synth, CC::LFO2_FILTER_DEPTH, 0);
    sendCC(synth, CC::LFO2_PWM_DEPTH,    0);
    sendCC(synth, CC::LFO2_AMP_DEPTH,    0);
    // Pitch envelope — depth=centre (0 semis), ADSR initialised, no effect until depth moved
    sendCC(synth, CC::PITCH_ENV_ATTACK,  5);   // short attack  (~10 ms)
    sendCC(synth, CC::PITCH_ENV_DECAY,   40);  // medium decay  (~200 ms)
    sendCC(synth, CC::PITCH_ENV_SUSTAIN, 0);   // sustain=0: classic transient pitch bend
    sendCC(synth, CC::PITCH_ENV_RELEASE, 10);  // short release (~50 ms)
    sendCC(synth, CC::PITCH_ENV_DEPTH,   64);  // 64 = 0 semitones = neutral, no effect
    // Velocity sensitivity — all off at init (full level regardless of velocity)
    sendCC(synth, CC::VELOCITY_AMP_SENS,    0);
    sendCC(synth, CC::VELOCITY_FILTER_SENS, 0);
    sendCC(synth, CC::VELOCITY_ENV_SENS,    0);

// JPFX Tone (neutral)
sendCC(synth, CC::FX_BASS_GAIN, 64);     // 0 dB (center)
sendCC(synth, CC::FX_TREBLE_GAIN, 64);   // 0 dB (center)

// JPFX Modulation (off)
sendCC(synth, CC::FX_MOD_EFFECT, 0);     // Off
sendCC(synth, CC::FX_MOD_MIX, 64);       // 50% mix (if enabled)
sendCC(synth, CC::FX_MOD_RATE, 0);       // Use preset rate
sendCC(synth, CC::FX_MOD_FEEDBACK, 0);   // Use preset feedback

// JPFX Delay (off)
sendCC(synth, CC::FX_JPFX_DELAY_EFFECT, 0);    // Off
sendCC(synth, CC::FX_JPFX_DELAY_MIX, 64);      // 50% mix (if enabled)
sendCC(synth, CC::FX_JPFX_DELAY_FEEDBACK, 0);  // Use preset feedback
sendCC(synth, CC::FX_JPFX_DELAY_TIME, 0);      // Use preset time

// JPFX Dry mix
sendCC(synth, CC::FX_DRY_MIX, 127);      // Full dry (effects off)

    sendCC(synth, CC::GLIDE_ENABLE, 0);
    sendCC(synth, CC::GLIDE_TIME,   0);
    sendCC(synth, CC::AMP_MOD_FIXED_LEVEL, 127);
    sendCC(synth, CC::POLY_MODE,    0);    // 0 = POLY mode
    sendCC(synth, CC::UNISON_DETUNE, 64);  // 64 = mid-range detune spread

    AudioInterrupts();
}

void loadRawPatchViaCC(SynthEngine& synth, const uint8_t data[64], uint8_t midiCh) {
    AudioNoInterrupts();
    for (const auto& row : JT8000Map::kSlots) {
        uint8_t idx0 = (row.byte1 >= 1) ? (row.byte1 - 1) : 0;
        if (idx0 >= 64) continue;
        uint8_t raw = data[idx0];
        uint8_t val = JT8000Map::toCC(raw, row.xf);
        synth.handleControlChange(midiCh, row.cc, val);
    }
    AudioInterrupts();
}

void loadMicrospherePreset(SynthEngine& synth, int index, uint8_t midiCh) {
    if (index < 0 || index >= JT8000_PRESET_COUNT) return;
    loadRawPatchViaCC(synth, JT8000_Presets[index].data, midiCh);
}

// ---------------------------------------------------------------------------
// TUS preset loader (CC-array format)
//
// Data is already in JT-8000 CC-space (exported from the HTML editor).
// No transform required — iterate data[0..127] and dispatch each CC.
//
// CC 0 is skipped (bank select / unused).
// CC 1 is skipped (mod wheel — live performance, not preset state).
//
// Cost: ~128 handleControlChange() calls per load. At ~1 µs each on
// Teensy 4.1 @ 816 MHz, total load time is well under 1 ms.
// ---------------------------------------------------------------------------
void loadTUSPreset(SynthEngine& synth, int index) {
    if (index < 0 || index >= kTUS_COUNT) return;

    // Copy from PROGMEM to stack (safe on Teensy ARM, required on AVR)
    TUSPatchCC p;
    memcpy_P(&p, &kTUS_Patches[index], sizeof(TUSPatchCC));

    AudioNoInterrupts();

    // Send every CC value. Start at CC 2 — CC 0 (bank select) and
    // CC 1 (mod wheel) are not preset parameters.
    for (uint8_t cc = 2; cc < 128; cc++) {
        synth.handleControlChange(1, cc, p.data[cc]);
    }

    AudioInterrupts();
}

} // namespace Presets
