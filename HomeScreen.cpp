// HomeScreen.cpp
// =============================================================================
// Scrollable accordion home screen for the JT-8000 TFT UI.
// See HomeScreen.h for design overview.
//
// PERFORMANCE:
//   The virtual viewport only draws sections/groups/controls that intersect
//   the visible 218px content area. A full 14-section list with one expanded
//   section typically has 5-8 visible elements — each a small fillRect + text.
//   Total draw cost for a frame with no changes: zero SPI (dirty flags skip).
//   Worst case (section expand): ~8 ms for the body area (measured).
//
// MIDI SAFETY:
//   Full repaints are phased across 3 draw() calls (same as old HomeScreen).
//   Each step completes in < 3 ms, allowing MIDI reads between steps.
// =============================================================================

#include "HomeScreen.h"
#include "WaveForms.h"         // waveformListAll, waveLongNames, waveformFromCC, ccFromWaveform
#include "AudioFilterVABank.h" // kVAFilterNames[], FILTER_COUNT
#include "StepSequencer.h"     // getStepValue, getStepCount, getCurrentStep
#include "CCDefs.h"
#include "Mapping.h"
#include <math.h>

using namespace MiniLayout;

// Static singleton for entry overlay callbacks
HomeScreen* HomeScreen::_instance = nullptr;

// Sentinel for out-of-range control access
const ControlDef HomeScreen::_emptyControl = { 255, CtrlType::NONE, "" };

// =============================================================================
// Constructor
// =============================================================================
HomeScreen::HomeScreen()
    : _display(nullptr)
    , _synth(nullptr)
    , _redrawStep(1)
    , _contentDirty(true)
    , _headerDirty(true)
    , _lastCpuPct(-1)
    , _lastHeaderMs(0)
    , _pendingCC(255)
    , _pendingCount(0)
    , _entryWasOpen(false)
{
    _cursor.sectionIdx = 0;
    _cursor.controlIdx = -1;  // highlight on section header
    _expandedSection   = -1;  // all collapsed initially
    _scrollY           = 0;

    memset(_prevVoiceActive, 0, sizeof(_prevVoiceActive));
}

// =============================================================================
// begin()
// =============================================================================
void HomeScreen::begin(ILI9341_t3n* disp, SynthEngine* synth) {
    _display  = disp;
    _synth    = synth;
    _instance = this;
    _entry.setDisplay(disp);
    markFullRedraw();
}

// =============================================================================
// markFullRedraw()
// =============================================================================
void HomeScreen::markFullRedraw() {
    _redrawStep    = 1;
    _contentDirty  = true;
    _headerDirty   = true;
    _lastCpuPct    = -1;
    memset(_prevVoiceActive, 0xFF, sizeof(_prevVoiceActive));
}

// =============================================================================
// syncFromEngine() — force all controls to refresh values from engine
// =============================================================================
void HomeScreen::syncFromEngine() {
    _contentDirty = true;
}

// =============================================================================
// Entry overlay access
// =============================================================================
bool HomeScreen::isEntryOpen() const { return _entry.isOpen(); }
TFTNumericEntry& HomeScreen::getEntry() { return _entry; }

void HomeScreen::onEntryEncoderDelta(int delta) {
    if (_entry.isOpen()) _entry.onEncoderDelta(delta);
}

// =============================================================================
// Content height calculations
// =============================================================================

int16_t HomeScreen::_calcSectionHeight(int16_t sectIdx) const {
    int16_t h = SEC_HDR_H;  // header always present
    if (sectIdx == _expandedSection) {
        h += _calcExpandedBodyHeight(sectIdx);
    }
    return h;
}

int16_t HomeScreen::_calcExpandedBodyHeight(int16_t sectIdx) const {
    if (sectIdx < 0 || sectIdx >= SECTION_COUNT) return 0;
    const SectionDef& sec = kSections[sectIdx];

    int16_t h = BODY_PAD_Y;  // top padding

    for (int g = 0; g < sec.groupCount; ++g) {
        const GroupDef& grp = sec.groups[g];
        if (grp.controlCount == 0) continue;

        h += GRP_HDR_H;  // group label

        // Calculate control row height based on tallest control type in group
        int16_t rowH = 0;
        for (int c = 0; c < grp.controlCount; ++c) {
            switch (grp.controls[c].type) {
                case CtrlType::KNOB:   rowH = max(rowH, KNOB_CELL_H); break;
                case CtrlType::SELECT: rowH = max(rowH, SEL_CELL_H);  break;
                case CtrlType::TOGGLE: rowH = max(rowH, TOG_CELL_H);  break;
                case CtrlType::GRID:   rowH = max(rowH, (int16_t)(GRID_BAR_H + GRID_NUM_H + 4)); break;
                default: break;
            }
        }
        h += rowH + GRP_PAD_BOTTOM;
    }

    h += BODY_PAD_Y;  // bottom padding
    return h;
}

int16_t HomeScreen::_calcTotalHeight() const {
    int16_t total = 0;
    for (int i = 0; i < SECTION_COUNT; ++i) {
        total += _calcSectionHeight(i);
    }
    return total;
}

int16_t HomeScreen::_calcSectionY(int16_t sectIdx) const {
    int16_t y = 0;
    for (int i = 0; i < sectIdx && i < SECTION_COUNT; ++i) {
        y += _calcSectionHeight(i);
    }
    return y;
}

// =============================================================================
// Group layout calculation
// =============================================================================
HomeScreen::GroupLayout HomeScreen::_calcGroupLayout(
    int16_t /*sectIdx*/, int16_t /*grpIdx*/, int16_t startY) const
{
    GroupLayout gl;
    gl.y     = startY;
    gl.ctrlY = startY + GRP_HDR_H;

    // Height = group header + tallest control + bottom padding
    // (caller iterates actual controls to find max height)
    gl.height = GRP_HDR_H + KNOB_CELL_H + GRP_PAD_BOTTOM;  // default to knob height
    return gl;
}

// =============================================================================
// Control flat-index helpers
// =============================================================================

int16_t HomeScreen::_controlCount(int16_t sectIdx) const {
    if (sectIdx < 0 || sectIdx >= SECTION_COUNT) return 0;
    return sectionControlCount(kSections[sectIdx]);
}

void HomeScreen::_controlToGroupCtrl(int16_t sectIdx, int16_t flatCtrl,
                                     int16_t& grpIdx, int16_t& ctrlInGrp) const
{
    grpIdx = 0;
    ctrlInGrp = 0;
    if (sectIdx < 0 || sectIdx >= SECTION_COUNT) return;

    const SectionDef& sec = kSections[sectIdx];
    int16_t idx = 0;
    for (int g = 0; g < sec.groupCount; ++g) {
        const GroupDef& grp = sec.groups[g];
        if (flatCtrl < idx + grp.controlCount) {
            grpIdx    = g;
            ctrlInGrp = flatCtrl - idx;
            return;
        }
        idx += grp.controlCount;
    }
}

const ControlDef& HomeScreen::_controlDef(int16_t sectIdx, int16_t flatCtrl) const {
    if (sectIdx < 0 || sectIdx >= SECTION_COUNT) return _emptyControl;
    const SectionDef& sec = kSections[sectIdx];
    int16_t idx = 0;
    for (int g = 0; g < sec.groupCount; ++g) {
        if (flatCtrl < idx + sec.groups[g].controlCount) {
            return sec.groups[g].controls[flatCtrl - idx];
        }
        idx += sec.groups[g].controlCount;
    }
    return _emptyControl;
}

// =============================================================================
// Scroll management
// =============================================================================

void HomeScreen::_clampScroll() {
    const int16_t totalH = _calcTotalHeight();
    const int16_t maxScroll = max((int16_t)0, (int16_t)(totalH - CONTENT_H));
    if (_scrollY < 0)         _scrollY = 0;
    if (_scrollY > maxScroll)  _scrollY = maxScroll;
}

void HomeScreen::_scrollToSection(int16_t sectIdx) {
    const int16_t sectY = _calcSectionY(sectIdx);
    // Position so section header is at top of viewport
    _scrollY = sectY;
    _clampScroll();
    _contentDirty = true;
}

void HomeScreen::_ensureVisible(int16_t virtualY, int16_t h) {
    // Ensure the rect (virtualY, h) is fully visible in the viewport
    if (virtualY < _scrollY) {
        _scrollY = virtualY;
    } else if (virtualY + h > _scrollY + CONTENT_H) {
        _scrollY = virtualY + h - CONTENT_H;
    }
    _clampScroll();
}

void HomeScreen::_scrollToCursor() {
    if (_cursor.sectionIdx < 0) return;

    const int16_t sectY = _calcSectionY(_cursor.sectionIdx);

    if (_cursor.controlIdx < 0) {
        // Cursor on section header
        _ensureVisible(sectY, SEC_HDR_H);
    } else {
        // Cursor on a control — need to calculate its position
        // For simplicity, ensure the entire expanded section is visible
        // or at least the header + some body
        _ensureVisible(sectY, min(_calcSectionHeight(_cursor.sectionIdx), CONTENT_H));
    }
    _contentDirty = true;
}

// =============================================================================
// Section expand/collapse
// =============================================================================

void HomeScreen::_toggleSection(int16_t sectIdx) {
    if (_expandedSection == sectIdx) {
        _collapseAll();
    } else {
        _expandSection(sectIdx);
    }
}

void HomeScreen::_expandSection(int16_t sectIdx) {
    _expandedSection = sectIdx;
    _cursor.sectionIdx = sectIdx;
    _cursor.controlIdx = -1;   // highlight section header
    _scrollToSection(sectIdx);
    _contentDirty = true;
}

void HomeScreen::_collapseAll() {
    _expandedSection = -1;
    _cursor.controlIdx = -1;
    _clampScroll();
    _contentDirty = true;
}

// =============================================================================
// draw() — phased redraw + incremental updates
// =============================================================================

void HomeScreen::draw() {
    if (!_display) return;

    // ---- Entry overlay takes priority ----
    if (_entry.isOpen()) {
        _entry.draw();
        _entryWasOpen = true;
        return;
    }

    // Entry just closed — the overlay painted over everything, force full redraw
    if (_entryWasOpen) {
        _entryWasOpen = false;
        _redrawStep   = 1;   // triggers phased fillScreen → header → content
        return;
    }

    // ---- Phased full-screen redraw ----
    if (_redrawStep > 0) {
        switch (_redrawStep) {
            case 1:
                _display->fillScreen(COLOUR_BACKGROUND);
                break;
            case 2:
                _drawHeader(true);
                break;
            case 3:
                _drawContent();
                _redrawStep = 0;
                _contentDirty = false;
                return;
        }
        _redrawStep++;
        return;  // yield for MIDI
    }

    // ---- Incremental path ----

    // Header: CPU% and voice dots, rate-limited
    const uint32_t now = millis();
    if (_headerDirty || (now - _lastHeaderMs) >= HEADER_REDRAW_MS) {
        _drawHeader(false);
        _lastHeaderMs = now;
        _headerDirty = false;
    }

    // Content: only when dirty (value change, scroll, expand/collapse)
    if (_contentDirty) {
        _drawContent();
        _contentDirty = false;
    }
}

// =============================================================================
// _drawHeader() — product name + voice dots + CPU%
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

    // ---- Voice activity dots (after product name) ----
    _drawVoiceDots();

    // ---- CPU% — right-aligned, only repaint when changed ----
    const int cpuNow = (int)AudioProcessorUsageMax();
    if (cpuNow != _lastCpuPct) {
        _lastCpuPct = cpuNow;
        const int16_t cpuX = SW - 58;
        _display->fillRect(cpuX, 2, 56, HEADER_H - 4, COLOUR_HEADER_BG);
        char buf[12];
        snprintf(buf, sizeof(buf), "CPU:%d%%", cpuNow);
        _display->setTextSize(1);
        const uint16_t cpuCol = (cpuNow > 80) ? COLOUR_METER_RED :
                                (cpuNow > 50) ? COLOUR_METER_YELLOW :
                                                 COLOUR_METER_GREEN;
        _display->setTextColor(cpuCol, COLOUR_HEADER_BG);
        _display->setCursor(cpuX, 7);
        _display->print(buf);
    }
}

void HomeScreen::_drawVoiceDots() {
    if (!_synth || !_display) return;

    // Voice dots after "JT.8000" text (approx x=52)
    const int16_t baseX = 54;
    const int16_t baseY = HEADER_H / 2;

    for (int v = 0; v < MAX_VOICES && v < 8; ++v) {
        const bool active = _synth->isVoiceActive(v);
        if (active == _prevVoiceActive[v]) continue;  // skip unchanged
        _prevVoiceActive[v] = active;

        const int16_t dotX = baseX + v * (VOICE_DOT_R * 2 + VOICE_DOT_GAP);
        const uint16_t col = active ? COLOUR_ACCENT_ORANGE : COLOUR_BORDER;

        // Clear + redraw dot
        _display->fillCircle(dotX, baseY, VOICE_DOT_R + 1, COLOUR_HEADER_BG);
        _display->fillCircle(dotX, baseY, VOICE_DOT_R, col);
    }
}

// =============================================================================
// _drawContent() — render visible sections in the virtual viewport
// =============================================================================

void HomeScreen::_drawContent() {
    if (!_display) return;

    // Clear only the content area — header is sacred
    _display->fillRect(0, CONTENT_Y, SW, CONTENT_H, COLOUR_BACKGROUND);

    // Content area bounds
    const int16_t clipTop = CONTENT_Y;
    const int16_t clipBot = CONTENT_Y + CONTENT_H;

    int16_t virtualY = 0;

    for (int s = 0; s < SECTION_COUNT; ++s) {
        const int16_t sectH = _calcSectionHeight(s);

        // Skip sections entirely above viewport
        if (virtualY + sectH <= _scrollY) {
            virtualY += sectH;
            continue;
        }

        // Stop if past bottom of viewport
        if (virtualY >= _scrollY + CONTENT_H) break;

        // Screen Y for this section (can be negative when partially scrolled above)
        const int16_t screenY = CONTENT_Y + (virtualY - _scrollY);

        const bool isHighlighted = (_cursor.sectionIdx == s && _cursor.controlIdx == -1);
        const bool isExpanded    = (_expandedSection == s);

        // Draw section header — only if it fits within content area (not over header bar)
        if (screenY >= clipTop && screenY + SEC_HDR_H <= clipBot) {
            SectionHeader::draw(*_display, 0, screenY, SW,
                                kSections[s].label, isExpanded, isHighlighted);
        }

        // Draw expanded body.
        // The body start may be above the viewport if the user scrolled down
        // within a tall section. _drawSectionBody clips individual elements
        // so nothing paints over the header bar.
        if (isExpanded) {
            const int16_t bodyScreenY = screenY + SEC_HDR_H;
            // Draw body if ANY part of it intersects the viewport.
            // Body extends from bodyScreenY to bodyScreenY + bodyHeight.
            const int16_t bodyH = _calcExpandedBodyHeight(s);
            if (bodyScreenY + bodyH > clipTop && bodyScreenY < clipBot) {
                _drawSectionBody(s, bodyScreenY);
            }
        }

        virtualY += sectH;
    }
}

// =============================================================================
// _drawSectionBody() — render groups + controls for an expanded section
// =============================================================================

void HomeScreen::_drawSectionBody(int16_t sectIdx, int16_t screenY) {
    if (sectIdx < 0 || sectIdx >= SECTION_COUNT) return;
    const SectionDef& sec = kSections[sectIdx];

    int16_t curY = screenY + BODY_PAD_Y;
    int16_t flatCtrl = 0;

    // Content area bounds for clipping
    const int16_t clipTop = CONTENT_Y;
    const int16_t clipBot = CONTENT_Y + CONTENT_H;

    for (int g = 0; g < sec.groupCount; ++g) {
        const GroupDef& grp = sec.groups[g];
        if (grp.controlCount == 0) continue;

        // Stop drawing if past viewport bottom
        if (curY >= clipBot) break;

        // Group header — only draw if fully within content area
        if (curY >= clipTop && curY + GRP_HDR_H <= clipBot) {
            GroupHeader::draw(*_display, BODY_PAD_X, curY,
                              SW - 2 * BODY_PAD_X, grp.label);
        }
        curY += GRP_HDR_H;

        // Calculate control row height (tallest control in group)
        int16_t rowH = 0;
        for (int c = 0; c < grp.controlCount; ++c) {
            switch (grp.controls[c].type) {
                case CtrlType::KNOB:   rowH = max(rowH, KNOB_CELL_H); break;
                case CtrlType::SELECT: rowH = max(rowH, SEL_CELL_H);  break;
                case CtrlType::TOGGLE: rowH = max(rowH, TOG_CELL_H);  break;
                case CtrlType::GRID:   rowH = max(rowH, (int16_t)(GRID_BAR_H + GRID_NUM_H + 4)); break;
                default: break;
            }
        }

        // Draw controls in a horizontal row — only if row top is within content area
        int16_t curX = BODY_PAD_X;
        for (int c = 0; c < grp.controlCount; ++c) {
            const ControlDef& ctrl = grp.controls[c];
            if (ctrl.type == CtrlType::NONE) continue;

            // Only draw if control row is fully within the clipped content area
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
// _drawControl() — draw a single control at the given screen position
// =============================================================================

void HomeScreen::_drawControl(int16_t sectIdx, int16_t flatCtrl,
                              int16_t screenX, int16_t screenY)
{
    const ControlDef& ctrl = _controlDef(sectIdx, flatCtrl);
    if (ctrl.cc == 255 || ctrl.type == CtrlType::NONE) return;
    if (!_synth || !_display) return;

    const uint8_t rawVal = _synth->getCC(ctrl.cc);
    const bool selected  = (_cursor.sectionIdx == sectIdx &&
                            _cursor.controlIdx == flatCtrl);

    switch (ctrl.type) {
        case CtrlType::KNOB: {
            // Format value text
            char valBuf[8];
            snprintf(valBuf, sizeof(valBuf), "%d", (int)rawVal);
            MiniKnob::draw(*_display, screenX, screenY,
                           rawVal, ctrl.label, valBuf, selected);
            break;
        }

        case CtrlType::SELECT: {
            const char* enumText = _enumTextForCC(ctrl.cc);
            const char* displayText = enumText ? enumText : "";
            // If no enum text, show raw value
            char fallback[8];
            if (!enumText || !enumText[0]) {
                snprintf(fallback, sizeof(fallback), "%d", (int)rawVal);
                displayText = fallback;
            }
            MiniSelect::draw(*_display, screenX, screenY,
                             ctrl.label, displayText, selected);
            break;
        }

        case CtrlType::TOGGLE: {
            const bool isOn = (rawVal > 63);
            MiniToggle::draw(*_display, screenX, screenY,
                             ctrl.label, isOn, selected);
            break;
        }

        case CtrlType::GRID: {
            // Step sequencer grid — read live data from StepSequencer
            const StepSequencer& seq = _synth->getSeq1();
            uint8_t stepVals[16];
            const uint8_t stepCount = (uint8_t)constrain(seq.getStepCount(), 1, 16);
            for (uint8_t i = 0; i < 16; ++i) {
                stepVals[i] = (i < stepCount) ? seq.getStepValue(i) : 64;
            }
            // Selected step = current SEQ_STEP_SELECT CC value
            const int8_t selStep = (int8_t)(_synth->getCC(CC::SEQ_STEP_SELECT) & 0x0F);
            // Playing step = sequencer's current position (-1 if not running)
            const int8_t playStep = (int8_t)seq.getCurrentStep();

            MiniGrid::draw(*_display, screenX, screenY,
                           SW - 2 * BODY_PAD_X, stepCount, stepVals,
                           selStep, playStep);
            break;
        }

        default: break;
    }
}

// =============================================================================
// Input: Encoder L (navigation)
// =============================================================================

void HomeScreen::onEncoderLeft(int delta) {
    if (!delta) return;

    // If entry overlay is open, route to it
    if (_entry.isOpen()) {
        _entry.onEncoderDelta(delta);
        return;
    }

    if (_expandedSection < 0) {
        // All collapsed: move between section headers
        _cursor.sectionIdx = (_cursor.sectionIdx + delta + SECTION_COUNT) % SECTION_COUNT;
        _cursor.controlIdx = -1;
        _scrollToCursor();
        _contentDirty = true;
    } else {
        // One section expanded: navigate header + controls
        const int16_t numCtrls = _controlCount(_expandedSection);
        // Total navigable items: 1 (this section header) + numCtrls + (SECTION_COUNT-1) other headers
        // Simplified: within expanded section, move between header and controls
        // Beyond the last control → next section header. Before first → prev section header.

        int16_t newCtrl = _cursor.controlIdx + delta;

        if (newCtrl < -1) {
            // Move to previous section header
            const int16_t prevSect = (_expandedSection - 1 + SECTION_COUNT) % SECTION_COUNT;
            _collapseAll();
            _cursor.sectionIdx = prevSect;
            _cursor.controlIdx = -1;
            _scrollToCursor();
        } else if (newCtrl >= numCtrls) {
            // Move to next section header
            const int16_t nextSect = (_expandedSection + 1) % SECTION_COUNT;
            _collapseAll();
            _cursor.sectionIdx = nextSect;
            _cursor.controlIdx = -1;
            _scrollToCursor();
        } else {
            // Stay within expanded section
            _cursor.controlIdx = newCtrl;
            _scrollToCursor();
        }
        _contentDirty = true;
    }
}

void HomeScreen::onEncoderLeftPress() {
    if (_entry.isOpen()) return;

    if (_cursor.controlIdx == -1) {
        // On a section header → toggle expand/collapse
        _toggleSection(_cursor.sectionIdx);
    } else {
        // On a control → open entry overlay
        _openEntry(_cursor.sectionIdx, _cursor.controlIdx);
    }
}

// =============================================================================
// Input: Encoder R (value editing)
// =============================================================================

void HomeScreen::onEncoderRight(int delta) {
    if (!delta) return;
    if (_entry.isOpen()) return;

    // Only adjust if cursor is on a control
    if (_cursor.controlIdx >= 0 && _expandedSection >= 0) {
        _adjustValue(_expandedSection, _cursor.controlIdx, delta);
        _contentDirty = true;
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
    // Entry overlay consumes all touches when open
    if (_entry.isOpen()) {
        _entry.onTouch(x, y);
        if (!_entry.isOpen()) {
            // Entry was closed (confirm or cancel) — refresh content
            _contentDirty = true;
        }
        return true;
    }

    // Header area — ignore
    if (y < CONTENT_Y) return false;

    // Map touch Y to virtual space
    const int16_t virtualTouchY = (y - CONTENT_Y) + _scrollY;

    // Walk sections to find what was tapped
    int16_t virtualY = 0;
    for (int s = 0; s < SECTION_COUNT; ++s) {
        const int16_t sectH = _calcSectionHeight(s);

        // Check if touch is in this section
        if (virtualTouchY >= virtualY && virtualTouchY < virtualY + sectH) {

            // Was the header tapped?
            if (virtualTouchY < virtualY + SEC_HDR_H) {
                _cursor.sectionIdx = s;
                _cursor.controlIdx = -1;
                _toggleSection(s);
                return true;
            }

            // Was a control in the expanded body tapped?
            if (s == _expandedSection) {
                // Walk groups/controls to find which one
                int16_t bodyY = virtualY + SEC_HDR_H + BODY_PAD_Y;
                int16_t flatCtrl = 0;
                const SectionDef& sec = kSections[s];

                for (int g = 0; g < sec.groupCount; ++g) {
                    const GroupDef& grp = sec.groups[g];
                    if (grp.controlCount == 0) continue;

                    bodyY += GRP_HDR_H;

                    // Row height
                    int16_t rowH = KNOB_CELL_H;  // default

                    // Check each control's hit box
                    int16_t ctrlX = BODY_PAD_X;
                    for (int c = 0; c < grp.controlCount; ++c) {
                        const ControlDef& ctrl = grp.controls[c];
                        if (ctrl.type == CtrlType::NONE) continue;

                        int16_t cellW = 0;
                        int16_t cellH = 0;
                        switch (ctrl.type) {
                            case CtrlType::KNOB:   cellW = KNOB_CELL_W; cellH = KNOB_CELL_H; break;
                            case CtrlType::SELECT: cellW = SEL_CELL_W;  cellH = SEL_CELL_H;  break;
                            case CtrlType::TOGGLE: cellW = TOG_CELL_W;  cellH = TOG_CELL_H;  break;
                            default: cellW = SW - 2 * BODY_PAD_X; cellH = GRID_BAR_H + GRID_NUM_H + 4; break;
                        }

                        // Hit test in virtual coords
                        if (x >= ctrlX && x < ctrlX + cellW &&
                            virtualTouchY >= bodyY && virtualTouchY < bodyY + cellH)
                        {
                            _cursor.sectionIdx = s;
                            _cursor.controlIdx = flatCtrl;

                            // Toggle controls: flip immediately
                            if (ctrl.type == CtrlType::TOGGLE && _synth) {
                                uint8_t cur = _synth->getCC(ctrl.cc);
                                _synth->setCC(ctrl.cc, (cur > 63) ? 0 : 127);
                            } else {
                                // Other controls: open entry
                                _openEntry(s, flatCtrl);
                            }
                            _contentDirty = true;
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
    _scrollY -= deltaY;  // swipe up = reveal below = increase scroll
    _clampScroll();
    _contentDirty = true;
}

// =============================================================================
// Value editing
// =============================================================================

void HomeScreen::_adjustValue(int16_t sectIdx, int16_t flatCtrl, int delta) {
    const ControlDef& ctrl = _controlDef(sectIdx, flatCtrl);
    if (ctrl.cc == 255 || !_synth) return;

    if (ctrl.type == CtrlType::TOGGLE) {
        // Toggle: any delta flips the state
        uint8_t cur = _synth->getCC(ctrl.cc);
        _synth->setCC(ctrl.cc, (cur > 63) ? 0 : 127);
    } else {
        // Continuous / enum: clamp to 0-127
        int16_t newVal = (int16_t)_synth->getCC(ctrl.cc) + delta;
        if (newVal < 0)   newVal = 0;
        if (newVal > 127) newVal = 127;
        _synth->setCC(ctrl.cc, (uint8_t)newVal);
    }
}

// =============================================================================
// Entry overlay — dispatcher
// =============================================================================

void HomeScreen::_openEntry(int16_t sectIdx, int16_t flatCtrl) {
    const ControlDef& ctrl = _controlDef(sectIdx, flatCtrl);
    if (ctrl.cc == 255 || !_synth) return;

    if (_isEnumCC(ctrl.cc)) {
        _openEnumEntry(ctrl.cc, ctrl.label);
    } else {
        _openNumericEntry(ctrl.cc, ctrl.label);
    }
}

// =============================================================================
// Enum entry — full label tables + CC↔index mapping
//
// Ported from SectionScreen::_openEnumEntry(). All enum label arrays, CC
// midpoint tables, and index→CC conversion logic are identical.
// =============================================================================

void HomeScreen::_openEnumEntry(uint8_t cc, const char* title) {
    if (!_synth) return;

    // ---- Static label tables (same as SectionScreen) ----

    static const char* kOnOff[]    = { "Off", "On" };
    static const char* kBypass[]   = { "Active", "Bypass" };
    static const char* kPolyMode[] = { "Poly", "Mono", "Unison" };
    static const char* kClkSrc[]   = { "Internal", "MIDI" };

    // Waveform names — from WaveForms.h (waveLongNames[])
    static constexpr int kWaveCount = (int)numWaveformsAll;
    // waveLongNames is a constexpr array in WaveForms.h
    const char* const* kWave = waveLongNames;

    static const char* kLFOwave[] = { "Sine", "Triangle", "Saw", "Square" };
    static const char* kLFOdest[] = { "Off", "Pitch", "Filter", "PWM" };

    // Sync / timing modes — 12 options: free + 11 note divisions
    static const char* kSync[] = {
        "Free", "1/1", "1/2", "1/4", "1/8", "1/16",
        "1/1T", "1/2T", "1/4T", "1/8T", "1/16T", "1/32"
    };

    // Mod FX: 12 items = Off + 11 presets
    static const char* kModFX[] = {
        "Off",
        "Chorus 1", "Chorus 2", "Chorus 3",
        "Flanger 1", "Flanger 2", "Flanger 3",
        "Phaser 1", "Phaser 2", "Phaser 3", "Phaser 4",
        "Super Chorus"
    };

    // Delay FX: 6 items = Off + 5 presets
    static const char* kDelayFX[] = {
        "Off",
        "Mono Short", "Mono Long",
        "Pan L>R", "Pan R>L", "Pan Stereo"
    };

    // Drive: 3 options
    static const char* kDrive[] = { "Bypass", "Soft Clip", "Hard Clip" };

    // Seq direction: 4 options
    static const char* kSeqDir[] = { "Forward", "Reverse", "Bounce", "Random" };

    // Seq destination: 5 options
    static const char* kSeqDest[] = { "None", "Pitch", "Filter", "PWM", "Amp" };

    // ---- Select label array + count based on CC ----

    const char* const* opts = kOnOff;
    int count   = 2;
    bool isOscWave = false;
    bool isFxMod   = false;
    bool isFxDelay = false;

    switch (cc) {
        case CC::OSC1_WAVE:
        case CC::OSC2_WAVE:
            opts = kWave; count = kWaveCount; isOscWave = true; break;
        case CC::LFO1_WAVEFORM:
        case CC::LFO2_WAVEFORM:         opts = kLFOwave;  count = 4;  break;
        case CC::LFO1_DESTINATION:
        case CC::LFO2_DESTINATION:      opts = kLFOdest;  count = 4;  break;
        case CC::LFO1_TIMING_MODE:
        case CC::LFO2_TIMING_MODE:
        case CC::DELAY_TIMING_MODE:
        case CC::SEQ_TIMING_MODE:       opts = kSync;     count = 12; break;
        case CC::BPM_CLOCK_SOURCE:      opts = kClkSrc;   count = 2;  break;
        case CC::FX_REVERB_BYPASS:      opts = kBypass;   count = 2;  break;
        case CC::GLIDE_ENABLE:
        case CC::SEQ_ENABLE:
        case CC::SEQ_RETRIGGER:         opts = kOnOff;    count = 2;  break;
        case CC::POLY_MODE:             opts = kPolyMode; count = 3;  break;
        case CC::FX_MOD_EFFECT:         opts = kModFX;    count = 12; isFxMod   = true; break;
        case CC::FX_JPFX_DELAY_EFFECT:  opts = kDelayFX;  count = 6;  isFxDelay = true; break;
        case CC::FX_DRIVE:              opts = kDrive;    count = 3;  break;
        case CC::SEQ_DIRECTION:         opts = kSeqDir;   count = 4;  break;
        case CC::SEQ_DESTINATION:       opts = kSeqDest;  count = 5;  break;
        case CC::FILTER_ENGINE: {
            static const char* k[] = { "OBXa", "VA Bank" };
            opts = k; count = CC::FILTER_ENGINE_COUNT;
        } break;
        case CC::FILTER_MODE: {
            static const char* k[] = {
                "4-Pole LP", "2-Pole LP", "2-Pole BP",
                "2-Pole Push", "Xpander", "Xpander+M"
            };
            opts = k; count = CC::FILTER_MODE_COUNT;
        } break;
        case CC::VA_FILTER_TYPE: {
            opts = kVAFilterNames; count = (int)FILTER_COUNT;
        } break;
        case CC::FILTER_OBXA_XPANDER_MODE: {
            static const char* k[] = {
                "LP4","LP3","LP2","LP1","HP3","HP2","HP1",
                "BP4","BP2","N2","PH3","HP2+LP1","HP3+LP1","N2+LP1","PH3+LP1"
            };
            opts = k; count = 15;
        } break;
        default: opts = kOnOff; count = 2; break;
    }

    // ---- Map current CC value to list index ----

    const uint8_t rawVal = _synth->getCC(cc);
    int curIdx;

    if (isOscWave) {
        WaveformType wt = waveformFromCC(rawVal);
        curIdx = 0;
        for (int i = 0; i < (int)numWaveformsAll; ++i) {
            if (waveformListAll[i] == wt) { curIdx = i; break; }
        }
    } else if (cc == CC::POLY_MODE) {
        curIdx = (rawVal <= 42) ? 0 : (rawVal <= 84) ? 1 : 2;
    } else if (isFxMod) {
        if (rawVal == 0) { curIdx = 0; }
        else { curIdx = constrain(((int)(rawVal - 1) * 11) / 127 + 1, 1, 11); }
    } else if (isFxDelay) {
        if (rawVal == 0) { curIdx = 0; }
        else { curIdx = constrain(((int)(rawVal - 1) * 5) / 127 + 1, 1, 5); }
    } else if (cc == CC::FX_DRIVE) {
        curIdx = (rawVal < 16) ? 0 : (rawVal < 64) ? 1 : 2;
    } else if (count == 2) {
        curIdx = (rawVal != 0) ? 1 : 0;
    } else {
        curIdx = (int)rawVal * count / 128;
    }

    _pendingCC    = cc;
    _pendingCount = count;

    // ---- Open the list picker with a callback that converts index → CC ----

    _entry.openEnum(title, opts, count, constrain(curIdx, 0, count - 1),
        [](int idx) {
            if (!_instance || !_instance->_synth) return;
            HomeScreen* s = _instance;
            uint8_t ccVal;

            if (s->_pendingCC == CC::OSC1_WAVE || s->_pendingCC == CC::OSC2_WAVE) {
                ccVal = (idx >= 0 && idx < (int)numWaveformsAll)
                        ? ccFromWaveform(waveformListAll[idx]) : 0;

            } else if (s->_pendingCC == CC::POLY_MODE) {
                static const uint8_t kMid[] = { 21, 63, 106 };
                ccVal = (idx >= 0 && idx < 3) ? kMid[idx] : 0;

            } else if (s->_pendingCC == CC::FX_MOD_EFFECT) {
                static const uint8_t kCC[] = { 0, 6, 17, 28, 40, 51, 62, 74, 85, 96, 108, 127 };
                ccVal = (idx >= 0 && idx < 12) ? kCC[idx] : 0;

            } else if (s->_pendingCC == CC::FX_JPFX_DELAY_EFFECT) {
                static const uint8_t kCC[] = { 0, 13, 38, 64, 89, 114 };
                ccVal = (idx >= 0 && idx < 6) ? kCC[idx] : 0;

            } else if (s->_pendingCC == CC::FX_DRIVE) {
                static const uint8_t kCC[] = { 0, 32, 96 };
                ccVal = (idx >= 0 && idx < 3) ? kCC[idx] : 0;

            } else if (s->_pendingCC == CC::FILTER_OBXA_XPANDER_MODE) {
                static const uint8_t kCC[] = {
                    4, 13, 21, 30, 38, 47, 55, 64, 72, 81, 89, 98, 106, 115, 123
                };
                ccVal = (idx >= 0 && idx < 15) ? kCC[idx] : 0;

            } else {
                // Generic: distribute evenly across 128 CC values
                ccVal = (uint8_t)((idx * 128 + (128 / s->_pendingCount) / 2) / s->_pendingCount);
            }

            s->_synth->setCC(s->_pendingCC, ccVal);
            s->_contentDirty = true;
        }
    );
}

// =============================================================================
// Numeric entry — human units ↔ CC conversion
//
// Ported from SectionScreen::_openNumericEntry(). Unit ranges and both
// CC→human and human→CC conversions are identical.
// =============================================================================

void HomeScreen::_openNumericEntry(uint8_t cc, const char* title) {
    if (!_synth) return;
    using namespace JT8000Map;

    const char* unit = "";
    int minV = 0, maxV = 127, curV = (int)_synth->getCC(cc);

    // ---- Map CC to human-readable range ----
    switch (cc) {
        case CC::FILTER_CUTOFF:
            unit = "Hz";  minV = 20;   maxV = 18000;
            curV = (int)_synth->getFilterCutoff();
            break;
        case CC::AMP_ATTACK:    case CC::AMP_DECAY:    case CC::AMP_RELEASE:
        case CC::FILTER_ENV_ATTACK: case CC::FILTER_ENV_DECAY: case CC::FILTER_ENV_RELEASE:
        case CC::PITCH_ENV_ATTACK:  case CC::PITCH_ENV_DECAY:  case CC::PITCH_ENV_RELEASE:
            unit = "ms";  minV = 1;    maxV = 11880;
            curV = (int)cc_to_time_ms(_synth->getCC(cc));
            break;
        case CC::AMP_SUSTAIN:   case CC::FILTER_ENV_SUSTAIN:  case CC::PITCH_ENV_SUSTAIN:
            unit = "%";   minV = 0;    maxV = 100;
            curV = (int)(_synth->getCC(cc) * 100 / 127);
            break;
        case CC::LFO1_FREQ:     case CC::LFO2_FREQ:
            unit = "Hz";  minV = 0;    maxV = 39;
            curV = (int)cc_to_lfo_hz(_synth->getCC(cc));
            break;
        case CC::FX_BASS_GAIN:  case CC::FX_TREBLE_GAIN:
            unit = "dB";  minV = -12;  maxV = 12;
            curV = (int)((_synth->getCC(cc) / 127.0f) * 24.0f - 12.0f);
            break;
        case CC::BPM_INTERNAL_TEMPO:
            unit = "BPM"; minV = 20;   maxV = 300;
            curV = (int)(20.0f + (_synth->getCC(cc) / 127.0f) * 280.0f);
            break;
        case CC::OSC1_PITCH_OFFSET: case CC::OSC2_PITCH_OFFSET:
            unit = "st";  minV = -24;  maxV = 24;
            curV = (int)(_synth->getCC(cc) * 48 / 127 - 24);
            break;
        case CC::OSC1_FINE_TUNE:    case CC::OSC2_FINE_TUNE:
            unit = "ct";  minV = -100; maxV = 100;
            curV = (int)(_synth->getCC(cc) * 200 / 127 - 100);
            break;
        case CC::PITCH_ENV_DEPTH:
            unit = "st";  minV = -12;  maxV = 12;
            curV = (int)((_synth->getCC(cc) - 64) * 12 / 63);
            break;
        case CC::PITCH_BEND_RANGE:
            unit = "st";  minV = 0;    maxV = 24;
            curV = (int)(_synth->getCC(cc) * 24 / 127);
            break;
        case CC::FX_DRIVE:
            unit = "%";   minV = 0;    maxV = 100;
            curV = (int)(_synth->getCC(cc) * 100 / 127);
            break;
        default: break;
    }

    _pendingCC = cc;

    // ---- Open numeric keypad with human→CC callback ----

    _entry.openNumeric(title, unit, minV, maxV, curV,
        [](int humanVal) {
            if (!_instance || !_instance->_synth) return;
            HomeScreen* s  = _instance;
            const uint8_t tcc = s->_pendingCC;
            uint8_t ccVal = 0;
            using namespace JT8000Map;

            switch (tcc) {
                case CC::FILTER_CUTOFF:
                    ccVal = obxa_cutoff_hz_to_cc((float)humanVal);            break;
                case CC::AMP_ATTACK:    case CC::AMP_DECAY:    case CC::AMP_RELEASE:
                case CC::FILTER_ENV_ATTACK: case CC::FILTER_ENV_DECAY: case CC::FILTER_ENV_RELEASE:
                case CC::PITCH_ENV_ATTACK:  case CC::PITCH_ENV_DECAY:  case CC::PITCH_ENV_RELEASE:
                    ccVal = time_ms_to_cc((float)humanVal);                   break;
                case CC::AMP_SUSTAIN:   case CC::FILTER_ENV_SUSTAIN:  case CC::PITCH_ENV_SUSTAIN:
                    ccVal = (uint8_t)(humanVal * 127 / 100);                  break;
                case CC::LFO1_FREQ:     case CC::LFO2_FREQ:
                    ccVal = lfo_hz_to_cc((float)humanVal);                    break;
                case CC::FX_BASS_GAIN:  case CC::FX_TREBLE_GAIN:
                    ccVal = (uint8_t)((humanVal + 12) * 127 / 24);           break;
                case CC::BPM_INTERNAL_TEMPO:
                    ccVal = (uint8_t)((humanVal - 20) * 127 / 280);          break;
                case CC::OSC1_PITCH_OFFSET: case CC::OSC2_PITCH_OFFSET:
                    ccVal = (uint8_t)((humanVal + 24) * 127 / 48);           break;
                case CC::OSC1_FINE_TUNE:    case CC::OSC2_FINE_TUNE:
                    ccVal = (uint8_t)((humanVal + 100) * 127 / 200);         break;
                case CC::PITCH_ENV_DEPTH:
                    ccVal = (uint8_t)constrain(64 + humanVal * 63 / 12, 0, 127); break;
                case CC::PITCH_BEND_RANGE:
                    ccVal = (uint8_t)(humanVal * 127 / 24);                   break;
                case CC::FX_DRIVE:
                    ccVal = (uint8_t)(humanVal * 127 / 100);                  break;
                default:
                    ccVal = (uint8_t)constrain(humanVal, 0, 127);             break;
            }

            s->_synth->setCC(tcc, ccVal);
            s->_contentDirty = true;
        }
    );
}

// =============================================================================
// Enum text queries — delegates to SynthEngine (for display in mini widgets)
// =============================================================================

const char* HomeScreen::_enumTextForCC(uint8_t cc) const {
    if (!_synth) return nullptr;
    switch (cc) {
        case CC::OSC1_WAVE:              return _synth->getOsc1WaveformName();
        case CC::OSC2_WAVE:              return _synth->getOsc2WaveformName();
        case CC::LFO1_WAVEFORM:          return _synth->getLFO1WaveformName();
        case CC::LFO2_WAVEFORM:          return _synth->getLFO2WaveformName();
        case CC::LFO1_DESTINATION:       return _synth->getLFO1DestinationName();
        case CC::LFO2_DESTINATION:       return _synth->getLFO2DestinationName();
        case CC::GLIDE_ENABLE:
        case CC::SEQ_ENABLE:
        case CC::SEQ_RETRIGGER:          return (_synth->getCC(cc) > 63) ? "On" : "Off";
        case CC::FX_REVERB_BYPASS:       return _synth->getFXReverbBypass() ? "Bypass" : "Active";
        case CC::FILTER_ENGINE:
            return (_synth->getFilterEngine() == CC::FILTER_ENGINE_VA) ? "VA Bank" : "OBXa";
        case CC::FILTER_MODE: {
            static const char* k[] = {
                "4-Pole LP","2-Pole LP","2-Pole BP","2-Pole Push","Xpander","Xpander+M"
            };
            const uint8_t m = _synth->getFilterMode();
            return (m < CC::FILTER_MODE_COUNT) ? k[m] : "?";
        }
        case CC::VA_FILTER_TYPE: {
            const uint8_t vt = _synth->getVAFilterType();
            return (vt < (uint8_t)FILTER_COUNT) ? kVAFilterNames[vt] : "?";
        }
        case CC::FILTER_OBXA_XPANDER_MODE: {
            static const char* k[] = {
                "LP4","LP3","LP2","LP1","HP3","HP2","HP1",
                "BP4","BP2","N2","PH3","HP2+LP1","HP3+LP1","N2+LP1","PH3+LP1"
            };
            const uint8_t m = _synth->getFilterXpanderMode();
            return (m < 15) ? k[m] : "?";
        }
        case CC::FX_MOD_EFFECT:          return _synth->getFXModEffectName();
        case CC::FX_JPFX_DELAY_EFFECT:   return _synth->getFXDelayEffectName();
        case CC::POLY_MODE: {
            const uint8_t v = _synth->getCC(CC::POLY_MODE);
            return (v <= 42) ? "Poly" : (v <= 84) ? "Mono" : "Unison";
        }
        case CC::FX_DRIVE: {
            const uint8_t v = _synth->getCC(CC::FX_DRIVE);
            return (v < 16) ? "Bypass" : (v < 64) ? "Soft Clip" : "Hard Clip";
        }
        case CC::SEQ_DIRECTION: {
            static const char* k[] = { "Forward","Reverse","Bounce","Random" };
            const uint8_t v = _synth->getCC(cc);
            return k[constrain(v * 4 / 128, 0, 3)];
        }
        case CC::SEQ_DESTINATION: {
            static const char* k[] = { "None","Pitch","Filter","PWM","Amp" };
            const uint8_t v = _synth->getCC(cc);
            return k[constrain(v * 5 / 128, 0, 4)];
        }
        case CC::LFO1_TIMING_MODE:
        case CC::LFO2_TIMING_MODE:
        case CC::DELAY_TIMING_MODE:
        case CC::SEQ_TIMING_MODE: {
            static const char* k[] = {
                "Free","1/1","1/2","1/4","1/8","1/16",
                "1/1T","1/2T","1/4T","1/8T","1/16T","1/32"
            };
            const uint8_t v = _synth->getCC(cc);
            return k[constrain(v * 12 / 128, 0, 11)];
        }
        case CC::BPM_CLOCK_SOURCE:
            return (_synth->getCC(cc) > 63) ? "MIDI" : "Internal";
        default: return nullptr;
    }
}

/*static*/ bool HomeScreen::_isEnumCC(uint8_t cc) {
    return (cc == CC::OSC1_WAVE            || cc == CC::OSC2_WAVE             ||
            cc == CC::LFO1_WAVEFORM        || cc == CC::LFO2_WAVEFORM         ||
            cc == CC::LFO1_DESTINATION     || cc == CC::LFO2_DESTINATION      ||
            cc == CC::LFO1_TIMING_MODE     || cc == CC::LFO2_TIMING_MODE      ||
            cc == CC::DELAY_TIMING_MODE    || cc == CC::BPM_CLOCK_SOURCE      ||
            cc == CC::GLIDE_ENABLE         || cc == CC::FX_REVERB_BYPASS      ||
            cc == CC::POLY_MODE            ||
            cc == CC::FX_MOD_EFFECT        || cc == CC::FX_JPFX_DELAY_EFFECT  ||
            cc == CC::FILTER_ENGINE        || cc == CC::FILTER_MODE           ||
            cc == CC::VA_FILTER_TYPE       || cc == CC::FILTER_OBXA_XPANDER_MODE ||
            cc == CC::FX_DRIVE             ||
            cc == CC::SEQ_ENABLE           || cc == CC::SEQ_RETRIGGER         ||
            cc == CC::SEQ_DIRECTION        || cc == CC::SEQ_DESTINATION       ||
            cc == CC::SEQ_TIMING_MODE);
}

/*static*/ bool HomeScreen::_isToggleCC(uint8_t cc) {
    return (cc == CC::GLIDE_ENABLE     || cc == CC::FX_REVERB_BYPASS ||
            cc == CC::SEQ_ENABLE       || cc == CC::SEQ_RETRIGGER);
}