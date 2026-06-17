// =============================================================================
// PresetBrowser.cpp — dual-layer preset browser implementation
// =============================================================================

#include "PresetBrowser.h"
#include "LayerManager.h"   // for EditTarget enum values

// =============================================================================
// open()
// =============================================================================
void PresetBrowser::open(SynthEngine* synth, int startIdx,
                          LoadCallback loadCb, EditTarget editTarget)
{
    _synth      = synth;
    _loadCb     = loadCb;
    _loadTarget = editTarget;
    _totalCount = Presets::presets_totalCount();
    _cursorIdx  = constrain(startIdx, 0, _totalCount - 1);
    _scrollTop  = _clampScrollTop(_cursorIdx - PBLayout::VISIBLE_ROWS / 2);
    _open       = true;
    _dirty      = true;
}

void PresetBrowser::close() {
    _open  = false;
    _dirty = false;
}

bool PresetBrowser::isOpen()   const { return _open;       }
int  PresetBrowser::selected() const { return _cursorIdx;  }

// =============================================================================
// Load target management
// =============================================================================
void PresetBrowser::setLoadTarget(EditTarget t) {
    if (_loadTarget != t) {
        _loadTarget  = t;
        _footerDirty = true;  // redraw footer to show new target highlight
    }
}

void PresetBrowser::cycleLoadTarget() {
    switch (_loadTarget) {
        case EditTarget::LAYER_A: _loadTarget = EditTarget::LAYER_B; break;
        case EditTarget::LAYER_B: _loadTarget = EditTarget::BOTH;    break;
        case EditTarget::BOTH:    _loadTarget = EditTarget::LAYER_A; break;
    }
    _footerDirty = true;
}

const char* PresetBrowser::_targetLabel() const {
    switch (_loadTarget) {
        case EditTarget::LAYER_A: return "A";
        case EditTarget::LAYER_B: return "B";
        case EditTarget::BOTH:    return "AB";
        default:                  return "A";
    }
}

// =============================================================================
// draw()
// =============================================================================
void PresetBrowser::draw(ILI9341_t3n& tft) {
    if (!_open) return;

    if (_dirty) {
        _drawHeader(tft);
        _drawFooter(tft);
        for (int r = 0; r < PBLayout::VISIBLE_ROWS; ++r) _drawRow(tft, r);
        _dirty       = false;
        _footerDirty = false;
        _prevCursor  = _cursorIdx;
        _prevScroll  = _scrollTop;
    } else {
        // Footer-only redraw (target changed)
        if (_footerDirty) {
            _drawHeader(tft);   // header shows target indicator too
            _drawFooter(tft);
            _footerDirty = false;
        }

        if (_prevCursor != _cursorIdx || _prevScroll != _scrollTop) {
            if (_prevScroll != _scrollTop) {
                for (int r = 0; r < PBLayout::VISIBLE_ROWS; ++r) _drawRow(tft, r);
            } else {
                _drawRowForIdx(tft, _prevCursor);
                _drawRowForIdx(tft, _cursorIdx);
            }
            _prevCursor = _cursorIdx;
            _prevScroll = _scrollTop;
        }
    }
}

// =============================================================================
// onEncoder() — L encoder: scroll cursor
// =============================================================================
void PresetBrowser::onEncoder(int delta) {
    if (!_open) return;
    _prevCursor = _cursorIdx;
    _prevScroll = _scrollTop;

    _cursorIdx = (_cursorIdx + delta + _totalCount) % _totalCount;

    if (_cursorIdx < _scrollTop) {
        _scrollTop = _cursorIdx;
    } else if (_cursorIdx >= _scrollTop + PBLayout::VISIBLE_ROWS) {
        _scrollTop = _cursorIdx - PBLayout::VISIBLE_ROWS + 1;
    }
}

// =============================================================================
// onEncoderRight() — R encoder: cycle load target
// =============================================================================
void PresetBrowser::onEncoderRight(int delta) {
    if (!_open) return;
    // Any R encoder movement cycles the target (direction ignored — just cycle)
    if (delta != 0) cycleLoadTarget();
}

// =============================================================================
// onEncoderPress() — confirm and close
// =============================================================================
void PresetBrowser::onEncoderPress() {
    if (!_open) return;
    _loadPreset(_cursorIdx);
    close();
}

// =============================================================================
// onTouch()
// =============================================================================
bool PresetBrowser::onTouch(int tx, int ty) {
    if (!_open) return false;

    // CANCEL button (header, top-right)
    if (tx >= PBLayout::CANCEL_X && tx < (int)(PBLayout::CANCEL_X + PBLayout::CANCEL_W) &&
        ty >= PBLayout::CANCEL_Y && ty < (int)(PBLayout::CANCEL_Y + PBLayout::CANCEL_H)) {
        close();
        return true;
    }

    // ---- Footer buttons ----
    const int fy = PBLayout::FTR_Y + 2;
    const int fh = PBLayout::BTN_H;

    if (ty >= fy && ty < fy + fh) {
        // PREV button
        if (tx >= PBLayout::PREV_X && tx < (int)(PBLayout::PREV_X + PBLayout::PREV_W)) {
            _prevCursor = _cursorIdx;
            _prevScroll = _scrollTop;
            _scrollTop  = _clampScrollTop(_scrollTop - PBLayout::VISIBLE_ROWS);
            if (_cursorIdx < _scrollTop ||
                _cursorIdx >= _scrollTop + PBLayout::VISIBLE_ROWS) {
                _cursorIdx = _scrollTop;
            }
            _dirty = true;
            return true;
        }

        // Target A button
        if (tx >= PBLayout::TGT_A_X && tx < (int)(PBLayout::TGT_A_X + PBLayout::TGT_A_W)) {
            setLoadTarget(EditTarget::LAYER_A);
            return true;
        }

        // Target B button
        if (tx >= PBLayout::TGT_B_X && tx < (int)(PBLayout::TGT_B_X + PBLayout::TGT_B_W)) {
            setLoadTarget(EditTarget::LAYER_B);
            return true;
        }

        // Target AB button
        if (tx >= PBLayout::TGT_AB_X && tx < (int)(PBLayout::TGT_AB_X + PBLayout::TGT_AB_W)) {
            setLoadTarget(EditTarget::BOTH);
            return true;
        }

        // NEXT button
        if (tx >= PBLayout::NEXT_X && tx < (int)(PBLayout::NEXT_X + PBLayout::NEXT_W)) {
            _prevCursor = _cursorIdx;
            _prevScroll = _scrollTop;
            _scrollTop  = _clampScrollTop(_scrollTop + PBLayout::VISIBLE_ROWS);
            if (_cursorIdx < _scrollTop ||
                _cursorIdx >= _scrollTop + PBLayout::VISIBLE_ROWS) {
                _cursorIdx = _scrollTop;
            }
            _dirty = true;
            return true;
        }

        return true;  // consumed touch in footer area
    }

    // List row tap
    if (ty >= PBLayout::LIST_Y && ty < (int)(PBLayout::LIST_Y + PBLayout::LIST_H)) {
        const int row = (ty - PBLayout::LIST_Y) / PBLayout::ROW_H;
        const int idx = _scrollTop + row;
        if (idx >= 0 && idx < _totalCount) {
            if (idx == _cursorIdx) {
                _loadPreset(idx);
                close();
            } else {
                _prevCursor = _cursorIdx;
                _prevScroll = _scrollTop;
                _cursorIdx  = idx;
            }
        }
        return true;
    }

    return true;  // consume all touches while open
}

// =============================================================================
// Private: draw helpers
// =============================================================================

void PresetBrowser::_drawHeader(ILI9341_t3n& tft) {
    tft.fillRect(0, 0, PBLayout::W, PBLayout::HDR_H, PBColour::HDR_BG);
    tft.drawFastHLine(0, PBLayout::HDR_H - 1, PBLayout::W, PBColour::BORDER);

    tft.setTextColor(PBColour::HDR_TEXT, PBColour::HDR_BG);
    tft.setTextSize(1);
    tft.setCursor(6, 9);
    tft.print("PRESET BROWSER");

    // Target indicator — shows which layer receives the loaded patch
    const char* tgtLabel = _targetLabel();
    const int tgtX = 130;
    tft.setTextColor(PBColour::BTN_SEL, PBColour::HDR_BG);
    tft.setCursor(tgtX, 9);
    tft.print("\x10 ");  // right-arrow character
    tft.print(tgtLabel);

    // CANCEL button
    tft.fillRect(PBLayout::CANCEL_X, PBLayout::CANCEL_Y,
                 PBLayout::CANCEL_W, PBLayout::CANCEL_H, PBColour::CANCEL_BG);
    tft.setTextColor(PBColour::SEL_TEXT, PBColour::CANCEL_BG);
    tft.setCursor(PBLayout::CANCEL_X + 14, PBLayout::CANCEL_Y + 6);
    tft.print("CANCEL");
}

void PresetBrowser::_drawFooter(ILI9341_t3n& tft) {
    tft.fillRect(0, PBLayout::FTR_Y, PBLayout::W, PBLayout::FTR_H, PBColour::FTR_BG);
    tft.drawFastHLine(0, PBLayout::FTR_Y, PBLayout::W, PBColour::BORDER);

    const int by = PBLayout::FTR_Y + 2;
    const int bh = PBLayout::BTN_H;
    const int ty = by + (bh / 2) - 3;  // text vertical centre

    // Helper lambda: draw a single footer button
    auto drawBtn = [&](int x, int w, const char* label, bool active) {
        uint16_t bg   = active ? PBColour::BTN_SEL : PBColour::BTN_BG;
        uint16_t text = active ? PBColour::BG      : PBColour::BTN_TEXT;
        tft.fillRect(x, by, w, bh, bg);
        tft.setTextColor(text, bg);
        tft.setTextSize(1);
        // Centre text horizontally
        int labelPx = strlen(label) * 6;  // approx 6px per char at size 1
        tft.setCursor(x + (w - labelPx) / 2, ty);
        tft.print(label);
    };

    // PREV
    drawBtn(PBLayout::PREV_X, PBLayout::PREV_W, "\x11 PV", false);

    // Target buttons — highlight the active one
    drawBtn(PBLayout::TGT_A_X,  PBLayout::TGT_A_W,  "A",
            _loadTarget == EditTarget::LAYER_A);
    drawBtn(PBLayout::TGT_B_X,  PBLayout::TGT_B_W,  "B",
            _loadTarget == EditTarget::LAYER_B);
    drawBtn(PBLayout::TGT_AB_X, PBLayout::TGT_AB_W, "AB",
            _loadTarget == EditTarget::BOTH);

    // NEXT
    drawBtn(PBLayout::NEXT_X, PBLayout::NEXT_W, "NX \x10", false);
}

void PresetBrowser::_drawRow(ILI9341_t3n& tft, int row) {
    const int idx = _scrollTop + row;
    const int y   = PBLayout::LIST_Y + row * PBLayout::ROW_H;
    const bool isSel = (idx == _cursorIdx);

    // Background
    uint16_t bg = isSel ? PBColour::SEL_BG
                        : ((row & 1) ? PBColour::ROW_ALT : PBColour::ROW_BG);
    tft.fillRect(0, y, PBLayout::W, PBLayout::ROW_H, bg);

    if (idx >= _totalCount) return;  // past end of list — blank row

    // Index number
    char buf[6];
    snprintf(buf, sizeof(buf), "%02d", idx);
    tft.setTextColor(isSel ? PBColour::SEL_TEXT : PBColour::IDX_TEXT, bg);
    tft.setTextSize(1);
    tft.setCursor(8, y + 8);
    if (isSel) tft.print("\x10 ");  // selection arrow
    else       tft.print("  ");
    tft.print(buf);

    // Preset name
    const char* name = Presets::presets_nameByGlobalIndex(idx);
    tft.setTextColor(isSel ? PBColour::SEL_TEXT : PBColour::ROW_TEXT, bg);
    tft.setCursor(46, y + 8);
    tft.print(name ? name : "---");

    // Row divider
    tft.drawFastHLine(0, y + PBLayout::ROW_H - 1, PBLayout::W, PBColour::BORDER);
}

void PresetBrowser::_drawRowForIdx(ILI9341_t3n& tft, int idx) {
    const int row = idx - _scrollTop;
    if (row >= 0 && row < PBLayout::VISIBLE_ROWS) {
        _drawRow(tft, row);
    }
}

int PresetBrowser::_clampScrollTop(int st) const {
    const int maxScroll = _totalCount - PBLayout::VISIBLE_ROWS;
    return constrain(st, 0, max(0, maxScroll));
}

void PresetBrowser::_loadPreset(int idx) {
    if (_loadCb) {
        _loadCb(idx, _loadTarget);
    } else if (_synth) {
        // Fallback: load directly to the stored engine reference
        Presets::presets_loadByGlobalIndex(*_synth, idx);
    }
}
