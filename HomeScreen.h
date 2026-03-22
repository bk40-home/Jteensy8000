// HomeScreen.h
// =============================================================================
// JT-8000 scrollable accordion home screen (v2 — granular dirty tracking).
//
// PERFORMANCE:
//   Encoder value change → redraws ONE control cell (~0.15 ms, not ~15 ms).
//   Cursor move → redraws TWO cells (old deselect + new select, ~0.3 ms).
//   Expand/collapse/scroll → full content redraw (structural, unavoidable).
//
// DIRTY FLAGS:
//   _layoutDirty      — structural change → full content clear + redraw
//   _ctrlDirty[i]     — one control needs repaint (value or highlight change)
//   _sectionHdrDirty  — a section header highlight changed (no body redraw)
//   _headerDirty      — top status bar needs repaint
// =============================================================================

#pragma once
#include <Arduino.h>
#include "ILI9341_t3n.h"
#include "SynthEngine.h"
#include "JT8000_Sections.h"
#include "JT8000Colours.h"
#include "TFTMiniWidgets.h"
#include "TFTWidgets.h"



class HomeScreen {
public:
    static constexpr int16_t SW         = 320;
    static constexpr int16_t SH         = 240;
    static constexpr int16_t HEADER_H   = 22;
    static constexpr int16_t CONTENT_Y  = HEADER_H;
    static constexpr int16_t CONTENT_H  = SH - HEADER_H;

    static constexpr int16_t VOICE_DOT_R   = 2;
    static constexpr int16_t VOICE_DOT_GAP = 4;
    static constexpr uint32_t HEADER_REDRAW_MS = 500;

    // Max controls in any single section (Effects=19, pad for safety)
    static constexpr int16_t MAX_SECT_CONTROLS = 24;

    HomeScreen();

    void begin(ILI9341_t3n* disp, SynthEngine* synth);
    void draw();
    void syncFromEngine();

    // Notify that a specific CC value changed (from MIDI input, preset load,
    // or encoder on another page). Marks the matching control dirty so the
    // next draw() cycle repaints it. Lightweight — just a flag set, no draw.
    void notifyCC(uint8_t cc);

    void onEncoderLeft(int delta);
    void onEncoderLeftPress();
    void onEncoderRight(int delta);
    void onEncoderRightPress();

    bool onTouch(int16_t x, int16_t y);
    void onSwipe(int deltaY);
    void markFullRedraw();

    bool isEntryOpen() const;
    TFTNumericEntry& getEntry();
    void onEntryEncoderDelta(int delta);

private:
    struct NavCursor { int16_t sectionIdx; int16_t controlIdx; };
    NavCursor _cursor;
    int16_t   _expandedSection;
    int16_t   _scrollY;

    // ---- Position cache ----
    struct CtrlPos { int16_t x, y; bool visible; };
    CtrlPos  _ctrlPos[MAX_SECT_CONTROLS];
    int16_t  _cachedCtrlCount;
    int16_t  _cachedExpandedSect;
    int16_t  _cachedScrollY;
    void _buildPositionCache();

    // ---- Section header screen-Y cache (for header-only redraws) ----
    int16_t _sectionScreenY[SECTION_COUNT];
    void _buildSectionYCache();

    // ---- Height calculations ----
    int16_t _calcTotalHeight() const;
    int16_t _calcSectionY(int16_t sectIdx) const;
    int16_t _calcSectionHeight(int16_t sectIdx) const;
    int16_t _calcExpandedBodyHeight(int16_t sectIdx) const;

    // ---- Control helpers ----
    int16_t _controlCount(int16_t sectIdx) const;
    void _controlToGroupCtrl(int16_t sectIdx, int16_t flatCtrl,
                             int16_t& grpIdx, int16_t& ctrlInGrp) const;
    const ControlDef& _controlDef(int16_t sectIdx, int16_t flatCtrl) const;

    // ---- Drawing ----
    void _drawHeader(bool full);
    void _drawVoiceDots();
    void _drawContent();          // structural: full clear + all visible
    void _drawDirtyControls();    // incremental: only flagged cells
    void _drawSectionHeaderAt(int16_t sectIdx, int16_t screenY);
    void _drawSectionBody(int16_t sectIdx, int16_t screenY);
    void _drawControl(int16_t sectIdx, int16_t flatCtrl,
                      int16_t screenX, int16_t screenY);

    // ---- Dirty helpers ----
    void _markControlDirty(int16_t flatCtrl);
    void _markAllControlsDirty();
    bool _anyControlDirty() const;
    void _clearAllControlDirty();

    // ---- Scroll ----
    void _clampScroll();
    void _scrollToSection(int16_t sectIdx);
    void _scrollToCursor();
    void _ensureVisible(int16_t virtualY, int16_t h);

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
    static HomeScreen* _instance;
    ILI9341_t3n*   _display;
    SynthEngine*   _synth;

    int            _redrawStep;
    bool           _layoutDirty;                     // structural → full content redraw
    bool           _ctrlDirty[MAX_SECT_CONTROLS];    // per-control dirty
    bool           _sectionHdrDirty[SECTION_COUNT];  // per-section-header dirty
    bool           _headerDirty;

    int            _lastCpuPct;
    bool           _prevVoiceActive[8];
    uint32_t       _lastHeaderMs;

    TFTNumericEntry _entry;
    uint8_t         _pendingCC;
    int             _pendingCount;
    bool            _entryWasOpen;

    static const ControlDef _emptyControl;
};
