// ---------------------------------------------------------------------------
// Presets.cpp — JT-8000 preset loading implementation
//
// Three banks:
//   Init templates   → loadInitTemplateByWave()   — CC-by-CC hard-coded defaults
//   Microsphere      → loadMicrospherePreset()    — 64-byte raw SysEx via kSlots mapping
//   TUS              → loadTUSPreset()            — direct CC-array, no transform
//
// All loaders wrap their CC sends in AudioNoInterrupts() / AudioInterrupts()
// to prevent partial-state audio glitches during preset switch.
// ---------------------------------------------------------------------------

#include "Presets.h"
#include "Mapping.h"
#include "CCDefs.h"
#include "Presets_Microsphere.h"
#include "TUS_Presets.h"

// ---------------------------------------------------------------------------
// Helper — send a single CC through the synth engine's handler
// ---------------------------------------------------------------------------
namespace {
inline void sendCC(SynthEngine& synth, uint8_t cc, uint8_t val, uint8_t ch = 1) {
    synth.handleControlChange(ch, cc, val);
}
}

namespace Presets {

// ---------------------------------------------------------------------------
// Bank layout — global index offsets
// ---------------------------------------------------------------------------
static const int kTEMPLATE_COUNT    = 9;
static const int kMICROSPHERE_START = kTEMPLATE_COUNT;                              // offset 9
static const int kTUS_START         = kMICROSPHERE_START + JT8000_PRESET_COUNT;     // offset 41

// ---------------------------------------------------------------------------
// Template names
// ---------------------------------------------------------------------------
const char* templateName(uint8_t idx) {
    static const char* names[9] = {
        "Init Wave 0","Init Wave 1","Init Wave 2","Init Wave 3","Init Wave 4",
        "Init Wave 5","Init Wave 6","Init Wave 7","Init Wave 8"
    };
    return (idx < 9) ? names[idx] : "Init";
}

// ---------------------------------------------------------------------------
// Counts
// ---------------------------------------------------------------------------
int presets_templateCount() { return kTEMPLATE_COUNT; }
int presets_totalCount()    { return kTEMPLATE_COUNT + JT8000_PRESET_COUNT + kTUS_COUNT; }

// ---------------------------------------------------------------------------
// Name lookup by global index
// ---------------------------------------------------------------------------
const char* presets_nameByGlobalIndex(int idx) {
    if (idx < kTEMPLATE_COUNT) return templateName((uint8_t)idx);
    if (idx < kTUS_START) {
        int bankIdx = idx - kMICROSPHERE_START;
        return (bankIdx >= 0 && bankIdx < JT8000_PRESET_COUNT) ? JT8000_Presets[bankIdx].name : "—";
    }
    int tusIdx = idx - kTUS_START;
    return (tusIdx >= 0 && tusIdx < kTUS_COUNT) ? kTUS_Patches[tusIdx].name : "—";
}

// ---------------------------------------------------------------------------
// Load by global index — wraps around at boundaries
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Init template — one per waveform type
// Sets every synthesis parameter to a clean default via individual CC sends.
// ---------------------------------------------------------------------------
void loadInitTemplateByWave(SynthEngine &synth, uint8_t waveIndex) {
    waveIndex %= numWaveformsAll;
    WaveformType wfType = waveformListAll[waveIndex];
    uint8_t waveCC = ccFromWaveform(wfType);

    AudioNoInterrupts();

    sendCC(synth, CC::OSC1_WAVE, waveCC);
    sendCC(synth, CC::OSC2_WAVE, waveCC);

    sendCC(synth, CC::OSC_MIX_BALANCE, 0);
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

    // Pitch envelope — neutral defaults
    sendCC(synth, CC::PITCH_ENV_ATTACK,  5);
    sendCC(synth, CC::PITCH_ENV_DECAY,   40);
    sendCC(synth, CC::PITCH_ENV_SUSTAIN, 0);
    sendCC(synth, CC::PITCH_ENV_RELEASE, 10);
    sendCC(synth, CC::PITCH_ENV_DEPTH,   64);   // 64 = 0 semitones = neutral

    // Velocity sensitivity — all off
    sendCC(synth, CC::VELOCITY_AMP_SENS,    0);
    sendCC(synth, CC::VELOCITY_FILTER_SENS, 0);
    sendCC(synth, CC::VELOCITY_ENV_SENS,    0);

    // JPFX Tone (neutral)
    sendCC(synth, CC::FX_BASS_GAIN, 64);
    sendCC(synth, CC::FX_TREBLE_GAIN, 64);

    // JPFX Modulation (off)
    sendCC(synth, CC::FX_MOD_EFFECT, 0);
    sendCC(synth, CC::FX_MOD_MIX, 64);
    sendCC(synth, CC::FX_MOD_RATE, 0);
    sendCC(synth, CC::FX_MOD_FEEDBACK, 0);

    // JPFX Delay (off)
    sendCC(synth, CC::FX_JPFX_DELAY_EFFECT, 0);
    sendCC(synth, CC::FX_JPFX_DELAY_MIX, 64);
    sendCC(synth, CC::FX_JPFX_DELAY_FEEDBACK, 0);
    sendCC(synth, CC::FX_JPFX_DELAY_TIME, 0);

    // JPFX Dry mix
    sendCC(synth, CC::FX_DRY_MIX, 127);

    sendCC(synth, CC::GLIDE_ENABLE, 0);
    sendCC(synth, CC::GLIDE_TIME,   0);
    sendCC(synth, CC::AMP_MOD_FIXED_LEVEL, 127);
    sendCC(synth, CC::POLY_MODE,    0);
    sendCC(synth, CC::UNISON_DETUNE, 64);

    AudioInterrupts();
}

// ---------------------------------------------------------------------------
// Microsphere bank — 64-byte raw SysEx via kSlots mapping table
// ---------------------------------------------------------------------------
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
// No transform required — iterate data[2..127] and dispatch each CC.
//
// CC 0 is unused (bank select). CC 1 is mod wheel (live, not preset state).
//
// Cost: 126 handleControlChange() calls per load. At ~1 µs each on
// Teensy 4.1 @ 816 MHz, total load time is well under 1 ms.
// ---------------------------------------------------------------------------
void loadTUSPreset(SynthEngine& synth, int index) {
    if (index < 0 || index >= kTUS_COUNT) return;

    // Copy from PROGMEM to stack (safe on Teensy ARM, required on AVR)
    TUSPatchCC p;
    memcpy_P(&p, &kTUS_Patches[index], sizeof(TUSPatchCC));

    AudioNoInterrupts();

    // Send every CC value starting at CC 2
    for (uint8_t cc = 2; cc < 128; cc++) {
        synth.handleControlChange(1, cc, p.data[cc]);
    }

    AudioInterrupts();
}

} // namespace Presets
