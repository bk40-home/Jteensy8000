// PresetBrowser.cpp
// =============================================================================
// Implementation of PresetBrowser — see PresetBrowser.h for design notes.
// =============================================================================

#include "PresetBrowser.h"

// =============================================================================
// open()
// =============================================================================
void PresetBrowser::open(SynthEngine* synth, int startIdx, LoadCallback loadCb) {
    _synth      = synth;
    _loadCb     = loadCb;
    _totalCount = Presets::presets_totalCount();
    _cursorIdx  = constrain(startIdx, 0, _totalCount - 1);
    // Centre the scroll window on the starting preset
    _scrollTop  = _clampScrollTop(_cursorIdx - PBLayout::VISIBLE_ROWS / 2);
    _open       = true;
    _dirty      = true;  // force full redraw on first draw()
}

void PresetBrowser::close() {
    _open  = false;
    _dirty = false;
}

bool PresetBrowser::isOpen()   const { return _open;       }
int  PresetBrowser::selected() const { return _cursorIdx;  }

// =============================================================================
// draw()
// Only the header and footer are truly static; they are drawn on full-dirty.
// Partial redraws update only the two rows that changed (old cursor, new cursor).
// =============================================================================
void PresetBrowser::draw(ILI9341_t3n& tft) {
    if (!_open) return;

    if (_dirty) {
        // Full redraw — header, footer, all visible rows
        _drawHeader(tft);
        _drawFooter(tft);
        for (int r = 0; r < PBLayout::VISIBLE_ROWS; ++r) _drawRow(tft, r);
        _dirty      = false;
        _prevCursor = _cursorIdx;
        _prevScroll = _scrollTop;
    } else if (_prevCursor != _cursorIdx || _prevScroll != _scrollTop) {
        if (_prevScroll != _scrollTop) {
            // Scroll happened — redraw all list rows
            for (int r = 0; r < PBLayout::VISIBLE_ROWS; ++r) _drawRow(tft, r);
        } else {
            // Cursor moved — redraw only old and new row (minimal SPI traffic)
            _drawRowForIdx(tft, _prevCursor);
            _drawRowForIdx(tft, _cursorIdx);
        }
        _prevCursor = _cursorIdx;
        _prevScroll = _scrollTop;
    }
}

// =============================================================================
// onEncoder() — scroll cursor; auto-scroll list to keep cursor visible
// =============================================================================
void PresetBrowser::onEncoder(int delta) {
    if (!_open) return;
    _prevCursor = _cursorIdx;
    _prevScroll = _scrollTop;

    _cursorIdx = (_cursorIdx + delta + _totalCount) % _totalCount;

    // Auto-scroll: keep cursor inside visible window
    if (_cursorIdx < _scrollTop) {
        _scrollTop = _cursorIdx;
    } else if (_cursorIdx >= _scrollTop + PBLayout::VISIBLE_ROWS) {
        _scrollTop = _cursorIdx - PBLayout::VISIBLE_ROWS + 1;
    }
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

    // PREV button (footer, left)
    if (tx >= 4 && tx < (int)(4 + PBLayout::BTN_W) &&
        ty >= PBLayout::FTR_Y && ty < (int)(PBLayout::FTR_Y + PBLayout::FTR_H)) {
        _prevCursor = _cursorIdx;
        _prevScroll = _scrollTop;
        _scrollTop  = _clampScrollTop(_scrollTop - PBLayout::VISIBLE_ROWS);
        // Move cursor to top of new page if it slid off-screen
        if (_cursorIdx < _scrollTop ||
            _cursorIdx >= _scrollTop + PBLayout::VISIBLE_ROWS) {
            _cursorIdx = _scrollTop;
        }
        _dirty = true;
        return true;
    }

    // NEXT button (footer, right)
    const int nextBtnX = PBLayout::W - 4 - PBLayout::BTN_W;
    if (tx >= nextBtnX && tx < (int)(nextBtnX + PBLayout::BTN_W) &&
        ty >= PBLayout::FTR_Y && ty < (int)(PBLayout::FTR_Y + PBLayout::FTR_H)) {
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

    // List row tap
    if (ty >= PBLayout::LIST_Y && ty < (int)(PBLayout::LIST_Y + PBLayout::LIST_H)) {
        const int row = (ty - PBLayout::LIST_Y) / PBLayout::ROW_H;
        const int idx = _scrollTop + row;
        if (idx >= 0 && idx < _totalCount) {
            if (idx == _cursorIdx) {
                // Second tap on same row = confirm load
                _loadPreset(idx);
                close();
            } else {
                // First tap = move cursor only
                _prevCursor = _cursorIdx;
                _prevScroll = _scrollTop;
                _cursorIdx  = idx;
            }
        }
        return true;
    }

    return true;  // consume all touches while open (block parent screen)
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

    // CANCEL button
    tft.fillRect(PBLayout::CANCEL_X, PBLayout::CANCEL_Y,
                 PBLayout::CANCEL_W, PBLayout::CANCEL_H, PBColour::CANCEL_BG);
    tft.setTextColor(PBColour::BTN_TEXT, PBColour::CANCEL_BG);
    tft.setCursor(PBLayout::CANCEL_X + 8, PBLayout::CANCEL_Y + 5);
    tft.print("CANCEL");
}

void PresetBrowser::_drawFooter(ILI9341_t3n& tft) {
    tft.fillRect(0, PBLayout::FTR_Y, PBLayout::W, PBLayout::FTR_H, PBColour::FTR_BG);
    tft.drawFastHLine(0, PBLayout::FTR_Y, PBLayout::W, PBColour::BORDER);

    // PREV button
    tft.fillRect(4, PBLayout::FTR_Y + 2, PBLayout::BTN_W, PBLayout::BTN_H, PBColour::BTN_BG);
    tft.setTextColor(PBColour::BTN_TEXT, PBColour::BTN_BG);
    tft.setCursor(14, PBLayout::FTR_Y + 8);
    tft.print("< PREV");

    // NEXT button
    const int nextX = PBLayout::W - 4 - PBLayout::BTN_W;
    tft.fillRect(nextX, PBLayout::FTR_Y + 2, PBLayout::BTN_W, PBLayout::BTN_H, PBColour::BTN_BG);
    tft.setTextColor(PBColour::BTN_TEXT, PBColour::BTN_BG);
    tft.setCursor(nextX + 10, PBLayout::FTR_Y + 8);
    tft.print("NEXT >");

    // Page counter (centred)
    const int page    = _scrollTop / PBLayout::VISIBLE_ROWS;
    const int maxPage = (_totalCount - 1) / PBLayout::VISIBLE_ROWS;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d / %d", page + 1, maxPage + 1);
    tft.setTextColor(PBColour::IDX_TEXT, PBColour::FTR_BG);
    tft.setCursor(PBLayout::W / 2 - 20, PBLayout::FTR_Y + 8);
    tft.print(buf);
}

void PresetBrowser::_drawRow(ILI9341_t3n& tft, int row) {
    const int      idx  = _scrollTop + row;
    const uint16_t y    = PBLayout::LIST_Y + row * PBLayout::ROW_H;
    const bool     isSel = (idx == _cursorIdx);

    // Background — selected / alternating zebra
    uint16_t bg;
    if (isSel) {
        bg = PBColour::SEL_BG;
    } else {
        bg = (row & 1) ? PBColour::ROW_ALT : PBColour::ROW_BG;
    }
    tft.fillRect(0, y, PBLayout::W, PBLayout::ROW_H, bg);

    if (idx < 0 || idx >= _totalCount) return;  // empty slot past end

    // Cursor arrow
    tft.setTextColor(isSel ? COLOUR_SYSTEXT : bg, bg);  // amber cursor arrow
    tft.setCursor(2, y + 8);
    tft.print(isSel ? ">" : " ");

    // Index — templates: "T0".."T8", bank patches: "00".."31"
    tft.setTextColor(PBColour::IDX_TEXT, bg);
    tft.setCursor(12, y + 8);
    char idxBuf[5];
    const int templateCount = Presets::presets_templateCount();
    if (idx < templateCount) {
        snprintf(idxBuf, sizeof(idxBuf), "T%d ", idx);
    } else {
        snprintf(idxBuf, sizeof(idxBuf), "%02d ", idx - templateCount);
    }
    tft.print(idxBuf);

    // Preset name
    const char* name = Presets::presets_nameByGlobalIndex(idx);
    tft.setTextColor(isSel ? PBColour::SEL_TEXT : PBColour::ROW_TEXT, bg);
    tft.setCursor(46, y + 8);
    tft.print(name ? name : "---");

    // Subtle row divider
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
        _loadCb(idx);
    } else if (_synth) {
        Presets::presets_loadByGlobalIndex(*_synth, idx);
    }
}
