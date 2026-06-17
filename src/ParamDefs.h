// =============================================================================
// ParamDefs.h — JT-8000 Unified Parameter Definitions
// =============================================================================
//
// SINGLE SOURCE OF TRUTH for all synth parameters.
//
// Replaces (Step 2 — when stable):
//   CCDefs.h          — CC number constants + name()
//   JT8000_Sections.h — UI section/group layout + CtrlType
//   PatchSchema.h     — patchable CC list (now auto-derived from scope)
//
// Does NOT replace:
//   Mapping.h         — DSP conversion curves (audio code, not metadata)
//   ParamMap.h/cpp    — SysEx 14-bit ParamID routing (protocol concern)
//   WaveForms.h       — Teensy waveform IDs & helpers (Audio Library)
//   LFOBlock.h        — LFODestination enum (runtime class)
//   BPMClockManager.h — TimingMode enum (runtime class)
//   StepSequencer.h   — SeqDirection enum (runtime class)
//   AudioFilterVABank.h — VAFilterType enum (runtime class)
//
// Those files keep their enums because the audio code uses them at runtime.
// ParamDefs references their name arrays for UI display, but doesn't own them.
//
// FLASH ONLY — all constexpr, zero RAM cost.
//
// © 2025 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace ParamDefs {

// =============================================================================
// Enums
// =============================================================================

/// How the parameter behaves on the UI
enum ParamType : uint8_t {
    CONTINUOUS = 0,   // 0–127 slider (or bipolar)
    SELECT,           // Discrete named options (listbox)
    TOGGLE,           // On/off (0 or 127)
    ENVELOPE,         // Part of an ADSR group (handled by envelope widget)
    GRID,             // Step sequencer grid (handled by grid widget)
};

/// Where the parameter is stored / who owns it
enum Scope : uint8_t {
    PATCH = 0,        // Per-patch, saved/restored by Patch.cpp
    PERF,             // Per-performance, owned by LayerManager
    GLOBAL_FX,        // Global reverb, owned by GlobalFX
    INTERNAL,         // Internal CCs (>127), not sent on MIDI wire
};

// =============================================================================
// Option name arrays — one per SELECT parameter
// =============================================================================
// These are the canonical display strings for discrete parameters.
// Bucket-midpoint encode/decode in Mapping.h and SynthEngine.cpp must
// agree on the option count. Adding an option here means updating the
// corresponding decode logic.

static constexpr const char* kOscWaveOptions[] = {
    "SIN", "SAW", "SQR", "TRI", "ARB", "PLS", "rSAW", "S&H",
    "vTRI", "BLS", "rBLS", "BLSQ", "BLP", "SSAW"
};
static constexpr uint8_t kOscWaveCount = sizeof(kOscWaveOptions) / sizeof(kOscWaveOptions[0]);

static constexpr const char* kPitchOffsetOptions[] = {
    "-24", "-12", "0", "+12", "+24"
};
static constexpr uint8_t kPitchOffsetCount = 5;

static constexpr const char* kFilterEngineOptions[] = {
    "OBXa", "VA"
};
static constexpr uint8_t kFilterEngineCount = 2;

static constexpr const char* kFilterModeOptions[] = {
    "4-Pole", "2-Pole", "2P BP", "2P Push", "Xpander", "Xpander M"
};
static constexpr uint8_t kFilterModeCount = 6;

// VA filter types — names from AudioFilterVABank.h kVAFilterNames[]
static constexpr const char* kVAFilterOptions[] = {
    "SVF LP2", "SVF HP2", "SVF BP2", "SVF NOTCH", "SVF AP",
    "Moog LP4", "Moog LP2", "Moog BP2", "Diode LP4",
    "Korg35 LP", "Korg35 HP", "TPT1 LP", "TPT1 HP"
};
static constexpr uint8_t kVAFilterCount = 13;

// Xpander sub-modes (0–14)
static constexpr const char* kXpanderModeOptions[] = {
    "0", "1", "2", "3", "4", "5", "6", "7",
    "8", "9", "10", "11", "12", "13", "14"
};
static constexpr uint8_t kXpanderModeCount = 15;

// LFO waveforms — same options as LFO block uses internally
static constexpr const char* kLFOWaveOptions[] = {
    "SIN", "TRI", "SAW", "SQR", "S&H", "NOISE"
};
static constexpr uint8_t kLFOWaveCount = 6;

// LFO destinations — names from LFOBlock.h LFODestNames[]
static constexpr const char* kLFODestOptions[] = {
    "None", "Pitch", "Filter", "PWM", "Amp"
};
static constexpr uint8_t kLFODestCount = 5;

// Timing modes — names from BPMClockManager.cpp TimingModeNames[]
static constexpr const char* kTimingModeOptions[] = {
    "Free", "4 Bars", "2 Bars", "1 Bar", "1/2", "1/4",
    "1/8", "1/16", "1/32", "1/4T", "1/8T", "1/16T"
};
static constexpr uint8_t kTimingModeCount = 12;

// Poly mode
static constexpr const char* kPolyModeOptions[] = {
    "Poly", "Mono", "Unison"
};
static constexpr uint8_t kPolyModeCount = 3;

// FX mod effect: -1=off, 0–10 = variations (12 options incl. OFF)
static constexpr const char* kFXModEffectOptions[] = {
    "OFF", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10"
};
static constexpr uint8_t kFXModEffectCount = 12;

// FX delay effect: -1=off, 0–4 = variations (6 options incl. OFF)
static constexpr const char* kFXDelayEffectOptions[] = {
    "OFF", "0", "1", "2", "3", "4"
};
static constexpr uint8_t kFXDelayEffectCount = 6;

// FX Drive: 0=off, soft, hard
static constexpr const char* kDriveOptions[] = {
    "OFF", "Soft", "Hard"
};
static constexpr uint8_t kDriveCount = 3;

// Sequencer direction
static constexpr const char* kSeqDirOptions[] = {
    "Fwd", "Rev", "Bounce", "Random"
};
static constexpr uint8_t kSeqDirCount = 4;

// Sequencer destination — same as LFO dest
static constexpr const char* kSeqDestOptions[] = {
    "None", "Pitch", "Filter", "PWM", "Amp"
};
static constexpr uint8_t kSeqDestCount = 5;

// Performance mode
static constexpr const char* kPerfModeOptions[] = {
    "Single", "Layer", "Split"
};
static constexpr uint8_t kPerfModeCount = 3;

// Edit target
static constexpr const char* kEditTargetOptions[] = {
    "A", "B", "Both"
};
static constexpr uint8_t kEditTargetCount = 3;

// Clock source
static constexpr const char* kClockSourceOptions[] = {
    "Internal", "Ext MIDI"
};
static constexpr uint8_t kClockSourceCount = 2;


// =============================================================================
// ParamDef — one entry per parameter
// =============================================================================
struct ParamDef {
    uint8_t     cc;           // CC number (0–127 for MIDI, 140+ for internal)
    const char* name;         // Human-readable name (was CCDefs::name())
    const char* label;        // Short UI label (was Sections CtrlType label)
    ParamType   type;         // CONTINUOUS / SELECT / TOGGLE / ENVELOPE / GRID
    Scope       scope;        // PATCH / PERF / GLOBAL_FX / INTERNAL
    uint8_t     defaultVal;   // Init patch default (CC value)
    bool        bipolar;      // true = CC 64 is zero
    uint8_t     optionCount;  // For SELECT: number of discrete options
    const char* const* options; // For SELECT: pointer to option name array
    uint8_t     section;      // Section index (0–15)
    uint8_t     group;        // Group index within section
};

// =============================================================================
// Section / Group layout
// =============================================================================
static constexpr uint8_t NUM_SECTIONS = 16;

static constexpr const char* kSectionNames[NUM_SECTIONS] = {
    "Oscillator 1",    // 0
    "Oscillator 2",    // 1
    "Mixer",           // 2
    "Filter",          // 3
    "Amp Envelope",    // 4
    "Filter Envelope", // 5
    "Pitch Envelope",  // 6
    "LFO 1",           // 7
    "LFO 2",           // 8
    "Effects",         // 9
    "Velocity",        // 10
    "Performance",     // 11
    "BPM Clock",       // 12
    "Step Sequencer",  // 13
    "Voice Mode",      // 14
    "Global Reverb",   // 15
};

static constexpr const char* kSectionTabs[NUM_SECTIONS] = {
    "OSC1", "OSC2", "MIX", "FLT", "AENV", "FENV", "PENV",
    "LF1", "LF2", "FX", "VEL", "PERF", "BPM", "SEQ", "VMDE", "GREV"
};

// Group names per section (nullptr terminates the list for each section)
// Referenced by group index in the ParamDef entries.
static constexpr const char* kGroupNames[][8] = {
    // [0] OSC1
    { "Wave & Tuning", "Supersaw", "Feedback", "DC & Ring", "Arbitrary", nullptr },
    // [1] OSC2
    { "Wave & Tuning", "Supersaw", "Feedback", "DC & Ring", "Arbitrary", nullptr },
    // [2] Mixer
    { "Levels", "Cross Mod & Sync", nullptr },
    // [3] Filter
    { "Core", "Engine", "Multimode", "Xpander", nullptr },
    // [4] Amp Env
    { "", nullptr },
    // [5] Filter Env
    { "", nullptr },
    // [6] Pitch Env
    { "", nullptr },
    // [7] LFO 1
    { "Core", "Per-Destination", "Routing", nullptr },
    // [8] LFO 2
    { "Core", "Per-Destination", "Routing", nullptr },
    // [9] Effects
    { "EQ & Drive", "Modulation", "Delay", "Output", nullptr },
    // [10] Velocity
    { "Sensitivity", nullptr },
    // [11] Performance
    { "Glide", "Voice", "Bend & Level", nullptr },
    // [12] BPM Clock
    { "Clock", nullptr },
    // [13] Step Sequencer
    { "Control", "Routing", "Timing", "Steps", nullptr },
    // [14] Voice Mode
    { "Layer Mode", "Voice Split", nullptr },
    // [15] Global Reverb
    { "Tank", "Extended", nullptr },
};

// =============================================================================
// CC Number Constants — backward-compatible namespace CC alias
// =============================================================================
// Drop-in replacement for CCDefs.h. Existing code that uses CC::OSC1_WAVE
// continues to work without modification.
namespace CC {
    // --- Oscillator Waveforms ---
    static constexpr uint8_t OSC1_WAVE              = 21;
    static constexpr uint8_t OSC2_WAVE              = 22;

    // --- Oscillator Pitch ---
    static constexpr uint8_t OSC1_PITCH_OFFSET      = 41;
    static constexpr uint8_t OSC2_PITCH_OFFSET      = 42;
    static constexpr uint8_t OSC1_DETUNE            = 43;
    static constexpr uint8_t OSC2_DETUNE            = 44;
    static constexpr uint8_t OSC1_FINE_TUNE         = 45;
    static constexpr uint8_t OSC2_FINE_TUNE         = 46;

    // --- Oscillator Mix ---
    static constexpr uint8_t OSC_MIX_BALANCE        = 47;
    static constexpr uint8_t OSC1_MIX               = 60;
    static constexpr uint8_t OSC2_MIX               = 61;
    static constexpr uint8_t SUB_MIX                = 58;
    static constexpr uint8_t NOISE_MIX              = 59;

    // --- Cross Mod & Sync ---
    static constexpr uint8_t OSC_CROSS_MOD_DEPTH    = 19;
    static constexpr uint8_t OSC_SYNC_ENABLE        = 20;

    // --- Supersaw ---
    static constexpr uint8_t SUPERSAW1_DETUNE       = 77;
    static constexpr uint8_t SUPERSAW1_MIX          = 78;
    static constexpr uint8_t SUPERSAW2_DETUNE       = 79;
    static constexpr uint8_t SUPERSAW2_MIX          = 80;

    // --- Oscillator Feedback ---
    static constexpr uint8_t OSC1_FEEDBACK_AMOUNT   = 123;
    static constexpr uint8_t OSC1_FEEDBACK_MIX      = 125;
    static constexpr uint8_t OSC2_FEEDBACK_AMOUNT   = 124;
    static constexpr uint8_t OSC2_FEEDBACK_MIX      = 126;

    // --- DC Oscillators & Ring ---
    static constexpr uint8_t OSC1_FREQ_DC           = 86;
    static constexpr uint8_t OSC1_SHAPE_DC          = 87;
    static constexpr uint8_t OSC2_FREQ_DC           = 88;
    static constexpr uint8_t OSC2_SHAPE_DC          = 89;
    static constexpr uint8_t RING1_MIX              = 91;
    static constexpr uint8_t RING2_MIX              = 92;

    // --- Arbitrary Waveform ---
    static constexpr uint8_t OSC1_ARB_BANK          = 101;
    static constexpr uint8_t OSC1_ARB_INDEX         = 83;
    static constexpr uint8_t OSC2_ARB_BANK          = 102;
    static constexpr uint8_t OSC2_ARB_INDEX         = 85;

    // --- Filter Core ---
    static constexpr uint8_t FILTER_CUTOFF          = 23;
    static constexpr uint8_t FILTER_RESONANCE       = 24;
    static constexpr uint8_t FILTER_ENV_AMOUNT      = 48;
    static constexpr uint8_t FILTER_KEY_TRACK       = 50;
    static constexpr uint8_t FILTER_OCTAVE_CONTROL  = 84;

    // --- Filter Topology ---
    static constexpr uint8_t FILTER_ENGINE          = 113;
    static constexpr uint8_t FILTER_MODE            = 112;
    static constexpr uint8_t VA_FILTER_TYPE         = 115;
    static constexpr uint8_t FILTER_OBXA_XPANDER_MODE   = 114;
    static constexpr uint8_t FILTER_OBXA_MULTIMODE      = 111;
    static constexpr uint8_t FILTER_OBXA_RES_MOD_DEPTH  = 117;

    // --- Filter mode constants ---
    static constexpr uint8_t FILTER_ENGINE_OBXA     = 0;
    static constexpr uint8_t FILTER_ENGINE_VA       = 1;
    static constexpr uint8_t FILTER_MODE_4POLE      = 0;
    static constexpr uint8_t FILTER_MODE_2POLE      = 1;
    static constexpr uint8_t FILTER_MODE_2POLE_BP   = 2;
    static constexpr uint8_t FILTER_MODE_2POLE_PUSH = 3;
    static constexpr uint8_t FILTER_MODE_XPANDER    = 4;
    static constexpr uint8_t FILTER_MODE_XPANDER_M  = 5;
    static constexpr uint8_t FILTER_MODE_COUNT      = 6;

    // --- Amp Envelope ---
    static constexpr uint8_t AMP_ATTACK             = 25;
    static constexpr uint8_t AMP_DECAY              = 26;
    static constexpr uint8_t AMP_SUSTAIN            = 27;
    static constexpr uint8_t AMP_RELEASE            = 28;

    // --- Amp Envelope Slopes ---
    static constexpr uint8_t AMP_ATTACK_CURVE       = 147;
    static constexpr uint8_t AMP_DECAY_CURVE        = 148;
    static constexpr uint8_t AMP_RELEASE_CURVE      = 149;

    // --- Filter Envelope ---
    static constexpr uint8_t FILTER_ENV_ATTACK      = 29;
    static constexpr uint8_t FILTER_ENV_DECAY       = 30;
    static constexpr uint8_t FILTER_ENV_SUSTAIN     = 31;
    static constexpr uint8_t FILTER_ENV_RELEASE     = 32;

    // --- Filter Envelope Slopes ---
    static constexpr uint8_t FILTER_ATTACK_CURVE    = 150;
    static constexpr uint8_t FILTER_DECAY_CURVE     = 151;
    static constexpr uint8_t FILTER_RELEASE_CURVE   = 152;

    // --- Pitch Envelope ---
    static constexpr uint8_t PITCH_ENV_ATTACK       = 65;
    static constexpr uint8_t PITCH_ENV_DECAY        = 66;
    static constexpr uint8_t PITCH_ENV_SUSTAIN      = 67;
    static constexpr uint8_t PITCH_ENV_RELEASE      = 68;
    static constexpr uint8_t PITCH_ENV_DEPTH        = 69;

    // --- Pitch Envelope Slopes ---
    static constexpr uint8_t PITCH_ATTACK_CURVE     = 153;
    static constexpr uint8_t PITCH_DECAY_CURVE      = 154;
    static constexpr uint8_t PITCH_RELEASE_CURVE    = 155;

    // --- LFO 1 ---
    static constexpr uint8_t LFO1_FREQ              = 54;
    static constexpr uint8_t LFO1_DEPTH             = 55;
    static constexpr uint8_t LFO1_DESTINATION       = 56;
    static constexpr uint8_t LFO1_WAVEFORM          = 62;
    static constexpr uint8_t LFO1_PITCH_DEPTH       = 33;
    static constexpr uint8_t LFO1_FILTER_DEPTH      = 34;
    static constexpr uint8_t LFO1_PWM_DEPTH         = 35;
    static constexpr uint8_t LFO1_AMP_DEPTH         = 36;
    static constexpr uint8_t LFO1_DELAY             = 37;
    static constexpr uint8_t LFO1_SYNC              = 120;

    // --- LFO 2 ---
    static constexpr uint8_t LFO2_FREQ              = 51;
    static constexpr uint8_t LFO2_DEPTH             = 52;
    static constexpr uint8_t LFO2_DESTINATION       = 53;
    static constexpr uint8_t LFO2_WAVEFORM          = 63;
    static constexpr uint8_t LFO2_PITCH_DEPTH       = 38;
    static constexpr uint8_t LFO2_FILTER_DEPTH      = 39;
    static constexpr uint8_t LFO2_PWM_DEPTH         = 40;
    static constexpr uint8_t LFO2_AMP_DEPTH         = 57;
    static constexpr uint8_t LFO2_DELAY             = 64;
    static constexpr uint8_t LFO2_SYNC              = 121;

    // --- Velocity ---
    static constexpr uint8_t VELOCITY_AMP_SENS      = 10;
    static constexpr uint8_t VELOCITY_FILTER_SENS   = 11;
    static constexpr uint8_t VELOCITY_ENV_SENS      = 12;

    // --- FX: EQ & Drive ---
    static constexpr uint8_t FX_BASS_GAIN           = 99;
    static constexpr uint8_t FX_TREBLE_GAIN         = 100;
    static constexpr uint8_t FX_DRIVE               = 16;

    // --- FX: Modulation ---
    static constexpr uint8_t FX_MOD_EFFECT          = 103;
    static constexpr uint8_t FX_MOD_MIX             = 104;
    static constexpr uint8_t FX_MOD_RATE            = 105;
    static constexpr uint8_t FX_MOD_FEEDBACK        = 106;

    // --- FX: Delay ---
    static constexpr uint8_t FX_JPFX_DELAY_EFFECT   = 107;
    static constexpr uint8_t FX_JPFX_DELAY_MIX      = 108;
    static constexpr uint8_t FX_JPFX_DELAY_FEEDBACK = 109;
    static constexpr uint8_t FX_JPFX_DELAY_TIME     = 110;
    static constexpr uint8_t FX_DELAY_SYNC          = 122;

    // --- FX: Output ---
    static constexpr uint8_t FX_DRY_MIX             = 74;
    static constexpr uint8_t FX_JPFX_MIX            = 76;

    // --- Global Reverb ---
    static constexpr uint8_t FX_REVERB_SIZE         = 70;
    static constexpr uint8_t FX_REVERB_DAMP         = 71;
    static constexpr uint8_t FX_REVERB_LODAMP       = 93;
    static constexpr uint8_t FX_REVERB_MIX          = 75;
    static constexpr uint8_t FX_REVERB_BYPASS       = 94;
    static constexpr uint8_t FX_REVERB_SHIMMER      = 95;
    static constexpr uint8_t FX_REVERB_FREEZE       = 96;
    static constexpr uint8_t FX_REVERB_LOWPASS      = 97;
    static constexpr uint8_t FX_REVERB_HIPASS       = 98;

    // --- Glide ---
    static constexpr uint8_t GLIDE_ENABLE           = 81;
    static constexpr uint8_t GLIDE_TIME             = 82;

    // --- Performance ---
    static constexpr uint8_t AMP_MOD_FIXED_LEVEL    = 90;
    static constexpr uint8_t POLY_MODE              = 14;
    static constexpr uint8_t UNISON_DETUNE          = 15;
    static constexpr uint8_t PITCH_BEND_RANGE       = 127;

    // --- Step Sequencer ---
    static constexpr uint8_t SEQ_ENABLE             = 2;
    static constexpr uint8_t SEQ_STEPS              = 3;
    static constexpr uint8_t SEQ_GATE_LENGTH        = 4;
    static constexpr uint8_t SEQ_SLIDE              = 5;
    static constexpr uint8_t SEQ_DIRECTION          = 6;
    static constexpr uint8_t SEQ_RATE               = 7;
    static constexpr uint8_t SEQ_DEPTH              = 8;
    static constexpr uint8_t SEQ_DESTINATION        = 9;
    static constexpr uint8_t SEQ_RETRIGGER          = 13;
    static constexpr uint8_t SEQ_STEP_SELECT        = 17;
    static constexpr uint8_t SEQ_STEP_VALUE         = 18;
    static constexpr uint8_t SEQ_TIMING_MODE        = 116;

    // --- BPM Clock ---
    static constexpr uint8_t BPM_CLOCK_SOURCE       = 118;
    static constexpr uint8_t BPM_TEMPO              = 119;

    // --- Performance Layer (internal CCs > 127, not sent on MIDI wire) ---
    static constexpr uint8_t PERF_MODE              = 140;
    static constexpr uint8_t PERF_VOICE_SPLIT       = 141;
    static constexpr uint8_t PERF_SPLIT_NOTE        = 142;
    static constexpr uint8_t PERF_BALANCE           = 143;
    static constexpr uint8_t PERF_EDIT_TARGET       = 144;
    static constexpr uint8_t PERF_MIDI_CHANNEL_A    = 145;
    static constexpr uint8_t PERF_MIDI_CHANNEL_B    = 146;
// -----------------------------------------------------------------
    // Backward-compatible aliases and count constants
    // These exist in the original CCDefs.h and are referenced by
    // existing code. The canonical names are listed above.
    // -----------------------------------------------------------------

    // Timing mode aliases (old names → canonical names)
    static constexpr uint8_t LFO1_TIMING_MODE       = 120;  // = LFO1_SYNC
    static constexpr uint8_t LFO2_TIMING_MODE       = 121;  // = LFO2_SYNC
    static constexpr uint8_t DELAY_TIMING_MODE      = 122;  // = FX_DELAY_SYNC

    // BPM alias
    static constexpr uint8_t BPM_INTERNAL_TEMPO     = 119;  // = BPM_TEMPO

    // Old delay CCs (unused in current audio path but still in ParamMap SysEx table)
    static constexpr uint8_t FX_DELAY_TIME          = 72;
    static constexpr uint8_t FX_DELAY_FEEDBACK      = 73;

    // Count constants (not real CCs — used for bounds checking)
    static constexpr uint8_t FILTER_ENGINE_COUNT    = 2;


} // namespace CC

// =============================================================================
// Master parameter table
// =============================================================================
// Column key:
//   cc, name, label, type, scope, default, bipolar, optCount, options, section, group
//
// Section indices match kSectionNames[]. Group indices match kGroupNames[][].

static constexpr ParamDef kParams[] = {

    // =========================================================================
    // [0] OSC 1
    // =========================================================================
    // --- Group 0: Wave & Tuning ---
    { CC::OSC1_WAVE,           "Osc1 Wave",     "WAVE",     SELECT,     PATCH,  0,   false, kOscWaveCount,      kOscWaveOptions,      0, 0 },
    { CC::OSC1_PITCH_OFFSET,   "Osc1 Pitch",    "PITCH",    SELECT,     PATCH,  64,  false, kPitchOffsetCount,  kPitchOffsetOptions,  0, 0 },
    { CC::OSC1_FINE_TUNE,      "Osc1 Fine",     "FINE",     CONTINUOUS, PATCH,  64,  true,  0, nullptr,                                0, 0 },
    { CC::OSC1_DETUNE,         "Osc1 Detune",   "DETUNE",   CONTINUOUS, PATCH,  64,  true,  0, nullptr,                                0, 0 },
    // --- Group 1: Supersaw ---
    { CC::SUPERSAW1_DETUNE,    "SS1 Detune",    "DET",      CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                0, 1 },
    { CC::SUPERSAW1_MIX,       "SS1 Mix",       "MIX",      CONTINUOUS, PATCH,  64,  false, 0, nullptr,                                0, 1 },
    // --- Group 2: Feedback ---
    { CC::OSC1_FEEDBACK_AMOUNT,"Osc1 FB Amt",   "AMT",      CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                0, 2 },
    { CC::OSC1_FEEDBACK_MIX,   "Osc1 FB Mix",   "MIX",      CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                0, 2 },
    // --- Group 3: DC & Ring ---
    { CC::OSC1_FREQ_DC,        "Osc1 Freq DC",  "FREQ DC",  CONTINUOUS, PATCH,  64,  true,  0, nullptr,                                0, 3 },
    { CC::OSC1_SHAPE_DC,       "Osc1 Shape DC", "SHAPE DC", CONTINUOUS, PATCH,  64,  true,  0, nullptr,                                0, 3 },
    { CC::RING1_MIX,           "Ring 1 Mix",    "RING",     CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                0, 3 },
    // --- Group 4: Arbitrary ---
    { CC::OSC1_ARB_BANK,       "Osc1 Arb Bank", "BANK",     CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                0, 4 },
    { CC::OSC1_ARB_INDEX,      "Osc1 Arb Idx",  "IDX",      CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                0, 4 },

    // =========================================================================
    // [1] OSC 2
    // =========================================================================
    { CC::OSC2_WAVE,           "Osc2 Wave",     "WAVE",     SELECT,     PATCH,  0,   false, kOscWaveCount,      kOscWaveOptions,      1, 0 },
    { CC::OSC2_PITCH_OFFSET,   "Osc2 Pitch",    "PITCH",    SELECT,     PATCH,  64,  false, kPitchOffsetCount,  kPitchOffsetOptions,  1, 0 },
    { CC::OSC2_FINE_TUNE,      "Osc2 Fine",     "FINE",     CONTINUOUS, PATCH,  64,  true,  0, nullptr,                                1, 0 },
    { CC::OSC2_DETUNE,         "Osc2 Detune",   "DETUNE",   CONTINUOUS, PATCH,  64,  true,  0, nullptr,                                1, 0 },
    { CC::SUPERSAW2_DETUNE,    "SS2 Detune",    "DET",      CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                1, 1 },
    { CC::SUPERSAW2_MIX,       "SS2 Mix",       "MIX",      CONTINUOUS, PATCH,  64,  false, 0, nullptr,                                1, 1 },
    { CC::OSC2_FEEDBACK_AMOUNT,"Osc2 FB Amt",   "AMT",      CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                1, 2 },
    { CC::OSC2_FEEDBACK_MIX,   "Osc2 FB Mix",   "MIX",      CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                1, 2 },
    { CC::OSC2_FREQ_DC,        "Osc2 Freq DC",  "FREQ DC",  CONTINUOUS, PATCH,  64,  true,  0, nullptr,                                1, 3 },
    { CC::OSC2_SHAPE_DC,       "Osc2 Shape DC", "SHAPE DC", CONTINUOUS, PATCH,  64,  true,  0, nullptr,                                1, 3 },
    { CC::RING2_MIX,           "Ring 2 Mix",    "RING",     CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                1, 3 },
    { CC::OSC2_ARB_BANK,       "Osc2 Arb Bank", "BANK",     CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                1, 4 },
    { CC::OSC2_ARB_INDEX,      "Osc2 Arb Idx",  "IDX",      CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                1, 4 },

    // =========================================================================
    // [2] MIXER
    // =========================================================================
    { CC::OSC_MIX_BALANCE,     "Osc Balance",   "BAL",      CONTINUOUS, PATCH,  64,  true,  0, nullptr,                                2, 0 },
    { CC::OSC1_MIX,            "Osc1 Mix",      "OSC1",     CONTINUOUS, PATCH,  100, false, 0, nullptr,                                2, 0 },
    { CC::OSC2_MIX,            "Osc2 Mix",      "OSC2",     CONTINUOUS, PATCH,  100, false, 0, nullptr,                                2, 0 },
    { CC::SUB_MIX,             "Sub Mix",       "SUB",      CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                2, 0 },
    { CC::NOISE_MIX,           "Noise Mix",     "NOISE",    CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                2, 0 },
    { CC::OSC_CROSS_MOD_DEPTH, "Cross Mod",     "XMOD",     CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                2, 1 },
    { CC::OSC_SYNC_ENABLE,     "Osc Sync",      "SYNC",     TOGGLE,     PATCH,  0,   false, 0, nullptr,                                2, 1 },

    // =========================================================================
    // [3] FILTER
    // =========================================================================
    { CC::FILTER_CUTOFF,       "Cutoff",        "CUTOFF",   CONTINUOUS, PATCH,  127, false, 0, nullptr,                                3, 0 },
    { CC::FILTER_RESONANCE,    "Resonance",     "RESO",     CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                3, 0 },
    { CC::FILTER_ENV_AMOUNT,   "Filt Env Amt",  "ENV",      CONTINUOUS, PATCH,  64,  true,  0, nullptr,                                3, 0 },
    { CC::FILTER_KEY_TRACK,    "Key Track",     "KEY TRK",  CONTINUOUS, PATCH,  64,  true,  0, nullptr,                                3, 0 },
    { CC::FILTER_ENGINE,       "Filt Engine",   "ENGINE",   SELECT,     PATCH,  0,   false, kFilterEngineCount, kFilterEngineOptions,  3, 1 },
    { CC::FILTER_MODE,         "Filt Mode",     "MODE",     SELECT,     PATCH,  0,   false, kFilterModeCount,   kFilterModeOptions,    3, 1 },
    { CC::VA_FILTER_TYPE,      "VA Type",       "VA TYPE",  SELECT,     PATCH,  0,   false, kVAFilterCount,     kVAFilterOptions,      3, 1 },
    { CC::FILTER_OCTAVE_CONTROL,"Oct Control",  "OCT CTRL", CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                3, 2 },
    { CC::FILTER_OBXA_RES_MOD_DEPTH,"Res Mod",  "RES MOD",  CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                3, 2 },
    { CC::FILTER_OBXA_MULTIMODE,"Multimode",    "MULTI",    CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                3, 2 },
    { CC::FILTER_OBXA_XPANDER_MODE,"Xpander",   "XP MODE",  SELECT,     PATCH,  0,   false, kXpanderModeCount,  kXpanderModeOptions,   3, 3 },

    // =========================================================================
    // [4] AMP ENVELOPE
    // =========================================================================
    { CC::AMP_ATTACK,          "Amp Attack",    "ATTACK",   ENVELOPE,   PATCH,  0,   false, 0, nullptr,                                4, 0 },
    { CC::AMP_ATTACK_CURVE,    "Amp Atk Slp",   "ATK SLOPE",ENVELOPE,   PATCH,  64,  false, 0, nullptr,                                4, 0 },
    { CC::AMP_DECAY,           "Amp Decay",     "DECAY",    ENVELOPE,   PATCH,  64,  false, 0, nullptr,                                4, 0 },
    { CC::AMP_DECAY_CURVE,     "Amp Dec Slp",   "DEC SLOPE",ENVELOPE,   PATCH,  64,  false, 0, nullptr,                                4, 0 },
    { CC::AMP_SUSTAIN,         "Amp Sustain",   "SUSTAIN",  ENVELOPE,   PATCH,  127, false, 0, nullptr,                                4, 0 },
    { CC::AMP_RELEASE,         "Amp Release",   "RELEASE",  ENVELOPE,   PATCH,  20,  false, 0, nullptr,                                4, 0 },
    { CC::AMP_RELEASE_CURVE,   "Amp Rel Slp",   "REL SLOPE",ENVELOPE,   PATCH,  64,  false, 0, nullptr,                                4, 0 },

    // =========================================================================
    // [5] FILTER ENVELOPE
    // =========================================================================
    { CC::FILTER_ENV_ATTACK,   "Filt Attack",   "ATTACK",   ENVELOPE,   PATCH,  0,   false, 0, nullptr,                                5, 0 },
    { CC::FILTER_ATTACK_CURVE, "Filt Atk Slp",  "ATK SLOPE",ENVELOPE,   PATCH,  64,  false, 0, nullptr,                                5, 0 },
    { CC::FILTER_ENV_DECAY,    "Filt Decay",    "DECAY",    ENVELOPE,   PATCH,  64,  false, 0, nullptr,                                5, 0 },
    { CC::FILTER_DECAY_CURVE,  "Filt Dec Slp",  "DEC SLOPE",ENVELOPE,   PATCH,  64,  false, 0, nullptr,                                5, 0 },
    { CC::FILTER_ENV_SUSTAIN,  "Filt Sustain",  "SUSTAIN",  ENVELOPE,   PATCH,  0,   false, 0, nullptr,                                5, 0 },
    { CC::FILTER_ENV_RELEASE,  "Filt Release",  "RELEASE",  ENVELOPE,   PATCH,  20,  false, 0, nullptr,                                5, 0 },
    { CC::FILTER_RELEASE_CURVE,"Filt Rel Slp",  "REL SLOPE",ENVELOPE,   PATCH,  64,  false, 0, nullptr,                                5, 0 },

    // =========================================================================
    // [6] PITCH ENVELOPE
    // =========================================================================
    { CC::PITCH_ENV_DEPTH,     "Pitch Depth",   "DEPTH",    CONTINUOUS, PATCH,  64,  true,  0, nullptr,                                6, 0 },
    { CC::PITCH_ENV_ATTACK,    "Pitch Attack",  "ATTACK",   ENVELOPE,   PATCH,  0,   false, 0, nullptr,                                6, 0 },
    { CC::PITCH_ATTACK_CURVE,  "Pitch Atk Slp", "ATK SLOPE",ENVELOPE,   PATCH,  64,  false, 0, nullptr,                                6, 0 },
    { CC::PITCH_ENV_DECAY,     "Pitch Decay",   "DECAY",    ENVELOPE,   PATCH,  64,  false, 0, nullptr,                                6, 0 },
    { CC::PITCH_DECAY_CURVE,   "Pitch Dec Slp", "DEC SLOPE",ENVELOPE,   PATCH,  64,  false, 0, nullptr,                                6, 0 },
    { CC::PITCH_ENV_SUSTAIN,   "Pitch Sustain", "SUSTAIN",  ENVELOPE,   PATCH,  0,   false, 0, nullptr,                                6, 0 },
    { CC::PITCH_ENV_RELEASE,   "Pitch Release", "RELEASE",  ENVELOPE,   PATCH,  20,  false, 0, nullptr,                                6, 0 },
    { CC::PITCH_RELEASE_CURVE, "Pitch Rel Slp", "REL SLOPE",ENVELOPE,   PATCH,  64,  false, 0, nullptr,                                6, 0 },

    // =========================================================================
    // [7] LFO 1
    // =========================================================================
    { CC::LFO1_WAVEFORM,       "LFO1 Wave",     "WAVE",     SELECT,     PATCH,  0,   false, kLFOWaveCount,      kLFOWaveOptions,       7, 0 },
    { CC::LFO1_FREQ,           "LFO1 Rate",     "RATE",     CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                 7, 0 },
    { CC::LFO1_DEPTH,          "LFO1 Depth",    "DEPTH",    CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                 7, 0 },
    { CC::LFO1_SYNC,           "LFO1 Sync",     "SYNC",     SELECT,     PATCH,  0,   false, kTimingModeCount,   kTimingModeOptions,    7, 0 },
    { CC::LFO1_PITCH_DEPTH,    "LFO1 Pitch",    "PITCH",    CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                 7, 1 },
    { CC::LFO1_FILTER_DEPTH,   "LFO1 Filter",   "FILTER",   CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                 7, 1 },
    { CC::LFO1_PWM_DEPTH,      "LFO1 PWM",      "PWM",      CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                 7, 1 },
    { CC::LFO1_AMP_DEPTH,      "LFO1 Amp",      "AMP",      CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                 7, 1 },
    { CC::LFO1_DESTINATION,    "LFO1 Dest",     "DEST",     SELECT,     PATCH,  0,   false, kLFODestCount,      kLFODestOptions,       7, 2 },
    { CC::LFO1_DELAY,          "LFO1 Delay",    "DELAY",    CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                 7, 2 },

    // =========================================================================
    // [8] LFO 2
    // =========================================================================
    { CC::LFO2_WAVEFORM,       "LFO2 Wave",     "WAVE",     SELECT,     PATCH,  0,   false, kLFOWaveCount,      kLFOWaveOptions,       8, 0 },
    { CC::LFO2_FREQ,           "LFO2 Rate",     "RATE",     CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                 8, 0 },
    { CC::LFO2_DEPTH,          "LFO2 Depth",    "DEPTH",    CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                 8, 0 },
    { CC::LFO2_SYNC,           "LFO2 Sync",     "SYNC",     SELECT,     PATCH,  0,   false, kTimingModeCount,   kTimingModeOptions,    8, 0 },
    { CC::LFO2_PITCH_DEPTH,    "LFO2 Pitch",    "PITCH",    CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                 8, 1 },
    { CC::LFO2_FILTER_DEPTH,   "LFO2 Filter",   "FILTER",   CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                 8, 1 },
    { CC::LFO2_PWM_DEPTH,      "LFO2 PWM",      "PWM",      CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                 8, 1 },
    { CC::LFO2_AMP_DEPTH,      "LFO2 Amp",      "AMP",      CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                 8, 1 },
    { CC::LFO2_DESTINATION,    "LFO2 Dest",     "DEST",     SELECT,     PATCH,  0,   false, kLFODestCount,      kLFODestOptions,       8, 2 },
    { CC::LFO2_DELAY,          "LFO2 Delay",    "DELAY",    CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                 8, 2 },

    // =========================================================================
    // [9] EFFECTS
    // =========================================================================
    { CC::FX_BASS_GAIN,        "Bass",          "BASS",     CONTINUOUS, PATCH,  64,  true,  0, nullptr,                                 9, 0 },
    { CC::FX_TREBLE_GAIN,      "Treble",        "TREBLE",   CONTINUOUS, PATCH,  64,  true,  0, nullptr,                                 9, 0 },
    { CC::FX_DRIVE,            "Drive",         "DRIVE",    SELECT,     PATCH,  0,   false, kDriveCount,        kDriveOptions,          9, 0 },
    { CC::FX_MOD_EFFECT,       "Mod FX",        "TYPE",     SELECT,     PATCH,  0,   false, kFXModEffectCount,  kFXModEffectOptions,    9, 1 },
    { CC::FX_MOD_MIX,          "Mod Mix",       "MIX",      CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                 9, 1 },
    { CC::FX_MOD_RATE,         "Mod Rate",      "RATE",     CONTINUOUS, PATCH,  64,  false, 0, nullptr,                                 9, 1 },
    { CC::FX_MOD_FEEDBACK,     "Mod FB",        "FB",       CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                 9, 1 },
    { CC::FX_JPFX_DELAY_EFFECT,"Delay FX",      "TYPE",     SELECT,     PATCH,  0,   false, kFXDelayEffectCount,kFXDelayEffectOptions,  9, 2 },
    { CC::FX_JPFX_DELAY_TIME,  "Delay Time",    "TIME",     CONTINUOUS, PATCH,  64,  false, 0, nullptr,                                 9, 2 },
    { CC::FX_JPFX_DELAY_MIX,   "Delay Mix",     "MIX",      CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                 9, 2 },
    { CC::FX_JPFX_DELAY_FEEDBACK,"Delay FB",    "FB",       CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                 9, 2 },
    { CC::FX_DELAY_SYNC,       "Delay Sync",    "SYNC",     SELECT,     PATCH,  0,   false, kTimingModeCount,   kTimingModeOptions,    9, 2 },
    { CC::FX_DRY_MIX,          "Dry Mix",       "DRY",      CONTINUOUS, PATCH,  127, false, 0, nullptr,                                 9, 3 },
    { CC::FX_JPFX_MIX,         "JPFX Mix",      "JPFX",     CONTINUOUS, PATCH,  127, false, 0, nullptr,                                 9, 3 },

    // =========================================================================
    // [10] VELOCITY
    // =========================================================================
    { CC::VELOCITY_AMP_SENS,   "Vel Amp",       "AMP",      CONTINUOUS, PATCH,  64,  false, 0, nullptr,                                10, 0 },
    { CC::VELOCITY_FILTER_SENS,"Vel Filter",    "FILTER",   CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                10, 0 },
    { CC::VELOCITY_ENV_SENS,   "Vel Env",       "ENV",      CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                10, 0 },

    // =========================================================================
    // [11] PERFORMANCE
    // =========================================================================
    { CC::GLIDE_ENABLE,        "Glide",         "ON",       TOGGLE,     PATCH,  0,   false, 0, nullptr,                                11, 0 },
    { CC::GLIDE_TIME,          "Glide Time",    "TIME",     CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                11, 0 },
    { CC::POLY_MODE,           "Poly Mode",     "MODE",     SELECT,     PATCH,  0,   false, kPolyModeCount,     kPolyModeOptions,     11, 1 },
    { CC::UNISON_DETUNE,       "Unison Det",    "UNI DET",  CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                11, 1 },
    { CC::PITCH_BEND_RANGE,    "Bend Range",    "BEND",     CONTINUOUS, PATCH,  12,  false, 0, nullptr,                                11, 2 },
    { CC::AMP_MOD_FIXED_LEVEL, "Amp Level",     "AMP LVL",  CONTINUOUS, PATCH,  127, false, 0, nullptr,                                11, 2 },

    // =========================================================================
    // [12] BPM CLOCK
    // =========================================================================
    { CC::BPM_CLOCK_SOURCE,    "Clock Src",     "SRC",      SELECT,     PERF,   0,   false, kClockSourceCount,  kClockSourceOptions,  12, 0 },
    { CC::BPM_TEMPO,           "BPM",           "BPM",      CONTINUOUS, PERF,   64,  false, 0, nullptr,                                12, 0 },

    // =========================================================================
    // [13] STEP SEQUENCER
    // =========================================================================
    { CC::SEQ_ENABLE,          "Seq Enable",    "ON",       TOGGLE,     PATCH,  0,   false, 0, nullptr,                                13, 0 },
    { CC::SEQ_STEPS,           "Seq Steps",     "STEPS",    CONTINUOUS, PATCH,  64,  false, 0, nullptr,                                13, 0 },
    { CC::SEQ_GATE_LENGTH,     "Seq Gate",      "GATE",     CONTINUOUS, PATCH,  64,  false, 0, nullptr,                                13, 0 },
    { CC::SEQ_SLIDE,           "Seq Slide",     "SLIDE",    CONTINUOUS, PATCH,  0,   false, 0, nullptr,                                13, 0 },
    { CC::SEQ_DIRECTION,       "Seq Dir",       "DIR",      SELECT,     PATCH,  0,   false, kSeqDirCount,       kSeqDirOptions,       13, 1 },
    { CC::SEQ_DESTINATION,     "Seq Dest",      "DEST",     SELECT,     PATCH,  0,   false, kSeqDestCount,      kSeqDestOptions,      13, 1 },
    { CC::SEQ_DEPTH,           "Seq Depth",     "DEPTH",    CONTINUOUS, PATCH,  64,  true,  0, nullptr,                                13, 1 },
    { CC::SEQ_RETRIGGER,       "Seq Retrig",    "RETRIG",   TOGGLE,     PATCH,  0,   false, 0, nullptr,                                13, 1 },
    { CC::SEQ_RATE,            "Seq Rate",      "RATE",     CONTINUOUS, PATCH,  64,  false, 0, nullptr,                                13, 2 },
    { CC::SEQ_TIMING_MODE,     "Seq Sync",      "SYNC",     SELECT,     PATCH,  0,   false, kTimingModeCount,   kTimingModeOptions,   13, 2 },
    // --- Group 3: Steps (grid widget) ---
    { CC::SEQ_STEP_SELECT,     "Step Select",   "SEL",      GRID,       PATCH,  0,   false, 0, nullptr,                                13, 3 },
    { CC::SEQ_STEP_VALUE,      "Step Value",    "VAL",      GRID,       PATCH,  64,  false, 0, nullptr,                                13, 3 },

    // =========================================================================
    // [14] VOICE MODE (internal CCs, not sent on MIDI wire)
    // =========================================================================
    { CC::PERF_MODE,           "Perf Mode",     "MODE",     SELECT,     INTERNAL, 0,  false, kPerfModeCount,    kPerfModeOptions,     14, 0 },
    { CC::PERF_EDIT_TARGET,    "Edit Target",   "EDIT",     SELECT,     INTERNAL, 0,  false, kEditTargetCount,  kEditTargetOptions,   14, 0 },
    { CC::PERF_MIDI_CHANNEL_A, "MIDI Ch A",     "CH A",     CONTINUOUS, INTERNAL, 0,  false, 0, nullptr,                               14, 0 },
    { CC::PERF_MIDI_CHANNEL_B, "MIDI Ch B",     "CH B",     CONTINUOUS, INTERNAL, 16, false, 0, nullptr,                               14, 0 },
    { CC::PERF_VOICE_SPLIT,    "Voice Split",   "VOICES",   CONTINUOUS, INTERNAL, 64, false, 0, nullptr,                               14, 1 },
    { CC::PERF_SPLIT_NOTE,     "Split Point",   "SPLIT",    CONTINUOUS, INTERNAL, 60, false, 0, nullptr,                               14, 1 },
    { CC::PERF_BALANCE,        "Layer Bal",     "BALANCE",  CONTINUOUS, INTERNAL, 64, true,  0, nullptr,                               14, 1 },

    // =========================================================================
    // [15] GLOBAL REVERB
    // =========================================================================
    { CC::FX_REVERB_SIZE,      "Rev Size",      "SIZE",     CONTINUOUS, GLOBAL_FX, 80,  false, 0, nullptr,                             15, 0 },
    { CC::FX_REVERB_DAMP,      "Rev Hi Damp",   "HI DMP",   CONTINUOUS, GLOBAL_FX, 60,  false, 0, nullptr,                             15, 0 },
    { CC::FX_REVERB_LODAMP,    "Rev Lo Damp",   "LO DMP",   CONTINUOUS, GLOBAL_FX, 40,  false, 0, nullptr,                             15, 0 },
    { CC::FX_REVERB_MIX,       "Rev Mix",       "MIX",      CONTINUOUS, GLOBAL_FX, 40,  false, 0, nullptr,                             15, 0 },
    { CC::FX_REVERB_BYPASS,    "Rev Bypass",    "BYPASS",   TOGGLE,     GLOBAL_FX, 0,   false, 0, nullptr,                             15, 0 },
    { CC::FX_REVERB_SHIMMER,   "Rev Shimmer",   "SHIMMER",  CONTINUOUS, GLOBAL_FX, 0,   false, 0, nullptr,                             15, 1 },
    { CC::FX_REVERB_FREEZE,    "Rev Freeze",    "FREEZE",   TOGGLE,     GLOBAL_FX, 0,   false, 0, nullptr,                             15, 1 },
    { CC::FX_REVERB_LOWPASS,   "Rev Lo Pass",   "LO PASS",  CONTINUOUS, GLOBAL_FX, 127, false, 0, nullptr,                             15, 1 },
    { CC::FX_REVERB_HIPASS,    "Rev Hi Pass",   "HI PASS",  CONTINUOUS, GLOBAL_FX, 0,   false, 0, nullptr,                             15, 1 },
};

static constexpr size_t kParamCount = sizeof(kParams) / sizeof(kParams[0]);

// =============================================================================
// Lookup helpers
// =============================================================================

/// Find a parameter definition by CC number. Returns nullptr if not found.
/// O(N) linear scan — fine for UI-rate lookups, not audio-hot.
inline const ParamDef* findByCC(uint8_t cc) {
    for (size_t i = 0; i < kParamCount; ++i) {
        if (kParams[i].cc == cc) return &kParams[i];
    }
    return nullptr;
}

// Reopen namespace CC to add the backward-compatible name() alias.
// Must be here (after kParams[] is defined) so it can see the table.
namespace CC {
    inline const char* name(uint8_t cc) {
        const ParamDef* p = findByCC(cc);
        return p ? p->name : "???";
    }
}

/// Count of parameters in a given section.
inline size_t countInSection(uint8_t section) {
    size_t n = 0;
    for (size_t i = 0; i < kParamCount; ++i) {
        if (kParams[i].section == section) ++n;
    }
    return n;
}

/// Iterate parameters in a section/group. Returns first match at or after startIdx.
/// Returns kParamCount if no more matches. Use in a loop:
///   for (size_t i = firstInSection(s); i < kParamCount; i = nextInSection(s, i+1))
inline size_t firstInSection(uint8_t section) {
    for (size_t i = 0; i < kParamCount; ++i) {
        if (kParams[i].section == section) return i;
    }
    return kParamCount;
}

inline size_t nextInSection(uint8_t section, size_t startIdx) {
    for (size_t i = startIdx; i < kParamCount; ++i) {
        if (kParams[i].section == section) return i;
    }
    return kParamCount;
}

// =============================================================================
// Auto-derived patchable CC list (replaces PatchSchema.h)
// =============================================================================
// At compile time, any param with scope == PATCH is patchable.
// This helper lets Patch.cpp iterate without a separate array.
inline bool isPatchable(uint8_t cc) {
    const ParamDef* p = findByCC(cc);
    return p && p->scope == PATCH;
}

// =============================================================================
// PatchSchema — ordered CC list for patch save/load and FactoryBank columns.
// =============================================================================
// This MUST stay as an explicit ordered array because FactoryBank.h depends
// on the exact column ordering. Do not auto-generate.
// When adding a new PATCH-scoped CC to kParams[], add it here too.
namespace PatchSchema {

static constexpr uint8_t kPatchableCCs[] = {
    // ---- Oscillator waveforms ----
    CC::OSC1_WAVE, CC::OSC2_WAVE,
    // ---- Oscillator pitch ----
    CC::OSC1_PITCH_OFFSET, CC::OSC2_PITCH_OFFSET,
    CC::OSC1_FINE_TUNE,    CC::OSC2_FINE_TUNE,
    CC::OSC1_DETUNE,       CC::OSC2_DETUNE,
    // ---- Oscillator mix ----
    CC::OSC_MIX_BALANCE,
    CC::OSC1_MIX, CC::OSC2_MIX,
    CC::SUB_MIX,  CC::NOISE_MIX,
    // ---- Supersaw ----
    CC::SUPERSAW1_DETUNE, CC::SUPERSAW1_MIX,
    CC::SUPERSAW2_DETUNE, CC::SUPERSAW2_MIX,
    // ---- Oscillator feedback ----
    CC::OSC1_FEEDBACK_AMOUNT, CC::OSC2_FEEDBACK_AMOUNT,
    CC::OSC1_FEEDBACK_MIX,    CC::OSC2_FEEDBACK_MIX,
    // ---- Filter core ----
    CC::FILTER_CUTOFF, CC::FILTER_RESONANCE,
    CC::FILTER_ENV_AMOUNT, CC::FILTER_KEY_TRACK,
    CC::FILTER_OCTAVE_CONTROL,
    // ---- Filter topology ----
    CC::FILTER_ENGINE, CC::FILTER_MODE, CC::VA_FILTER_TYPE,
    CC::FILTER_OBXA_XPANDER_MODE,
    CC::FILTER_OBXA_MULTIMODE, CC::FILTER_OBXA_RES_MOD_DEPTH,
    // ---- Amp envelope ----
    CC::AMP_ATTACK, CC::AMP_DECAY, CC::AMP_SUSTAIN, CC::AMP_RELEASE,
    // ---- Filter envelope ----
    CC::FILTER_ENV_ATTACK, CC::FILTER_ENV_DECAY,
    CC::FILTER_ENV_SUSTAIN, CC::FILTER_ENV_RELEASE,
    // ---- LFO1 ----
    CC::LFO1_FREQ, CC::LFO1_DEPTH, CC::LFO1_DESTINATION, CC::LFO1_WAVEFORM,
    CC::LFO1_PITCH_DEPTH, CC::LFO1_FILTER_DEPTH, CC::LFO1_PWM_DEPTH, CC::LFO1_AMP_DEPTH,
    CC::LFO1_DELAY,
    // ---- LFO2 ----
    CC::LFO2_FREQ, CC::LFO2_DEPTH, CC::LFO2_DESTINATION, CC::LFO2_WAVEFORM,
    CC::LFO2_PITCH_DEPTH, CC::LFO2_FILTER_DEPTH, CC::LFO2_PWM_DEPTH, CC::LFO2_AMP_DEPTH,
    CC::LFO2_DELAY,
    // ---- Pitch envelope ----
    CC::PITCH_ENV_ATTACK, CC::PITCH_ENV_DECAY,
    CC::PITCH_ENV_SUSTAIN, CC::PITCH_ENV_RELEASE,
    CC::PITCH_ENV_DEPTH,
    // ---- Velocity sensitivity ----
    CC::VELOCITY_AMP_SENS, CC::VELOCITY_FILTER_SENS, CC::VELOCITY_ENV_SENS,
    // ---- FX: Tone / Drive ----
    CC::FX_BASS_GAIN, CC::FX_TREBLE_GAIN, CC::FX_DRIVE,
    // ---- FX: Modulation ----
    CC::FX_MOD_EFFECT, CC::FX_MOD_MIX, CC::FX_MOD_RATE, CC::FX_MOD_FEEDBACK,
    // ---- FX: Delay ----
    CC::FX_JPFX_DELAY_EFFECT, CC::FX_JPFX_DELAY_MIX,
    CC::FX_JPFX_DELAY_FEEDBACK, CC::FX_JPFX_DELAY_TIME,
    // ---- FX: Output mix ----
    CC::FX_DRY_MIX, CC::FX_JPFX_MIX,
    // ---- Glide / global ----
    CC::GLIDE_ENABLE, CC::GLIDE_TIME,
    CC::AMP_MOD_FIXED_LEVEL,
    CC::POLY_MODE, CC::UNISON_DETUNE,
    // ---- Ring / DC ----
    CC::RING1_MIX, CC::RING2_MIX,
    CC::OSC1_FREQ_DC, CC::OSC1_SHAPE_DC,
    CC::OSC2_FREQ_DC, CC::OSC2_SHAPE_DC,
    // ---- Cross Modulation / Sync ----
    CC::OSC_CROSS_MOD_DEPTH, CC::OSC_SYNC_ENABLE,
    // ---- Pitch Bend ----
    CC::PITCH_BEND_RANGE,
    // ---- Extended Reverb ----
    CC::FX_REVERB_SHIMMER, CC::FX_REVERB_FREEZE,
    CC::FX_REVERB_LOWPASS, CC::FX_REVERB_HIPASS,
    // ---- Arbitrary Waveforms ----
    CC::OSC1_ARB_BANK, CC::OSC2_ARB_BANK,
    CC::OSC1_ARB_INDEX, CC::OSC2_ARB_INDEX,
};

static constexpr int kPatchableCount =
    sizeof(kPatchableCCs) / sizeof(kPatchableCCs[0]);

} // namespace PatchSchema

} // namespace ParamDefs

// =============================================================================
// Make CC::, PatchSchema::, and ParamDefs types accessible without prefix.
// Every file that includes ParamDefs.h gets this — no need for CCDefs.h.
// =============================================================================
using namespace ParamDefs;
