// SectionScreen.cpp
// =============================================================================
// Implementation of SectionScreen — see SectionScreen.h for design notes.
// =============================================================================

#include "SectionScreen.h"
#include "WaveForms.h"  // waveformListAll, waveLongNames, waveformFromCC, ccFromWaveform

// Static context pointer — one SectionScreen active at a time
SectionScreen* SectionScreen::_ctx = nullptr;

// =============================================================================
// Constructor — pre-build four rows at fixed Y positions.
// CC / name / colour are assigned later in open() → _rebuildRows().
// =============================================================================
SectionScreen::SectionScreen()
    : _display(nullptr)
    , _section(nullptr)
    , _synth(nullptr)
    , _activePage(0)
    , _selectedRow(0)
    , _onBack(nullptr)
    , _needsFullRedraw(true)
    , _needsTabRedraw(false)
    , _pendingCC(255)
    , _pendingCount(2)
    , _row0(0, PARAMS_Y + 0*ROW_H, SW, ROW_H, 255, "---", COLOUR_GLOBAL)
    , _row1(0, PARAMS_Y + 1*ROW_H, SW, ROW_H, 255, "---", COLOUR_GLOBAL)
    , _row2(0, PARAMS_Y + 2*ROW_H, SW, ROW_H, 255, "---", COLOUR_GLOBAL)
    , _row3(0, PARAMS_Y + 3*ROW_H, SW, ROW_H, 255, "---", COLOUR_GLOBAL)
{
    _rows[0] = &_row0;
    _rows[1] = &_row1;
    _rows[2] = &_row2;
    _rows[3] = &_row3;
}

// =============================================================================
// begin()
// =============================================================================
void SectionScreen::begin(ILI9341_t3n* display) {
    _display = display;
    _entry.setDisplay(display);
    for (int i = 0; i < 4; ++i) _rows[i]->setDisplay(display);

    // Set static context before wiring callbacks
    _ctx = this;

    // Wire row tap callbacks — C function pointers via static _ctx
    _row0.setCallback([](uint8_t cc){ if (_ctx) _ctx->_onRowTap(cc); });
    _row1.setCallback([](uint8_t cc){ if (_ctx) _ctx->_onRowTap(cc); });
    _row2.setCallback([](uint8_t cc){ if (_ctx) _ctx->_onRowTap(cc); });
    _row3.setCallback([](uint8_t cc){ if (_ctx) _ctx->_onRowTap(cc); });
}

void SectionScreen::setBackCallback(BackCallback cb) { _onBack = cb; }

// =============================================================================
// open()
// =============================================================================
void SectionScreen::open(const SectionDef& section, SynthEngine& synth) {
    _section     = &section;
    _synth       = &synth;
    _activePage  = 0;
    _selectedRow = 0;
    _entry.close();         // discard any stale entry overlay
    _rebuildRows();         // assign CC / name / colour for page 0
    _needsFullRedraw = true;
    _needsTabRedraw  = false;
}

// =============================================================================
// draw()
// =============================================================================
void SectionScreen::draw() {
    if (!_display || !_section) return;

    // Entry overlay gets the whole screen while open
    if (_entry.isOpen()) {
        _entry.draw();
        return;
    }

    if (_needsFullRedraw) {
        _display->fillScreen(COLOUR_BACKGROUND);
        _drawHeader();
        _drawTabs();
        for (int i = 0; i < 4; ++i) {
            _rows[i]->markDirty();
            _rows[i]->draw();
        }
        _drawFooter();
        _needsFullRedraw = false;
        _needsTabRedraw  = false;
        return;
    }

    if (_needsTabRedraw) {
        _drawTabs();
        _needsTabRedraw = false;
    }

    // Rows repaint themselves when their dirty flag is set
    for (int i = 0; i < 4; ++i) _rows[i]->draw();
}

// =============================================================================
// syncFromEngine()
// =============================================================================
void SectionScreen::syncFromEngine() {
    if (!_section || !_synth) return;
    const int pageIdx = _section->pages[_activePage];
    if (pageIdx >= UIPage::NUM_PAGES) return;

    for (int r = 0; r < 4; ++r) {
        const uint8_t cc = UIPage::ccMap[pageIdx][r];
        if (cc == 255) {
            _rows[r]->setValue(0, nullptr);
            continue;
        }
        const uint8_t rawCC = _synth->getCC(cc);
        _rows[r]->setValue(rawCC, _enumText(cc));
    }
}

// =============================================================================
// Touch routing
// =============================================================================
bool SectionScreen::onTouch(int16_t x, int16_t y) {
    // Entry overlay consumes all touches while open
    if (_entry.isOpen()) {
        _entry.onTouch(x, y);
        if (!_entry.isOpen()) {
            _needsFullRedraw = true;  // entry closed — repaint screen
        }
        return true;
    }

    // Header back-arrow (leftmost 20 px of header row)
    if (y < HEADER_H && x < 20) {
        if (_onBack) _onBack();
        return true;
    }

    // Tab bar
    if (y >= HEADER_H && y < HEADER_H + TABS_H) {
        _onTabTouch(x);
        return true;
    }

    // Parameter rows — also update selection to tapped row
    for (int i = 0; i < 4; ++i) {
        if (_rows[i]->onTouch(x, y)) {
            _setSelectedRow(i);
            return true;
        }
    }
    return false;
}

// =============================================================================
// Encoder input
// =============================================================================
void SectionScreen::onEncoderLeft(int delta) {
    if (delta == 0) return;
    // When entry is open the left encoder scrolls the enum list (if in enum mode).
    // For numeric entry we have no digit-selection action yet — just absorb.
    if (_entry.isOpen()) {
        _entry.onEncoderDelta(delta);
        return;
    }
    _setSelectedRow((_selectedRow + delta + 4) % 4);
}

void SectionScreen::onEncoderRight(int delta) {
    if (_entry.isOpen() || delta == 0 || !_synth || !_section) return;
    const int     pageIdx = _section->pages[_activePage];
    if (pageIdx >= UIPage::NUM_PAGES) return;
    const uint8_t cc      = UIPage::ccMap[pageIdx][_selectedRow];
    if (cc == 255) return;
    const int     newVal  = constrain((int)_synth->getCC(cc) + delta, 0, 127);
    _synth->setCC(cc, (uint8_t)newVal);
    syncFromEngine();
}

void SectionScreen::onBackPress() {
    if (_entry.isOpen()) {
        _entry.close();
        _needsFullRedraw = true;
        return;
    }
    if (_onBack) _onBack();
}

void SectionScreen::onEditPress() {
    if (_entry.isOpen() || !_synth || !_section) return;
    const int     pageIdx = _section->pages[_activePage];
    if (pageIdx >= UIPage::NUM_PAGES) return;
    const uint8_t cc      = UIPage::ccMap[pageIdx][_selectedRow];
    if (cc == 255) return;
    _openEntry(cc);
}

bool SectionScreen::isEntryOpen() const { return _entry.isOpen(); }

// =============================================================================
// onSwipeAdjust()  — touch swipe up/down adjusts the CC at the swiped row.
//
// The touch point (x, y) is used to hit-test which of the 4 visible rows was
// swiped, so the user does not need to first select a row before adjusting.
// If the swipe lands on an empty slot (cc == 255) it is silently ignored.
//
// steps = +1 (swipe up → increment) or -1 (swipe down → decrement).
// =============================================================================
void SectionScreen::onSwipeAdjust(int16_t x, int16_t y, int steps) {
    if (_entry.isOpen() || !_synth || !_section) return;

    // Hit-test which row was swiped
    int hitRow = -1;
    for (int i = 0; i < 4; ++i) {
        if (_rows[i] && _rows[i]->hitTest(x, y)) {
            hitRow = i;
            break;
        }
    }
    if (hitRow < 0) return;

    const int     pageIdx = _section->pages[_activePage];
    if (pageIdx >= UIPage::NUM_PAGES) return;
    const uint8_t cc      = UIPage::ccMap[pageIdx][hitRow];
    if (cc == 255) return;

    // Clamp new value to MIDI range
    const int newVal = constrain((int)_synth->getCC(cc) + steps, 0, 127);
    _synth->setCC(cc, (uint8_t)newVal);

    // Select the adjusted row so right-encoder can fine-tune from same row
    _setSelectedRow(hitRow);
    syncFromEngine();
}

// =============================================================================
// Private: header
// =============================================================================
void SectionScreen::_drawHeader() {
    if (!_section) return;
    _display->fillRect(0, 0, SW, HEADER_H, COLOUR_HEADER_BG);
    _display->drawFastHLine(0, HEADER_H - 1, SW, _section->colour);

    // Back arrow — 2-arg: spaces render over header background
    _display->setTextSize(1);
    _display->setTextColor(COLOUR_TEXT_DIM, COLOUR_HEADER_BG);
    _display->setCursor(4, 8);
    _display->print("<");

    // Section name — 2-arg: spaces render over header background
    _display->setTextColor(_section->colour, COLOUR_HEADER_BG);
    _display->setCursor(14, 8);
    _display->print(_section->label);

    // CPU% right-aligned
    char buf[14];
    snprintf(buf, sizeof(buf), "CPU:%d%%", (int)AudioProcessorUsageMax());
    const int16_t bw = (int16_t)(strlen(buf) * 6);
    _display->setTextColor(COLOUR_TEXT_DIM, COLOUR_HEADER_BG);
    _display->setCursor(SW - bw - 4, 8);
    _display->print(buf);
}

// =============================================================================
// Private: page tabs
// =============================================================================
void SectionScreen::_drawTabs() {
    if (!_section) return;
    _display->fillRect(0, HEADER_H, SW, TABS_H, COLOUR_BACKGROUND);

    const int tabCount = _section->pageCount;
    if (tabCount == 0) return;
    const int16_t tabW = SW / tabCount;

    for (int t = 0; t < tabCount; ++t) {
        const int pageIdx = _section->pages[t];
        if (pageIdx >= UIPage::NUM_PAGES) continue;

        const int16_t  tx     = t * tabW;
        const bool     active = (t == _activePage);
        const uint16_t bg     = active ? _section->colour : COLOUR_HEADER_BG;

        _display->fillRect(tx, HEADER_H, tabW - 1, TABS_H, bg);

        // First word of page title (≤7 chars, stops at first space)
        const char* title = UIPage::pageTitle[pageIdx];
        char shortTitle[8];
        int  c = 0;
        while (title[c] && title[c] != ' ' && c < 7) {
            shortTitle[c] = title[c]; c++;
        }
        shortTitle[c] = '\0';

        _display->setTextSize(1);
        // 2-arg: spaces render correctly over active/inactive tab bg
        _display->setTextColor(active ? COLOUR_BACKGROUND : COLOUR_TEXT_DIM, bg);
        const int16_t tw = (int16_t)(strlen(shortTitle) * 6);
        _display->setCursor(tx + (tabW - 1 - tw) / 2, HEADER_H + 5);
        _display->print(shortTitle);
    }

    _display->drawFastHLine(0, HEADER_H + TABS_H - 1, SW, _section->colour);
}

// =============================================================================
// Private: footer
// =============================================================================
void SectionScreen::_drawFooter() {
    const int16_t fy = SH - FOOTER_H;
    _display->fillRect(0, fy, SW, FOOTER_H, COLOUR_HEADER_BG);
    _display->drawFastHLine(0, fy, SW, COLOUR_BORDER);
    _display->setTextSize(1);
    _display->setTextColor(COLOUR_TEXT_DIM, COLOUR_HEADER_BG);
    _display->setCursor(4, fy + 9);
    _display->print("L:<Back  R:Adjust  Hold-R:Edit");
}

// =============================================================================
// Private: row rebuild
// =============================================================================
void SectionScreen::_rebuildRows() {
    if (!_section) return;
    const int pageIdx = _section->pages[_activePage];
    if (pageIdx >= UIPage::NUM_PAGES) return;

    for (int r = 0; r < 4; ++r) {
        const uint8_t  cc     = UIPage::ccMap[pageIdx][r];
        const char*    name   = UIPage::ccNames[pageIdx][r];
        const uint16_t colour = _ccColour(cc);
        _configRow(*_rows[r], cc, name ? name : "---", colour);
    }

    for (int i = 0; i < 4; ++i) _rows[i]->setSelected(i == _selectedRow);
    syncFromEngine();
}

/*static*/ void SectionScreen::_configRow(TFTParamRow& row, uint8_t cc,
                                          const char* name, uint16_t colour) {
    row.configure(cc, name, colour);
}

// =============================================================================
// Private: tab touch
// =============================================================================
void SectionScreen::_onTabTouch(int16_t x) {
    if (!_section) return;
    const int tabCount = _section->pageCount;
    const int tabW     = SW / tabCount;
    const int tapped   = x / tabW;
    if (tapped < 0 || tapped >= tabCount) return;
    if (tapped == _activePage) return;

    _activePage      = tapped;
    _needsTabRedraw  = true;
    _rebuildRows();
    _needsFullRedraw = true;   // rows changed — full repaint needed
}

// =============================================================================
// Private: row selection
// =============================================================================
void SectionScreen::_setSelectedRow(int row) {
    if (row == _selectedRow) return;
    const int prev = _selectedRow;
    _selectedRow   = row;
    _rows[prev]->setSelected(false);
    _rows[row]->setSelected(true);
}

// =============================================================================
// Private: entry overlay — route from row tap
// =============================================================================
void SectionScreen::_onRowTap(uint8_t cc) {
    if (cc == 255 || !_synth) return;
    _openEntry(cc);
}

void SectionScreen::_openEntry(uint8_t cc) {
    if (!_synth) return;
    const char* name = _ccName(cc);
    if (_isEnumCC(cc)) _openEnumEntry(cc, name);
    else               _openNumericEntry(cc, name);
}

// =============================================================================
// Private: enum entry
// =============================================================================
void SectionScreen::_openEnumEntry(uint8_t cc, const char* title) {
    // ----- Static option tables (must outlive the lambda callback) -----

    // OSC waveforms — built from waveShortNames[] in WaveForms.h.
    // numWaveformsAll (14) covers: SIN SAW SQR TRI ARB PLS rSAW S&H vTRI BLS rBLS BLSQ BLP SSAW
    // Using waveLongNames gives nicer readability in the list picker.
    // The list must be static const char* pointers; waveLongNames[] already is.
    static const char* const* kWave   = waveLongNames;   // all 14 waveforms
    static const int kWaveCount       = (int)numWaveformsAll;

    // LFO waveforms — only the first 4 (Sine/Saw/Square/Triangle) are implemented
    static const char* kLFOwave[] = { "Sine","Sawtooth","Square","Triangle" };

    static const char* kLFOdest[] = { "Pitch","Filter","Shape","Amp" };

    static const char* kSync[]    = { "Free","4 bar","2 bar","1 bar",
                                      "1/2","1/4","1/8","1/16",
                                      "1/4T","1/8T","1/16T","1/32T" };
    static const char* kClkSrc[]  = { "Internal","External" };
    static const char* kOnOff[]   = { "Off","On" };
    static const char* kBypass[]  = { "Active","Bypass" };
    static const char* kPolyMode[]= { "Poly","Mono","Unison" };

    const char* const* opts = kOnOff;
    int                count = 2;

    // For OSC waves: use waveformFromCC to find the current index
    bool isOscWave = false;

    switch (cc) {
        case CC::OSC1_WAVE:
        case CC::OSC2_WAVE:
            opts      = kWave;
            count     = kWaveCount;
            isOscWave = true;
            break;
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
        default:                   opts = kOnOff;   count = 2;  break;
    }

    // Determine current selected index.
    // OSC waves use waveformFromCC() for exact round-trip mapping.
    // POLY_MODE maps the 3-zone CC encoding used by the engine (0-42/43-84/85-127).
    // Boolean CCs (On/Off, Active/Bypass) store 0=off, 1=on — do not use division.
    // All other enums: linear scale across 0..127.
    int curIdx;
    const uint8_t rawVal = _synth->getCC(cc);
    if (isOscWave) {
        // Waveform CCs use a non-linear lookup — must search the table
        WaveformType wt = waveformFromCC(rawVal);
        curIdx = 0;
        for (int i = 0; i < (int)numWaveformsAll; ++i) {
            if (waveformListAll[i] == wt) { curIdx = i; break; }
        }
    } else if (cc == CC::POLY_MODE) {
        // Engine encodes poly mode in three equal zones across 0..127
        curIdx = (rawVal <= 42) ? 0 : (rawVal <= 84) ? 1 : 2;
    } else if (count == 2) {
        // Boolean CCs: engine stores 0=off, 1=on — division would misread value=1 as 0
        curIdx = (rawVal != 0) ? 1 : 0;
    } else {
        // General multi-option enum: linear scale
        curIdx = (int)rawVal * count / 128;
    }

    _pendingCC    = cc;
    _pendingCount = count;

    _entry.openEnum(title, opts, count, constrain(curIdx, 0, count - 1),
        [](int idx) {
            if (!_ctx || !_ctx->_synth) return;
            SectionScreen* s = _ctx;
            uint8_t ccVal;
            // OSC wave: use ccFromWaveform() for exact, round-trip-safe mapping
            if (s->_pendingCC == CC::OSC1_WAVE || s->_pendingCC == CC::OSC2_WAVE) {
                if (idx >= 0 && idx < (int)numWaveformsAll) {
                    ccVal = ccFromWaveform(waveformListAll[idx]);
                } else {
                    ccVal = 0;
                }
            } else if (s->_pendingCC == CC::POLY_MODE) {
                // Map list index 0/1/2 to zone midpoints (Poly=21, Mono=63, Unison=106)
                // Engine reads: 0-42=Poly, 43-84=Mono, 85-127=Unison
                static const uint8_t kPolyMidpoints[] = { 21, 63, 106 };
                ccVal = (idx >= 0 && idx < 3) ? kPolyMidpoints[idx] : 0;
            } else {
                // General enum: map index to CC midpoint of its zone
                ccVal = (uint8_t)
                    ((idx * 128 + (128 / s->_pendingCount) / 2) / s->_pendingCount);
            }
            s->_synth->setCC(s->_pendingCC, ccVal);
            s->syncFromEngine();
        }
    );
}

// =============================================================================
// Private: numeric entry
// =============================================================================
void SectionScreen::_openNumericEntry(uint8_t cc, const char* title) {
    using namespace JT8000Map;

    // Defaults — raw 0..127 range
    const char* unit = "";
    int minV = 0, maxV = 127, curV = (int)_synth->getCC(cc);

    // Per-CC human-unit ranges and current-value conversions
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
            // Bipolar ±12 semitones. CC 64 = zero (centre).
            unit = "st";  minV = -12;  maxV = 12;
            curV = (int)((_synth->getCC(cc) - 64) * 12 / 63);
            break;
        case CC::PITCH_BEND_RANGE:
            // 0..127 → 0..24 semitones. No midpoint — always unipolar positive.
            unit = "st";  minV = 0;    maxV = 24;
            curV = (int)(_synth->getCC(cc) * 24 / 127);
            break;
        default:
            break;
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
                    ccVal = obxa_cutoff_hz_to_cc((float)humanVal);          break;
                case CC::AMP_ATTACK:    case CC::AMP_DECAY:    case CC::AMP_RELEASE:
                case CC::FILTER_ENV_ATTACK: case CC::FILTER_ENV_DECAY: case CC::FILTER_ENV_RELEASE:
                case CC::PITCH_ENV_ATTACK:  case CC::PITCH_ENV_DECAY:  case CC::PITCH_ENV_RELEASE:
                    ccVal = time_ms_to_cc((float)humanVal);                 break;
                case CC::AMP_SUSTAIN:   case CC::FILTER_ENV_SUSTAIN:  case CC::PITCH_ENV_SUSTAIN:
                    ccVal = (uint8_t)(humanVal * 127 / 100);                break;
                case CC::LFO1_FREQ:     case CC::LFO2_FREQ:
                    ccVal = lfo_hz_to_cc((float)humanVal);                  break;
                case CC::FX_BASS_GAIN:  case CC::FX_TREBLE_GAIN:
                    ccVal = (uint8_t)((humanVal + 12) * 127 / 24);         break;
                case CC::BPM_INTERNAL_TEMPO:
                    ccVal = (uint8_t)((humanVal - 20) * 127 / 280);        break;
                case CC::OSC1_PITCH_OFFSET: case CC::OSC2_PITCH_OFFSET:
                    ccVal = (uint8_t)((humanVal + 24) * 127 / 48);         break;
                case CC::OSC1_FINE_TUNE:    case CC::OSC2_FINE_TUNE:
                    ccVal = (uint8_t)((humanVal + 100) * 127 / 200);       break;
                case CC::PITCH_ENV_DEPTH:
                    // Bipolar: 0 semitones → CC 64. ±12st maps to 0..127.
                    ccVal = (uint8_t)constrain(64 + humanVal * 63 / 12, 0, 127); break;
                case CC::PITCH_BEND_RANGE:
                    ccVal = (uint8_t)(humanVal * 127 / 24);                break;
                default:
                    ccVal = (uint8_t)constrain(humanVal, 0, 127);           break;
            }
            s->_synth->setCC(tcc, ccVal);
            s->syncFromEngine();
        }
    );
}

// =============================================================================
// Private: helpers
// =============================================================================

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
            // Read the stored CC value and decode the same three zones the engine uses
            const uint8_t v = _synth->getCC(CC::POLY_MODE);
            return (v <= 42) ? "Poly" : (v <= 84) ? "Mono" : "Unison";
        }
        case CC::FX_REVERB_BYPASS:   return _synth->getFXReverbBypass() ? "Bypass" : "Active";
        case CC::FILTER_OBXA_TWO_POLE: return _synth->getFilterTwoPole() ? "On" : "Off";
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
            cc == CC::FILTER_OBXA_TWO_POLE  ||
            cc == CC::FILTER_OBXA_BP_BLEND_2_POLE ||
            cc == CC::FILTER_OBXA_PUSH_2_POLE     ||
            cc == CC::FILTER_OBXA_XPANDER_4_POLE);
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