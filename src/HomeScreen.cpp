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
 // HomeScreen.cpp
// =============================================================================
// Scrollable accordion home screen — v2 with granular dirty tracking.
//
// PERFORMANCE:
//   Encoder R (value change):  marks 1 control dirty → ~0.15 ms SPI
//   Encoder L (cursor move):   marks 2 controls dirty → ~0.3 ms SPI
//   Expand/collapse/scroll:    _layoutDirty → full content redraw (~8 ms)
//   Entry overlay close:       phased full redraw (3 frames × ~3 ms)
//
// vs v1: every encoder tick was ~10-15 ms → MIDI dropout. Now < 0.5 ms.
// =============================================================================

#include "HomeScreen.h"

#include "WaveForms.h"
#include "AudioFilterVABank.h"
#include "StepSequencer.h"
#include "CCDefs.h"
#include "Mapping.h"
#include <math.h>

using namespace MiniLayout;

HomeScreen* HomeScreen::_instance = nullptr;
const ControlDef HomeScreen::_emptyControl = { 255, CtrlType::NONE, "" };

// =============================================================================
// Constructor
// =============================================================================
HomeScreen::HomeScreen()
    : _cachedCtrlCount(0), _cachedExpandedSect(-1), _cachedScrollY(-1)
    , _display(nullptr), _synth(nullptr)
    , _redrawStep(1)
    , _layoutDirty(true), _headerDirty(true)
    , _lastCpuPct(-1), _lastHeaderMs(0)
    , _pendingCC(255), _pendingCount(0), _entryWasOpen(false)
{
    _cursor.sectionIdx = 0;
    _cursor.controlIdx = -1;
    _expandedSection   = -1;
    _scrollY           = 0;
    _envCursor         = 0;
    _envFlavour        = EnvFlavour::AMP;

    memset(_prevVoiceActive, 0, sizeof(_prevVoiceActive));
    _clearAllControlDirty();
    memset(_sectionHdrDirty, 0, sizeof(_sectionHdrDirty));
    memset(_sectionScreenY, 0, sizeof(_sectionScreenY));
    memset(_ctrlPos, 0, sizeof(_ctrlPos));
}

// =============================================================================
// begin / markFullRedraw / syncFromEngine / entry access
// =============================================================================

   void HomeScreen::begin(ILI9341_t3n* disp, SynthEngine* synth, LayerManager* mgr) {
    _display  = disp;
    _synth    = synth;
    _layerMgr = mgr;
    _instance = this;
    _entry.setDisplay(disp);
    markFullRedraw();
}

void HomeScreen::markFullRedraw() {
    _redrawStep   = 1;
    _layoutDirty  = true;
    _headerDirty  = true;
    _lastCpuPct   = -1;
    memset(_prevVoiceActive, 0xFF, sizeof(_prevVoiceActive));
    _markAllControlsDirty();
    memset(_sectionHdrDirty, true, sizeof(_sectionHdrDirty));
}

void HomeScreen::syncFromEngine() {
    // After preset load: mark all visible controls dirty for value refresh
    _markAllControlsDirty();
}

// ---------------------------------------------------------------------------
// notifyCC — mark the control matching a CC number as dirty for repaint.
//
// Only checks the currently expanded section (collapsed sections have no
// visible controls to repaint). Called from the CC notifier callback in
// the .ino — runs at MIDI handler frequency so must be fast.
// ---------------------------------------------------------------------------
void HomeScreen::notifyCC(uint8_t cc) {
    if (_expandedSection < 0 || _expandedSection >= SECTION_COUNT) return;
    const SectionDef& sec = kSections[_expandedSection];
    int16_t flat = 0;
    for (int g = 0; g < sec.groupCount; ++g) {
        for (int c = 0; c < sec.groups[g].controlCount; ++c) {
            if (sec.groups[g].controls[c].cc == cc) {
                _markControlDirty(flat);
                return; // Each CC appears at most once in a section
            }
            ++flat;
        }
    }
}

bool HomeScreen::isEntryOpen() const { return _entry.isOpen(); }
TFTNumericEntry& HomeScreen::getEntry() { return _entry; }
void HomeScreen::onEntryEncoderDelta(int delta) {
    if (_entry.isOpen()) _entry.onEncoderDelta(delta);
}

// =============================================================================
// Dirty flag helpers
// =============================================================================

void HomeScreen::_markControlDirty(int16_t flatCtrl) {
    if (flatCtrl >= 0 && flatCtrl < MAX_SECT_CONTROLS)
        _ctrlDirty[flatCtrl] = true;
}

void HomeScreen::_markAllControlsDirty() {
    memset(_ctrlDirty, true, sizeof(_ctrlDirty));
}

void HomeScreen::_clearAllControlDirty() {
    memset(_ctrlDirty, 0, sizeof(_ctrlDirty));
}

bool HomeScreen::_anyControlDirty() const {
    for (int i = 0; i < MAX_SECT_CONTROLS; ++i)
        if (_ctrlDirty[i]) return true;
    return false;
}

// =============================================================================
// Height calculations (unchanged from v1)
// =============================================================================

int16_t HomeScreen::_calcSectionHeight(int16_t sectIdx) const {
    int16_t h = SEC_HDR_H;
    if (sectIdx == _expandedSection) h += _calcExpandedBodyHeight(sectIdx);
    return h;
}

int16_t HomeScreen::_calcExpandedBodyHeight(int16_t sectIdx) const {
    if (sectIdx < 0 || sectIdx >= SECTION_COUNT) return 0;
    const SectionDef& sec = kSections[sectIdx];
    int16_t h = BODY_PAD_Y;
    for (int g = 0; g < sec.groupCount; ++g) {
        const GroupDef& grp = sec.groups[g];
        if (grp.controlCount == 0) continue;
        h += GRP_HDR_H;
        int16_t rowH = 0;
        for (int c = 0; c < grp.controlCount; ++c) {
            switch (grp.controls[c].type) {
                case CtrlType::KNOB:   rowH = max(rowH, KNOB_CELL_H); break;
                case CtrlType::SELECT: rowH = max(rowH, SEL_CELL_H);  break;
                case CtrlType::TOGGLE: rowH = max(rowH, TOG_CELL_H);  break;
                case CtrlType::GRID:   rowH = max(rowH, (int16_t)SGRID_TOTAL_H); break;
                case CtrlType::ENVELOPE: rowH = max(rowH, (int16_t)MiniEnvLayout::ENV_CELL_H); break;
                default: break;
            }
        }
        h += rowH + GRP_PAD_BOTTOM;
    }
    h += BODY_PAD_Y;
    return h;
}

int16_t HomeScreen::_calcTotalHeight() const {
    int16_t total = 0;
    for (int i = 0; i < SECTION_COUNT; ++i) total += _calcSectionHeight(i);
    return total;
}

int16_t HomeScreen::_calcSectionY(int16_t sectIdx) const {
    int16_t y = 0;
    for (int i = 0; i < sectIdx && i < SECTION_COUNT; ++i) y += _calcSectionHeight(i);
    return y;
}

// =============================================================================
// _calcControlVirtualY — virtual Y of the row containing a flat control index
//
// Replicates the same geometry walk as _buildPositionCache() but returns
// virtual (pre-scroll) coordinates rather than screen coordinates.
// Called only on encoder movement so the tiny cost is irrelevant.
// =============================================================================
int16_t HomeScreen::_calcControlVirtualY(int16_t sectIdx, int16_t flatCtrl) const
{
    if (sectIdx < 0 || sectIdx >= SECTION_COUNT) return 0;

    const SectionDef& sec  = kSections[sectIdx];
    const int16_t     sectY = _calcSectionY(sectIdx);
    // body starts after section header + top padding
    int16_t curY    = sectY + SEC_HDR_H + BODY_PAD_Y;
    int16_t counter = 0;

    for (int g = 0; g < sec.groupCount; ++g) {
        const GroupDef& grp = sec.groups[g];
        if (grp.controlCount == 0) continue;

        curY += GRP_HDR_H;   // group label row

        // Row height for this group — same logic as _calcExpandedBodyHeight
        int16_t rowH = 0;
        for (int c = 0; c < grp.controlCount; ++c) {
            switch (grp.controls[c].type) {
                case CtrlType::KNOB:   rowH = max(rowH, KNOB_CELL_H);            break;
                case CtrlType::SELECT: rowH = max(rowH, SEL_CELL_H);             break;
                case CtrlType::TOGGLE: rowH = max(rowH, TOG_CELL_H);             break;
                case CtrlType::GRID:   rowH = max(rowH, (int16_t)SGRID_TOTAL_H); break;
                case CtrlType::ENVELOPE: rowH = max(rowH, (int16_t)MiniEnvLayout::ENV_CELL_H); break;
                default: break;
            }
        }

        // If flatCtrl lives in this group, return the current row Y
        if (flatCtrl < counter + grp.controlCount)
            return curY;

        counter += grp.controlCount;
        curY    += rowH + GRP_PAD_BOTTOM;
    }

    return curY;   // fallback: clamp to end of body (should not be reached)
}

// =============================================================================
// Control helpers (unchanged from v1)
// =============================================================================

int16_t HomeScreen::_controlCount(int16_t sectIdx) const {
    if (sectIdx < 0 || sectIdx >= SECTION_COUNT) return 0;
    return sectionControlCount(kSections[sectIdx]);
}

void HomeScreen::_controlToGroupCtrl(int16_t sectIdx, int16_t flatCtrl,
                                     int16_t& grpIdx, int16_t& ctrlInGrp) const {
    grpIdx = 0; ctrlInGrp = 0;
    if (sectIdx < 0 || sectIdx >= SECTION_COUNT) return;
    const SectionDef& sec = kSections[sectIdx];
    int16_t idx = 0;
    for (int g = 0; g < sec.groupCount; ++g) {
        if (flatCtrl < idx + sec.groups[g].controlCount) {
            grpIdx = g; ctrlInGrp = flatCtrl - idx; return;
        }
        idx += sec.groups[g].controlCount;
    }
}

const ControlDef& HomeScreen::_controlDef(int16_t sectIdx, int16_t flatCtrl) const {
    if (sectIdx < 0 || sectIdx >= SECTION_COUNT) return _emptyControl;
    const SectionDef& sec = kSections[sectIdx];
    int16_t idx = 0;
    for (int g = 0; g < sec.groupCount; ++g) {
        if (flatCtrl < idx + sec.groups[g].controlCount)
            return sec.groups[g].controls[flatCtrl - idx];
        idx += sec.groups[g].controlCount;
    }
    return _emptyControl;
}

// =============================================================================
// Position cache — built once per layout change, used for all incremental draws
// =============================================================================

void HomeScreen::_buildSectionYCache() {
    int16_t virtualY = 0;
    for (int s = 0; s < SECTION_COUNT; ++s) {
        _sectionScreenY[s] = CONTENT_Y + (virtualY - _scrollY);
        virtualY += _calcSectionHeight(s);
    }
}

void HomeScreen::_buildPositionCache() {
    _cachedExpandedSect = _expandedSection;
    _cachedScrollY      = _scrollY;
    _cachedCtrlCount    = 0;
    memset(_ctrlPos, 0, sizeof(_ctrlPos));

    if (_expandedSection < 0 || _expandedSection >= SECTION_COUNT) return;

    const SectionDef& sec = kSections[_expandedSection];
    const int16_t sectVirtualY = _calcSectionY(_expandedSection);
    const int16_t bodyScreenY  = CONTENT_Y + (sectVirtualY - _scrollY) + SEC_HDR_H;

    const int16_t clipTop = CONTENT_Y;
    const int16_t clipBot = CONTENT_Y + CONTENT_H;

    int16_t curY = bodyScreenY + BODY_PAD_Y;
    int16_t flatCtrl = 0;

    for (int g = 0; g < sec.groupCount; ++g) {
        const GroupDef& grp = sec.groups[g];
        if (grp.controlCount == 0) continue;

        curY += GRP_HDR_H;

        // Row height
        int16_t rowH = 0;
        for (int c = 0; c < grp.controlCount; ++c) {
            switch (grp.controls[c].type) {
                case CtrlType::KNOB:   rowH = max(rowH, KNOB_CELL_H); break;
                case CtrlType::SELECT: rowH = max(rowH, SEL_CELL_H);  break;
                case CtrlType::TOGGLE: rowH = max(rowH, TOG_CELL_H);  break;
                case CtrlType::GRID:   rowH = max(rowH, (int16_t)SGRID_TOTAL_H); break;
                case CtrlType::ENVELOPE: rowH = max(rowH, (int16_t)MiniEnvLayout::ENV_CELL_H); break;
                default: break;
            }
        }

        int16_t curX = BODY_PAD_X;
        for (int c = 0; c < grp.controlCount; ++c) {
            const ControlDef& ctrl = grp.controls[c];
            if (ctrl.type == CtrlType::NONE) { flatCtrl++; continue; }

            if (flatCtrl < MAX_SECT_CONTROLS) {
                _ctrlPos[flatCtrl].x = curX;
                _ctrlPos[flatCtrl].y = curY;
                _ctrlPos[flatCtrl].visible =
                    (curY >= clipTop && curY + rowH <= clipBot);
            }

            switch (ctrl.type) {
                case CtrlType::KNOB:   curX += KNOB_CELL_W + CTRL_GAP_X; break;
                case CtrlType::SELECT: curX += SEL_CELL_W + CTRL_GAP_X;  break;
                case CtrlType::TOGGLE: curX += TOG_CELL_W + CTRL_GAP_X;  break;
                case CtrlType::GRID:   curX += SW - 2 * BODY_PAD_X;      break;
                default: break;
            }
            flatCtrl++;
        }
        curY += rowH + GRP_PAD_BOTTOM;
    }
    _cachedCtrlCount = flatCtrl;
}

// =============================================================================
// Scroll management
// =============================================================================

void HomeScreen::_clampScroll() {
    const int16_t totalH = _calcTotalHeight();
    const int16_t maxScroll = max((int16_t)0, (int16_t)(totalH - CONTENT_H));
    if (_scrollY < 0)        _scrollY = 0;
    if (_scrollY > maxScroll) _scrollY = maxScroll;
}

void HomeScreen::_scrollToSection(int16_t sectIdx) {
    _scrollY = _calcSectionY(sectIdx);
    _clampScroll();
    _layoutDirty = true;
}

void HomeScreen::_ensureVisible(int16_t virtualY, int16_t h) {
    const int16_t oldScroll = _scrollY;
    if (virtualY < _scrollY) {
        _scrollY = virtualY;
    } else if (virtualY + h > _scrollY + CONTENT_H) {
        _scrollY = virtualY + h - CONTENT_H;
    }
    _clampScroll();
    // Only mark layout dirty if scroll actually changed
    if (_scrollY != oldScroll) _layoutDirty = true;
}

void HomeScreen::_scrollToCursor() {
    if (_cursor.sectionIdx < 0) return;
    const int16_t sectY = _calcSectionY(_cursor.sectionIdx);

    if (_cursor.controlIdx < 0) {
        // No control selected — keep the section header visible
        _ensureVisible(sectY, SEC_HDR_H);
    } else {
        // Scroll to the virtual Y of the specific control row.
        // Using the tallest possible cell height so the full row is guaranteed visible.
        const int16_t ctrlVY = _calcControlVirtualY(_cursor.sectionIdx,
                                                      _cursor.controlIdx);
        const int16_t rowH   = max(KNOB_CELL_H, max(SEL_CELL_H, TOG_CELL_H));
        _ensureVisible(ctrlVY, rowH);

        // If the section header has scrolled off the top as a side-effect,
        // pin it back — the user needs context of which section they are in.
        if (_scrollY > sectY) {
            _scrollY = sectY;
            _clampScroll();
            _layoutDirty = true;
        }
    }
}

// =============================================================================
// Section expand/collapse — these are structural changes → _layoutDirty
// =============================================================================

void HomeScreen::_toggleSection(int16_t sectIdx) {
    if (_expandedSection == sectIdx) _collapseAll();
    else _expandSection(sectIdx);
}

void HomeScreen::_expandSection(int16_t sectIdx) {
    _expandedSection = sectIdx;
    _cursor.sectionIdx = sectIdx;
    _cursor.controlIdx = -1;
    _scrollToSection(sectIdx);
    _layoutDirty = true;
}

void HomeScreen::_collapseAll() {
    _expandedSection = -1;
    _cursor.controlIdx = -1;
    _clampScroll();
    _layoutDirty = true;
}

// =============================================================================
// draw() — main render dispatch
// =============================================================================

void HomeScreen::draw() {
    if (!_display) return;

    // ---- Entry overlay ----
    if (_entry.isOpen()) {
        _entry.draw();
        _entryWasOpen = true;
        return;
    }
    if (_entryWasOpen) {
        _entryWasOpen = false;
        _redrawStep = 1;  // phased full redraw after overlay closes
        return;
    }

    // ---- Phased full redraw (mode entry / overlay close) ----
    if (_redrawStep > 0) {
        switch (_redrawStep) {
            case 1:
                _display->fillScreen(COLOUR_BACKGROUND);
                break;
            case 2: _drawHeader(true); break;
            case 3:
                _drawContent();
                _redrawStep = 0;
                _layoutDirty = false;
                _clearAllControlDirty();
                return;
        }
        _redrawStep++;
        return;
    }

    // ---- Incremental path ----

    // Header (rate-limited)
    const uint32_t now = millis();
    if (_headerDirty || (now - _lastHeaderMs) >= HEADER_REDRAW_MS) {
        _drawHeader(false);
        _lastHeaderMs = now;
        _headerDirty = false;
    }

    // Structural layout change → full content redraw
    if (_layoutDirty) {
        _drawContent();
        _layoutDirty = false;
        _clearAllControlDirty();
        return;
    }

    // Section header highlight changes (cursor moved between sections)
    for (int s = 0; s < SECTION_COUNT; ++s) {
        if (_sectionHdrDirty[s]) {
            _sectionHdrDirty[s] = false;
            const int16_t sy = _sectionScreenY[s];
            if (sy >= CONTENT_Y && sy + SEC_HDR_H <= CONTENT_Y + CONTENT_H) {
                const bool hl = (_cursor.sectionIdx == s && _cursor.controlIdx == -1);
                SectionHeader::draw(*_display, 0, sy, SW,
                                    kSections[s].label, (s == _expandedSection), hl);
            }
        }
    }

    // Per-control incremental redraws
    if (_anyControlDirty()) {
        _drawDirtyControls();
    }
}

// =============================================================================
// _drawContent() — structural full redraw (expand/collapse/scroll)
// =============================================================================

void HomeScreen::_drawContent() {
    if (!_display) return;

    _display->fillRect(0, CONTENT_Y, SW, CONTENT_H, COLOUR_BACKGROUND);

    const int16_t clipTop = CONTENT_Y;
    const int16_t clipBot = CONTENT_Y + CONTENT_H;
    int16_t virtualY = 0;

    _buildSectionYCache();

    for (int s = 0; s < SECTION_COUNT; ++s) {
        const int16_t sectH = _calcSectionHeight(s);
        if (virtualY + sectH <= _scrollY) { virtualY += sectH; continue; }
        if (virtualY >= _scrollY + CONTENT_H) break;

        const int16_t screenY = _sectionScreenY[s];
        const bool isHL = (_cursor.sectionIdx == s && _cursor.controlIdx == -1);
        const bool isExp = (_expandedSection == s);

        if (screenY >= clipTop && screenY + SEC_HDR_H <= clipBot) {
            SectionHeader::draw(*_display, 0, screenY, SW,
                                kSections[s].label, isExp, isHL);
        }

        if (isExp) {
            const int16_t bodyScreenY = screenY + SEC_HDR_H;
            const int16_t bodyH = _calcExpandedBodyHeight(s);
            if (bodyScreenY + bodyH > clipTop && bodyScreenY < clipBot) {
                _drawSectionBody(s, bodyScreenY);
            }
        }
        virtualY += sectH;
    }

    // Rebuild position cache after structural redraw
    _buildPositionCache();
    memset(_sectionHdrDirty, 0, sizeof(_sectionHdrDirty));
}

// =============================================================================
// _drawDirtyControls() — incremental: only flagged cells
// =============================================================================

void HomeScreen::_drawDirtyControls() {
    if (_expandedSection < 0) { _clearAllControlDirty(); return; }

    // Rebuild cache if stale
    if (_cachedExpandedSect != _expandedSection || _cachedScrollY != _scrollY) {
        _buildPositionCache();
    }

    for (int i = 0; i < _cachedCtrlCount && i < MAX_SECT_CONTROLS; ++i) {
        if (!_ctrlDirty[i]) continue;
        _ctrlDirty[i] = false;

        if (!_ctrlPos[i].visible) continue;

        _drawControl(_expandedSection, i, _ctrlPos[i].x, _ctrlPos[i].y);
    }
}

// =============================================================================
// _drawSectionBody() — full body draw (called from _drawContent only)
// =============================================================================

void HomeScreen::_drawSectionBody(int16_t sectIdx, int16_t screenY) {
    if (sectIdx < 0 || sectIdx >= SECTION_COUNT) return;
    const SectionDef& sec = kSections[sectIdx];
    const int16_t clipTop = CONTENT_Y;
    const int16_t clipBot = CONTENT_Y + CONTENT_H;
    int16_t curY = screenY + BODY_PAD_Y;
    int16_t flatCtrl = 0;

    for (int g = 0; g < sec.groupCount; ++g) {
        const GroupDef& grp = sec.groups[g];
        if (grp.controlCount == 0) continue;
        if (curY >= clipBot) break;

        if (curY >= clipTop && curY + GRP_HDR_H <= clipBot) {
            GroupHeader::draw(*_display, BODY_PAD_X, curY,
                              SW - 2 * BODY_PAD_X, grp.label);
        }
        curY += GRP_HDR_H;

        int16_t rowH = 0;
        for (int c = 0; c < grp.controlCount; ++c) {
            switch (grp.controls[c].type) {
                case CtrlType::KNOB:   rowH = max(rowH, KNOB_CELL_H); break;
                case CtrlType::SELECT: rowH = max(rowH, SEL_CELL_H);  break;
                case CtrlType::TOGGLE: rowH = max(rowH, TOG_CELL_H);  break;
                case CtrlType::GRID:   rowH = max(rowH, (int16_t)SGRID_TOTAL_H); break;
                case CtrlType::ENVELOPE: rowH = max(rowH, (int16_t)MiniEnvLayout::ENV_CELL_H); break;
                default: break;
            }
        }

        int16_t curX = BODY_PAD_X;
        for (int c = 0; c < grp.controlCount; ++c) {
            const ControlDef& ctrl = grp.controls[c];
            if (ctrl.type == CtrlType::NONE) { flatCtrl++; continue; }

            if (curY >= clipTop && curY + rowH <= clipBot) {
                _drawControl(sectIdx, flatCtrl, curX, curY);
            }

            switch (ctrl.type) {
                case CtrlType::KNOB:   curX += KNOB_CELL_W + CTRL_GAP_X; break;
                case CtrlType::SELECT: curX += SEL_CELL_W + CTRL_GAP_X;  break;
                case CtrlType::TOGGLE: curX += TOG_CELL_W + CTRL_GAP_X;  break;
                case CtrlType::GRID:   curX += SW - 2 * BODY_PAD_X;      break;
                default: break;
            }
            flatCtrl++;
        }
        curY += rowH + GRP_PAD_BOTTOM;
    }
}

// =============================================================================
// _drawControl() — draw a single control (used by both full and incremental)
// =============================================================================

void HomeScreen::_drawControl(int16_t sectIdx, int16_t flatCtrl,
                              int16_t screenX, int16_t screenY) {
    const ControlDef& ctrl = _controlDef(sectIdx, flatCtrl);
    if (ctrl.cc == 255 || ctrl.type == CtrlType::NONE) return;
    if (!_synth || !_display) return;

    const uint8_t rawVal = _getCC(ctrl.cc);
    const bool selected  = (_cursor.sectionIdx == sectIdx &&
                            _cursor.controlIdx == flatCtrl);

    switch (ctrl.type) {
        case CtrlType::KNOB: {
            char valBuf[8];
            snprintf(valBuf, sizeof(valBuf), "%d", (int)rawVal);
            MiniKnob::draw(*_display, screenX, screenY,
                           rawVal, ctrl.label, valBuf, selected);
            break;
        }
        case CtrlType::SELECT: {
            const char* enumText = _enumTextForCC(ctrl.cc);
            char fallback[8];
            if (!enumText || !enumText[0]) {
                snprintf(fallback, sizeof(fallback), "%d", (int)rawVal);
                enumText = fallback;
            }
            MiniSelect::draw(*_display, screenX, screenY,
                             ctrl.label, enumText, selected);
            break;
        }
        case CtrlType::TOGGLE: {
            MiniToggle::draw(*_display, screenX, screenY,
                             ctrl.label, (rawVal > 63), selected);
            break;
        }
        case CtrlType::GRID: {
            const StepSequencer& seq = _engine().getSeq1();
            uint8_t stepVals[16];
            const uint8_t stepCount = (uint8_t)constrain(seq.getStepCount(), 1, 16);
            for (uint8_t i = 0; i < 16; ++i)
                stepVals[i] = (i < stepCount) ? seq.getStepValue(i) : 0;
            const int8_t selStep  = (int8_t)(_getCC(CC::SEQ_STEP_SELECT) & 0x0F);
            const int8_t playStep = (int8_t)seq.getCurrentStep();
            MiniSliderGrid::draw(*_display, screenX, screenY,
                                 SW - 2 * BODY_PAD_X, stepCount, stepVals,
                                 selStep, playStep);
            break;
        }
        case CtrlType::ENVELOPE: {
        // Determine flavour from the sentinel CC
        EnvFlavour flav = EnvFlavour::AMP;
        if (ctrl.cc == CC::FILTER_ENV_ATTACK) flav = EnvFlavour::FILTER;
        if (ctrl.cc == CC::PITCH_ENV_ATTACK)  flav = EnvFlavour::PITCH;

        // Selected point: only if cursor is on this control
        int8_t selPt = -1;
        if (selected) {
            selPt = MiniEnvelope::cursorToPoint(flav, _envCursor);
        }

        // Wrap _getCC into a static-compatible lambda via _instance
        auto getCCfn = [](uint8_t cc) -> uint8_t {
            return _instance ? _instance->_getCC(cc) : 0;
        };

        MiniEnvelope::draw(*_display, screenX, screenY,
                            SW - 2 * BODY_PAD_X,
                            flav, getCCfn, selPt);
        break;
    }
        default: break;
    }
}

// =============================================================================
// _drawHeader (unchanged from v1)
// =============================================================================

void HomeScreen::_drawHeader(bool full) {
    if (full) {
        _display->fillRect(0, 0, SW, HEADER_H, COLOUR_HEADER_BG);
        _display->drawFastHLine(0, HEADER_H - 1, SW, COLOUR_BORDER);
        _display->setTextSize(1);
        _display->setTextColor(COLOUR_ACCENT_ORANGE, COLOUR_HEADER_BG);
        _display->setCursor(4, 7);
        _display->print("JT.8000");
        _lastCpuPct = -1;
        memset(_prevVoiceActive, 0xFF, sizeof(_prevVoiceActive));
    }
    _drawVoiceDots();
    const int cpuNow = (int)AudioProcessorUsage();
    if (cpuNow != _lastCpuPct) {
        _lastCpuPct = cpuNow;
        const int16_t cpuX = SW - 58;
        _display->fillRect(cpuX, 2, 56, HEADER_H - 4, COLOUR_HEADER_BG);
        char buf[12];
        snprintf(buf, sizeof(buf), "CPU:%d%%", cpuNow);
        _display->setTextSize(1);
        const uint16_t cpuCol = (cpuNow > 95) ? COLOUR_METER_RED :
                                (cpuNow > 90) ? COLOUR_METER_YELLOW : COLOUR_ACCENT_ORANGE;
        _display->setTextColor(cpuCol, COLOUR_HEADER_BG);
        _display->setCursor(cpuX, 7);
        _display->print(buf);
    }
}

void HomeScreen::_drawVoiceDots() {
    if (!_synth || !_display) return;
    const int16_t baseX = 54;
    const int16_t baseY = HEADER_H / 2;
    for (int v = 0; v < MAX_VOICES && v < 8; ++v) {
        const bool active = _engine().isVoiceActive(v);
        if (active == _prevVoiceActive[v]) continue;
        _prevVoiceActive[v] = active;
        const int16_t dotX = baseX + v * (VOICE_DOT_R * 2 + VOICE_DOT_GAP);
        _display->fillCircle(dotX, baseY, VOICE_DOT_R + 1, COLOUR_HEADER_BG);
        _display->fillCircle(dotX, baseY, VOICE_DOT_R,
                             active ? COLOUR_ACCENT_ORANGE : COLOUR_BORDER);
    }
}

// =============================================================================
// Input: Encoder L (navigation) — granular dirty flags
// =============================================================================

void HomeScreen::onEncoderLeft(int delta) {
    if (!delta) return;
    if (_entry.isOpen()) { _entry.onEncoderDelta(delta); return; }

    const int16_t oldSect = _cursor.sectionIdx;
    const int16_t oldCtrl = _cursor.controlIdx;

    if (_expandedSection < 0) {
        // All collapsed: move between section headers
        _cursor.sectionIdx = (_cursor.sectionIdx + delta + SECTION_COUNT) % SECTION_COUNT;
        _cursor.controlIdx = -1;
        _scrollToCursor();

        // Mark only old + new header dirty (not full layout)
        if (!_layoutDirty) {
            _sectionHdrDirty[oldSect] = true;
            _sectionHdrDirty[_cursor.sectionIdx] = true;
        }
    } else {
        const int16_t numCtrls = _controlCount(_expandedSection);
        int16_t newCtrl = _cursor.controlIdx + delta;

        if (newCtrl < -1) {
            const int16_t prevSect = (_expandedSection - 1 + SECTION_COUNT) % SECTION_COUNT;
            _collapseAll();  // sets _layoutDirty
            _cursor.sectionIdx = prevSect;
            _cursor.controlIdx = -1;
            _scrollToCursor();
        } else if (newCtrl >= numCtrls) {
            const int16_t nextSect = (_expandedSection + 1) % SECTION_COUNT;
            _collapseAll();  // sets _layoutDirty
            _cursor.sectionIdx = nextSect;
            _cursor.controlIdx = -1;
            _scrollToCursor();
        } else {
            _cursor.controlIdx = newCtrl;
              if (newCtrl >= 0) {
                const ControlDef& c = _controlDef(_expandedSection, newCtrl);
                if (c.type == CtrlType::ENVELOPE) {
                    _envCursor = 0;
                    _envFlavour = EnvFlavour::AMP;
                    if (c.cc == CC::FILTER_ENV_ATTACK) _envFlavour = EnvFlavour::FILTER;
                    if (c.cc == CC::PITCH_ENV_ATTACK)  _envFlavour = EnvFlavour::PITCH;
                }
            }
            //_scrollToCursor();

            // Mark only old + new control dirty (NOT full layout)
            if (!_layoutDirty) {
                if (oldCtrl == -1) {
                    // Was on header, now on control
                    _sectionHdrDirty[oldSect] = true;
                    _markControlDirty(newCtrl);
                } else if (newCtrl == -1) {
                    // Was on control, now on header
                    _markControlDirty(oldCtrl);
                    _sectionHdrDirty[_cursor.sectionIdx] = true;
                } else {
                    // Control to control
                    _markControlDirty(oldCtrl);
                    _markControlDirty(newCtrl);
                }
            }
        }
    }
}

void HomeScreen::onEncoderLeftPress() {
    if (_entry.isOpen()) return;
    if (_cursor.controlIdx == -1) {
        _toggleSection(_cursor.sectionIdx);  // sets _layoutDirty
    } else {
          if (_cursor.controlIdx >= 0) {
                const ControlDef& ctrl = _controlDef(_cursor.sectionIdx, _cursor.controlIdx);
                if (ctrl.type == CtrlType::ENVELOPE) {
                    // Determine flavour
                    EnvFlavour flav = EnvFlavour::AMP;
                    if (ctrl.cc == CC::FILTER_ENV_ATTACK) flav = EnvFlavour::FILTER;
                    if (ctrl.cc == CC::PITCH_ENV_ATTACK)  flav = EnvFlavour::PITCH;

                    // Cycle to next sub-point
                    _envCursor = (_envCursor + 1) % MiniEnvelope::pointCount(flav);
                    _markControlDirty(_cursor.controlIdx);
                    return;
                }
                _openEntry(_cursor.sectionIdx, _cursor.controlIdx);
            }
    }
}

// =============================================================================
// Input: Encoder R (value editing) — marks ONE control dirty
// =============================================================================

void HomeScreen::onEncoderRight(int delta) {
    if (!delta || _entry.isOpen()) return;
    if (_cursor.controlIdx >= 0 && _expandedSection >= 0) {
        _adjustValue(_expandedSection, _cursor.controlIdx, delta);
        _markControlDirty(_cursor.controlIdx);  // ONE cell, not full layout
    }
}

void HomeScreen::onEncoderRightPress() {
    if (_entry.isOpen()) return;
    if (_cursor.controlIdx >= 0 && _expandedSection >= 0) {
        _openEntry(_expandedSection, _cursor.controlIdx);
    }
}

// =============================================================================
// Input: Touch
// =============================================================================

bool HomeScreen::onTouch(int16_t x, int16_t y) {
    if (_entry.isOpen()) {
        _entry.onTouch(x, y);
        return true;
    }
    if (y < CONTENT_Y) return false;

    const int16_t virtualTouchY = (y - CONTENT_Y) + _scrollY;
    int16_t virtualY = 0;

    for (int s = 0; s < SECTION_COUNT; ++s) {
        const int16_t sectH = _calcSectionHeight(s);
        if (virtualTouchY >= virtualY && virtualTouchY < virtualY + sectH) {

            if (virtualTouchY < virtualY + SEC_HDR_H) {
                _cursor.sectionIdx = s;
                _cursor.controlIdx = -1;
                _toggleSection(s);
                return true;
            }

            if (s == _expandedSection) {
                int16_t bodyY = virtualY + SEC_HDR_H + BODY_PAD_Y;
                int16_t flatCtrl = 0;
                const SectionDef& sec = kSections[s];

                for (int g = 0; g < sec.groupCount; ++g) {
                    const GroupDef& grp = sec.groups[g];
                    if (grp.controlCount == 0) continue;
                    bodyY += GRP_HDR_H;
                    int16_t rowH = KNOB_CELL_H;

                    int16_t ctrlX = BODY_PAD_X;
                    for (int c = 0; c < grp.controlCount; ++c) {
                        const ControlDef& ctrl = grp.controls[c];
                        if (ctrl.type == CtrlType::NONE) { flatCtrl++; continue; }

                        int16_t cellW = 0, cellH = 0;
                        switch (ctrl.type) {
                            case CtrlType::KNOB:   cellW = KNOB_CELL_W; cellH = KNOB_CELL_H; break;
                            case CtrlType::SELECT: cellW = SEL_CELL_W;  cellH = SEL_CELL_H;  break;
                            case CtrlType::TOGGLE: cellW = TOG_CELL_W;  cellH = TOG_CELL_H;  break;
                            default: cellW = SW - 2*BODY_PAD_X; cellH = SGRID_TOTAL_H; break;
                        }

                        if (x >= ctrlX && x < ctrlX + cellW &&
                            virtualTouchY >= bodyY && virtualTouchY < bodyY + cellH) {

                            const int16_t oldCtrl = _cursor.controlIdx;
                            _cursor.sectionIdx = s;
                            _cursor.controlIdx = flatCtrl;

                            if (ctrl.type == CtrlType::TOGGLE && _synth) {
                                // Toggle: flip immediately
                                uint8_t cur = _getCC(ctrl.cc);
                                _setCC(ctrl.cc, (cur > 63) ? 0 : 127);
                                _markControlDirty(flatCtrl);
                            } else if (ctrl.type == CtrlType::GRID && _synth) {
                                // Grid: hit-test to find which step was tapped
                                // Use screen-space X and the cached/calculated grid position
                                const int16_t gridScreenY = CONTENT_Y + (bodyY - _scrollY);
                                const StepSequencer& seq = _engine().getSeq1();
                                const uint8_t stepCount = (uint8_t)constrain(seq.getStepCount(), 1, 16);
                                const int8_t tapped = MiniSliderGrid::hitTestStep(
                                    ctrlX, gridScreenY, SW - 2 * BODY_PAD_X,
                                    stepCount, x, y);
                                if (tapped >= 0) {
                                    // Select this step
                                    _setCC(CC::SEQ_STEP_SELECT, (uint8_t)tapped);
                                    _markControlDirty(flatCtrl);
                                    // Open numeric entry for step VALUE
                                    _pendingCC = CC::SEQ_STEP_VALUE;
                                    const uint8_t stepVal = seq.getStepValue(tapped);
                                    char title[16];
                                    snprintf(title, sizeof(title), "Step %d", tapped + 1);
                                    _entry.openNumeric(title, "", 0, 127, (int)stepVal,
                                        [](int val) {
                                            if (!_instance || !_instance->_synth) return;
                                            _instance->_setCC(CC::SEQ_STEP_VALUE,
                                                (uint8_t)constrain(val, 0, 127));
                                            _instance->_markAllControlsDirty();
                                        });
                                }
                            } else {
                                // Knob/Select: open entry overlay
                                _openEntry(s, flatCtrl);
                            }
                            if (oldCtrl >= 0 && oldCtrl != flatCtrl)
                                _markControlDirty(oldCtrl);
                            return true;
                        }
                        ctrlX += cellW + CTRL_GAP_X;
                        flatCtrl++;
                    }
                    bodyY += rowH + GRP_PAD_BOTTOM;
                }
            }
            return true;
        }
        virtualY += sectH;
    }
    return false;
}

void HomeScreen::onSwipe(int deltaY) {
    _scrollY -= deltaY;
    _clampScroll();
    _layoutDirty = true;  // scroll = structural (positions change)
}

// =============================================================================
// Value editing
// =============================================================================

void HomeScreen::_adjustValue(int16_t sectIdx, int16_t flatCtrl, int delta) {
    const ControlDef& ctrl = _controlDef(sectIdx, flatCtrl);
    if (ctrl.cc == 255 || !_synth) return;

    if (ctrl.type == CtrlType::TOGGLE) {
        // Toggle: any delta flips state
        uint8_t cur = _getCC(ctrl.cc);
        _setCC(ctrl.cc, (cur > 63) ? 0 : 127);

    } else if (ctrl.type == CtrlType::GRID) {
        // Grid: Encoder R adjusts the VALUE of the currently SELECTED step.
        // The selected step index is in SEQ_STEP_SELECT.
        // The value to adjust is SEQ_STEP_VALUE.
        const uint8_t selStep = _getCC(CC::SEQ_STEP_SELECT) & 0x0F;
        const uint8_t curVal  = _engine().getSeq1().getStepValue(selStep);
        int16_t newVal = (int16_t)curVal + delta;
        // First set which step we're editing, then set its value
        _setCC(CC::SEQ_STEP_SELECT, selStep);
        _setCC(CC::SEQ_STEP_VALUE, (uint8_t)constrain(newVal, 0, 127));
      } else if (ctrl.type == CtrlType::ENVELOPE) {
        // Determine flavour
        EnvFlavour flav = EnvFlavour::AMP;
        if (ctrl.cc == CC::FILTER_ENV_ATTACK) flav = EnvFlavour::FILTER;
        if (ctrl.cc == CC::PITCH_ENV_ATTACK)  flav = EnvFlavour::PITCH;

        // Get the CC for the currently selected sub-point
        int8_t globalPt = MiniEnvelope::cursorToPoint(flav, _envCursor);
        uint8_t ptCC = MiniEnvelope::pointToCC(flav, globalPt);
        if (ptCC != 255) {
            int16_t newVal = (int16_t)_getCC(ptCC) + delta;
            _setCC(ptCC, (uint8_t)constrain(newVal, 0, 127));
        } else {
            // Knob/Select: adjust the control's own CC
            int16_t newVal = (int16_t)_getCC(ctrl.cc) + delta;
            _setCC(ctrl.cc, (uint8_t)constrain(newVal, 0, 127));
        }
    }
}

// =============================================================================
// Entry overlay — dispatcher + enum/numeric (unchanged from v1)
// =============================================================================

void HomeScreen::_openEntry(int16_t sectIdx, int16_t flatCtrl) {
    const ControlDef& ctrl = _controlDef(sectIdx, flatCtrl);
    if (ctrl.cc == 255 || !_synth) return;

    if (ctrl.type == CtrlType::GRID) {
        // Grid: open numeric entry for the currently selected step's VALUE
        const uint8_t selStep = _getCC(CC::SEQ_STEP_SELECT) & 0x0F;
        const uint8_t stepVal = _engine().getSeq1().getStepValue(selStep);
        _pendingCC = CC::SEQ_STEP_VALUE;
        char title[16];
        snprintf(title, sizeof(title), "Step %d", selStep + 1);
        _entry.openNumeric(title, "", 0, 127, (int)stepVal,
            [](int val) {
                if (!_instance || !_instance->_synth) return;
                _instance->_setCC(CC::SEQ_STEP_VALUE,
                    (uint8_t)constrain(val, 0, 127));
                _instance->_markAllControlsDirty();
            });
    } else if (_isEnumCC(ctrl.cc)) {
        _openEnumEntry(ctrl.cc, ctrl.label);
    } else {
        _openNumericEntry(ctrl.cc, ctrl.label);
    }
}

void HomeScreen::_openEnumEntry(uint8_t cc, const char* title) {
    if (!_synth) return;

    static const char* kOnOff[]    = { "Off", "On" };
    static const char* kBypass[]   = { "Active", "Bypass" };
    static const char* kPolyMode[] = { "Poly", "Mono", "Unison" };
    static const char* kClkSrc[]   = { "Internal", "MIDI" };
    static constexpr int kWaveCount = (int)numWaveformsAll;
    const char* const* kWave = waveLongNames;
    static const char* kLFOwave[] = { "Sine", "Triangle", "Saw", "Square" };
    static const char* kLFOdest[] = { "Off", "Pitch", "Filter", "PWM" };
    static const char* kSync[] = { "Free","1/1","1/2","1/4","1/8","1/16","1/1T","1/2T","1/4T","1/8T","1/16T","1/32" };
    static const char* kModFX[] = { "Off","Chorus 1","Chorus 2","Chorus 3","Flanger 1","Flanger 2","Flanger 3","Phaser 1","Phaser 2","Phaser 3","Phaser 4","Super Chorus" };
    static const char* kDelayFX[] = { "Off","Mono Short","Mono Long","Pan L>R","Pan R>L","Pan Stereo" };
    static const char* kDrive[] = { "Bypass", "Soft Clip", "Hard Clip" };
    static const char* kSeqDir[] = { "Forward","Reverse","Bounce","Random" };
    static const char* kSeqDest[] = { "None","Pitch","Filter","PWM","Amp" };

    const char* const* opts = kOnOff; int count = 2;
    bool isOscWave = false, isFxMod = false, isFxDelay = false;

    switch (cc) {
        case CC::OSC1_WAVE: case CC::OSC2_WAVE:
            opts = kWave; count = kWaveCount; isOscWave = true; break;
        case CC::LFO1_WAVEFORM: case CC::LFO2_WAVEFORM: opts = kLFOwave; count = 4; break;
        case CC::LFO1_DESTINATION: case CC::LFO2_DESTINATION: opts = kLFOdest; count = 4; break;
        case CC::LFO1_TIMING_MODE: case CC::LFO2_TIMING_MODE:
        case CC::DELAY_TIMING_MODE: case CC::SEQ_TIMING_MODE: opts = kSync; count = 12; break;
        case CC::BPM_CLOCK_SOURCE: opts = kClkSrc; count = 2; break;
        case CC::FX_REVERB_BYPASS: opts = kBypass; count = 2; break;
        case CC::GLIDE_ENABLE: case CC::SEQ_ENABLE: case CC::SEQ_RETRIGGER:
        case CC::OSC_SYNC_ENABLE: opts = kOnOff; count = 2; break;
        case CC::POLY_MODE: opts = kPolyMode; count = 3; break;
        case CC::FX_MOD_EFFECT: opts = kModFX; count = 12; isFxMod = true; break;
        case CC::FX_JPFX_DELAY_EFFECT: opts = kDelayFX; count = 6; isFxDelay = true; break;
        case CC::FX_DRIVE: opts = kDrive; count = 3; break;
        case CC::SEQ_DIRECTION: opts = kSeqDir; count = 4; break;
        case CC::SEQ_DESTINATION: opts = kSeqDest; count = 5; break;
        case CC::PERF_MODE: { static const char* k[]={"Single","Layer","Split"}; opts=k; count=3; } break;
        case CC::PERF_EDIT_TARGET: { static const char* k[]={"Layer A","Layer B","Both"}; opts=k; count=3; } break;
        case CC::PERF_VOICE_SPLIT: { static const char* k[]={"1+7","2+6","3+5","4+4","5+3","6+2","7+1"}; opts=k; count=7; } break;
        case CC::PERF_SPLIT_NOTE: {
            // Build note name list — C-1 to G9 (128 notes)
            static char noteNames[128][5];
            static const char* notePtrs[128];
            static bool built = false;
            if (!built) {
                static const char* nn[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                for (int i = 0; i < 128; ++i) {
                    snprintf(noteNames[i], 5, "%s%d", nn[i%12], (i/12)-1);
                    notePtrs[i] = noteNames[i];
                }
                built = true;
            }
            opts = notePtrs; count = 128;
        } break;
        case CC::PERF_BALANCE: { static const char* k[]={"A only","A > B","A+B equal","B > A","B only"}; opts=k; count=5; } break;
        case CC::PERF_MIDI_CHANNEL_A:
        case CC::PERF_MIDI_CHANNEL_B: {
            // 16-channel dropdown. Built once, reused for both CCs.
            static char chNames[16][6];
            static const char* chPtrs[16];
            static bool chBuilt = false;
            if (!chBuilt) {
                for (int i = 0; i < 16; ++i) {
                    snprintf(chNames[i], 6, "Ch %d", i + 1);
                    chPtrs[i] = chNames[i];
                }
                chBuilt = true;
            }
            opts = chPtrs; count = 16;
        } break;
        case CC::FILTER_ENGINE: { static const char* k[]={"OBXa","VA Bank"}; opts=k; count=CC::FILTER_ENGINE_COUNT; } break;
        case CC::FILTER_MODE: { static const char* k[]={"4-Pole LP","2-Pole LP","2-Pole BP","2-Pole Push","Xpander","Xpander+M"}; opts=k; count=CC::FILTER_MODE_COUNT; } break;
        case CC::VA_FILTER_TYPE: opts = kVAFilterNames; count = (int)FILTER_COUNT; break;
        case CC::FILTER_OBXA_XPANDER_MODE: { static const char* k[]={"LP4","LP3","LP2","LP1","HP3","HP2","HP1","BP4","BP2","N2","PH3","HP2+LP1","HP3+LP1","N2+LP1","PH3+LP1"}; opts=k; count=15; } break;
        default: opts = kOnOff; count = 2; break;
    }

    const uint8_t rawVal = _getCC(cc);
    int curIdx;
    if (isOscWave) {
        WaveformType wt = waveformFromCC(rawVal); curIdx = 0;
        for (int i = 0; i < (int)numWaveformsAll; ++i) { if (waveformListAll[i] == wt) { curIdx = i; break; } }
    } else if (cc == CC::POLY_MODE) { curIdx = (rawVal <= 42) ? 0 : (rawVal <= 84) ? 1 : 2; }
    else if (cc == CC::PERF_MODE) { curIdx = (rawVal <= 42) ? 0 : (rawVal <= 84) ? 1 : 2; }
    else if (cc == CC::PERF_EDIT_TARGET) { curIdx = (rawVal <= 42) ? 0 : (rawVal <= 84) ? 1 : 2; }
    else if (cc == CC::PERF_VOICE_SPLIT) { curIdx = constrain((int)rawVal * 7 / 128, 0, 6); }
    else if (cc == CC::PERF_SPLIT_NOTE) { curIdx = constrain((int)rawVal, 0, 127); }
    else if (cc == CC::PERF_BALANCE) { curIdx = (rawVal < 26) ? 0 : (rawVal < 58) ? 1 : (rawVal < 70) ? 2 : (rawVal < 102) ? 3 : 4; }
    else if (cc == CC::PERF_MIDI_CHANNEL_A || cc == CC::PERF_MIDI_CHANNEL_B) {
        // Firmware decode: channel = ((v * 16) / 128) + 1. curIdx is 0-based.
        curIdx = constrain(((int)rawVal * 16) / 128, 0, 15);
    }
    else if (isFxMod) { curIdx = (rawVal == 0) ? 0 : constrain(((int)(rawVal-1)*11)/127+1, 1, 11); }
    else if (isFxDelay) { curIdx = (rawVal == 0) ? 0 : constrain(((int)(rawVal-1)*5)/127+1, 1, 5); }
    else if (cc == CC::FX_DRIVE) { curIdx = (rawVal < 16) ? 0 : (rawVal < 64) ? 1 : 2; }
    // Sequencer direction: firmware uses (v*3)/127
    else if (cc == CC::SEQ_DIRECTION) { curIdx = constrain((int)(rawVal * 3) / 127, 0, 3); }
    // Sequencer destination: firmware uses (v*4)/127
    else if (cc == CC::SEQ_DESTINATION) { curIdx = constrain((int)(rawVal * 4) / 127, 0, 4); }
    else if (count == 2) { curIdx = (rawVal != 0) ? 1 : 0; }
    else { curIdx = (int)rawVal * count / 128; }

    _pendingCC = cc; _pendingCount = count;

    _entry.openEnum(title, opts, count, constrain(curIdx, 0, count-1),
        [](int idx) {
            if (!_instance || !_instance->_synth) return;
            HomeScreen* s = _instance; uint8_t ccVal;
            if (s->_pendingCC==CC::OSC1_WAVE||s->_pendingCC==CC::OSC2_WAVE) {
                ccVal = (idx>=0&&idx<(int)numWaveformsAll) ? ccFromWaveform(waveformListAll[idx]) : 0;
            } else if (s->_pendingCC==CC::POLY_MODE) { static const uint8_t m[]={21,63,106}; ccVal=(idx>=0&&idx<3)?m[idx]:0;
            } else if (s->_pendingCC==CC::PERF_MODE) { static const uint8_t m[]={21,63,106}; ccVal=(idx>=0&&idx<3)?m[idx]:0;
            } else if (s->_pendingCC==CC::PERF_EDIT_TARGET) { static const uint8_t m[]={21,63,106}; ccVal=(idx>=0&&idx<3)?m[idx]:0;
            } else if (s->_pendingCC==CC::PERF_VOICE_SPLIT) { ccVal=(uint8_t)constrain(idx*128/7, 0, 127);
            } else if (s->_pendingCC==CC::PERF_SPLIT_NOTE) { ccVal=(uint8_t)constrain(idx, 0, 127);
            } else if (s->_pendingCC==CC::PERF_BALANCE) { static const uint8_t m[]={0,32,64,96,127}; ccVal=(idx>=0&&idx<5)?m[idx]:64;
            } else if (s->_pendingCC==CC::PERF_MIDI_CHANNEL_A || s->_pendingCC==CC::PERF_MIDI_CHANNEL_B) {
                // Per-channel bucket centres. Using a LUT rather than a
                // formula so round-tripping through ccToChoiceIndex is
                // stable — the firmware uses truncating integer division
                // (v * 16) / 128 which rounds down to a bucket; we emit
                // the CENTRE of each bucket so the chosen channel comes
                // back correctly on the next read.
                static const uint8_t m[] = {
                    4, 12, 20, 28, 36, 44, 52, 60,
                    68, 76, 84, 92, 100, 108, 116, 124
                };
                ccVal = (idx >= 0 && idx < 16) ? m[idx] : 4;
            } else if (s->_pendingCC==CC::FX_MOD_EFFECT) { static const uint8_t m[]={0,6,17,28,40,51,62,74,85,96,108,127}; ccVal=(idx>=0&&idx<12)?m[idx]:0;
            } else if (s->_pendingCC==CC::FX_JPFX_DELAY_EFFECT) { static const uint8_t m[]={0,13,38,64,89,114}; ccVal=(idx>=0&&idx<6)?m[idx]:0;
            } else if (s->_pendingCC==CC::FX_DRIVE) { static const uint8_t m[]={0,32,96}; ccVal=(idx>=0&&idx<3)?m[idx]:0;
            } else if (s->_pendingCC==CC::FILTER_OBXA_XPANDER_MODE) { static const uint8_t m[]={4,13,21,30,38,47,55,64,72,81,89,98,106,115,123}; ccVal=(idx>=0&&idx<15)?m[idx]:0;
            // Sequencer direction: firmware uses (v*3)/127 → 4 buckets
            } else if (s->_pendingCC==CC::SEQ_DIRECTION) { static const uint8_t m[]={0,43,85,127}; ccVal=(idx>=0&&idx<4)?m[idx]:0;
            // Sequencer destination: firmware uses (v*4)/127 → 5 buckets
            } else if (s->_pendingCC==CC::SEQ_DESTINATION) { static const uint8_t m[]={0,32,64,96,127}; ccVal=(idx>=0&&idx<5)?m[idx]:0;
            } else { ccVal=(uint8_t)((idx*128+(128/s->_pendingCount)/2)/s->_pendingCount); }
            s->_setCC(s->_pendingCC, ccVal);
            s->_markAllControlsDirty();  // preset/enum change may affect display text
        });
}

void HomeScreen::_openNumericEntry(uint8_t cc, const char* title) {
    if (!_synth) return;
    using namespace JT8000Map;
    const char* unit = ""; int minV = 0, maxV = 127, curV = (int)_getCC(cc);
    switch (cc) {
        case CC::FILTER_CUTOFF: unit="Hz"; minV=20; maxV=18000; curV=(int)_engine().getFilterCutoff(); break;
        case CC::AMP_ATTACK: case CC::AMP_DECAY: case CC::AMP_RELEASE:
        case CC::FILTER_ENV_ATTACK: case CC::FILTER_ENV_DECAY: case CC::FILTER_ENV_RELEASE:
        case CC::PITCH_ENV_ATTACK: case CC::PITCH_ENV_DECAY: case CC::PITCH_ENV_RELEASE:
            unit="ms"; minV=1; maxV=11880; curV=(int)cc_to_time_ms(_getCC(cc)); break;
        case CC::AMP_SUSTAIN: case CC::FILTER_ENV_SUSTAIN: case CC::PITCH_ENV_SUSTAIN:
            unit="%"; minV=0; maxV=100; curV=(int)(_getCC(cc)*100/127); break;
        case CC::LFO1_FREQ: case CC::LFO2_FREQ:
            unit="Hz"; minV=0; maxV=39; curV=(int)cc_to_lfo_hz(_getCC(cc)); break;
        case CC::FX_BASS_GAIN: case CC::FX_TREBLE_GAIN:
            unit="dB"; minV=-12; maxV=12; curV=(int)((_getCC(cc)/127.0f)*24.0f-12.0f); break;
        case CC::BPM_INTERNAL_TEMPO:
            unit="BPM"; minV=20; maxV=300; curV=(int)(20.0f+(_getCC(cc)/127.0f)*280.0f); break;
        case CC::OSC1_PITCH_OFFSET: case CC::OSC2_PITCH_OFFSET:
            unit="st"; minV=-24; maxV=24; curV=(int)(_getCC(cc)*48/127-24); break;
        case CC::OSC1_FINE_TUNE: case CC::OSC2_FINE_TUNE:
            unit="ct"; minV=-100; maxV=100; curV=(int)(_getCC(cc)*200/127-100); break;
        case CC::PITCH_ENV_DEPTH:
            unit="st"; minV=-12; maxV=12; curV=(int)((_getCC(cc)-64)*12/63); break;
        case CC::PITCH_BEND_RANGE:
            unit="st"; minV=0; maxV=24; curV=(int)(_getCC(cc)*24/127); break;
        case CC::FX_DRIVE:
            unit="%"; minV=0; maxV=100; curV=(int)(_getCC(cc)*100/127); break;
        default: break;
    }
    _pendingCC = cc;
    _entry.openNumeric(title, unit, minV, maxV, curV,
        [](int humanVal) {
            if (!_instance || !_instance->_synth) return;
            HomeScreen* s = _instance; const uint8_t tcc = s->_pendingCC;
            uint8_t ccVal = 0; using namespace JT8000Map;
            switch (tcc) {
                case CC::FILTER_CUTOFF: ccVal=obxa_cutoff_hz_to_cc((float)humanVal); break;
                case CC::AMP_ATTACK: case CC::AMP_DECAY: case CC::AMP_RELEASE:
                case CC::FILTER_ENV_ATTACK: case CC::FILTER_ENV_DECAY: case CC::FILTER_ENV_RELEASE:
                case CC::PITCH_ENV_ATTACK: case CC::PITCH_ENV_DECAY: case CC::PITCH_ENV_RELEASE:
                    ccVal=time_ms_to_cc((float)humanVal); break;
                case CC::AMP_SUSTAIN: case CC::FILTER_ENV_SUSTAIN: case CC::PITCH_ENV_SUSTAIN:
                    ccVal=(uint8_t)(humanVal*127/100); break;
                case CC::LFO1_FREQ: case CC::LFO2_FREQ: ccVal=lfo_hz_to_cc((float)humanVal); break;
                case CC::FX_BASS_GAIN: case CC::FX_TREBLE_GAIN: ccVal=(uint8_t)((humanVal+12)*127/24); break;
                case CC::BPM_INTERNAL_TEMPO: ccVal=(uint8_t)((humanVal-20)*127/280); break;
                case CC::OSC1_PITCH_OFFSET: case CC::OSC2_PITCH_OFFSET: ccVal=(uint8_t)((humanVal+24)*127/48); break;
                case CC::OSC1_FINE_TUNE: case CC::OSC2_FINE_TUNE: ccVal=(uint8_t)((humanVal+100)*127/200); break;
                case CC::PITCH_ENV_DEPTH: ccVal=(uint8_t)constrain(64+humanVal*63/12,0,127); break;
                case CC::PITCH_BEND_RANGE: ccVal=(uint8_t)(humanVal*127/24); break;
                case CC::FX_DRIVE: ccVal=(uint8_t)(humanVal*127/100); break;
                default: ccVal=(uint8_t)constrain(humanVal,0,127); break;
            }
            s->_setCC(tcc, ccVal);
            s->_markAllControlsDirty();
        });
}

// =============================================================================
// Enum text display (unchanged)
// =============================================================================

const char* HomeScreen::_enumTextForCC(uint8_t cc) const {
    if (!_synth) return nullptr;
    switch (cc) {
        case CC::OSC1_WAVE: return _engine().getOsc1WaveformName();
        case CC::OSC2_WAVE: return _engine().getOsc2WaveformName();
        case CC::LFO1_WAVEFORM: return _engine().getLFO1WaveformName();
        case CC::LFO2_WAVEFORM: return _engine().getLFO2WaveformName();
        case CC::LFO1_DESTINATION: return _engine().getLFO1DestinationName();
        case CC::LFO2_DESTINATION: return _engine().getLFO2DestinationName();
        case CC::GLIDE_ENABLE: case CC::SEQ_ENABLE: case CC::SEQ_RETRIGGER:
        case CC::OSC_SYNC_ENABLE:
            return (_getCC(cc) > 63) ? "On" : "Off";
        case CC::FX_REVERB_BYPASS: {
            // Reverb lives globally now. Read bypass state from LayerManager's
            // shared GlobalFX instance rather than the per-layer engine.
            const bool bypassed = _layerMgr ? _layerMgr->getGlobalFX().getReverbBypass() : false;
            return bypassed ? "Bypass" : "Active";
        }
        case CC::FILTER_ENGINE: return (_engine().getFilterEngine()==CC::FILTER_ENGINE_VA) ? "VA Bank" : "OBXa";
        case CC::FILTER_MODE: { static const char* k[]={"4-Pole LP","2-Pole LP","2-Pole BP","2-Pole Push","Xpander","Xpander+M"}; return ((_engine().getFilterMode())<CC::FILTER_MODE_COUNT)?k[_engine().getFilterMode()]:"?"; }
        case CC::VA_FILTER_TYPE: { const uint8_t vt=_engine().getVAFilterType(); return (vt<(uint8_t)FILTER_COUNT)?kVAFilterNames[vt]:"?"; }
        case CC::FILTER_OBXA_XPANDER_MODE: { static const char* k[]={"LP4","LP3","LP2","LP1","HP3","HP2","HP1","BP4","BP2","N2","PH3","HP2+LP1","HP3+LP1","N2+LP1","PH3+LP1"}; return (_engine().getFilterXpanderMode()<15)?k[_engine().getFilterXpanderMode()]:"?"; }
        case CC::FX_MOD_EFFECT: return _engine().getFXModEffectName();
        case CC::FX_JPFX_DELAY_EFFECT: return _engine().getFXDelayEffectName();
        case CC::POLY_MODE: { const uint8_t v=_getCC(CC::POLY_MODE); return (v<=42)?"Poly":(v<=84)?"Mono":"Unison"; }
        case CC::FX_DRIVE: { const uint8_t v=_getCC(CC::FX_DRIVE); return (v<16)?"Bypass":(v<64)?"Soft Clip":"Hard Clip"; }
        case CC::SEQ_DIRECTION: { static const char* k[]={"Forward","Reverse","Bounce","Random"}; return k[constrain((int)(_getCC(cc)*3)/127,0,3)]; }
        case CC::SEQ_DESTINATION: { static const char* k[]={"None","Pitch","Filter","PWM","Amp"}; return k[constrain((int)(_getCC(cc)*4)/127,0,4)]; }
        case CC::LFO1_TIMING_MODE: case CC::LFO2_TIMING_MODE: case CC::DELAY_TIMING_MODE: case CC::SEQ_TIMING_MODE:
            { static const char* k[]={"Free","1/1","1/2","1/4","1/8","1/16","1/1T","1/2T","1/4T","1/8T","1/16T","1/32"}; return k[constrain(_getCC(cc)*12/128,0,11)]; }
        case CC::BPM_CLOCK_SOURCE: return (_getCC(cc)>63)?"MIDI":"Internal";
        case CC::PERF_MODE: { const uint8_t v=_getCC(cc); return (v<=42)?"Single":(v<=84)?"Layer":"Split"; }
        case CC::PERF_EDIT_TARGET: { const uint8_t v=_getCC(cc); return (v<=42)?"Layer A":(v<=84)?"Layer B":"Both"; }
        case CC::PERF_VOICE_SPLIT: {
            static char vsBuf[8];
            uint8_t va = constrain((int)_getCC(cc) * 7 / 128 + 1, 1, 7);
            snprintf(vsBuf, sizeof(vsBuf), "%d+%d", va, 8 - va);
            return vsBuf;
        }
        case CC::PERF_SPLIT_NOTE: {
            static char snBuf[8];
            static const char* nn[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
            uint8_t n = _getCC(cc);
            snprintf(snBuf, sizeof(snBuf), "%s%d", nn[n % 12], (n / 12) - 1);
            return snBuf;
        }
        case CC::PERF_BALANCE: {
            static char blBuf[8];
            uint8_t v = _getCC(cc);
            if (v < 60)       snprintf(blBuf, sizeof(blBuf), "A %d", 64 - v);
            else if (v > 68)  snprintf(blBuf, sizeof(blBuf), "%d B", v - 64);
            else              snprintf(blBuf, sizeof(blBuf), "= AB");
            return blBuf;
        }
        case CC::PERF_MIDI_CHANNEL_A:
        case CC::PERF_MIDI_CHANNEL_B: {
            // Firmware encodes CC → channel via ((v * 16) / 128) + 1.
            // Render buffer is shared with the PERF_SPLIT_NOTE case's snBuf
            // below? No — PERF_SPLIT_NOTE has its own. Use a local static
            // buffer here. The two cases can share a single buffer because
            // only one can be on-screen at a time in the redraw loop.
            static char chBuf[8];
            const uint8_t raw = _getCC(cc);
            const uint8_t chan = (raw * 16) / 128 + 1;
            snprintf(chBuf, sizeof(chBuf), "Ch %u", (unsigned)chan);
            return chBuf;
        }
        default: return nullptr;
    }
}

/*static*/ bool HomeScreen::_isEnumCC(uint8_t cc) {
    return (cc==CC::OSC1_WAVE||cc==CC::OSC2_WAVE||cc==CC::LFO1_WAVEFORM||cc==CC::LFO2_WAVEFORM||
            cc==CC::LFO1_DESTINATION||cc==CC::LFO2_DESTINATION||cc==CC::LFO1_TIMING_MODE||cc==CC::LFO2_TIMING_MODE||
            cc==CC::DELAY_TIMING_MODE||cc==CC::BPM_CLOCK_SOURCE||cc==CC::GLIDE_ENABLE||cc==CC::FX_REVERB_BYPASS||
            cc==CC::POLY_MODE||cc==CC::FX_MOD_EFFECT||cc==CC::FX_JPFX_DELAY_EFFECT||cc==CC::FILTER_ENGINE||
            cc==CC::FILTER_MODE||cc==CC::VA_FILTER_TYPE||cc==CC::FILTER_OBXA_XPANDER_MODE||cc==CC::FX_DRIVE||
            cc==CC::SEQ_ENABLE||cc==CC::SEQ_RETRIGGER||cc==CC::SEQ_DIRECTION||cc==CC::SEQ_DESTINATION||cc==CC::SEQ_TIMING_MODE||
            cc==CC::OSC_SYNC_ENABLE||
            cc==CC::PERF_MODE||cc==CC::PERF_EDIT_TARGET||cc==CC::PERF_VOICE_SPLIT||cc==CC::PERF_SPLIT_NOTE||cc==CC::PERF_BALANCE||
            cc==CC::PERF_MIDI_CHANNEL_A||cc==CC::PERF_MIDI_CHANNEL_B);
}

/*static*/ bool HomeScreen::_isToggleCC(uint8_t cc) {
    return (cc==CC::GLIDE_ENABLE||cc==CC::FX_REVERB_BYPASS||cc==CC::SEQ_ENABLE||cc==CC::SEQ_RETRIGGER||
            cc==CC::OSC_SYNC_ENABLE);
}
