// JT8000_Sections.h
// =============================================================================
// Section table — links JE-8086 front-panel groups to UIPage page indices.
//
// A "section" is a named group of related UIPages shown as a tile on the home
// screen.  Tapping a tile opens a SectionScreen listing all pages in that group,
// EXCEPT for special sections that use the sentinel page index SECTION_PAGE_BROWSER
// (0xFF) — these open the PresetBrowser modal instead of a SectionScreen.
//
// Mapping to JE-8086 black-bordered panel groups:
//   OSC 1   → OSC1 Core / Mix+Saw / DC+Ring / Feedback
//   OSC 2   → OSC2 Core / Mix+Saw / DC+Ring / Feedback + Sources mixer
//   FILTER  → Filter main / Mod / 2-pole / Xpander
//   ENV     → Amp ADSR / Filter ADSR
//   LFO     → LFO1 / LFO2 / Sync modes
//   FX      → JPFX Tone+Mod / Mod params / Delay / Reverb / Mix
//   GLOBAL  → Performance / Arb waveforms / BPM clock
//   PRESETS → opens PresetBrowser (no UIPage — sentinel 0xFF)

//  NEW (pages 26-31):
//   LFO1Dep  : 26 L1 Pitch / L1 Filter / L1 PWM / L1 Amp
//   LFO1Dly  : 27 L1 Delay / L1 Freq / L1 Wave / -
//   LFO2Dep  : 28 L2 Pitch / L2 Filter / L2 PWM / L2 Amp
//   LFO2Dly  : 29 L2 Delay / L2 Freq / L2 Wave / -
//   PitchEnv : 30 PE ADSR
//   PitchV   : 31 PE Depth / Vel Amp / Vel Filter / Vel Env
//
// No audio or engine dependencies — include freely.
// =============================================================================

#pragma once
#include <Arduino.h>
#include "JT8000Colours.h"

// Sentinel: a pages[] entry of SECTION_PAGE_BROWSER means
// "tapping this section opens the PresetBrowser, not a SectionScreen".
static constexpr uint8_t SECTION_PAGE_BROWSER = 0xFF;

// Max UIPages per section (increase if needed)
static constexpr int SECTION_MAX_PAGES = 6;

// Total sections shown on home screen (7 synth sections + 1 Presets tile)
static constexpr int SECTION_COUNT = 8;

// -----------------------------------------------------------------------------
// SectionDef — one section entry
// -----------------------------------------------------------------------------
struct SectionDef {
    const char* label;                        // short name shown on tile (≤8 chars)
    uint16_t    colour;                       // RGB565 accent colour
    uint8_t     pages[SECTION_MAX_PAGES];     // UIPage indices, 0xFF = special action
    uint8_t     pageCount;                    // number of valid pages (0 for browser tile)
};

// Helper: returns true if this section should open the PresetBrowser
inline bool sectionIsBrowser(const SectionDef& s) {
    return s.pageCount == 0 || s.pages[0] == SECTION_PAGE_BROWSER;
}

// -----------------------------------------------------------------------------
// Section table — 8 sections covering all 26 UIPages (0..25) + Presets tile
//
// COLOUR NOTE: All colours use the BGR565 pre-swapped values from JT8000Colours.h.
// The ILI9341 panel has MADCTL_BGR set, which swaps R and B hardware channels.
// Using bare RGB565 constants (RED, YELLOW, GREEN) shows the wrong colour:
//   YELLOW 0xFFE0 (RGB565) → displays as cyan on this BGR panel
//   RED    0xF800 (RGB565) → displays as blue on this BGR panel
// Always use COLOUR_* constants from JT8000Colours.h for correct display.
//
// Section colour assignments (from existing JT-8000 palette):
//   OSC    → COLOUR_OSC    (bright cyan  #00CBFF)
//   FILTER → COLOUR_FILTER (amber-yellow #FFB428)
//   ENV    → COLOUR_ENV    (magenta-pink #DC32A0)
//   LFO    → COLOUR_LFO    (orange       #FF6C00)
//   FX     → COLOUR_FX     (light cyan   #00DCFF)
//   GLOBAL → COLOUR_GLOBAL (neutral grey #828291)
//   PRESETS→ COLOUR_ACCENT (red          #FF1C18)
// -----------------------------------------------------------------------------
static const SectionDef kSections[SECTION_COUNT] = {

    // 0 — OSC 1  (pages 0-3)  cyan
    { "OSC 1",   COLOUR_OSC,    { 0, 1, 2, 3, 255, 255 }, 4 },

    // 1 — OSC 2  (pages 4-8)  cyan (matches OSC 1 — same signal group)
    { "OSC 2",   COLOUR_OSC,    { 4, 5, 6, 7, 8, 255 },   5 },

    // 2 — FILTER (pages 9-12)  amber-yellow
    { "FILTER",  COLOUR_FILTER, { 9, 10, 11, 12, 255, 255 }, 4 },

    // 3 — ENV    (pages 13-14, 30-31)  magenta-pink
    { "ENV",     COLOUR_ENV,    { 13, 14, 30, 31, 255, 255 }, 4 },

    // 4 — LFO   (pages 26-29)  orange
    { "LFO",     COLOUR_LFO,    { 26, 27, 28, 29, 255, 255 }, 4 },

    // 5 — FX    (pages 18-22)  light cyan
    { "FX",      COLOUR_FX,     { 18, 19, 20, 21, 22, 255 }, 5 },

    // 6 — GLOBAL (pages 23-25, 32)  neutral grey
    { "GLOBAL",  COLOUR_GLOBAL, { 23, 24, 25, 32, 255, 255 }, 4 },

    // 7 — SEQ   (pages 33-35)  lime-green
    { "SEQ",     COLOUR_SEQ,    { 33, 34, 35, 255, 255, 255 }, 3 },

    // 8 — PRESETS: sentinel 0xFF → UIManager opens PresetBrowser instead of SectionScreen
    //    pageCount = 0 signals "no pages, this is a browser tile"
    //{ "PRESETS", COLOUR_ACCENT, { SECTION_PAGE_BROWSER, 255, 255, 255, 255, 255 }, 0 },
};
