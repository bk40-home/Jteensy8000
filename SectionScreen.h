// SectionScreen.h
// =============================================================================
// Section sub-screen — shows all parameters for one synth section.
//
// Layout (320 × 240 px):
//   y=0    Header 24 px — section label, back arrow, CPU%
//   y=24   Tabs   18 px — page selector (max 8 pages)
//   y=42   Params 172 px — 4 × TFTParamRow (each ~43 px)
//   y=214  Footer 26 px — encoder hint text
//
// Navigation:
//   Tap page tab         → switch page (rows reconfigure inline)
//   Tap row / long-R     → open TFTNumericEntry overlay
//   Left encoder delta   → scroll selected-row highlight
//   Right encoder delta  → nudge selected CC value ±1
//   Left enc short press → back to home
//   Back arrow (header)  → back to home
//
// Sync:
//   syncFromEngine() re-reads all 4 CC values for the current page and
//   updates rows; only repaints rows where the value changed.
//
// Entry overlay:
//   TFTNumericEntry is embedded (not on a push/pop stack) so it can close
//   cleanly and return focus to this screen without tearing.
//
// Static context pointer:
//   Row tap callbacks use a C function pointer, so they can't capture "this".
//   _ctx is a static SectionScreen* set in begin() — safe because only one
//   SectionScreen is active at a time.
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
    // ---- Layout ----
    static constexpr int16_t SW        = 320;
    static constexpr int16_t SH        = 240;
    static constexpr int16_t HEADER_H  = 24;
    static constexpr int16_t TABS_H    = 18;
    static constexpr int16_t FOOTER_H  = 26;
    static constexpr int16_t PARAMS_Y  = HEADER_H + TABS_H;          // y=42
    static constexpr int16_t PARAMS_H  = SH - PARAMS_Y - FOOTER_H;  // 172 px
    static constexpr int16_t ROW_H     = PARAMS_H / 4;               // 43 px each

    SectionScreen();

    // -------------------------------------------------------------------------
    // begin() — call once after construction.
    // Wires display and row tap callbacks.
    // -------------------------------------------------------------------------
    void begin(ILI9341_t3n* display);

    void setBackCallback(BackCallback cb);

    // -------------------------------------------------------------------------
    // open() — activate for a given section.
    // Resets to page 0, clears any open entry overlay.
    // -------------------------------------------------------------------------
    void open(const SectionDef& section, SynthEngine& synth);

    // -------------------------------------------------------------------------
    // draw() — call every frame while this screen is visible.
    // Delegates to the entry overlay when it is open.
    // -------------------------------------------------------------------------
    void draw();

    // -------------------------------------------------------------------------
    // syncFromEngine() — re-read 4 CC values for the current page.
    // Repaints only rows whose value changed.
    // -------------------------------------------------------------------------
    void syncFromEngine();

    // ---- Touch routing ----
    bool onTouch(int16_t x, int16_t y);

    // ---- Encoder input ----
    void onEncoderLeft(int delta);              // scroll row highlight
    void onEncoderRight(int delta);             // nudge selected CC ±1
    void onBackPress();                         // left enc short → back
    void onEditPress();                         // right enc long → open entry

    // Swipe-to-adjust: touch swipe up/down on a row adjusts its CC ±steps.
    // Call from UIManager when GESTURE_SWIPE_UP/DOWN fires in SECTION mode.
    // (x, y) = touch point; steps = +1 or -1.
    void onSwipeAdjust(int16_t x, int16_t y, int steps);

    bool isEntryOpen() const;

private:
    // ---- Static context for row lambda callbacks ----
    static SectionScreen* _ctx;

    // ---- Draw helpers ----
    void _drawHeader();
    void _drawTabs();
    void _drawFooter();

    // ---- Row management ----
    void _rebuildRows();
    static void _configRow(TFTParamRow& row, uint8_t cc,
                           const char* name, uint16_t colour);

    // ---- Tab touch ----
    void _onTabTouch(int16_t x);

    // ---- Row selection ----
    void _setSelectedRow(int row);

    // ---- Entry overlay ----
    void _onRowTap(uint8_t cc);
    void _openEntry(uint8_t cc);
    void _openEnumEntry(uint8_t cc, const char* title);
    void _openNumericEntry(uint8_t cc, const char* title);

    // ---- Helper queries ----
    const char* _enumText(uint8_t cc) const;    // enum label for display
    const char* _ccName(uint8_t cc)   const;    // look up param name
    static bool     _isEnumCC(uint8_t cc);      // keypad vs list picker
    static uint16_t _ccColour(uint8_t cc);      // category accent colour

    // ---- Members ----
    ILI9341_t3n*       _display;
    const SectionDef*  _section;
    SynthEngine*       _synth;
    int                _activePage;
    int                _selectedRow;
    BackCallback       _onBack;
    bool               _needsFullRedraw;
    bool               _needsTabRedraw;

    // Stored for static lambda to map confirmed value back to CC
    uint8_t  _pendingCC;
    int      _pendingCount;

    TFTNumericEntry _entry;

    TFTParamRow  _row0, _row1, _row2, _row3;
    TFTParamRow* _rows[4];
};
