// HomeScreen.h
// =============================================================================
// JT-8000 scrollable accordion home screen.
//
// Replaces the tile-grid + scope home screen with a vertically scrollable list
// of collapsible sections matching the HTML editor's layout. One section is
// expanded at a time (accordion mode). Controls within the expanded section
// are drawn as mini knobs, selects, and toggles using TFTMiniWidgets.
//
// Layout (320 × 240 px):
//   y=0    Header   22 px  — product name, voice dots, CPU%
//   y=22   Content 218 px  — scrollable section list (virtual viewport)
//
// Navigation:
//   Encoder L rotate  — move highlight (section headers + controls)
//   Encoder L press   — expand/collapse section, or open entry overlay
//   Encoder R rotate  — adjust highlighted control value ±1
//   Encoder R press   — open entry overlay on highlighted control
//   Touch tap header  — expand/collapse that section
//   Touch tap control — select + open entry overlay
//   Touch swipe up/dn — scroll the section list
//
// Rendering:
//   Virtual scroll with dirty-region tracking. Only controls visible in the
//   240px viewport are drawn. Section expand/collapse marks the viewport dirty
//   from the changed section downward. Value changes mark only the single
//   control cell dirty. Phased full repaint on first draw / mode entry.
//
// Memory:
//   No heap. All section/group/control data is const (JT8000_Sections.h).
//   Value cache is a flat uint8_t[MAX_CC] read from SynthEngine::getCC().
//   Enum text pointers come from SynthEngine query methods.
// =============================================================================

#pragma once
#include <Arduino.h>
#include "ILI9341_t3n.h"
#include "SynthEngine.h"
#include "JT8000_Sections.h"
#include "JT8000Colours.h"
#include "TFTMiniWidgets.h"
#include "TFTWidgets.h"      // TFTNumericEntry for value entry overlay

// Defined in Jteensy8000.ino
//extern AudioScopeTap scopeTap;

class HomeScreen {
public:
    // ---- Screen geometry ----
    static constexpr int16_t SW         = 320;
    static constexpr int16_t SH         = 240;
    static constexpr int16_t HEADER_H   = 22;
    static constexpr int16_t CONTENT_Y  = HEADER_H;
    static constexpr int16_t CONTENT_H  = SH - HEADER_H;  // 218 px visible

    // ---- Voice dots ----
    static constexpr int16_t VOICE_DOT_R = 2;
    static constexpr int16_t VOICE_DOT_GAP = 4;

    // ---- Timing ----
    static constexpr uint32_t HEADER_REDRAW_MS = 500;

    HomeScreen();

    void begin(ILI9341_t3n* disp, SynthEngine* synth);

    // Call every display frame (~30 fps). Rate-limited by UIManager.
    void draw();

    // Sync all control values from engine. Call after preset load.
    void syncFromEngine();

    // ---- Input routing (called by UIManager_TFT) ----
    void onEncoderLeft(int delta);
    void onEncoderLeftPress();
    void onEncoderRight(int delta);
    void onEncoderRightPress();

    bool onTouch(int16_t x, int16_t y);
    void onSwipe(int deltaY);   // positive = scroll content down (reveal above)

    // Force full repaint on next draw()
    void markFullRedraw();

    // Entry overlay access (UIManager needs to check before routing input)
    bool isEntryOpen() const;
    TFTNumericEntry& getEntry();

    // For UIManager to route encoder to entry when open
    void onEntryEncoderDelta(int delta);

private:
    // ---- Navigation state ----
    // The highlight cursor addresses either a section header or a control.
    // When _expandedSection == -1, all sections are collapsed and the cursor
    // moves between headers only. When a section is expanded, the cursor
    // moves between that section's controls + the headers above/below.
    struct NavCursor {
        int16_t sectionIdx;    // which section is highlighted (-1 = none)
        int16_t controlIdx;    // which control within section (-1 = header)
    };

    NavCursor _cursor;
    int16_t   _expandedSection;   // which section is open (-1 = all collapsed)
    int16_t   _scrollY;           // virtual Y offset (0 = top of list)

    // ---- Content height calculation ----
    int16_t _calcTotalHeight() const;
    int16_t _calcSectionY(int16_t sectIdx) const;
    int16_t _calcSectionHeight(int16_t sectIdx) const;
    int16_t _calcExpandedBodyHeight(int16_t sectIdx) const;

    // ---- Group layout calculation ----
    // Returns the Y offset (relative to section body top) and height of a group.
    // Also calculates control positions within the group.
    struct GroupLayout {
        int16_t  y;            // Y relative to section body top
        int16_t  height;       // total group height (header + controls + padding)
        int16_t  ctrlY;        // Y of control row relative to section body top
    };
    GroupLayout _calcGroupLayout(int16_t sectIdx, int16_t grpIdx,
                                int16_t startY) const;

    // ---- Control flat-index helpers ----
    // Convert (sectionIdx, controlIdx) to/from a flat control index within
    // the expanded section for sequential navigation.
    int16_t _controlCount(int16_t sectIdx) const;
    // Given flat control index, find (groupIdx, ctrlInGroup).
    void _controlToGroupCtrl(int16_t sectIdx, int16_t flatCtrl,
                             int16_t& grpIdx, int16_t& ctrlInGrp) const;
    // Get the ControlDef for a flat control index.
    const ControlDef& _controlDef(int16_t sectIdx, int16_t flatCtrl) const;

    // ---- Drawing ----
    void _drawHeader(bool full);
    void _drawVoiceDots();
    void _drawContent();
    void _drawSectionHeader(int16_t sectIdx, int16_t screenY);
    void _drawSectionBody(int16_t sectIdx, int16_t screenY);
    void _drawGroup(int16_t sectIdx, int16_t grpIdx,
                    int16_t screenX, int16_t screenY, int16_t availW);
    void _drawControl(int16_t sectIdx, int16_t flatCtrl,
                      int16_t screenX, int16_t screenY);

    // ---- Scroll management ----
    void _clampScroll();
    void _scrollToSection(int16_t sectIdx);
    void _scrollToCursor();
    void _ensureVisible(int16_t screenY, int16_t h);

    // ---- Section expand/collapse ----
    void _toggleSection(int16_t sectIdx);
    void _expandSection(int16_t sectIdx);
    void _collapseAll();

    // ---- Value editing ----
    void _adjustValue(int16_t sectIdx, int16_t flatCtrl, int delta);
    void _openEntry(int16_t sectIdx, int16_t flatCtrl);
    void _openEnumEntry(uint8_t cc, const char* title);
    void _openNumericEntry(uint8_t cc, const char* title);
    const char* _enumTextForCC(uint8_t cc) const;
    static bool _isEnumCC(uint8_t cc);
    static bool _isToggleCC(uint8_t cc);

    // ---- Members ----
    static HomeScreen* _instance;  // singleton for static entry callbacks
    ILI9341_t3n*   _display;
    SynthEngine*   _synth;

    // Phased full-screen redraw (same pattern as old HomeScreen)
    int            _redrawStep;    // 0 = idle, 1-3 = phased steps

    // Dirty tracking
    bool           _contentDirty;  // true = full content area needs repaint
    bool           _headerDirty;

    // Header caches (skip SPI writes when unchanged)
    int            _lastCpuPct;
    bool           _prevVoiceActive[8];  // max 8 voices for dot display
    uint32_t       _lastHeaderMs;

    // Value entry overlay
    TFTNumericEntry _entry;
    uint8_t         _pendingCC;    // CC being edited in overlay
    int             _pendingCount; // enum option count (for generic index→CC calc)
    bool            _entryWasOpen; // tracks entry→closed transition for full redraw

    // Dummy ControlDef for out-of-range access
    static const ControlDef _emptyControl;
};