// TFTWidgets.cpp
// =============================================================================
// Implementation of all JT-8000 TFT widget classes.
// See TFTWidgets.h for design notes and class documentation.
// =============================================================================

#include "TFTWidgets.h"

// =============================================================================
// Global theme instance — all widgets reference this via gTheme
// =============================================================================
TFTTheme gTheme;


// =============================================================================
// TFTWidget — abstract base
// =============================================================================

TFTWidget::TFTWidget(int16_t x, int16_t y, int16_t w, int16_t h)
    : _x(x), _y(y), _w(w), _h(h)
    , _dirty(true)      // start dirty: first draw paints the widget
    , _visible(true)
    , _display(nullptr)
{}

void TFTWidget::setDisplay(ILI9341_t3n* display) {
    _display = display;
}

void TFTWidget::draw() {
    // Guard: skip if nothing to do or hardware not ready
    if (!_dirty || !_visible || !_display) return;
    doDraw();
    _dirty = false;
}

void TFTWidget::markDirty()        { _dirty = true; }
bool TFTWidget::isDirty()   const  { return _dirty;   }
bool TFTWidget::isVisible() const  { return _visible; }

void TFTWidget::setVisible(bool v) {
    _visible = v;
    markDirty();
}

bool TFTWidget::onTouch(int16_t x, int16_t y) {
    return hitTest(x, y);
}

int16_t TFTWidget::getX() const { return _x; }
int16_t TFTWidget::getY() const { return _y; }
int16_t TFTWidget::getW() const { return _w; }
int16_t TFTWidget::getH() const { return _h; }

bool TFTWidget::hitTest(int16_t x, int16_t y) const {
    return (x >= _x && x < _x + _w &&
            y >= _y && y < _y + _h);
}

void TFTWidget::clearRect(uint16_t colour) {
    if (_display) _display->fillRect(_x, _y, _w, _h, colour);
}

void TFTWidget::drawTextCentred(const char* text, uint16_t colour,
                                uint8_t fontSize, int16_t dy) {
    if (!text || !_display) return;
    _display->setTextSize(fontSize);
    _display->setTextColor(colour);
    const int16_t textW = (int16_t)(strlen(text) * 6 * fontSize);
    const int16_t cx    = _x + (_w - textW) / 2;
    const int16_t cy    = _y + (_h / 2) - (4 * fontSize) + dy;
    _display->setCursor(cx, cy);
    _display->print(text);
}

void TFTWidget::drawTextAt(int16_t lx, int16_t ly, const char* text,
                           uint16_t colour, uint8_t fontSize) {
    if (!text || !_display) return;
    _display->setTextSize(fontSize);
    _display->setTextColor(colour);
    _display->setCursor(lx, ly);
    _display->print(text);
}

void TFTWidget::drawTextRight(int16_t rx, int16_t ly, const char* text,
                              uint16_t colour, uint8_t fontSize) {
    if (!text || !_display) return;
    const int16_t textW = (int16_t)(strlen(text) * 6 * fontSize);
    drawTextAt(rx - textW, ly, text, colour, fontSize);
}


// =============================================================================
// TFTButton
// =============================================================================

TFTButton::TFTButton(int16_t x, int16_t y, int16_t w, int16_t h,
                     const char* label, Style style)
    : TFTWidget(x, y, w, h)
    , _label(label)
    , _style(style)
    , _pressed(false)
    , _callback(nullptr)
{}

void TFTButton::setLabel(const char* label) {
    if (label == _label) return;
    _label = label;
    markDirty();
}

void TFTButton::setCallback(Callback cb) { _callback = cb; }

void TFTButton::triggerCallback() {
    if (_callback) _callback();
}

bool TFTButton::onTouch(int16_t x, int16_t y) {
    if (!hitTest(x, y)) return false;
    if (!_pressed) {
        _pressed = true;
        markDirty();    // repaint with pressed colour immediately
    }
    return true;
}

void TFTButton::onTouchRelease(int16_t x, int16_t y) {
    if (!_pressed) return;
    _pressed = false;
    markDirty();
    // Fire only when finger lifts inside bounds (slide-out = cancel)
    if (hitTest(x, y) && _callback) _callback();
}

void TFTButton::doDraw() {
    if (!_display) return;

    // Background colour: pressed overrides style
    uint16_t bgCol;
    if (_pressed) {
        bgCol = gTheme.buttonPress;
    } else {
        switch (_style) {
            case STYLE_CONFIRM: bgCol = gTheme.keyConfirm;  break;
            case STYLE_CANCEL:  bgCol = gTheme.accent;      break;
            default:            bgCol = gTheme.buttonNormal; break;
        }
    }

    // Filled rounded rect (r=4) + border
    _display->fillRect(_x, _y, _w, _h, bgCol);
    _display->drawRect(_x, _y, _w, _h, gTheme.border);

    // Centred label text
    if (_label) {
        const uint16_t textCol = _pressed ? gTheme.bg : gTheme.buttonText;
        drawTextCentred(_label, textCol, 1);
    }
}


// =============================================================================
// TFTRadioGroup
// =============================================================================

TFTRadioGroup::TFTRadioGroup(int16_t x, int16_t y, int16_t w, int16_t h)
    : TFTWidget(x, y, w, h)
    , _numOptions(0)
    , _selected(-1)
    , _callback(nullptr)
{
    // Zero per-cell dirty arrays; full paint on first draw()
    for (int i = 0; i < RADIO_MAX_OPTIONS; ++i) {
        _optionDirty[i] = true;
        _labels[i]      = nullptr;
    }
}

void TFTRadioGroup::setOptions(const char* const* labels, int count) {
    _numOptions = (count < RADIO_MAX_OPTIONS) ? count : RADIO_MAX_OPTIONS;
    for (int i = 0; i < _numOptions; ++i) _labels[i] = labels[i];
    // Force full repaint on option change
    markDirty();
    for (int i = 0; i < _numOptions; ++i) _optionDirty[i] = true;
}

void TFTRadioGroup::setSelected(int index, bool fireCallback) {
    if (index < 0 || index >= _numOptions) return;
    if (index == _selected) return;

    const int prev = _selected;
    _selected = index;

    // Only dirty the two cells that actually changed
    if (prev >= 0 && prev < _numOptions) _optionDirty[prev] = true;
    _optionDirty[index] = true;
    _dirty = true;  // signal TFTWidget::draw() to call doDraw()

    if (fireCallback && _callback) _callback(_selected);
}

int  TFTRadioGroup::getSelected()     const { return _selected; }
void TFTRadioGroup::setCallback(Callback cb) { _callback = cb; }

bool TFTRadioGroup::onTouch(int16_t x, int16_t y) {
    if (!hitTest(x, y) || _numOptions == 0) return false;

    const int16_t cellW = _w / _numOptions;
    const int     idx   = (x - _x) / cellW;

    if (idx >= 0 && idx < _numOptions) {
        setSelected(idx, /*fireCallback=*/true);
    }
    return true;
}

void TFTRadioGroup::doDraw() {
    if (_numOptions == 0 || !_display) return;

    const int16_t cellW = _w / _numOptions;
    const int16_t midY  = _y + _h / 2;
    const int16_t circR = 5;    // radio circle radius (px)
    const uint8_t fSize = 1;

    for (int i = 0; i < _numOptions; ++i) {
        if (!_optionDirty[i]) continue;   // skip unchanged cells

        const bool    sel   = (i == _selected);
        const int16_t cellX = _x + i * cellW;

        // Clear cell background
        _display->fillRect(cellX, _y, cellW, _h, gTheme.bg);

        const int16_t cx = cellX + 4 + circR;   // circle X centre

        // Radio circle — filled=selected, outline=unselected
        if (sel) {
            _display->fillCircle(cx, midY, circR, gTheme.radioFill);
            _display->drawCircle(cx, midY, circR, gTheme.radioBorder);
        } else {
            _display->fillCircle(cx, midY, circR, gTheme.bg);
            _display->drawCircle(cx, midY, circR, gTheme.radioBorder);
        }

        // Label text to the right of the circle
        if (_labels[i]) {
            const int16_t  labelX = cx + circR + 3;
            const int16_t  labelY = midY - 4 * fSize;
            const uint16_t col    = sel ? gTheme.radioFill : gTheme.textDim;
            _display->setTextSize(fSize);
            _display->setTextColor(col, gTheme.bg);  // 2-arg: spaces over cell background
            _display->setCursor(labelX, labelY);
            _display->print(_labels[i]);
        }

        _optionDirty[i] = false;
    }
}


// =============================================================================
// TFTParamRow
// =============================================================================

TFTParamRow::TFTParamRow(int16_t x, int16_t y, int16_t w, int16_t h,
                         uint8_t cc, const char* name, uint16_t colour)
    : TFTWidget(x, y, w, h)
    , _cc(cc)
    , _colour(colour)
    , _selected(false)
    , _rawValue(0)
    , _onTap(nullptr)
{
    // Copy name to avoid storing a pointer to transient data
    strncpy(_name, name ? name : "---", PROW_NAME_LEN - 1);
    _name[PROW_NAME_LEN - 1] = '\0';
    _valText[0] = '\0';
}

void TFTParamRow::setCallback(TapCallback cb) { _onTap = cb; }

void TFTParamRow::setValue(uint8_t rawValue, const char* text) {
    bool changed = false;

    if (rawValue != _rawValue) {
        _rawValue = rawValue;
        changed   = true;
    }

    // Build candidate text in local buffer then compare
    char newText[PROW_VAL_LEN];
    if (text && text[0]) {
        strncpy(newText, text, PROW_VAL_LEN - 1);
        newText[PROW_VAL_LEN - 1] = '\0';
    } else {
        snprintf(newText, PROW_VAL_LEN, "%d", (int)rawValue);
    }

    if (strncmp(newText, _valText, PROW_VAL_LEN) != 0) {
        strncpy(_valText, newText, PROW_VAL_LEN - 1);
        _valText[PROW_VAL_LEN - 1] = '\0';
        changed = true;
    }

    if (changed) markDirty();
}

void TFTParamRow::configure(uint8_t cc, const char* name, uint16_t colour) {
    bool changed = false;

    if (_cc != cc)         { _cc = cc;         changed = true; }
    if (_colour != colour) { _colour = colour; changed = true; }

    char newName[PROW_NAME_LEN];
    strncpy(newName, name ? name : "---", PROW_NAME_LEN - 1);
    newName[PROW_NAME_LEN - 1] = '\0';

    if (strncmp(newName, _name, PROW_NAME_LEN) != 0) {
        strncpy(_name, newName, PROW_NAME_LEN);
        changed = true;
    }

    if (changed) {
        _rawValue   = 0;
        _valText[0] = '\0';
        markDirty();
    }
}

void TFTParamRow::setSelected(bool sel) {
    if (sel == _selected) return;
    _selected = sel;
    markDirty();
}

bool    TFTParamRow::isSelected() const { return _selected; }
uint8_t TFTParamRow::getCC()      const { return _cc;       }

bool TFTParamRow::onTouch(int16_t x, int16_t y) {
    if (_cc == 255 || !hitTest(x, y)) return false;
    if (_onTap) _onTap(_cc);
    return true;
}

void TFTParamRow::doDraw() {
    if (!_display) return;

    static constexpr int16_t PAD   = 5;    // left/right inner padding
    static constexpr int16_t BAR_Height = 12;   // value bar height (px) — usable on touch
    const int16_t contentH = _h - 1;       // 1 px gap between rows

    // Background and bottom separator
    const uint16_t bgCol = _selected ? _colour : COLOUR_HEADER_BG;
    _display->fillRect(_x, _y, _w, contentH, bgCol);
    _display->drawFastHLine(_x, _y + contentH, _w, COLOUR_BORDER);

    // Empty slot — just a dash
    if (_cc == 255) {
        _display->setTextSize(1);
        // 2-arg setTextColor: spaces render as bgCol, not as COLOUR_BACKGROUND
        _display->setTextColor(COLOUR_TEXT_DIM, bgCol);
        _display->setCursor(_x + PAD, _y + (contentH - 8) / 2);
        _display->print("---");
        return;
    }

    const uint16_t textCol = _selected ? COLOUR_BACKGROUND : COLOUR_TEXT;
    const uint16_t dimCol  = _selected ? COLOUR_BACKGROUND : COLOUR_TEXT_DIM;
    const uint16_t barCol  = _selected ? COLOUR_BACKGROUND : _colour;

    // Parameter name — 2-arg setTextColor so spaces erase over bgCol
    _display->setTextSize(1);
    _display->setTextColor(textCol, bgCol);
    _display->setCursor(_x + PAD, _y + 6);
    _display->print(_name);

    // Value text (right-aligned) — also 2-arg for correct space background
    if (_valText[0]) {
        const int16_t valW = (int16_t)(strlen(_valText) * 6);
        _display->setTextColor(_selected ? COLOUR_BACKGROUND : _colour, bgCol);
        _display->setCursor(_x + _w - PAD - valW - 8, _y + 6);
        _display->print(_valText);
    }

    // Edit arrow (far right, dim)
    _display->setTextColor(dimCol, bgCol);
    _display->setCursor(_x + _w - PAD - 6, _y + 6);
    _display->print(">");

    // Value bar (bottom of row)
    const int16_t barY    = _y + contentH - BAR_Height - 2;
    const int16_t barMaxW = _w - 2 * PAD;
    // Multiply before dividing to keep precision on integer arithmetic
    const int16_t barFill = (int16_t)((int32_t)barMaxW * _rawValue / 127);

    _display->drawFastHLine(_x + PAD, barY, barMaxW, COLOUR_BORDER);
    if (barFill > 0) {
        _display->fillRect(_x + PAD, barY, barFill, BAR_Height, barCol);
    }
}


// =============================================================================
// TFTSectionTile
// =============================================================================

TFTSectionTile::TFTSectionTile(int16_t x, int16_t y, int16_t w, int16_t h,
                               const SectionDef& section)
    : TFTWidget(x, y, w, h)
    , _section(section)
    , _pressed(false)
    , _callback(nullptr)
{}

void TFTSectionTile::setCallback(Callback cb) { _callback = cb; }
void TFTSectionTile::activate() { if (_callback) _callback(); }

bool TFTSectionTile::onTouch(int16_t x, int16_t y) {
    if (!hitTest(x, y)) return false;
    if (!_pressed) { _pressed = true; markDirty(); }
    return true;
}

void TFTSectionTile::onTouchRelease(int16_t x, int16_t y) {
    if (!_pressed) return;
    _pressed = false;
    markDirty();
    if (hitTest(x, y) && _callback) _callback();
}

void TFTSectionTile::doDraw() {
    if (!_display) return;

    // Background: slightly lighter navy tint when pressed, dark panel normally.
    // 0x20A2 = BGR565 very dark navy (matches COLOUR_HEADER_BG family)
    const uint16_t bgCol = _pressed
        ? COLOUR_HEADER_BG   // pressed: use header colour for feedback
        : (uint16_t)0x20A2;  // normal: very dark navy (BGR565)

    _display->fillRect(_x, _y, _w, _h, bgCol);

    // Outer border — section colour when pressed, border otherwise
    _display->drawRect(_x, _y, _w, _h, _pressed ? _section.colour : COLOUR_BORDER);

    // 2 px accent bar along the top
    _display->drawFastHLine(_x + 1, _y + 1, _w - 2, _section.colour);
    _display->drawFastHLine(_x + 1, _y + 2, _w - 2, _section.colour);

    // Section label — centred, section colour; 2-arg so spaces use bgCol
    _display->setTextSize(1);
    _display->setTextColor(_section.colour, bgCol);
    const int16_t labelW = (int16_t)(strlen(_section.label) * 6);
    _display->setCursor(_x + (_w - labelW) / 2, _y + (_h / 2) - 8);
    _display->print(_section.label);

    // Page-count hint — centred, dim; 2-arg so spaces use bgCol
    char hint[6];
    snprintf(hint, sizeof(hint), "%dp", _section.pageCount);
    const int16_t hintW = (int16_t)(strlen(hint) * 6);
    _display->setTextColor(COLOUR_TEXT_DIM, bgCol);
    _display->setCursor(_x + (_w - hintW) / 2, _y + (_h / 2) + 2);
    _display->print(hint);
}


// =============================================================================
// TFTNumericEntry
// =============================================================================

TFTNumericEntry::TFTNumericEntry()
    : _display(nullptr), _mode(MODE_CLOSED)
    , _minVal(0), _maxVal(127), _currentVal(0)
    , _digitCount(0), _editing(false), _negative(false)
    , _selectedEnum(0), _numEnumOptions(0)
    , _callback(nullptr)
    , _scrollOffset(0)
    , _fullRedraw(false)
    , _valueDirty(false)
{
    _titleBuf[0] = '\0';
    _unitBuf[0]  = '\0';
    _digitBuf[0] = '\0';
    for (int i = 0; i < ENTRY_MAX_ENUM; ++i) _enumLabels[i] = nullptr;
}

void TFTNumericEntry::setDisplay(ILI9341_t3n* d) { _display = d; }

void TFTNumericEntry::openNumeric(const char* title, const char* unit,
                                  int minVal, int maxVal, int currentVal,
                                  Callback cb) {
    if (!_display) return;

    _mode       = MODE_NUMBER;
    _minVal     = minVal;
    _maxVal     = maxVal;
    _currentVal = currentVal;
    _callback   = cb;

    strncpy(_titleBuf, title ? title : "", ENTRY_TITLE_LEN - 1);
    _titleBuf[ENTRY_TITLE_LEN - 1] = '\0';
    strncpy(_unitBuf, unit ? unit : "", ENTRY_UNIT_LEN - 1);
    _unitBuf[ENTRY_UNIT_LEN - 1] = '\0';

    // Start in hint mode: show current value dimmed until user types
    _digitBuf[0] = '\0';
    _digitCount  = 0;
    _editing     = false;
    // Pre-set negative flag from current value so ± key reflects reality on first open
    _negative    = (currentVal < 0);

    _fullRedraw  = true;
    _valueDirty  = false;
}

void TFTNumericEntry::openEnum(const char* title, const char* const* labels,
                               int count, int currentIdx, Callback cb) {
    if (!_display) return;

    _mode           = MODE_ENUM;
    _callback       = cb;
    _numEnumOptions = (count < ENTRY_MAX_ENUM) ? count : ENTRY_MAX_ENUM;
    _selectedEnum   = constrain(currentIdx, 0, _numEnumOptions - 1);
    _scrollOffset   = 0;

    strncpy(_titleBuf, title ? title : "", ENTRY_TITLE_LEN - 1);
    _titleBuf[ENTRY_TITLE_LEN - 1] = '\0';

    for (int i = 0; i < _numEnumOptions; ++i) _enumLabels[i] = labels[i];

    _scrollToSelection();

    _fullRedraw = true;
    _valueDirty = false;
}

void TFTNumericEntry::draw() {
    if (_mode == MODE_CLOSED || !_display) return;
    if (_fullRedraw) {
        _drawFull();
        _fullRedraw = false;
        _valueDirty = false;
    } else if (_valueDirty) {
        _drawValueBox();
        _valueDirty = false;
    }
}

bool TFTNumericEntry::onTouch(int16_t x, int16_t y) {
    if (_mode == MODE_CLOSED) return false;
    if (_mode == MODE_NUMBER) _handleNumericTouch(x, y);
    else                      _handleEnumTouch(x, y);
    return true;    // consume all touches while open
}

bool TFTNumericEntry::isOpen()  const { return _mode != MODE_CLOSED; }
TFTNumericEntry::Mode TFTNumericEntry::getMode() const { return _mode; }

// ---------------------------------------------------------------------------
// onEncoderDelta() — scroll the enum list while it is open.
//
// delta > 0 = scroll towards later (higher-index) items
// delta < 0 = scroll towards earlier items
//
// Moves the highlighted selection by |delta| items, clamped to valid range,
// then scrolls the viewport to keep the selection visible.
// Redraws only the list region — not the full screen.
// ---------------------------------------------------------------------------
void TFTNumericEntry::onEncoderDelta(int delta) {
    if (_mode != MODE_ENUM || delta == 0 || _numEnumOptions == 0) return;

    // Move selection, clamp to [0, numOptions-1]
    const int newSel = constrain(_selectedEnum + delta, 0, _numEnumOptions - 1);
    if (newSel == _selectedEnum) return;   // already at limit

    _selectedEnum = newSel;

    // Scroll viewport to keep selection visible
    _scrollToSelection();

    // Redraw list only — avoids full-screen repaint noise every encoder step
    _drawEnumList();
}
void TFTNumericEntry::close()         { _mode = MODE_CLOSED; }

// ---- Full-screen draw -------------------------------------------------------

void TFTNumericEntry::_drawFull() {
    _display->fillScreen(gTheme.bg);

    // Title bar
    _display->fillRect(0, 0, SW, TB_Height, gTheme.headerBg);
    _display->setTextSize(2);
    _display->setTextColor(gTheme.textNormal, gTheme.headerBg);
    _display->setCursor(6, 7);
    _display->print(_titleBuf);

    _drawCancelButton(false);

    if (_mode == MODE_NUMBER) {
        _drawValueBox();
        _drawKeypad();
    } else {
        _drawEnumList();
        _drawEnumButtons();
    }
}

void TFTNumericEntry::_drawCancelButton(bool pressed) {
    const uint16_t bg = pressed ? gTheme.buttonPress : gTheme.accent;
    _display->fillRect(CANCEL_X, CANCEL_Y, CANCEL_Width, CANCEL_Height, bg);
    _display->setTextSize(1);
    _display->setTextColor(gTheme.buttonText, bg);
    const int16_t lx = CANCEL_X + (CANCEL_Width - 6 * 6) / 2;
    _display->setCursor(lx, 11);
    _display->print("Cancel");
}

void TFTNumericEntry::_drawValueBox() {
    _display->fillRect(KP_X, VB_Y, KP_Width, VB_Height, gTheme.entryBg);
    _display->drawRect(KP_X, VB_Y, KP_Width, VB_Height, gTheme.border);

    // Build display string: digits (or hint) + unit
    char dispBuf[ENTRY_MAX_DIGITS + ENTRY_UNIT_LEN + 2];

    if (!_editing || _digitCount == 0) {
        // Hint mode: show current value dimmed
        snprintf(dispBuf, sizeof(dispBuf), "%d %s", _currentVal, _unitBuf);
        _display->setTextColor(gTheme.textDim, gTheme.entryBg);
    } else {
        // Editing: show sign prefix + digits + unit
        snprintf(dispBuf, sizeof(dispBuf), "%s%s %s",
                 (_negative ? "-" : ""), _digitBuf, _unitBuf);
        _display->setTextColor(gTheme.entryText, gTheme.entryBg);
    }

    _display->setTextSize(2);
    const int16_t tw = (int16_t)(strlen(dispBuf) * 12);  // 6px × 2 per char
    const int16_t tx = KP_X + (KP_Width - tw) / 2;
    const int16_t ty = VB_Y + (VB_Height - 14) / 2;
    _display->setCursor(tx, ty);
    _display->print(dispBuf);
}

void TFTNumericEntry::_drawKeypad() {
    // Rows [7,8,9], [4,5,6], [1,2,3]
    const int digits[3][3] = { {7,8,9}, {4,5,6}, {1,2,3} };
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const int16_t kx = KP_X + col * (KEY_Width + KEY_GAP);
            const int16_t ky = KP_Y + row * (KEY_Height + KEY_GAP);
            _drawKey(kx, ky, KEY_Width, KEY_Height, _digitStr(digits[row][col]),
                     gTheme.keyBg, false);
        }
    }

    // Bottom row: layout depends on whether negative values are possible.
    // When minVal < 0 we show [0] [±] [←] [OK] to allow sign entry.
    // When minVal >= 0 we show the original wider [0] [←] [OK].
    if (_minVal < 0) {
        // Highlight ± key in amber when currently negative
        const uint16_t signBg = _negative ? COLOUR_SELECTED : gTheme.keyBg;
        _drawKey(KP_X,                                              BR_Y, BRS_0_Width,  KEY_Height, "0",  gTheme.keyBg,    false);
        _drawKey(KP_X + BRS_0_Width + KEY_GAP,                     BR_Y, BRS_S_Width,  KEY_Height, "+/-",signBg,          false);
        _drawKey(KP_X + BRS_0_Width + BRS_S_Width + 2*KEY_GAP,     BR_Y, BRS_BK_Width, KEY_Height, "<-", gTheme.keyBackspace, false);
        _drawKey(KP_X + BRS_0_Width + BRS_S_Width + BRS_BK_Width + 3*KEY_GAP, BR_Y, BRS_CO_Width, KEY_Height, "OK", gTheme.keyConfirm, false);
    } else {
        _drawKey(KP_X,                                          BR_Y, BR0_Width,  KEY_Height, "0",  gTheme.keyBg,       false);
        _drawKey(KP_X + BR0_Width + KEY_GAP,                    BR_Y, BRBK_Width, KEY_Height, "<-", gTheme.keyBackspace, false);
        _drawKey(KP_X + BR0_Width + BRBK_Width + 2*KEY_GAP,    BR_Y, BRCO_Width, KEY_Height, "OK", gTheme.keyConfirm,  false);
    }
}

void TFTNumericEntry::_drawKey(int16_t kx, int16_t ky, int16_t kw, int16_t kh,
                               const char* label, uint16_t bgCol, bool pressed) {
    const uint16_t bg = pressed ? gTheme.buttonPress : bgCol;
    _display->fillRect(kx, ky, kw, kh, bg);
    _display->drawRect(kx, ky, kw, kh, gTheme.keyBorder);
    _display->setTextSize(1);
    _display->setTextColor(gTheme.keyText, bg);  // 2-arg: spaces over key background
    const int16_t tw = (int16_t)(strlen(label) * 6);
    _display->setCursor(kx + (kw - tw) / 2, ky + (kh - 8) / 2);
    _display->print(label);
}

/*static*/ const char* TFTNumericEntry::_digitStr(int d) {
    // Note: static buffer — not reentrant; fine here (single-threaded UI)
    static char buf[2] = "0";
    buf[0] = '0' + (char)d;
    return buf;
}

// ---- Numeric touch handler --------------------------------------------------

void TFTNumericEntry::_handleNumericTouch(int16_t x, int16_t y) {
    // Cancel button
    if (x >= CANCEL_X && x < CANCEL_X + CANCEL_Width &&
        y >= CANCEL_Y && y < CANCEL_Y + CANCEL_Height) {
        close();
        return;
    }

    // Digit rows [7,8,9] [4,5,6] [1,2,3]
    const int digits[3][3] = { {7,8,9}, {4,5,6}, {1,2,3} };
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const int16_t kx = KP_X + col * (KEY_Width + KEY_GAP);
            const int16_t ky = KP_Y + row * (KEY_Height + KEY_GAP);
            if (x >= kx && x < kx + KEY_Width && y >= ky && y < ky + KEY_Height) {
                _appendDigit(digits[row][col]);
                return;
            }
        }
    }

    // Bottom row — layout depends on whether negative entry is allowed.
    // Two distinct layouts share the same Y band (BR_Y).
    if (_minVal < 0) {
        // --- Sign-key layout: [0] [±/-] [<-] [OK] ---
        // Pre-compute X boundaries for each button.
        const int16_t x0Start  = KP_X;
        const int16_t xSignStart = x0Start  + BRS_0_Width  + KEY_GAP;
        const int16_t xBkStart   = xSignStart + BRS_S_Width  + KEY_GAP;
        const int16_t xOkStart   = xBkStart   + BRS_BK_Width + KEY_GAP;

        if (y >= BR_Y && y < BR_Y + KEY_Height) {
            if (x >= x0Start && x < x0Start + BRS_0_Width) {
                _appendDigit(0);
                return;
            }
            if (x >= xSignStart && x < xSignStart + BRS_S_Width) {
                _toggleSign();
                _valueDirty = true;
                return;
            }
            if (x >= xBkStart && x < xBkStart + BRS_BK_Width) {
                _backspace();
                return;
            }
            if (x >= xOkStart) {
                _confirm();
                return;
            }
        }
    } else {
        // --- Standard layout: [0] [<-] [OK] ---
        if (y >= BR_Y && y < BR_Y + KEY_Height) {
            if (x >= KP_X && x < KP_X + BR0_Width) {
                _appendDigit(0);
                return;
            }
            if (x >= KP_X + BR0_Width + KEY_GAP &&
                x < KP_X + BR0_Width + KEY_GAP + BRBK_Width) {
                _backspace();
                return;
            }
            if (x >= KP_X + BR0_Width + BRBK_Width + 2*KEY_GAP) {
                _confirm();
                return;
            }
        }
    }
}

void TFTNumericEntry::_toggleSign() {
    // Only allow sign toggle when negative values are in range.
    if (_minVal >= 0) return;
    _negative    = !_negative;
    _valueDirty  = true;
}

void TFTNumericEntry::_appendDigit(int d) {
    if (_digitCount >= ENTRY_MAX_DIGITS - 1) return;  // buffer full

    // First keypress: clear hint and start fresh (overwrite mode)
    if (!_editing) {
        _digitBuf[0] = '\0';
        _digitCount  = 0;
        _editing     = true;
    }

    // Allow lone zero, but block leading zeros on multi-digit numbers
    if (_digitCount == 0 && d == 0) {
        _digitBuf[0] = '0';
        _digitBuf[1] = '\0';
        _digitCount  = 1;
        _valueDirty  = true;
        return;
    }

    _digitBuf[_digitCount++] = '0' + (char)d;
    _digitBuf[_digitCount]   = '\0';
    _valueDirty = true;
}

void TFTNumericEntry::_backspace() {
    if (_digitCount > 0) {
        _digitBuf[--_digitCount] = '\0';
        if (_digitCount == 0) _editing = false;  // back to hint mode
        _valueDirty = true;
    }
}

void TFTNumericEntry::_confirm() {
    int val;
    if (_editing && _digitCount > 0) {
        // Parse magnitude from digit buffer, then apply sign.
        val = atoi(_digitBuf);
        if (_negative) val = -val;
        // Clamp to valid range (handles both positive and negative bounds).
        val = constrain(val, _minVal, _maxVal);
    } else {
        val = _currentVal;  // no digits typed → keep current value unchanged
    }
    close();
    if (_callback) _callback(val);
}

// ---- Enum list helpers ------------------------------------------------------

void TFTNumericEntry::_drawEnumList() {
    const int listY = TB_Height + 2;
    const int listH = EN_BTN_Y - listY - 2;

    _display->fillRect(0, listY, SW, listH, gTheme.bg);

    for (int r = 0; r < EN_ROWS; ++r) {
        const int     idx = _scrollOffset + r;
        if (idx >= _numEnumOptions) break;

        const int16_t ry  = listY + r * EN_ROW_Height;
        const bool    sel = (idx == _selectedEnum);

        _display->fillRect(0, ry, SW, EN_ROW_Height - 1,
                           sel ? gTheme.selectedBg : gTheme.bg);

        if (_enumLabels[idx]) {
            _display->setTextSize(2);
            // 2-arg: spaces render over correct row background
            _display->setTextColor(sel ? gTheme.textOnSelect : gTheme.textNormal,
                                   sel ? gTheme.selectedBg  : gTheme.bg);
            _display->setCursor(10, ry + (EN_ROW_Height - 14) / 2);
            _display->print(_enumLabels[idx]);
        }
    }
}

void TFTNumericEntry::_drawEnumButtons() {
    // Confirm (right)
    _display->fillRect(180, EN_BTN_Y, 130, 30, gTheme.keyConfirm);
    _display->setTextSize(1);
    _display->setTextColor(gTheme.buttonText, gTheme.keyConfirm);
    _display->setCursor(212, EN_BTN_Y + 11);
    _display->print("Confirm");

    // Cancel (left)
    _display->fillRect(10, EN_BTN_Y, 130, 30, gTheme.accent);
    _display->setTextColor(gTheme.buttonText, gTheme.accent);
    _display->setCursor(42, EN_BTN_Y + 11);
    _display->print("Cancel");
}

void TFTNumericEntry::_handleEnumTouch(int16_t x, int16_t y) {
    // Confirm button (right half of footer)
    if (x >= 180 && y >= EN_BTN_Y && y < EN_BTN_Y + 30) {
        const int confirmed = _selectedEnum;
        close();
        if (_callback) _callback(confirmed);
        return;
    }
    // Cancel button (left half of footer)
    if (x < 140 && y >= EN_BTN_Y && y < EN_BTN_Y + 30) {
        close();
        return;
    }

    // List rows — tap to select; swipe detection handled at higher level
    const int listY = TB_Height + 2;
    for (int r = 0; r < EN_ROWS; ++r) {
        const int16_t ry = listY + r * EN_ROW_Height;
        if (y >= ry && y < ry + EN_ROW_Height) {
            const int idx = _scrollOffset + r;
            if (idx < _numEnumOptions && idx != _selectedEnum) {
                _selectedEnum = idx;
                _drawEnumList();   // immediate partial redraw of list only
            }
            return;
        }
    }
}

void TFTNumericEntry::_scrollToSelection() {
    if (_selectedEnum < _scrollOffset) {
        _scrollOffset = _selectedEnum;
    } else if (_selectedEnum >= _scrollOffset + EN_ROWS) {
        _scrollOffset = _selectedEnum - EN_ROWS + 1;
    }
    if (_scrollOffset < 0) _scrollOffset = 0;
}


// =============================================================================
// TFTScreen
// =============================================================================

TFTScreen::TFTScreen()
    : _display(nullptr), _numWidgets(0), _bgColour(0x0000)
{}

void TFTScreen::setDisplay(ILI9341_t3n* display) {
    _display = display;
    for (int i = 0; i < _numWidgets; ++i) {
        if (_widgets[i]) _widgets[i]->setDisplay(display);
    }
}

void TFTScreen::setBackground(uint16_t colour) { _bgColour = colour; }

bool TFTScreen::addWidget(TFTWidget* widget) {
    if (_numWidgets >= MAX_WIDGETS || !widget) return false;
    widget->setDisplay(_display);
    _widgets[_numWidgets++] = widget;
    return true;
}

void TFTScreen::markAllDirty() {
    for (int i = 0; i < _numWidgets; ++i) {
        if (_widgets[i]) _widgets[i]->markDirty();
    }
}

void TFTScreen::clearAndRedraw() {
    if (_display) _display->fillScreen(_bgColour);
    markAllDirty();
}

void TFTScreen::draw() {
    // Cost proportional to dirty-widget count, not total count
    for (int i = 0; i < _numWidgets; ++i) {
        if (_widgets[i]) _widgets[i]->draw();
    }
}

bool TFTScreen::onTouch(int16_t x, int16_t y) {
    // First widget that claims the event stops routing
    for (int i = 0; i < _numWidgets; ++i) {
        if (_widgets[i] && _widgets[i]->onTouch(x, y)) return true;
    }
    return false;
}

void TFTScreen::onTouchRelease(int16_t x, int16_t y) {
    // Broadcast to all widgets so buttons always restore normal appearance
    for (int i = 0; i < _numWidgets; ++i) {
        if (_widgets[i]) _widgets[i]->onTouchRelease(x, y);
    }
}

int TFTScreen::numWidgets() const { return _numWidgets; }


// =============================================================================
// TFTScreenManager
// =============================================================================

TFTScreenManager::TFTScreenManager()
    : _display(nullptr), _stackDepth(0)
{}

void TFTScreenManager::setDisplay(ILI9341_t3n* display) {
    _display = display;
    _numericEntry.setDisplay(display);
}

bool TFTScreenManager::push(TFTScreen* screen) {
    if (_stackDepth >= SCREEN_STACK_DEPTH || !screen) return false;
    screen->setDisplay(_display);
    screen->clearAndRedraw();   // full clear + all widgets dirty
    _stack[_stackDepth++] = screen;
    return true;
}

bool TFTScreenManager::pop() {
    if (_stackDepth <= 1) return false;  // never pop root screen
    --_stackDepth;
    // Restore the revealed screen with a full repaint (pixels were overwritten)
    if (_stack[_stackDepth - 1]) {
        _stack[_stackDepth - 1]->clearAndRedraw();
    }
    return true;
}

TFTScreen* TFTScreenManager::topScreen() {
    return (_stackDepth > 0) ? _stack[_stackDepth - 1] : nullptr;
}

int TFTScreenManager::stackDepth() const { return _stackDepth; }

TFTNumericEntry& TFTScreenManager::numericEntry() { return _numericEntry; }
bool             TFTScreenManager::isEntryOpen()  const { return _numericEntry.isOpen(); }

void TFTScreenManager::update(bool newTouch, int16_t tx, int16_t ty,
                               bool newRelease, int16_t rx, int16_t ry) {
    // Entry overlay takes full priority over the screen stack
    if (_numericEntry.isOpen()) {
        _numericEntry.draw();
        if (newTouch) _numericEntry.onTouch(tx, ty);
        return;
    }

    // Normal screen
    TFTScreen* top = topScreen();
    if (!top) return;

    top->draw();
    if (newTouch)   top->onTouch(tx, ty);
    if (newRelease) top->onTouchRelease(rx, ry);
}
