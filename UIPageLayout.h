// UIPageLayout.h
// =============================================================================
// UI-ONLY page layout: maps display pages → CC IDs and short labels.
// Used by UIManager_MicroDexed (ILI9341) for parameter display and editing.
//
// Rules:
//   - 255 = empty slot (no CC assigned; draw "---")
//   - Every CC referenced here MUST exist in CCDefs.h
//   - ccNames must match ccMap 1:1 in order
//   - This file has no audio or engine dependencies — include freely
//
// Page organisation (24 pages, 4 params each = 96 parameter slots):
//
//   Osc1     :  0 Wave/pitch/fine/detune
//              1 Osc1 mix / supersaw det+mix
//              2 FreqDC / ShapeDC / Ring1 / -
//              3 Feedback amt / mix / - / -
//
//   Osc2     :  4 Wave/pitch/fine/detune
//              5 Osc2 mix / supersaw det+mix
//              6 FreqDC / ShapeDC / Ring2 / -
//              7 Feedback amt / mix / - / -
//
//   Sources  :  8 Sub / Noise / Osc1Mix / Osc2Mix
//
//   Filter   :  9 Cutoff / Res / Env Amt / KeyTrack
//              10 Oct Ctrl / Q Depth / Multimode / -
//              11 2Pole / BPBlend / Push2p / -
//              12 Xpander4p / XpMode / - / -
//
//   Envelope : 13 Amp ADSR
//              14 Filter ADSR
//
//   LFO      : 15 LFO1 freq/depth/dest/wave
//              16 LFO2 freq/depth/dest/wave
//
//   LFO sync : 17 LFO1 timing / LFO2 timing / - / -
//
//   FX       : 18 Bass / Treble / ModFX / ModMix
//              19 ModRate / ModFB / DelayFX / DelayMix
//              20 DelayFB / DelayTime / DlySync / -
//              21 RevSize / RevDamp / RevLoDamp / RevBypass
//              22 DryMix / JPFXMix / RevMix / -
//
//   Global   : 23 GlideEn / GlideTime / AmpMod / -
//              24 OSC1 Bank / OSC1 Wave# / OSC2 Bank / OSC2 Wave#
//
//   BPM      : 25 ClockSrc / IntBPM / DelaySync / -
//
//  NEW (pages 26-31):
//   LFO1Dep  : 26 L1 Pitch / L1 Filter / L1 PWM / L1 Amp
//   LFO1Dly  : 27 L1 Delay / L1 Freq / L1 Wave / -
//   LFO2Dep  : 28 L2 Pitch / L2 Filter / L2 PWM / L2 Amp
//   LFO2Dly  : 29 L2 Delay / L2 Freq / L2 Wave / -
//   PitchEnv : 30 PE ADSR
//   PitchV   : 31 PE Depth / Vel Amp / Vel Filter / Vel Env
//
//  STEP SEQUENCER (pages 33-35):
//   SeqCtrl  : 33 Enable / Steps / Gate / Slide
//   SeqRoute : 34 Direction / Destination / Depth / Retrigger
//   SeqTime  : 35 Rate / Timing Mode / StepSel / StepVal
// =============================================================================

#pragma once
#include <Arduino.h>
#include "CCDefs.h"

namespace UIPage {

static constexpr int NUM_PAGES       = 36;  // 0..35  (pages 33-35 = Step Sequencer)
static constexpr int PARAMS_PER_PAGE = 4;

// ---------------------------------------------------------------------------
// CC map — which CC number each encoder/knob on each page controls.
// 255 = no parameter (slot is empty).
// ---------------------------------------------------------------------------
static constexpr uint8_t ccMap[NUM_PAGES][PARAMS_PER_PAGE] = {

    // =========================================================================
    // OSC 1  (pages 0-3)
    // =========================================================================

    // Page 0: OSC1 core — waveform, coarse pitch, fine tune, detune spread
    { CC::OSC1_WAVE, CC::OSC1_PITCH_OFFSET, CC::OSC1_FINE_TUNE, CC::OSC1_DETUNE },

    // Page 1: OSC1 level and supersaw
    { CC::OSC1_MIX, CC::OSC_MIX_BALANCE, CC::SUPERSAW1_DETUNE, CC::SUPERSAW1_MIX },

    // Page 2: OSC1 DC mod inputs and ring modulator 1 level
    { CC::OSC1_FREQ_DC, CC::OSC1_SHAPE_DC, CC::RING1_MIX, 255 },

    // Page 3: OSC1 feedback (wavefolder / FM self-feedback)
    { CC::OSC1_FEEDBACK_AMOUNT, CC::OSC1_FEEDBACK_MIX, 255, 255 },

    // =========================================================================
    // OSC 2  (pages 4-7)
    // =========================================================================

    // Page 4: OSC2 core
    { CC::OSC2_WAVE, CC::OSC2_PITCH_OFFSET, CC::OSC2_FINE_TUNE, CC::OSC2_DETUNE },

    // Page 5: OSC2 level and supersaw
    { CC::OSC2_MIX, CC::SUPERSAW2_DETUNE, CC::SUPERSAW2_MIX, 255 },

    // Page 6: OSC2 DC mod inputs and ring modulator 2 level
    { CC::OSC2_FREQ_DC, CC::OSC2_SHAPE_DC, CC::RING2_MIX, 255 },

    // Page 7: OSC2 feedback
    { CC::OSC2_FEEDBACK_AMOUNT, CC::OSC2_FEEDBACK_MIX, 255, 255 },

    // =========================================================================
    // SOURCES  (page 8)
    // =========================================================================

    // Page 8: Sub oscillator, noise, and per-oscillator mix levels
    { CC::SUB_MIX, CC::NOISE_MIX, CC::OSC1_MIX, CC::OSC2_MIX },

    // =========================================================================
    // FILTER  (pages 9-12)
    // =========================================================================

    // Page 9: Main filter controls
    { CC::FILTER_CUTOFF, CC::FILTER_RESONANCE, CC::FILTER_ENV_AMOUNT, CC::FILTER_KEY_TRACK },

    // Page 10: Filter modulation depth and multimode blend
    { CC::FILTER_OCTAVE_CONTROL, CC::FILTER_OBXA_RES_MOD_DEPTH, CC::FILTER_OBXA_MULTIMODE, 255 },

    // Page 11: Filter engine + topology selectors
    // FILTER_ENGINE switches between OBXa and VA bank.
    // FILTER_MODE selects OBXa topology (only relevant when engine = OBXa).
    // VA_FILTER_TYPE selects VA bank topology (only relevant when engine = VA).
    { CC::FILTER_ENGINE, CC::FILTER_MODE, CC::VA_FILTER_TYPE, 255 },

    // Page 12: Xpander sub-mode (0..14) — only relevant when OBXa Xpander+M mode
    { CC::FILTER_OBXA_XPANDER_MODE, 255, 255, 255 },

    // =========================================================================
    // ENVELOPES  (pages 13-14)
    // =========================================================================

    // Page 13: Amplifier envelope ADSR
    { CC::AMP_ATTACK, CC::AMP_DECAY, CC::AMP_SUSTAIN, CC::AMP_RELEASE },

    // Page 14: Filter envelope ADSR
    { CC::FILTER_ENV_ATTACK, CC::FILTER_ENV_DECAY, CC::FILTER_ENV_SUSTAIN, CC::FILTER_ENV_RELEASE },

    // =========================================================================
    // LFOs  (pages 15-17)
    // =========================================================================

    // Page 15: LFO1 — rate, depth, destination, waveform
    { CC::LFO1_FREQ, CC::LFO1_DEPTH, CC::LFO1_DESTINATION, CC::LFO1_WAVEFORM },

    // Page 16: LFO2 — rate, depth, destination, waveform
    { CC::LFO2_FREQ, CC::LFO2_DEPTH, CC::LFO2_DESTINATION, CC::LFO2_WAVEFORM },

    // Page 17: LFO BPM sync modes (free / note divisions)
    { CC::LFO1_TIMING_MODE, CC::LFO2_TIMING_MODE, 255, 255 },

    // =========================================================================
    // FX  (pages 18-22)
    // =========================================================================

    // Page 18: JPFX tone shaping + modulation effect selection and wet level
    { CC::FX_BASS_GAIN, CC::FX_TREBLE_GAIN, CC::FX_MOD_EFFECT, CC::FX_MOD_MIX },

    // Page 19: JPFX modulation parameters + delay effect selection and wet level
    { CC::FX_MOD_RATE, CC::FX_MOD_FEEDBACK, CC::FX_JPFX_DELAY_EFFECT, CC::FX_JPFX_DELAY_MIX },

    // Page 20: JPFX delay parameters + BPM sync
    { CC::FX_JPFX_DELAY_FEEDBACK, CC::FX_JPFX_DELAY_TIME, CC::DELAY_TIMING_MODE, 255 },

    // Page 21: Reverb parameters and bypass toggle
    { CC::FX_REVERB_SIZE, CC::FX_REVERB_DAMP, CC::FX_REVERB_LODAMP, CC::FX_REVERB_BYPASS },

    // Page 22: Output mix levels — dry / JPFX wet / reverb wet / drive
    { CC::FX_DRY_MIX, CC::FX_JPFX_MIX, CC::FX_REVERB_MIX, CC::FX_DRIVE },

    // =========================================================================
    // GLOBAL  (pages 23-24)
    // =========================================================================

    // Page 23: Performance — glide, amp modulation fixed level, poly/mono/unison mode
    { CC::GLIDE_ENABLE, CC::GLIDE_TIME, CC::POLY_MODE, CC::UNISON_DETUNE },

    // Page 24: Arbitrary waveform (AKWF) bank and table index for both oscs
    { CC::OSC1_ARB_BANK, CC::OSC1_ARB_INDEX, CC::OSC2_ARB_BANK, CC::OSC2_ARB_INDEX },

    // =========================================================================
    // BPM CLOCK  (page 25)
    // =========================================================================

    // Page 25: BPM clock source and internal tempo
    { CC::BPM_CLOCK_SOURCE, CC::BPM_INTERNAL_TEMPO, 255, 255 },

 

    // Page 26/7
    // LFO1 per-destination depths and delay
    {CC::LFO1_PITCH_DEPTH, CC::LFO1_FILTER_DEPTH, CC::LFO1_PWM_DEPTH, CC::LFO1_AMP_DEPTH},
    {CC::LFO1_DELAY, CC::LFO1_FREQ, CC::LFO1_DESTINATION, CC::LFO1_WAVEFORM },
    // Page 28/9
    // LFO2 per-destination depths and delay
    {CC::LFO2_PITCH_DEPTH, CC::LFO2_FILTER_DEPTH, CC::LFO2_PWM_DEPTH, CC::LFO2_AMP_DEPTH},
    {CC::LFO2_DELAY, CC::LFO2_FREQ, CC::LFO2_DESTINATION, CC::LFO2_WAVEFORM },

    // Page 30/31
    // Pitch envelope
    {CC::PITCH_ENV_ATTACK, CC::PITCH_ENV_DECAY, CC::PITCH_ENV_SUSTAIN, CC::PITCH_ENV_RELEASE},
    {CC::PITCH_ENV_DEPTH, CC::VELOCITY_AMP_SENS, CC::VELOCITY_FILTER_SENS, CC::VELOCITY_ENV_SENS },

    // Page 32 — GLOBAL: pitch bend range (CC 127)
    {CC::PITCH_BEND_RANGE, 255, 255, 255 },

    // =========================================================================
    // STEP SEQUENCER (pages 33-35)
    // =========================================================================

    // Page 33: Seq core — on/off, step count, gate, slide
    { CC::SEQ_ENABLE, CC::SEQ_STEPS, CC::SEQ_GATE_LENGTH, CC::SEQ_SLIDE },

    // Page 34: Seq routing — direction, destination, depth, retrigger
    { CC::SEQ_DIRECTION, CC::SEQ_DESTINATION, CC::SEQ_DEPTH, CC::SEQ_RETRIGGER },

    // Page 35: Seq timing + step editor — rate, sync, step select, step value
    { CC::SEQ_RATE, CC::SEQ_TIMING_MODE, CC::SEQ_STEP_SELECT, CC::SEQ_STEP_VALUE },

};

// ---------------------------------------------------------------------------
// Display names — short strings shown next to each parameter on screen.
// Must be 1:1 with ccMap above.  Keep ≤ 10 chars for ILI9341 layout.
// ---------------------------------------------------------------------------
static constexpr const char* ccNames[NUM_PAGES][PARAMS_PER_PAGE] = {

    // Page 0 — OSC1 core
    { "OSC1 Wave", "OSC1 Pitch", "OSC1 Fine",  "OSC1 Det"  },

    // Page 1 — OSC1 mix / supersaw
    { "Osc1 Mix",  "Osc Bal",    "SSaw1 Det",  "SSaw1 Mix" },

    // Page 2 — OSC1 DC / Ring1
    { "Freq DC1",  "Shape DC1",  "Ring1 Mix",  "---"       },

    // Page 3 — OSC1 feedback
    { "O1 FB Amt", "O1 FB Mix",  "---",        "---"       },

    // Page 4 — OSC2 core
    { "OSC2 Wave", "OSC2 Pitch", "OSC2 Fine",  "OSC2 Det"  },

    // Page 5 — OSC2 mix / supersaw
    { "Osc2 Mix",  "SSaw2 Det",  "SSaw2 Mix",  "---"       },

    // Page 6 — OSC2 DC / Ring2
    { "Freq DC2",  "Shape DC2",  "Ring2 Mix",  "---"       },

    // Page 7 — OSC2 feedback
    { "O2 FB Amt", "O2 FB Mix",  "---",        "---"       },

    // Page 8 — Sources
    { "Sub Mix",   "Noise Mix",  "Osc1 Mix",   "Osc2 Mix"  },

    // Page 9 — Filter main
    { "Cutoff",    "Resonance",  "Flt Env",    "Key Track" },

    // Page 10 — Filter mod
    { "Oct Ctrl",  "Q Depth",    "Multimode",  "---"       },

    // Page 11 — Filter engine / topology
    { "Flt Engine", "Flt Mode",   "VA Type",    "---"       },

    // Page 12 — Xpander sub-mode (active when FilterMode == Xpander+M)
    { "Xpand Mode","---",        "---",        "---"       },

    // Page 13 — Amp envelope
    { "Amp Att",   "Amp Dec",    "Amp Sus",    "Amp Rel"   },

    // Page 14 — Filter envelope
    { "Flt Att",   "Flt Dec",    "Flt Sus",    "Flt Rel"   },

    // Page 15 — LFO1
    { "LFO1 Freq", "LFO1 Depth", "LFO1 Dest",  "LFO1 Wave" },

    // Page 16 — LFO2
    { "LFO2 Freq", "LFO2 Depth", "LFO2 Dest",  "LFO2 Wave" },

    // Page 17 — LFO sync
    { "LFO1 Sync", "LFO2 Sync",  "---",        "---"       },

    // Page 18 — JPFX tone + mod effect
    { "Bass",      "Treble",     "Mod FX",     "Mod Mix"   },

    // Page 19 — Mod params + delay effect
    { "Mod Rate",  "Mod FB",     "Dly FX",     "Dly Mix"   },

    // Page 20 — Delay params + sync
    { "Dly FB",    "Dly Time",   "Dly Sync",   "---"       },

    // Page 21 — Reverb
    { "Rev Size",  "Rev Damp",   "Rev LoDamp", "Rev Bypass"},

    // Page 22 — Output mix
    { "Dry Mix",   "JPFX Mix",   "Rev Mix",    "Drive"     },

    // Page 23 — Global / Performance
    { "Glide On",  "Glide Time", "Poly Mode",  "Uni Det"   },

    // Page 24 — Arbitrary waveforms
    { "OSC1 Bank", "OSC1 Wave#", "OSC2 Bank",  "OSC2 Wave#"},

    // Page 25 — BPM clock
    { "Clock Src", "Int BPM",    "---",        "---"       },

    // Page 26 — LFO1 per-dest depths
    { "L1 Pitch",  "L1 Filter",  "L1 PWM",     "L1 Amp"    },

    // Page 27 — LFO1 delay + refs: DELAY, FREQ, DESTINATION, WAVEFORM
    { "L1 Delay",  "L1 Freq",    "L1 Dest",    "L1 Wave"   },

    // Page 28 — LFO2 per-dest depths
    { "L2 Pitch",  "L2 Filter",  "L2 PWM",     "L2 Amp"    },

    // Page 29 — LFO2 delay + refs: DELAY, FREQ, DESTINATION, WAVEFORM
    { "L2 Delay",  "L2 Freq",    "L2 Dest",    "L2 Wave"   },

    // Page 30 — Pitch envelope ADSR
    { "PE Att",    "PE Dec",     "PE Sus",     "PE Rel"    },

    // Page 31 — Pitch env depth + velocity
    { "PE Depth",  "Vel Amp",    "Vel Flt",    "Vel Env"   },

    // Page 32
    { "Bend Rng",  "",          "",           ""          },

    // Page 33 — Seq core
    { "Seq On",    "Seq Steps", "Seq Gate",   "Seq Slide" },

    // Page 34 — Seq routing
    { "Seq Dir",   "Seq Dest",  "Seq Depth",  "Seq Retrig"},

    // Page 35 — Seq timing + step editor
    { "Seq Rate",  "Seq Sync",  "Step Sel",   "Step Val"  },
};

// ---------------------------------------------------------------------------
// Page title strings — shown in the display header bar.
// Keep ≤ 20 chars.
// ---------------------------------------------------------------------------
static constexpr const char* pageTitle[NUM_PAGES] = {
    "OSC1 Core",         //  0
    "OSC1 Mix & Saw",    //  1
    "OSC1 DC & Ring",    //  2
    "OSC1 Feedback",     //  3
    "OSC2 Core",         //  4
    "OSC2 Mix & Saw",    //  5
    "OSC2 DC & Ring",    //  6
    "OSC2 Feedback",     //  7
    "Sources",           //  8
    "Filter",            //  9
    "Filter Mod",        // 10
    "Filter 2-Pole",     // 11
    "Filter Xpander",    // 12
    "Amp Envelope",      // 13
    "Filter Envelope",   // 14
    "LFO 1",             // 15
    "LFO 2",             // 16
    "LFO Sync",          // 17
    "JPFX Tone+Mod",     // 18
    "JPFX Mod Params",   // 19
    "JPFX Delay",        // 20
    "Reverb",            // 21
    "FX Mix",            // 22
    "Performance",       // 23
    "Arb Waveforms",     // 24
    "BPM Clock",         // 25
    "Depths LFO1",       // 26  tab shows "Depths"
    "Delay LFO1",        // 27  tab shows "Delay"
    "Depths LFO2",       // 28  tab shows "Depths" — same section, scrollable
    "Delay LFO2",        // 29  tab shows "Delay"
    "Pitch Env ADSR",    // 30  tab shows "Pitch"
    "Vel+PEnv Depth",    // 31  tab shows "Vel+PEnv"
    "Global 2",          // 32  pitch bend range
    "Seq Control",       // 33  enable, steps, gate, slide
    "Seq Routing",       // 34  direction, dest, depth, retrigger
    "Seq Timing",        // 35  rate, sync, step select, step value

};

// ---------------------------------------------------------------------------
// Layout type — controls which widget style SectionScreen uses for a page.
//
//   LAYOUT_ROWS4   : 4 TFTParamRow text rows (enum/mode pages, mixed pages)
//   LAYOUT_KNOBS4  : 4 TFTKnob in a single row (standard 4-param pages)
//   LAYOUT_KNOBS8  : 8 TFTKnob in 2 rows of 4 (when showing full section at once)
//   LAYOUT_SLIDERS4: 4 TFTSlider full-width (ADSR and similar grouped params)
// ---------------------------------------------------------------------------
enum class LayoutType : uint8_t {
    LAYOUT_ROWS4    = 0,   // text row list (enum, mode, mixed)
    LAYOUT_KNOBS4   = 1,   // 4 knobs in one row
    LAYOUT_SLIDERS4 = 2,   // 4 full-width horizontal sliders
};

static constexpr LayoutType layoutMap[NUM_PAGES] = {
    // OSC 1
    LayoutType::LAYOUT_ROWS4,      //  0 OSC1 Core     — waveform is an enum, rows suit it
    LayoutType::LAYOUT_KNOBS4,     //  1 OSC1 Mix+Saw  — continuous levels
    LayoutType::LAYOUT_KNOBS4,     //  2 OSC1 DC+Ring  — continuous levels
    LayoutType::LAYOUT_KNOBS4,     //  3 OSC1 Feedback — continuous levels

    // OSC 2
    LayoutType::LAYOUT_ROWS4,      //  4 OSC2 Core     — waveform enum
    LayoutType::LAYOUT_KNOBS4,     //  5 OSC2 Mix+Saw
    LayoutType::LAYOUT_KNOBS4,     //  6 OSC2 DC+Ring
    LayoutType::LAYOUT_KNOBS4,     //  7 OSC2 Feedback

    // Sources
    LayoutType::LAYOUT_KNOBS4,     //  8 Sources

    // Filter
    LayoutType::LAYOUT_KNOBS4,     //  9 Filter main
    LayoutType::LAYOUT_KNOBS4,     // 10 Filter mod
    LayoutType::LAYOUT_ROWS4,      // 11 Filter 2-pole  — boolean toggles
    LayoutType::LAYOUT_ROWS4,      // 12 Filter Xpander — enum mode

    // Envelopes — sliders so all 4 ADSR are comparable at a glance
    LayoutType::LAYOUT_SLIDERS4,   // 13 Amp ADSR
    LayoutType::LAYOUT_SLIDERS4,   // 14 Filter ADSR

    // LFOs
    LayoutType::LAYOUT_ROWS4,      // 15 LFO1 — waveform + destination enums
    LayoutType::LAYOUT_ROWS4,      // 16 LFO2
    LayoutType::LAYOUT_ROWS4,      // 17 LFO sync — enum modes

    // FX
    LayoutType::LAYOUT_KNOBS4,     // 18 JPFX Tone+Mod
    LayoutType::LAYOUT_KNOBS4,     // 19 Mod params+delay
    LayoutType::LAYOUT_KNOBS4,     // 20 Delay params
    LayoutType::LAYOUT_KNOBS4,     // 21 Reverb
    LayoutType::LAYOUT_KNOBS4,     // 22 FX Mix

    // Global
    LayoutType::LAYOUT_ROWS4,      // 23 Performance   — glide on/off, poly mode (enums)
    LayoutType::LAYOUT_ROWS4,      // 24 Arb waveforms
    LayoutType::LAYOUT_ROWS4,      // 25 BPM clock     — enum + numeric

    // LFO depths
    LayoutType::LAYOUT_KNOBS4,     // 26 LFO1 depths
    LayoutType::LAYOUT_KNOBS4,     // 27 LFO1 delay+refs
    LayoutType::LAYOUT_KNOBS4,     // 28 LFO2 depths
    LayoutType::LAYOUT_KNOBS4,     // 29 LFO2 delay+refs

    // Pitch envelope
    LayoutType::LAYOUT_SLIDERS4,   // 30 Pitch Env ADSR
    LayoutType::LAYOUT_KNOBS4,     // 31 PE Depth + velocity

    // Global 2
    LayoutType::LAYOUT_ROWS4,      // 32 Pitch bend range

    // Step Sequencer
    LayoutType::LAYOUT_ROWS4,      // 33 Seq Control   — enable toggle + continuous
    LayoutType::LAYOUT_ROWS4,      // 34 Seq Routing   — enums + continuous
    LayoutType::LAYOUT_ROWS4,      // 35 Seq Timing    — mixed enum + step editor
};

} // namespace UIPage