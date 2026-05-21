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
 // JT8000_Sections.h
// =============================================================================
// Section/group/control data model for the JT-8000 scrollable TFT UI.
//
// Mirrors the HTML editor's SECTIONS[] array — same 14 sections, same groups,
// same control assignments. The TFT renders these as an accordion list:
// one section expanded at a time, tap header to toggle open/closed.
//
// DATA FLOW:
//   JT8000_Sections.h  (this file — static data)
//     → HomeScreen     (reads sections, renders accordion + mini widgets)
//       → TFTWidgets   (draws individual knobs / selects / toggles)
//         → SynthEngine (getCC / setCC for live values)
//
// CONTROL TYPES:
//   CTRL_KNOB    — continuous 0-127, rendered as mini arc knob
//   CTRL_SELECT  — enumerated list, rendered as dropdown box
//   CTRL_TOGGLE  — on/off boolean, rendered as pill switch
//   CTRL_GRID    — step sequencer grid (special case, one per section max)
//
// MEMORY:
//   All data is constexpr / const in flash. Zero heap allocation.
//   Group and control arrays are fixed-size to avoid pointers-to-pointers.
//
// No audio or engine dependencies — include freely.
// =============================================================================

#pragma once
#include <Arduino.h>
#include "CCDefs.h"
#include "JT8000Colours.h"

// ---------------------------------------------------------------------------
// Capacity constants
// ---------------------------------------------------------------------------
static constexpr int SEC_MAX_GROUPS   = 6;   // max groups per section
static constexpr int GRP_MAX_CONTROLS = 8;   // max controls per group
static constexpr int SECTION_COUNT    = 16;  // total sections (HTML sync pending)

// ---------------------------------------------------------------------------
// Control type — determines which mini widget is drawn
// ---------------------------------------------------------------------------
enum class CtrlType : uint8_t {
    KNOB    = 0,   // continuous 0-127 arc knob
    SELECT  = 1,   // enum dropdow
    TOGGLE  = 2,   // on/off pill
    GRID    = 3,   // step sequencer bar grid (special)
    ENVELOPE = 4,   // ← ADD THIS
    NONE    = 255   // empty slot (padding)
};

// ---------------------------------------------------------------------------
// ControlDef — one parameter control
// ---------------------------------------------------------------------------
struct ControlDef {
    uint8_t    cc;       // MIDI CC number from CCDefs.h (255 = empty)
    CtrlType   type;     // widget type to render
    const char* label;   // short display label (uppercase, <=8 chars)
};

// ---------------------------------------------------------------------------
// GroupDef — a labelled cluster of controls within a section
// ---------------------------------------------------------------------------
struct GroupDef {
    const char*  label;                          // group header text
    ControlDef   controls[GRP_MAX_CONTROLS];     // controls in this group
    uint8_t      controlCount;                   // number of valid controls
};

// ---------------------------------------------------------------------------
// SectionDef — one collapsible accordion section
// ---------------------------------------------------------------------------
struct SectionDef {
    const char*  label;                      // section header text
    GroupDef     groups[SEC_MAX_GROUPS];      // groups within this section
    uint8_t      groupCount;                 // number of valid groups
};

// ---------------------------------------------------------------------------
// Helper: count total controls in a section (for layout calculations)
// ---------------------------------------------------------------------------
inline uint8_t sectionControlCount(const SectionDef& s) {
    uint8_t total = 0;
    for (uint8_t g = 0; g < s.groupCount; ++g) {
        total += s.groups[g].controlCount;
    }
    return total;
}

// ---------------------------------------------------------------------------
// Shorthand macros for readability (undefined at end of file)
// ---------------------------------------------------------------------------
#define K(cc, lbl)   { cc, CtrlType::KNOB,   lbl }
#define S(cc, lbl)   { cc, CtrlType::SELECT, lbl }
#define T(cc, lbl)   { cc, CtrlType::TOGGLE, lbl }
#define G(cc, lbl)   { cc, CtrlType::GRID,   lbl }
#define E(cc, lbl)   { cc, CtrlType::ENVELOPE, lbl }
#define EMPTY        { 255, CtrlType::NONE, "" }

// =============================================================================
// SECTION TABLE — 16 sections
// (HTML editor currently has 14; Voice Mode [14] and Global Reverb [15]
//  are firmware additions from the Phase 2/3 refactor — the HTML editor
//  needs matching additions as part of the three-codebase sync.)
// =============================================================================
static const SectionDef kSections[SECTION_COUNT] = {

// ─────────────────────────────────────────────────────────────────────────────
// 0 — Oscillator 1
// ─────────────────────────────────────────────────────────────────────────────
{ "Oscillator 1", {
    { "Wave & Tuning", {
        S(CC::OSC1_WAVE,         "WAVE"),
        S(CC::OSC1_PITCH_OFFSET, "PITCH"),
        S(CC::OSC1_FINE_TUNE,    "FINE"),
        S(CC::OSC1_DETUNE,       "DETUNE"),
        EMPTY, EMPTY, EMPTY, EMPTY
    }, 4 },
    { "Supersaw", {
        K(CC::SUPERSAW1_DETUNE,  "DET"),
        K(CC::SUPERSAW1_MIX,     "MIX"),
        EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY
    }, 2 },
    { "Feedback", {
        K(CC::OSC1_FEEDBACK_AMOUNT, "AMT"),
        K(CC::OSC1_FEEDBACK_MIX,    "MIX"),
        EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY
    }, 2 },
    { "DC & Ring", {
        K(CC::OSC1_FREQ_DC,  "FREQ DC"),
        K(CC::OSC1_SHAPE_DC, "SHAPE DC"),
        K(CC::RING1_MIX,     "RING"),
        EMPTY, EMPTY, EMPTY, EMPTY, EMPTY
    }, 3 },
    { "Arbitrary", {
        K(CC::OSC1_ARB_BANK,  "BANK"),
        K(CC::OSC1_ARB_INDEX, "IDX"),
        EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY
    }, 2 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 }
}, 5 },

// ─────────────────────────────────────────────────────────────────────────────
// 1 — Oscillator 2
// ─────────────────────────────────────────────────────────────────────────────
{ "Oscillator 2", {
    { "Wave & Tuning", {
        S(CC::OSC2_WAVE,         "WAVE"),
        S(CC::OSC2_PITCH_OFFSET, "PITCH"),
        S(CC::OSC2_FINE_TUNE,    "FINE"),
        S(CC::OSC2_DETUNE,       "DETUNE"),
        EMPTY, EMPTY, EMPTY, EMPTY
    }, 4 },
    { "Supersaw", {
        K(CC::SUPERSAW2_DETUNE,  "DET"),
        K(CC::SUPERSAW2_MIX,     "MIX"),
        EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY
    }, 2 },
    { "Feedback", {
        K(CC::OSC2_FEEDBACK_AMOUNT, "AMT"),
        K(CC::OSC2_FEEDBACK_MIX,    "MIX"),
        EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY
    }, 2 },
    { "DC & Ring", {
        K(CC::OSC2_FREQ_DC,  "FREQ DC"),
        K(CC::OSC2_SHAPE_DC, "SHAPE DC"),
        K(CC::RING2_MIX,     "RING"),
        EMPTY, EMPTY, EMPTY, EMPTY, EMPTY
    }, 3 },
    { "Arbitrary", {
        K(CC::OSC2_ARB_BANK,  "BANK"),
        K(CC::OSC2_ARB_INDEX, "IDX"),
        EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY
    }, 2 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 }
}, 5 },

// ─────────────────────────────────────────────────────────────────────────────
// 2 — Mixer
// ─────────────────────────────────────────────────────────────────────────────
{ "Mixer", {
    { "Levels", {
        K(CC::OSC_MIX_BALANCE, "BAL"),
        K(CC::OSC1_MIX,        "OSC1"),
        K(CC::OSC2_MIX,        "OSC2"),
        K(CC::SUB_MIX,         "SUB"),
        K(CC::NOISE_MIX,       "NOISE"),
        EMPTY, EMPTY, EMPTY
    }, 5 },
    { "Cross Mod & Sync", {
        K(CC::OSC_CROSS_MOD_DEPTH, "XMOD"),
        T(CC::OSC_SYNC_ENABLE,     "SYNC"),
        EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY
    }, 2 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 }
}, 2 },   // groupCount: 1 → 2

// ─────────────────────────────────────────────────────────────────────────────
// 3 — Filter
// ─────────────────────────────────────────────────────────────────────────────
{ "Filter", {
    { "Core", {
        K(CC::FILTER_CUTOFF,      "CUTOFF"),
        K(CC::FILTER_RESONANCE,   "RESO"),
        K(CC::FILTER_ENV_AMOUNT,  "ENV"),
        K(CC::FILTER_KEY_TRACK,   "KEY TRK"),
        EMPTY, EMPTY, EMPTY, EMPTY
    }, 4 },
    { "Engine", {
        S(CC::FILTER_ENGINE,  "ENGINE"),
        S(CC::FILTER_MODE,    "OBXa MODE"),
        S(CC::VA_FILTER_TYPE, "VA TYPE"),
        EMPTY, EMPTY, EMPTY, EMPTY, EMPTY
    }, 3 },
    { "Multimode", {
        K(CC::FILTER_OCTAVE_CONTROL,       "OCT CTRL"),
        K(CC::FILTER_OBXA_RES_MOD_DEPTH,   "RES MOD"),
        K(CC::FILTER_OBXA_MULTIMODE,       "MULTI"),
        EMPTY, EMPTY, EMPTY, EMPTY, EMPTY
    }, 3 },
    { "Xpander", {
        S(CC::FILTER_OBXA_XPANDER_MODE, "XP MODE"),
        EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY
    }, 1 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 }
}, 4 },

// ─────────────────────────────────────────────────────────────────────────────
// 4 — Amp Envelope
// ─────────────────────────────────────────────────────────────────────────────
{ "Amp Envelope", {
    { "", { E(CC::AMP_ATTACK, "AMP"),
            EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY }, 1 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 }
}, 1 },

// ─────────────────────────────────────────────────────────────────────────────
// 5 — Filter Envelope
// ─────────────────────────────────────────────────────────────────────────────
{ "Filter Envelope", {
    { "", { E(CC::FILTER_ENV_ATTACK, "FILTER"),
            EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY }, 1 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 }
}, 1 },

// ─────────────────────────────────────────────────────────────────────────────
// 6 — Pitch Envelope
// ─────────────────────────────────────────────────────────────────────────────
{ "Pitch Envelope", {
    { "", { E(CC::PITCH_ENV_ATTACK, "PITCH"),
            EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY }, 1 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 }
}, 1 },

// ─────────────────────────────────────────────────────────────────────────────
// 7 — LFO 1
// ─────────────────────────────────────────────────────────────────────────────
{ "LFO 1", {
    { "Core", {
        S(CC::LFO1_WAVEFORM,     "WAVE"),
        K(CC::LFO1_FREQ,         "RATE"),
        K(CC::LFO1_DEPTH,        "DEPTH"),
        S(CC::LFO1_TIMING_MODE,  "SYNC"),
        EMPTY, EMPTY, EMPTY, EMPTY
    }, 4 },
    { "Per-Dest", {
        K(CC::LFO1_PITCH_DEPTH,  "PITCH"),
        K(CC::LFO1_FILTER_DEPTH, "FILTER"),
        K(CC::LFO1_PWM_DEPTH,    "PWM"),
        K(CC::LFO1_AMP_DEPTH,    "AMP"),
        EMPTY, EMPTY, EMPTY, EMPTY
    }, 4 },
    { "Routing", {
        S(CC::LFO1_DESTINATION, "DEST"),
        K(CC::LFO1_DELAY,      "DELAY"),
        EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY
    }, 2 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 }
}, 3 },

// ─────────────────────────────────────────────────────────────────────────────
// 8 — LFO 2
// ─────────────────────────────────────────────────────────────────────────────
{ "LFO 2", {
    { "Core", {
        S(CC::LFO2_WAVEFORM,     "WAVE"),
        K(CC::LFO2_FREQ,         "RATE"),
        K(CC::LFO2_DEPTH,        "DEPTH"),
        S(CC::LFO2_TIMING_MODE,  "SYNC"),
        EMPTY, EMPTY, EMPTY, EMPTY
    }, 4 },
    { "Per-Dest", {
        K(CC::LFO2_PITCH_DEPTH,  "PITCH"),
        K(CC::LFO2_FILTER_DEPTH, "FILTER"),
        K(CC::LFO2_PWM_DEPTH,    "PWM"),
        K(CC::LFO2_AMP_DEPTH,    "AMP"),
        EMPTY, EMPTY, EMPTY, EMPTY
    }, 4 },
    { "Routing", {
        S(CC::LFO2_DESTINATION, "DEST"),
        K(CC::LFO2_DELAY,      "DELAY"),
        EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY
    }, 2 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 }
}, 3 },

// ─────────────────────────────────────────────────────────────────────────────
// 9 — Effects — JP
// ─────────────────────────────────────────────────────────────────────────────
{ "Effects", {
    { "EQ & Drive", {
        K(CC::FX_BASS_GAIN,   "BASS"),
        K(CC::FX_TREBLE_GAIN, "TREBLE"),
        S(CC::FX_DRIVE,       "DRIVE"),
        EMPTY, EMPTY, EMPTY, EMPTY, EMPTY
    }, 3 },
    { "Modulation", {
        S(CC::FX_MOD_EFFECT,   "TYPE"),
        K(CC::FX_MOD_MIX,      "MIX"),
        K(CC::FX_MOD_RATE,     "RATE"),
        K(CC::FX_MOD_FEEDBACK, "FB"),
        EMPTY, EMPTY, EMPTY, EMPTY
    }, 4 },
    { "Delay", {
        S(CC::FX_JPFX_DELAY_EFFECT,   "TYPE"),
        K(CC::FX_JPFX_DELAY_TIME,     "TIME"),
        K(CC::FX_JPFX_DELAY_MIX,      "MIX"),
        K(CC::FX_JPFX_DELAY_FEEDBACK, "FB"),
        S(CC::DELAY_TIMING_MODE,       "SYNC"),
        EMPTY, EMPTY, EMPTY
    }, 5 },
    // Reverb and Rev Ext groups REMOVED — they moved to the dedicated
    // "Global Reverb" section (section 15). Reverb is no longer per-layer
    // in the firmware; see GlobalFX.h.
    { "Output", {
        K(CC::FX_DRY_MIX,  "DRY"),
        K(CC::FX_JPFX_MIX, "JPFX"),
        EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY
    }, 2 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
}, 4 },

// ─────────────────────────────────────────────────────────────────────────────
// 10 — Velocity
// ─────────────────────────────────────────────────────────────────────────────
{ "Velocity", {
    { "Sensitivity", {
        K(CC::VELOCITY_AMP_SENS,    "AMP"),
        K(CC::VELOCITY_FILTER_SENS, "FILTER"),
        K(CC::VELOCITY_ENV_SENS,    "ENV"),
        EMPTY, EMPTY, EMPTY, EMPTY, EMPTY
    }, 3 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 }
}, 1 },

// ─────────────────────────────────────────────────────────────────────────────
// 11 — Performance
// ─────────────────────────────────────────────────────────────────────────────
{ "Performance", {
    { "Glide", {
        T(CC::GLIDE_ENABLE, "ON"),
        K(CC::GLIDE_TIME,   "TIME"),
        EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY
    }, 2 },
    { "Voice", {
        S(CC::POLY_MODE,     "MODE"),
        K(CC::UNISON_DETUNE, "UNI DET"),
        EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY
    }, 2 },
    { "Bend & Level", {
        K(CC::PITCH_BEND_RANGE,    "BEND"),
        K(CC::AMP_MOD_FIXED_LEVEL, "AMP LVL"),
        EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY
    }, 2 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 }
}, 3 },

// ─────────────────────────────────────────────────────────────────────────────
// 12 — BPM Clock
// ─────────────────────────────────────────────────────────────────────────────
{ "BPM Clock", {
    { "Clock", {
        S(CC::BPM_CLOCK_SOURCE,    "SRC"),
        K(CC::BPM_INTERNAL_TEMPO,  "BPM"),
        S(CC::DELAY_TIMING_MODE,   "DLY SYNC"),
        EMPTY, EMPTY, EMPTY, EMPTY, EMPTY
    }, 3 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 }
}, 1 },

// ─────────────────────────────────────────────────────────────────────────────
// 13 — Step Sequencer
//
// The Steps group uses CTRL_GRID for the step-value grid widget.
// All other controls are standard knobs/selects/toggles.
// ─────────────────────────────────────────────────────────────────────────────
{ "Step Sequencer", {
    { "Control", {
        T(CC::SEQ_ENABLE,      "ON"),
        K(CC::SEQ_STEPS,       "STEPS"),
        K(CC::SEQ_GATE_LENGTH, "GATE"),
        K(CC::SEQ_SLIDE,       "SLIDE"),
        EMPTY, EMPTY, EMPTY, EMPTY
    }, 4 },
    { "Routing", {
        S(CC::SEQ_DIRECTION,   "DIR"),
        S(CC::SEQ_DESTINATION, "DEST"),
        K(CC::SEQ_DEPTH,       "DEPTH"),
        T(CC::SEQ_RETRIGGER,   "RETRIG"),
        EMPTY, EMPTY, EMPTY, EMPTY
    }, 4 },
    { "Timing", {
        K(CC::SEQ_RATE,        "RATE"),
        S(CC::SEQ_TIMING_MODE, "SYNC"),
        EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY
    }, 2 },
    { "Steps", {
        G(CC::SEQ_STEP_SELECT, "GRID"),
        EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY
    }, 1 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 }
}, 4 },

  
// ─────────────────────────────────────────────────────────────────────────────
// 14 — Performance (dual-layer)
//
// Internal CCs (140-144) — not MIDI-automatable, controlled via TFT UI.
// LayerManager handles these; HomeScreen routes via LayerManager::getCC/setCC.
// ─────────────────────────────────────────────────────────────────────────────
{ "Voice Mode", {
    { "Layer Mode", {
        S(CC::PERF_MODE,         "MODE"),
        S(CC::PERF_EDIT_TARGET,  "EDIT"),
        S(CC::PERF_MIDI_CHANNEL_A,"CH_A"),
        S(CC::PERF_MIDI_CHANNEL_B,"CH_B"), EMPTY, EMPTY, EMPTY
    }, 4 },
    { "Voice Split", {
        S(CC::PERF_VOICE_SPLIT,  "VOICES"),
        S(CC::PERF_SPLIT_NOTE,   "SPLIT"),
        S(CC::PERF_BALANCE,      "BALANCE"),
        EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,
    }, 3 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 }
}, 2 },

// ─────────────────────────────────────────────────────────────────────────────
// 15 — Global Reverb
//
// Reverb is a SINGLE shared tank used by both layers (Phase 3 GlobalFX move).
// These CCs are intercepted by LayerManager and routed to _globalFx; they do
// NOT reach either engine. Because the tank is shared, its state is stored
// per-Performance, not per-Patch.
//
// Groups:
//   "Tank"    — size / damping / master wet / bypass. The core reverb shape.
//   "Rev Ext" — shimmer, freeze, post-tank EQ. "Extended" in the JP-8000
//               sense — these are creative controls beyond the core plate.
//
// LOWPASS / HIPASS act on the wet signal AFTER the tank (different from
// HI DMP / LO DMP which act INSIDE the tank and change decay rate).
// ─────────────────────────────────────────────────────────────────────────────
{ "Global Reverb", {
    { "Tank", {
        K(CC::FX_REVERB_SIZE,   "SIZE"),
        K(CC::FX_REVERB_DAMP,   "HI DMP"),
        K(CC::FX_REVERB_LODAMP, "LO DMP"),
        K(CC::FX_REVERB_MIX,    "MIX"),
        T(CC::FX_REVERB_BYPASS, "BYPASS"),
        EMPTY, EMPTY, EMPTY
    }, 5 },
    { "Rev Ext", {
        K(CC::FX_REVERB_SHIMMER, "SHIMMER"),
        T(CC::FX_REVERB_FREEZE,  "FREEZE"),
        K(CC::FX_REVERB_LOWPASS, "LO PASS"),
        K(CC::FX_REVERB_HIPASS,  "HI PASS"),
        EMPTY, EMPTY, EMPTY, EMPTY
    }, 4 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
    { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 }
}, 2 },
};
// end kSections[]

// ---------------------------------------------------------------------------
// Clean up shorthand macros — do not leak into other translation units
// ---------------------------------------------------------------------------
#undef K
#undef S
#undef T
#undef G
#undef EMPTY
