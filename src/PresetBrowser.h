#pragma once
// =============================================================================
// PresetBrowser.h — Full-screen preset list with layer target selector
// =============================================================================
//
// Updated for dual-layer support:
//   - Load target: A / B / Both — selects which layer receives the preset.
//   - Target buttons in footer replace the old two-button layout.
//   - Header shows current target as visual feedback.
//   - All existing behaviour preserved: encoder scroll, tap-to-select,
//     double-tap-to-confirm, PREV/NEXT paging, CANCEL.
//
// Layout (320 × 240 px):
//   ┌──────────────────────────────────────┐
//   │  PRESET BROWSER  [→ A]     [CANCEL]  │  ← header  28 px
//   ├──────────────────────────────────────┤
//   │► 00 PORTAPAD                         │  ← selected (highlighted)
//   │  01 CHROME PD                        │
//   │  02 LYRE                             │
//   │  03 WISHFISH                         │  ← 7 visible rows × 26 px
//   │  04 PULSAR                           │
//   │  05 TAPESTORM                        │
//   │  06 TIBEPIUM                         │
//   ├──────────────────────────────────────┤
//   │ [◄PV] [A] [B] [AB] [NX►]            │  ← footer  30 px
//   └──────────────────────────────────────┘
//
// Interaction (new):
//   Tap [A] / [B] / [AB]  → set load target layer
//   Encoder R              → cycle target: A → B → AB → A
//   (all other interactions unchanged from original)
// =============================================================================

#include "ILI9341_t3n.h"
#include "Presets.h"
#include "SynthEngine.h"
#include "JT8000Colours.h"

// Forward declaration — avoids circular include with LayerManager.h
enum class EditTarget : uint8_t;

// ─────────────────────────────────────────────────────────────────────────────
// Layout constants
// ─────────────────────────────────────────────────────────────────────────────
namespace PBLayout {
    static constexpr uint16_t W            = 320;
    static constexpr uint16_t H            = 240;
    static constexpr uint16_t HDR_H        = 28;
    static constexpr uint16_t FTR_H        = 30;
    static constexpr uint16_t ROW_H        = 26;
    static constexpr int      VISIBLE_ROWS = 7;
    static constexpr uint16_t LIST_Y       = HDR_H;
    static constexpr uint16_t LIST_H       = VISIBLE_ROWS * ROW_H;   // 182 px
    static constexpr uint16_t FTR_Y        = LIST_Y + LIST_H;        // 210
    static constexpr uint16_t BTN_H        = FTR_H - 4;

    // Footer button widths and positions (5 buttons across 320 px)
    //   [◄PV 52px] [A 40px] [B 40px] [AB 44px] [NX► 52px]  + gaps
    static constexpr uint16_t PREV_X       = 4;
    static constexpr uint16_t PREV_W       = 52;
    static constexpr uint16_t TGT_A_X      = 62;
    static constexpr uint16_t TGT_A_W      = 40;
    static constexpr uint16_t TGT_B_X      = 108;
    static constexpr uint16_t TGT_B_W      = 40;
    static constexpr uint16_t TGT_AB_X     = 154;
    static constexpr uint16_t TGT_AB_W     = 44;
    static constexpr uint16_t NEXT_X       = W - 52 - 4;
    static constexpr uint16_t NEXT_W       = 52;

    // Header cancel button
    static constexpr uint16_t CANCEL_W     = 70;
    static constexpr uint16_t CANCEL_X     = W - CANCEL_W - 4;
    static constexpr uint16_t CANCEL_Y     = 2;
    static constexpr uint16_t CANCEL_H     = HDR_H - 4;
}

// ─────────────────────────────────────────────────────────────────────────────
// Colour palette — standard RGB565 via JT8000Colours.h
// ─────────────────────────────────────────────────────────────────────────────
namespace PBColour {
    static constexpr uint16_t BG        = COLOUR_BACKGROUND;       // #101428  deep navy
    static constexpr uint16_t HDR_BG    = COLOUR_HEADER_BG;        // #19233C  dark navy panel
    static constexpr uint16_t HDR_TEXT  = COLOUR_TEXT;              // #D2D7E1  warm off-white
    static constexpr uint16_t ROW_BG    = COLOUR_SURFACE;          // #111620  section body fill
    static constexpr uint16_t ROW_ALT   = COLOUR_SURFACE2;         // #161C28  alternating row
    static constexpr uint16_t SEL_BG    = COLOUR_ACCENT_ORANGE;    // #FFA000  selected row
    static constexpr uint16_t SEL_TEXT  = COLOUR_BACKGROUND;       // #101428  dark text on orange
    static constexpr uint16_t ROW_TEXT  = COLOUR_TEXT;              // #D2D7E1  standard text
    static constexpr uint16_t IDX_TEXT  = COLOUR_TEXT_DIM;          // #787D8C  steel grey index
    static constexpr uint16_t FTR_BG    = COLOUR_HEADER_BG;        // #19233C  footer panel
    static constexpr uint16_t BTN_BG    = COLOUR_SURFACE3;         // #1C2436  button resting bg
    static constexpr uint16_t BTN_TEXT  = COLOUR_TEXT;              // #D2D7E1  button label
    static constexpr uint16_t BTN_SEL   = COLOUR_ACCENT_ORANGE;    // #FFA000  active target
    static constexpr uint16_t CANCEL_BG = COLOUR_ACCENT_RED;       // #FF1C18  cancel (functional)
    static constexpr uint16_t BORDER    = COLOUR_BORDER;           // #2D3750  blue-grey borders
}

// =============================================================================
class PresetBrowser {
public:
    // Callback: called when user confirms a selection.
    // globalIndex = preset index, target = which layer to load to.
    using LoadCallback = void(*)(int globalIndex, EditTarget target);

    // Legacy callback (no target) — for backward compatibility
    using LoadCallbackLegacy = void(*)(int globalIndex);

    PresetBrowser() = default;

    // -------------------------------------------------------------------------
    // open() — show the browser.
    // startIdx    — currently loaded patch index (pre-selects cursor)
    // loadCb      — called on confirm with (index, target)
    // editTarget  — initial load target (default LAYER_A)
    // -------------------------------------------------------------------------
    void open(SynthEngine* synth, int startIdx = 0,
              LoadCallback loadCb = nullptr,
              EditTarget editTarget = (EditTarget)0);

    void close();

    bool isOpen()   const;
    int  selected() const;

    // Current load target
    EditTarget getLoadTarget() const { return _loadTarget; }
    void       setLoadTarget(EditTarget t);

    // Cycle load target: A → B → Both → A
    void cycleLoadTarget();

    // Call every frame while isOpen() — only repaints changed rows
    void draw(ILI9341_t3n& tft);

    // L encoder: delta = +1 (down) or -1 (up); wraps around
    void onEncoder(int delta);

    // R encoder: cycle load target
    void onEncoderRight(int delta);

    // Confirm selected preset and close
    void onEncoderPress();

    // Returns true if the browser consumed the touch
    bool onTouch(int tx, int ty);

private:
    // ---- Draw helpers ----
    void _drawHeader(ILI9341_t3n& tft);
    void _drawFooter(ILI9341_t3n& tft);
    void _drawRow(ILI9341_t3n& tft, int row);
    void _drawRowForIdx(ILI9341_t3n& tft, int idx);

    // ---- Helpers ----
    int  _clampScrollTop(int st) const;
    void _loadPreset(int idx);
    const char* _targetLabel() const;

    // ---- Members ----
    SynthEngine*  _synth       = nullptr;
    LoadCallback  _loadCb      = nullptr;
    bool          _open        = false;
    bool          _dirty       = false;
    bool          _footerDirty = false;     // redraw footer only (target change)
    int           _totalCount  = 0;
    int           _cursorIdx   = 0;
    int           _scrollTop   = 0;
    int           _prevCursor  = -1;
    int           _prevScroll  = -1;
    EditTarget    _loadTarget  = (EditTarget)0;  // LAYER_A
};
