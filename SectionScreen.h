// SectionScreen.h
// =============================================================================
// JT-8000 section sub-screen — shows all parameters for one synth section.
//
// Layout (320 × 240 px):
//   y=0    Header  24 px — section label, back arrow, CPU%
//   y=24   Tabs    18 px — page selector (max SECTION_MAX_PAGES tabs)
//   y=42   Params 172 px — widget area (layout determined per page)
//   y=214  Footer  26 px — encoder hint text
//
// Widget layouts (selected via UIPage::layoutMap[pageIndex]):
//
//   LAYOUT_ROWS4    — 4 × TFTParamRow (~43 px each)
//                     Used for enum/boolean/mode parameter pages
//
//   LAYOUT_KNOBS4   — 4 × TFTKnob in a single row
//                     Each knob: 80 px wide × 172 px tall area
//                     Used for continuous-parameter pages
//
//   LAYOUT_SLIDERS4 — 4 × TFTSlider stacked vertically (~43 px each)
//                     Full-width horizontal sliders with thumb
//                     Used for ADSR pages so all 4 are visually comparable
//
// Navigation:
//   Tap widget           → open TFTNumericEntry overlay (or list picker)
//   Tap page tab         → switch page, widgets reconfigure
//   Left encoder delta   → scroll selected-widget highlight
//   Right encoder delta  → nudge selected CC value ±1
//   Left enc short press → back to home
//
// Widget pools:
//   All widgets are value members (no heap). Only the pool matching
//   _activeLayout is drawn/touched. configure() calls repurpose them
//   for each new page without reconstruction.
// =============================================================================

#pragma once
#include <Arduino.h>
#include "ILI9341_t3n.h"
#include "SynthEngine.h"
#include "UIPageLayout.h"
#include "JT8000_Sections.h"
#include "JT8000Colours.h"
#include "TFTWidgets.h"
#include "Mapping.h"
#include "CCDefs.h"

using BackCallback = void (*)();

class SectionScreen {
public:
    // ---- Layout geometry ----
    static constexpr int16_t SW        = 320;
    static constexpr int16_t SH        = 240;
    static constexpr int16_t HEADER_H  = 24;
    static constexpr int16_t TABS_H    = 18;
    static constexpr int16_t FOOTER_H  = 26;
    static constexpr int16_t PARAMS_Y  = HEADER_H + TABS_H;          // y=42
    static constexpr int16_t PARAMS_H  = SH - PARAMS_Y - FOOTER_H;  // 172 px
    static constexpr int16_t ROW_H     = PARAMS_H / 4;               // 43 px
    static constexpr int16_t KNOB_COL_W = SW / 4;                    // 80 px
    // CPU% in header is only repainted when value changes or every 500 ms max
    static constexpr uint32_t HEADER_REDRAW_MS = 500;

    SectionScreen();

    void begin(ILI9341_t3n* display);
    void setBackCallback(BackCallback cb);
    void open(const SectionDef& section, SynthEngine& synth);

    void draw();
    void syncFromEngine();

    bool onTouch(int16_t x, int16_t y);

    void onEncoderLeft(int delta);
    void onEncoderRight(int delta);
    void onBackPress();
    void onEditPress();
    void onSwipeAdjust(int16_t x, int16_t y, int steps);

    bool isEntryOpen()  const;
    // Returns true while a phased full repaint is in progress.
    // UIManager uses this to suppress syncFromEngine() during redraws.
    bool isRedrawing()  const { return _redrawStep > 0; }

private:
    // ---- Static context for widget callbacks ----
    static SectionScreen* _ctx;

    // ---- Draw helpers ----
    void _drawHeader();
    void _updateHeaderCpu();  // per-frame CPU% update without full header repaint
    void _drawTabs();
    void _drawFooter();

    // ---- Widget management ----
    void _rebuildWidgets();   // reconfigure active pool for _activePage
    void _drawActiveWidgets();
    void _markActiveWidgetsDirty();

    // ---- Widget selection ----
    void _setSelectedWidget(int i);
    void _applySelected(int i, bool sel);  // set highlight on active pool widget i
    bool _getSelected(int i) const;

    // ---- Value sync helpers ----
    uint8_t     _widgetCC(int i)      const;
    const char* _enumText(uint8_t cc) const;

    // ---- Tab touch ----
    void _onTabTouch(int16_t x);

    // ---- Entry overlay ----
    void _onWidgetTap(uint8_t cc);
    void _openEntry(uint8_t cc);
    void _openEnumEntry(uint8_t cc, const char* title);
    void _openNumericEntry(uint8_t cc, const char* title);

    // ---- Helper queries ----
    const char* _ccName(uint8_t cc)   const;
    static bool     _isEnumCC(uint8_t cc);
    static uint16_t _ccColour(uint8_t cc);

    // ---- State ----
    ILI9341_t3n*          _display;
    const SectionDef*     _section;
    SynthEngine*          _synth;
    int                   _activePage;
    int                   _selectedWidget;
    UIPage::LayoutType    _activeLayout;
    BackCallback          _onBack;
    bool                  _needsTabRedraw;

    // ---- Phased full-screen redraw ----
    // A full repaint (fillScreen + header + tabs + widgets + footer) takes
    // 10-30 ms, which blocks MIDI reads for that entire duration.
    // Instead, we split the repaint across multiple loop() iterations using
    // _redrawStep.  Each draw() call advances one step and returns immediately,
    // allowing MIDI reads to run between steps.
    //
    //  Step 0 = idle (no redraw pending)
    //  Step 1 = fillScreen
    //  Step 2 = _drawHeader
    //  Step 3 = _drawTabs
    //  Step 4 = _drawActiveWidgets (marks dirty first)
    //  Step 5 = _drawFooter  → back to 0
    //
    // _needsTabRedraw remains a single-step flag (tabs only, no fillScreen).
    int  _redrawStep;   // 0 = idle; >0 = step in progress

    // Header rate-limiting: CPU% is only repainted when its value changes.
    // Without this, _drawHeader() runs a fillRect + print every 33 ms (30 Hz).
    uint32_t _lastSectionHeaderMs = 0;   // timestamp of last header redraw
    int      _lastSectionCpuPct   = -1;  // last CPU% drawn; -1 forces redraw

    uint8_t  _pendingCC;
    int      _pendingCount;

    TFTNumericEntry _entry;

    // ---- Pre-allocated widget pools (no heap) ----

    // LAYOUT_ROWS4 pool
    TFTParamRow  _row0, _row1, _row2, _row3;
    TFTParamRow* _rows[4];

    // LAYOUT_KNOBS4 pool
    TFTKnob  _knob0, _knob1, _knob2, _knob3;
    TFTKnob* _knobs[4];

    // LAYOUT_SLIDERS4 pool
    TFTSlider  _slider0, _slider1, _slider2, _slider3;
    TFTSlider* _sliders[4];
};
