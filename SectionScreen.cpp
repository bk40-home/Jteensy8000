// SectionScreen.cpp
// =============================================================================
// Implementation of SectionScreen — see SectionScreen.h for design notes.
// =============================================================================

#include "SectionScreen.h"
#include "WaveForms.h"   // waveformListAll, waveLongNames, waveformFromCC, ccFromWaveform

SectionScreen* SectionScreen::_ctx = nullptr;

// =============================================================================
// Constructor — pre-build all widget pools at fixed geometry.
// CC/name/colour are assigned later in open() → _rebuildWidgets().
// =============================================================================
SectionScreen::SectionScreen()
    : _display(nullptr)
    , _section(nullptr)
    , _synth(nullptr)
    , _activePage(0)
    , _selectedWidget(0)
    , _activeLayout(UIPage::LayoutType::LAYOUT_ROWS4)
    , _onBack(nullptr)
    , _redrawStep(1)
    , _needsTabRedraw(false)
    , _pendingCC(255)
    , _pendingCount(2)
    // Row pool — stacked vertically in the params area
    , _row0(0, PARAMS_Y + 0*ROW_H, SW, ROW_H, 255, "---", COLOUR_GLOBAL)
    , _row1(0, PARAMS_Y + 1*ROW_H, SW, ROW_H, 255, "---", COLOUR_GLOBAL)
    , _row2(0, PARAMS_Y + 2*ROW_H, SW, ROW_H, 255, "---", COLOUR_GLOBAL)
    , _row3(0, PARAMS_Y + 3*ROW_H, SW, ROW_H, 255, "---", COLOUR_GLOBAL)
    // Knob pool — 4 columns across the params area
    , _knob0(0*KNOB_COL_W, PARAMS_Y, KNOB_COL_W, PARAMS_H, 255, "---", COLOUR_GLOBAL)
    , _knob1(1*KNOB_COL_W, PARAMS_Y, KNOB_COL_W, PARAMS_H, 255, "---", COLOUR_GLOBAL)
    , _knob2(2*KNOB_COL_W, PARAMS_Y, KNOB_COL_W, PARAMS_H, 255, "---", COLOUR_GLOBAL)
    , _knob3(3*KNOB_COL_W, PARAMS_Y, KNOB_COL_W, PARAMS_H, 255, "---", COLOUR_GLOBAL)
    // Slider pool — stacked vertically (same geometry as rows)
    , _slider0(0, PARAMS_Y + 0*ROW_H, SW, ROW_H, 255, "---", COLOUR_GLOBAL)
    , _slider1(0, PARAMS_Y + 1*ROW_H, SW, ROW_H, 255, "---", COLOUR_GLOBAL)
    , _slider2(0, PARAMS_Y + 2*ROW_H, SW, ROW_H, 255, "---", COLOUR_GLOBAL)
    , _slider3(0, PARAMS_Y + 3*ROW_H, SW, ROW_H, 255, "---", COLOUR_GLOBAL)
{
    _rows[0]    = &_row0;    _rows[1]    = &_row1;
    _rows[2]    = &_row2;    _rows[3]    = &_row3;
    _knobs[0]   = &_knob0;  _knobs[1]   = &_knob1;
    _knobs[2]   = &_knob2;  _knobs[3]   = &_knob3;
    _sliders[0] = &_slider0; _sliders[1] = &_slider1;
    _sliders[2] = &_slider2; _sliders[3] = &_slider3;
}

// =============================================================================
// begin()
// =============================================================================
void SectionScreen::begin(ILI9341_t3n* display) {
    _display = display;
    _entry.setDisplay(display);

    _ctx = this;

    // Wire display and tap callbacks for all three pools.
    // Callbacks use static _ctx so they work as plain C function pointers.
    for (int i = 0; i < 4; ++i) {
        _rows[i]->setDisplay(display);
        _knobs[i]->setDisplay(display);
        _sliders[i]->setDisplay(display);
    }

    _row0.setCallback([](uint8_t cc){ if (_ctx) _ctx->_onWidgetTap(cc); });
    _row1.setCallback([](uint8_t cc){ if (_ctx) _ctx->_onWidgetTap(cc); });
    _row2.setCallback([](uint8_t cc){ if (_ctx) _ctx->_onWidgetTap(cc); });
    _row3.setCallback([](uint8_t cc){ if (_ctx) _ctx->_onWidgetTap(cc); });

    _knob0.setCallback([](uint8_t cc){ if (_ctx) _ctx->_onWidgetTap(cc); });
    _knob1.setCallback([](uint8_t cc){ if (_ctx) _ctx->_onWidgetTap(cc); });
    _knob2.setCallback([](uint8_t cc){ if (_ctx) _ctx->_onWidgetTap(cc); });
    _knob3.setCallback([](uint8_t cc){ if (_ctx) _ctx->_onWidgetTap(cc); });

    _slider0.setCallback([](uint8_t cc){ if (_ctx) _ctx->_onWidgetTap(cc); });
    _slider1.setCallback([](uint8_t cc){ if (_ctx) _ctx->_onWidgetTap(cc); });
    _slider2.setCallback([](uint8_t cc){ if (_ctx) _ctx->_onWidgetTap(cc); });
    _slider3.setCallback([](uint8_t cc){ if (_ctx) _ctx->_onWidgetTap(cc); });
}

void SectionScreen::setBackCallback(BackCallback cb) { _onBack = cb; }

// =============================================================================
// open()
// =============================================================================
void SectionScreen::open(const SectionDef& section, SynthEngine& synth) {
    _section        = &section;
    _synth          = &synth;
    _activePage     = 0;
    _selectedWidget = 0;
    _entry.close();
    _rebuildWidgets();
    _redrawStep          = 1;
    _needsTabRedraw      = false;
    _lastSectionCpuPct   = -1;   // force CPU% repaint on first draw after open
}

// =============================================================================
// _rebuildWidgets()
// Assigns CC/name/colour to the active widget pool for the current page.
// Selects the layout type from UIPage::layoutMap.
// =============================================================================
void SectionScreen::_rebuildWidgets() {
    if (!_section) return;

    const int pageIdx = _section->pages[_activePage];
    if (pageIdx >= UIPage::NUM_PAGES) return;

    _activeLayout = UIPage::layoutMap[pageIdx];

    for (int i = 0; i < 4; ++i) {
        const uint8_t cc     = UIPage::ccMap[pageIdx][i];
        const char*   name   = UIPage::ccNames[pageIdx][i];
        const uint16_t col   = _ccColour(cc);

        _rows[i]->configure(cc, name, col);
        _knobs[i]->configure(cc, name, col);
        _sliders[i]->configure(cc, name, col);
    }

    // Restore selection highlight on the correct pool
    for (int i = 0; i < 4; ++i) {
        const bool sel = (i == _selectedWidget);
        _rows[i]->setSelected(sel);
        _knobs[i]->setSelected(sel);
        _sliders[i]->setSelected(sel);
    }

    syncFromEngine();
}

// =============================================================================
// syncFromEngine()
// =============================================================================
void SectionScreen::syncFromEngine() {
    if (!_section || !_synth) return;
    const int pageIdx = _section->pages[_activePage];
    if (pageIdx >= UIPage::NUM_PAGES) return;

    for (int i = 0; i < 4; ++i) {
        const uint8_t cc = UIPage::ccMap[pageIdx][i];
        if (cc == 255) {
            _rows[i]->setValue(0, nullptr);
            _knobs[i]->setValue(0, nullptr);
            _sliders[i]->setValue(0, nullptr);
            continue;
        }
        const uint8_t rawCC  = _synth->getCC(cc);
        const char*   eText  = _enumText(cc);
        _rows[i]->setValue(rawCC, eText);
        _knobs[i]->setValue(rawCC, eText);
        _sliders[i]->setValue(rawCC, eText);
    }
}

// =============================================================================
// draw()
//
// MIDI-safe redraw strategy:
//   A full screen repaint takes 10-30 ms.  If done in one call it blocks
//   loop(), starving myusb.Task() and midi1.read() for that duration —
//   enough to drop notes during a page change.
//
//   Solution: split the full repaint across 5 consecutive draw() calls using
//   _redrawStep.  Each call does one step and returns, so MIDI reads happen
//   between every step (~1 ms each).  Total latency = 5 × frame interval
//   (5 × 33 ms = ~165 ms visible build-up) which is unnoticeable to the user
//   but keeps the MIDI gap under 2 ms per step.
//
//   Step 0 = idle
//   Step 1 = fillScreen (clears previous content)
//   Step 2 = _drawHeader
//   Step 3 = _drawTabs
//   Step 4 = _markActiveWidgetsDirty + _drawActiveWidgets
//   Step 5 = _drawFooter → step back to 0
// =============================================================================
void SectionScreen::draw() {
    if (!_display || !_section) return;

    // Entry overlay takes over the whole screen — draw it and return
    if (_entry.isOpen()) {
        _entry.draw();
        return;
    }

    // ---- Phased full-screen redraw ----
    if (_redrawStep > 0) {
        switch (_redrawStep) {
            case 1:
                _display->fillScreen(COLOUR_BACKGROUND);
                break;
            case 2:
                _drawHeader();
                break;
            case 3:
                _drawTabs();
                break;
            case 4:
                _markActiveWidgetsDirty();
                _drawActiveWidgets();
                break;
            case 5:
                _drawFooter();
                _redrawStep = 0;   // done — fall through to idle path next call
                _needsTabRedraw = false;
                return;
        }
        _redrawStep++;
        return;   // yield to loop() — MIDI reads run before next step
    }

    // ---- Idle path: incremental updates only ----
    if (_needsTabRedraw) {
        _drawTabs();
        _needsTabRedraw = false;
    }

    // Rate-limited CPU% update in header
    _updateHeaderCpu();

    // Widgets: only redraws rows/knobs that are dirty (value changed)
    _drawActiveWidgets();
}

// =============================================================================
// _drawActiveWidgets() / _markActiveWidgetsDirty()
// Route draw calls to whichever pool is active.
// =============================================================================
void SectionScreen::_drawActiveWidgets() {
    switch (_activeLayout) {
        case UIPage::LayoutType::LAYOUT_ROWS4:
            for (int i = 0; i < 4; ++i) _rows[i]->draw();
            break;
        case UIPage::LayoutType::LAYOUT_KNOBS4:
            for (int i = 0; i < 4; ++i) _knobs[i]->draw();
            break;
        case UIPage::LayoutType::LAYOUT_SLIDERS4:
            for (int i = 0; i < 4; ++i) _sliders[i]->draw();
            break;
    }
}

void SectionScreen::_markActiveWidgetsDirty() {
    for (int i = 0; i < 4; ++i) {
        _rows[i]->markDirty();
        _knobs[i]->markDirty();
        _sliders[i]->markDirty();
    }
}

// =============================================================================
// _applySelected() / _getSelected()
// =============================================================================
void SectionScreen::_applySelected(int i, bool sel) {
    _rows[i]->setSelected(sel);
    _knobs[i]->setSelected(sel);
    _sliders[i]->setSelected(sel);
}

bool SectionScreen::_getSelected(int i) const {
    switch (_activeLayout) {
        case UIPage::LayoutType::LAYOUT_ROWS4:    return _rows[i]->isSelected();
        case UIPage::LayoutType::LAYOUT_KNOBS4:   return _knobs[i]->isSelected();
        case UIPage::LayoutType::LAYOUT_SLIDERS4: return _sliders[i]->isSelected();
    }
    return false;
}

void SectionScreen::_setSelectedWidget(int i) {
    const int prev = _selectedWidget;
    _selectedWidget = i;
    _applySelected(prev, false);
    _applySelected(i,    true);
}

// =============================================================================
// Touch routing
// =============================================================================
bool SectionScreen::onTouch(int16_t x, int16_t y) {
    if (_entry.isOpen()) {
        _entry.onTouch(x, y);
        if (!_entry.isOpen()) _redrawStep = 1;
        return true;
    }

    // Header back arrow
    if (y < HEADER_H && x < 28) {
        if (_onBack) _onBack();
        return true;
    }

    // Tabs
    if (y >= HEADER_H && y < HEADER_H + TABS_H) {
        _onTabTouch(x);
        return true;
    }

    // Widget area — route to active pool
    if (y >= PARAMS_Y && y < PARAMS_Y + PARAMS_H) {
        switch (_activeLayout) {
            case UIPage::LayoutType::LAYOUT_ROWS4:
                for (int i = 0; i < 4; ++i) {
                    if (_rows[i]->onTouch(x, y)) {
                        _setSelectedWidget(i);
                        return true;
                    }
                }
                break;
            case UIPage::LayoutType::LAYOUT_KNOBS4:
                for (int i = 0; i < 4; ++i) {
                    if (_knobs[i]->onTouch(x, y)) {
                        _setSelectedWidget(i);
                        return true;
                    }
                }
                break;
            case UIPage::LayoutType::LAYOUT_SLIDERS4:
                for (int i = 0; i < 4; ++i) {
                    if (_sliders[i]->onTouch(x, y)) {
                        _setSelectedWidget(i);
                        // For sliders: if touch is on the track, set value directly
                        const int16_t tv = _sliders[i]->trackTouchValue(x);
                        if (tv >= 0 && _synth) {
                            const uint8_t cc = _sliders[i]->getCC();
                            if (cc != 255) {
                                _synth->setCC(cc, (uint8_t)tv);
                                syncFromEngine();
                                return true;
                            }
                        }
                        return true;
                    }
                }
                break;
        }
    }
    return false;
}

// =============================================================================
// Encoder input
// =============================================================================
void SectionScreen::onEncoderLeft(int delta) {
    // When the entry overlay is open (e.g. waveform enum selector),
    // left encoder scrolls the list — mirroring onEncoderRight behaviour.
    if (_entry.isOpen()) {
        _entry.onEncoderDelta(delta);
        return;
    }
    // Otherwise scroll between the four parameter widgets on this section page.
    int next = _selectedWidget + delta;
    next = constrain(next, 0, 3);
    _setSelectedWidget(next);
}

void SectionScreen::onEncoderRight(int delta) {
    if (_entry.isOpen()) {
        _entry.onEncoderDelta(delta);
        return;
    }
    if (!_synth) return;
    const uint8_t cc = _widgetCC(_selectedWidget);
    if (cc == 255) return;
    const int newVal = constrain((int)_synth->getCC(cc) + delta, 0, 127);
    _synth->setCC(cc, (uint8_t)newVal);
    syncFromEngine();
}

void SectionScreen::onBackPress() {
    if (_entry.isOpen()) { _entry.close(); _redrawStep = 1; return; }
    if (_onBack) _onBack();
}

void SectionScreen::onEditPress() {
    if (_entry.isOpen()) return;
    const uint8_t cc = _widgetCC(_selectedWidget);
    if (cc != 255) _openEntry(cc);
}

void SectionScreen::onSwipeAdjust(int16_t /*x*/, int16_t /*y*/, int steps) {
    if (!_synth || _entry.isOpen()) return;
    const uint8_t cc = _widgetCC(_selectedWidget);
    if (cc == 255) return;
    const int newVal = constrain((int)_synth->getCC(cc) + steps, 0, 127);
    _synth->setCC(cc, (uint8_t)newVal);
    syncFromEngine();
}

bool SectionScreen::isEntryOpen() const { return _entry.isOpen(); }

// =============================================================================
// Private: header / tabs / footer draw
// =============================================================================
void SectionScreen::_drawHeader() {
    if (!_display || !_section) return;
    _display->fillRect(0, 0, SW, HEADER_H, gTheme.headerBg);

    // Back arrow
    _display->setTextSize(1);
    _display->setTextColor(gTheme.textNormal, gTheme.headerBg);
    _display->setCursor(4, 8);
    _display->print("<");

    // Section label (centred)
    const int16_t labelW = (int16_t)(strlen(_section->label) * 12);
    _display->setTextSize(2);
    _display->setTextColor(_section->colour, gTheme.headerBg);
    _display->setCursor((SW - labelW) / 2, 4);
    _display->print(_section->label);

    // CPU% (right side) -- drawn unconditionally on full header repaint.
    // Subsequent per-frame updates use _drawHeaderCpuOnly() which is rate-limited.
    {
        char cpuBuf[8];
        const int cpuNow = (int)AudioProcessorUsage();
        snprintf(cpuBuf, sizeof(cpuBuf), "%u%%", (unsigned)cpuNow);
        _display->setTextSize(1);
        _display->setTextColor(gTheme.textDim, gTheme.headerBg);
        const int16_t cpuW = (int16_t)(strlen(cpuBuf) * 6);
        _display->setCursor(SW - cpuW - 4, 8);
        _display->print(cpuBuf);
        _lastSectionCpuPct = cpuNow;  // sync cache
        _lastSectionHeaderMs = millis();
    }
}

// =============================================================================
// _updateHeaderCpu()
// Called every draw() frame to keep the CPU% display current WITHOUT doing a
// full header repaint.  Only writes to the display when the integer value
// changes, keeping SPI traffic to near-zero during steady operation.
// =============================================================================
void SectionScreen::_updateHeaderCpu() {
    if (!_display) return;
    const uint32_t now = millis();
    if ((now - _lastSectionHeaderMs) < HEADER_REDRAW_MS) return;
    _lastSectionHeaderMs = now;

    const int cpuNow = (int)AudioProcessorUsage();
    if (cpuNow == _lastSectionCpuPct) return;  // nothing changed — skip SPI
    _lastSectionCpuPct = cpuNow;

    // Erase old CPU% text region and redraw with new value.
    // Region is right-side of header: same position as _drawHeader() uses.
    char cpuBuf[8];
    snprintf(cpuBuf, sizeof(cpuBuf), "%u%%", (unsigned)cpuNow);
    const int16_t cpuW = (int16_t)(strlen(cpuBuf) * 6);
    const int16_t cpuX = SW - cpuW - 4;
    // Clear a fixed-width region large enough for "100%" (4 chars = 24 px)
    _display->fillRect(SW - 30, 2, 28, HEADER_H - 4, gTheme.headerBg);
    _display->setTextSize(1);
    _display->setTextColor(gTheme.textDim, gTheme.headerBg);
    _display->setCursor(cpuX, 8);
    _display->print(cpuBuf);
}

void SectionScreen::_drawTabs() {
    if (!_display || !_section) return;
    _display->fillRect(0, HEADER_H, SW, TABS_H, gTheme.bg);

    const int n    = _section->pageCount;
    if (n == 0) return;
    const int tabW = SW / n;

    for (int i = 0; i < n; ++i) {
        const int16_t tx  = (int16_t)(i * tabW);
        const bool active = (i == _activePage);
        const uint16_t bg = active ? _section->colour : gTheme.headerBg;
        const uint16_t fg = active ? COLOUR_BACKGROUND : gTheme.textDim;

        _display->fillRect(tx, HEADER_H, tabW - 1, TABS_H, bg);

        const int pageIdx = _section->pages[i];
        const char* title = (pageIdx < UIPage::NUM_PAGES)
                            ? UIPage::pageTitle[pageIdx] : "?";
        _display->setTextSize(1);
        _display->setTextColor(fg, bg);
        // Truncate label to fit tab width
        char tbuf[10];
        strncpy(tbuf, title, sizeof(tbuf) - 1);
        tbuf[sizeof(tbuf) - 1] = '\0';
        const int16_t tw = (int16_t)(strlen(tbuf) * 6);
        _display->setCursor(tx + (tabW - 1 - tw) / 2, HEADER_H + 5);
        _display->print(tbuf);
    }
}

void SectionScreen::_drawFooter() {
    if (!_display) return;
    _display->fillRect(0, SH - FOOTER_H, SW, FOOTER_H, gTheme.headerBg);
    _display->setTextSize(1);
    _display->setTextColor(gTheme.textDim, gTheme.headerBg);
    _display->setCursor(4, SH - FOOTER_H + 8);
    _display->print("L:scroll  R:nudge  R-hold:edit  L-press:back");
}

// =============================================================================
// Private: tab touch
// =============================================================================
void SectionScreen::_onTabTouch(int16_t x) {
    if (!_section || _section->pageCount == 0) return;
    const int tabW = SW / _section->pageCount;
    const int tapped = x / tabW;
    if (tapped < 0 || tapped >= _section->pageCount) return;
    if (tapped == _activePage) return;

    _activePage = tapped;
    _rebuildWidgets();
    _redrawStep     = 1;
    _needsTabRedraw = false;
}

// =============================================================================
// Private: widget tap → entry overlay
// =============================================================================
void SectionScreen::_onWidgetTap(uint8_t cc) {
    if (cc == 255 || !_synth) return;
    _openEntry(cc);
}

void SectionScreen::_openEntry(uint8_t cc) {
    if (!_synth) return;
    const char* name = _ccName(cc);
    if (_isEnumCC(cc)) _openEnumEntry(cc, name);
    else               _openNumericEntry(cc, name);
}

void SectionScreen::_openEnumEntry(uint8_t cc, const char* title) {
    static const char* const* kWave   = waveLongNames;
    static const int kWaveCount       = (int)numWaveformsAll;
    static const char* kLFOwave[] = { "Sine","Sawtooth","Square","Triangle" };
    static const char* kLFOdest[] = { "Pitch","Filter","Shape","Amp" };
    static const char* kSync[]    = { "Free","4 bar","2 bar","1 bar",
                                      "1/2","1/4","1/8","1/16",
                                      "1/4T","1/8T","1/16T","1/32T" };
    static const char* kClkSrc[]  = { "Internal","External" };
    static const char* kOnOff[]   = { "Off","On" };
    static const char* kBypass[]  = { "Active","Bypass" };
    static const char* kPolyMode[]= { "Poly","Mono","Unison" };

    // Modulation FX: 12 items = Off + 11 presets.
    // CC midpoints match CCDispatch.h: cc=0 → Off; cc=1..127 → (cc-1)*11/127 → 0..10
    static const char* kModFX[] = {
        "Off",
        "Chorus 1", "Chorus 2", "Chorus 3",
        "Flanger 1", "Flanger 2", "Deep Flanger",
        "Phaser 1", "Phaser 2", "Phaser 3", "Phaser 4",
        "Super Chorus"
    };

    // Delay FX: 6 items = Off + 5 presets.
    // CC midpoints match CCDispatch.h: cc=0 → Off; cc=1..127 → (cc-1)*5/127 → 0..4
    static const char* kDelayFX[] = {
        "Off",
        "Mono Short", "Mono Long",
        "Pan L>R", "Pan R>L", "Pan Stereo"
    };

    const char* const* opts = kOnOff;
    int                count = 2;
    bool isOscWave  = false;
    bool isFxMod    = false;
    bool isFxDelay  = false;

    switch (cc) {
        case CC::OSC1_WAVE:
        case CC::OSC2_WAVE:
            opts = kWave; count = kWaveCount; isOscWave = true; break;
        case CC::LFO1_WAVEFORM:
        case CC::LFO2_WAVEFORM:    opts = kLFOwave; count = 4;  break;
        case CC::LFO1_DESTINATION:
        case CC::LFO2_DESTINATION: opts = kLFOdest; count = 4;  break;
        case CC::LFO1_TIMING_MODE:
        case CC::LFO2_TIMING_MODE:
        case CC::DELAY_TIMING_MODE: opts = kSync;   count = 12; break;
        case CC::BPM_CLOCK_SOURCE: opts = kClkSrc;  count = 2;  break;
        case CC::FX_REVERB_BYPASS: opts = kBypass;  count = 2;  break;
        case CC::POLY_MODE:        opts = kPolyMode; count = 3;  break;
        // FX effect preset lists — use fixed-midpoint CC tables (not generic formula)
        case CC::FX_MOD_EFFECT:    opts = kModFX;   count = 12; isFxMod   = true; break;
        case CC::FX_JPFX_DELAY_EFFECT:  opts = kDelayFX; count = 6;  isFxDelay = true; break;
        // Filter engine: OBXa or VA bank
        case CC::FILTER_ENGINE: {
            static const char* kEngineNames[CC::FILTER_ENGINE_COUNT] = {
                "OBXa", "VA Bank"
            };
            opts = kEngineNames; count = CC::FILTER_ENGINE_COUNT;
        } break;
        // Filter topology selector — 6 named modes (OBXa engine)
        case CC::FILTER_MODE: {
            static const char* kFltMode[CC::FILTER_MODE_COUNT] = {
                "4-Pole LP",    // FILTER_MODE_4POLE
                "2-Pole LP",    // FILTER_MODE_2POLE
                "2-Pole BP",    // FILTER_MODE_2POLE_BP
                "2-Pole Push",  // FILTER_MODE_2POLE_PUSH
                "Xpander",      // FILTER_MODE_XPANDER
                "Xpander+M"     // FILTER_MODE_XPANDER_M
            };
            opts = kFltMode; count = CC::FILTER_MODE_COUNT;
        } break;
        // VA bank topology — 13 named topologies from kVAFilterNames[]
        case CC::VA_FILTER_TYPE: {
            // kVAFilterNames is defined in AudioFilterVABank.h
            opts = kVAFilterNames; count = (int)FILTER_COUNT;
        } break;
        // Xpander 15-mode topology selector (only active when FilterMode == XPANDER_M)
        case CC::FILTER_OBXA_XPANDER_MODE: {
            static const char* kXpMode[15] = {
                "LP4", "LP3", "LP2", "LP1",
                "HP3", "HP2", "HP1",
                "BP4", "BP2",
                "N2",  "PH3",
                "HP2+LP1", "HP3+LP1", "N2+LP1", "PH3+LP1"
            };
            opts = kXpMode; count = 15;
        } break;
        default:                   opts = kOnOff;   count = 2;  break;
    }

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
        // Map raw CC → list index by finding the nearest midpoint bucket.
        // cc=0 is always Off (index 0).
        if (rawVal == 0) {
            curIdx = 0;
        } else {
            // CCDispatch.h formula: variation = (cc-1)*11/127
            const int var = ((int)(rawVal - 1) * 11) / 127;
            curIdx = constrain(var + 1, 1, 11);  // +1 because index 0 = Off
        }
    } else if (isFxDelay) {
        // Map raw CC → list index.  cc=0 is Off (index 0).
        if (rawVal == 0) {
            curIdx = 0;
        } else {
            // CCDispatch.h formula: variation = (cc-1)*5/127
            const int var = ((int)(rawVal - 1) * 5) / 127;
            curIdx = constrain(var + 1, 1, 5);   // +1 because index 0 = Off
        }
    } else if (count == 2) {
        curIdx = (rawVal != 0) ? 1 : 0;
    } else {
        curIdx = (int)rawVal * count / 128;
    }

    _pendingCC    = cc;
    _pendingCount = count;

    _entry.openEnum(title, opts, count, constrain(curIdx, 0, count - 1),
        [](int idx) {
            if (!_ctx || !_ctx->_synth) return;
            SectionScreen* s = _ctx;
            uint8_t ccVal;

            if (s->_pendingCC == CC::OSC1_WAVE || s->_pendingCC == CC::OSC2_WAVE) {
                // Waveform: look up the CC midpoint for the selected waveform
                ccVal = (idx >= 0 && idx < (int)numWaveformsAll)
                        ? ccFromWaveform(waveformListAll[idx]) : 0;

            } else if (s->_pendingCC == CC::POLY_MODE) {
                static const uint8_t kPolyMidpoints[] = { 21, 63, 106 };
                ccVal = (idx >= 0 && idx < 3) ? kPolyMidpoints[idx] : 0;

            } else if (s->_pendingCC == CC::FX_MOD_EFFECT) {
                // Use the exact CC midpoint table so the value always lands
                // in the correct CCDispatch.h bucket regardless of rounding.
                static const uint8_t kModFXcc[] = { 0, 6, 17, 28, 40, 51, 62, 74, 85, 96, 108, 127 };
                ccVal = (idx >= 0 && idx < 12) ? kModFXcc[idx] : 0;

            } else if (s->_pendingCC == CC::FX_JPFX_DELAY_EFFECT) {
                // Same approach for delay FX bucket midpoints
                static const uint8_t kDelayFXcc[] = { 0, 13, 38, 64, 89, 114 };
                ccVal = (idx >= 0 && idx < 6) ? kDelayFXcc[idx] : 0;

            } else if (s->_pendingCC == CC::FILTER_OBXA_XPANDER_MODE) {
                // 15 modes: CC 0-127 divided into 15 equal buckets (value * 15 / 128).
                // Store the midpoint of each bucket so decode is unambiguous.
                // Midpoint of bucket i = (i * 128 + 64) / 15 — but pre-computed for clarity.
                static const uint8_t kXpModeCC[15] = {
                    4,  13,  21,  30,  38,  47,  55,
                   64,  72,  81,  89,  98, 106, 115, 123
                };
                ccVal = (idx >= 0 && idx < 15) ? kXpModeCC[idx] : 0;

            } else {
                // Generic: distribute evenly across 128 CC values
                ccVal = (uint8_t)
                    ((idx * 128 + (128 / s->_pendingCount) / 2) / s->_pendingCount);
            }

            s->_synth->setCC(s->_pendingCC, ccVal);
            s->syncFromEngine();
        }
    );
}

// =============================================================================
// Private: numeric entry overlay
// =============================================================================
void SectionScreen::_openNumericEntry(uint8_t cc, const char* title) {
    using namespace JT8000Map;

    const char* unit = "";
    int minV = 0, maxV = 127, curV = (int)_synth->getCC(cc);

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

    _entry.openNumeric(title, unit, minV, maxV, curV,
        [](int humanVal) {
            if (!_ctx || !_ctx->_synth) return;
            SectionScreen* s   = _ctx;
            const uint8_t  tcc = s->_pendingCC;
            uint8_t        ccVal = 0;
            using namespace JT8000Map;

            switch (tcc) {
                case CC::FILTER_CUTOFF:
                    ccVal = obxa_cutoff_hz_to_cc((float)humanVal);           break;
                case CC::AMP_ATTACK:    case CC::AMP_DECAY:    case CC::AMP_RELEASE:
                case CC::FILTER_ENV_ATTACK: case CC::FILTER_ENV_DECAY: case CC::FILTER_ENV_RELEASE:
                case CC::PITCH_ENV_ATTACK:  case CC::PITCH_ENV_DECAY:  case CC::PITCH_ENV_RELEASE:
                    ccVal = time_ms_to_cc((float)humanVal);                  break;
                case CC::AMP_SUSTAIN:   case CC::FILTER_ENV_SUSTAIN:  case CC::PITCH_ENV_SUSTAIN:
                    ccVal = (uint8_t)(humanVal * 127 / 100);                 break;
                case CC::LFO1_FREQ:     case CC::LFO2_FREQ:
                    ccVal = lfo_hz_to_cc((float)humanVal);                   break;
                case CC::FX_BASS_GAIN:  case CC::FX_TREBLE_GAIN:
                    ccVal = (uint8_t)((humanVal + 12) * 127 / 24);          break;
                case CC::BPM_INTERNAL_TEMPO:
                    ccVal = (uint8_t)((humanVal - 20) * 127 / 280);         break;
                case CC::OSC1_PITCH_OFFSET: case CC::OSC2_PITCH_OFFSET:
                    ccVal = (uint8_t)((humanVal + 24) * 127 / 48);          break;
                case CC::OSC1_FINE_TUNE:    case CC::OSC2_FINE_TUNE:
                    ccVal = (uint8_t)((humanVal + 100) * 127 / 200);        break;
                case CC::PITCH_ENV_DEPTH:
                    ccVal = (uint8_t)constrain(64 + humanVal * 63 / 12, 0, 127); break;
                case CC::PITCH_BEND_RANGE:
                    ccVal = (uint8_t)(humanVal * 127 / 24);                  break;
                case CC::FX_DRIVE:
                    ccVal = (uint8_t)(humanVal * 127 / 100);                 break;
                default:
                    ccVal = (uint8_t)constrain(humanVal, 0, 127);            break;
            }
            s->_synth->setCC(tcc, ccVal);
            s->syncFromEngine();
        }
    );
}

// =============================================================================
// Private: helpers
// =============================================================================
uint8_t SectionScreen::_widgetCC(int i) const {
    if (!_section) return 255;
    const int pageIdx = _section->pages[_activePage];
    if (pageIdx >= UIPage::NUM_PAGES) return 255;
    return UIPage::ccMap[pageIdx][i];
}

const char* SectionScreen::_enumText(uint8_t cc) const {
    if (!_synth) return nullptr;
    switch (cc) {
        case CC::OSC1_WAVE:          return _synth->getOsc1WaveformName();
        case CC::OSC2_WAVE:          return _synth->getOsc2WaveformName();
        case CC::LFO1_WAVEFORM:      return _synth->getLFO1WaveformName();
        case CC::LFO2_WAVEFORM:      return _synth->getLFO2WaveformName();
        case CC::LFO1_DESTINATION:   return _synth->getLFO1DestinationName();
        case CC::LFO2_DESTINATION:   return _synth->getLFO2DestinationName();
        case CC::GLIDE_ENABLE:       return _synth->getGlideEnabled() ? "On" : "Off";
        case CC::POLY_MODE: {
            const uint8_t v = _synth->getCC(CC::POLY_MODE);
            return (v <= 42) ? "Poly" : (v <= 84) ? "Mono" : "Unison";
        }
        case CC::FX_REVERB_BYPASS:   return _synth->getFXReverbBypass() ? "Bypass" : "Active";
        // Filter engine selector — OBXa or VA bank
        case CC::FILTER_ENGINE: {
            return (_synth->getFilterEngine() == CC::FILTER_ENGINE_VA) ? "VA Bank" : "OBXa";
        }
        // Single filter topology CC — returns the human-readable mode name
        case CC::FILTER_MODE: {
            static const char* kFltModeNames[CC::FILTER_MODE_COUNT] = {
                "4-Pole LP", "2-Pole LP", "2-Pole BP",
                "2-Pole Push", "Xpander", "Xpander+M"
            };
            const uint8_t m = _synth->getFilterMode();
            return (m < CC::FILTER_MODE_COUNT) ? kFltModeNames[m] : "?";
        }
        // VA bank topology name — shows the active ZDF filter type
        case CC::VA_FILTER_TYPE: {
            const uint8_t vt = _synth->getVAFilterType();
            return (vt < (uint8_t)FILTER_COUNT) ? kVAFilterNames[vt] : "?";
        }
        case CC::FILTER_OBXA_XPANDER_MODE: {
            // 15 OBXa Xpander pole-mix topologies — matches poleMixFactors[] order in AudioFilterOBXa_OBXf.cpp
            static const char* kXpanderModeNames[15] = {
                "LP4", "LP3", "LP2", "LP1",
                "HP3", "HP2", "HP1",
                "BP4", "BP2",
                "N2",  "PH3",
                "HP2+LP1", "HP3+LP1", "N2+LP1", "PH3+LP1"
            };
            const uint8_t m = _synth->getFilterXpanderMode();
            return (m < 15) ? kXpanderModeNames[m] : "?";
        }
        // FX effect preset names — delegates to FXChainBlock name tables
        case CC::FX_MOD_EFFECT:      return _synth->getFXModEffectName();   // "Off" or "Chorus 1" … "Super Chorus"
        case CC::FX_JPFX_DELAY_EFFECT:    return _synth->getFXDelayEffectName(); // "Off" or "Mono Short" … "Pan Stereo"
        default:                     return nullptr;
    }
}

const char* SectionScreen::_ccName(uint8_t cc) const {
    if (!_section) return "?";
    const int pageIdx = _section->pages[_activePage];
    if (pageIdx >= UIPage::NUM_PAGES) return "?";
    for (int r = 0; r < 4; ++r) {
        if (UIPage::ccMap[pageIdx][r] == cc) return UIPage::ccNames[pageIdx][r];
    }
    return "?";
}

/*static*/ bool SectionScreen::_isEnumCC(uint8_t cc) {
    return (cc == CC::OSC1_WAVE             || cc == CC::OSC2_WAVE              ||
            cc == CC::LFO1_WAVEFORM         || cc == CC::LFO2_WAVEFORM          ||
            cc == CC::LFO1_DESTINATION      || cc == CC::LFO2_DESTINATION       ||
            cc == CC::LFO1_TIMING_MODE      || cc == CC::LFO2_TIMING_MODE       ||
            cc == CC::DELAY_TIMING_MODE     || cc == CC::BPM_CLOCK_SOURCE       ||
            cc == CC::GLIDE_ENABLE          || cc == CC::FX_REVERB_BYPASS       ||
            cc == CC::POLY_MODE             ||
            cc == CC::FX_MOD_EFFECT         || cc == CC::FX_JPFX_DELAY_EFFECT        ||  // FX effect preset lists
            // Filter engine / topology selectors
            cc == CC::FILTER_ENGINE         ||
            cc == CC::FILTER_MODE           ||
            cc == CC::VA_FILTER_TYPE        ||
            cc == CC::FILTER_OBXA_XPANDER_MODE);
}

/*static*/ uint16_t SectionScreen::_ccColour(uint8_t cc) {
    if (cc == 255)                                                                  return COLOUR_GLOBAL;
    if (cc >= CC::OSC1_WAVE       && cc <= CC::OSC2_FEEDBACK_MIX)                  return COLOUR_OSC;
    if (cc >= CC::FILTER_CUTOFF   && cc <= CC::FILTER_OBXA_RES_MOD_DEPTH)          return COLOUR_FILTER;
    if (cc >= CC::AMP_ATTACK      && cc <= CC::FILTER_ENV_RELEASE)                 return COLOUR_ENV;
    if (cc >= CC::LFO1_FREQ       && cc <= CC::LFO2_TIMING_MODE)                   return COLOUR_LFO;
    if (cc >= CC::FX_BASS_GAIN    && cc <= CC::FX_REVERB_BYPASS)                   return COLOUR_FX;
    return COLOUR_GLOBAL;
}
