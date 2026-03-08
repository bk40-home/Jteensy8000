// TFTWidgets.h
// =============================================================================
// Combined TFT UI widget library for the JT-8000 synthesizer.
//
// Contains all widget class DECLARATIONS. Implementations live in TFTWidgets.cpp.
//
// Class hierarchy:
//   TFTWidget          — abstract base (dirty-flag, hit-test, draw guards)
//     TFTButton        — touchable label button with press flash
//     TFTRadioGroup    — horizontal radio selector (per-cell dirty repaints)
//     TFTParamRow      — parameter row: name, value bar, value text
//     TFTSectionTile   — home-screen section tile with accent bar
//
// Standalone classes (no TFTWidget base):
//   TFTScreen          — container managing up to MAX_WIDGETS per page
//   TFTScreenManager   — navigation stack + embedded TFTNumericEntry overlay
//   TFTNumericEntry    — full-screen keypad or list picker value editor
//
// Design rules (enforced everywhere):
//   - No heap allocation (no new/delete, no Arduino String)
//   - Never call fillScreen() inside a widget — clear own rect only
//   - Only draw when _dirty is true (cleared after each draw)
//   - All string buffers are fixed-size stack arrays
//   - Audio must never be blocked — doDraw() must complete in < 1 ms
// =============================================================================

#pragma once
#include <Arduino.h>
#include "ILI9341_t3n.h"
#include "JT8000Colours.h"
#include "JT8000_Sections.h"

// ─────────────────────────────────────────────────────────────────────────────
// CAPACITY CONSTANTS
// ─────────────────────────────────────────────────────────────────────────────
static constexpr int MAX_WIDGETS        = 16;   // widgets per TFTScreen
static constexpr int RADIO_MAX_OPTIONS  = 12;   // options per TFTRadioGroup
static constexpr int SCREEN_STACK_DEPTH = 4;    // push/pop nesting depth
static constexpr int ENTRY_MAX_DIGITS   = 7;    // typed digits in numeric entry
static constexpr int ENTRY_MAX_ENUM     = 64;   // options in list picker
static constexpr int ENTRY_TITLE_LEN    = 24;   // title bar string length
static constexpr int ENTRY_UNIT_LEN     = 8;    // unit string length (Hz, ms …)
static constexpr int PROW_NAME_LEN      = 12;   // param-row name buffer
static constexpr int PROW_VAL_LEN       = 16;   // param-row value + unit string


// =============================================================================
// TFTTheme — centralised colour constants
//
// All widgets reference gTheme so the whole UI can be recoloured from one
// place.  Colours are RGB565 (same as ILI9341_t3n).
// =============================================================================
struct TFTTheme {
    // ---- Backgrounds ----
    uint16_t bg            = COLOUR_BACKGROUND; // screen / widget background
    uint16_t headerBg      = COLOUR_HEADER_BG;  // header / footer bar
    uint16_t selectedBg    = COLOUR_SELECTED;   // selected-row highlight (amber)

    // ---- Text ----
    uint16_t textNormal    = COLOUR_TEXT;       // standard text
    uint16_t textDim       = COLOUR_TEXT_DIM;   // secondary / hint text
    uint16_t textOnSelect  = COLOUR_BACKGROUND; // text drawn on amber selected row (must be dark)

    // ---- Borders ----
    uint16_t border        = COLOUR_BORDER;     // separator lines

    // ---- Widget chrome ----
    uint16_t radioBorder   = 0xC618;            // radio button outline (pure grey — same in RGB/BGR)
    uint16_t radioFill     = COLOUR_SELECTED;   // radio button filled indicator (amber)
    uint16_t buttonNormal  = 0x59E5;            // #283C5F dark blue-grey (BGR565)
    uint16_t buttonPress   = COLOUR_ACCENT;     // pressed button flash (red)
    uint16_t buttonText    = COLOUR_TEXT;       // button label text
    uint16_t accent        = COLOUR_ACCENT;     // alerts, cancel button (red)

    // ---- Value bar ----
    uint16_t barTrack      = COLOUR_BORDER;     // empty track behind value bar

    // ---- Keypad / numeric entry ----
    uint16_t keyBg         = 0x51A5;            // #283755 key bg — BGR565 pre-swapped
    uint16_t keyText       = COLOUR_TEXT;       // numpad key label text
    uint16_t keyBorder     = 0x7A87;            // #3A5078 key divider — BGR565
    uint16_t keyConfirm    = 0x2C02;            // #148228 confirm green — BGR565
    uint16_t keyCancel     = COLOUR_ACCENT;     // cancel key (red)
    uint16_t keyBackspace  = 0x5A48;            // #464B5A backspace grey — BGR565
    uint16_t entryBg       = 0x1861;            // #0A0F1E entry field bg — BGR565
    uint16_t entryText     = 0xFFFF;            // #FFFFFF entered digits white
};

// Global theme instance — defined once in TFTWidgets.cpp.
// Widgets reference this; never carry their own copies.
extern TFTTheme gTheme;


// =============================================================================
// TFTWidget — abstract base class
// =============================================================================
class TFTWidget {
public:
    // -------------------------------------------------------------------------
    // Position and size are immutable after construction.
    // Call setDisplay() before any draw() or touch calls.
    // -------------------------------------------------------------------------
    TFTWidget(int16_t x, int16_t y, int16_t w, int16_t h);
    virtual ~TFTWidget() = default;

    // Bind display hardware — must be called before draw()
    void setDisplay(ILI9341_t3n* display);

    // draw() guards: only calls doDraw() when dirty + visible + display set
    void draw();

    void markDirty();               // schedule repaint next draw()
    bool isDirty()   const;
    bool isVisible() const;
    void setVisible(bool v);

    // Touch — return true if event was consumed
    virtual bool onTouch(int16_t x, int16_t y);
    virtual void onTouchRelease(int16_t /*x*/, int16_t /*y*/) {}

    // Geometry
    int16_t getX() const;
    int16_t getY() const;
    int16_t getW() const;
    int16_t getH() const;
    bool    hitTest(int16_t x, int16_t y) const;

protected:
    // Subclass paint implementation — only called when dirty
    virtual void doDraw() = 0;

    // Convenience helpers
    void clearRect(uint16_t colour);

    // Draw text centred in widget rect at vertical offset dy
    void drawTextCentred(const char* text, uint16_t colour,
                         uint8_t fontSize = 1, int16_t dy = 0);

    // Draw text at absolute screen coordinates
    void drawTextAt(int16_t lx, int16_t ly, const char* text,
                    uint16_t colour, uint8_t fontSize = 1);

    // Draw text right-aligned so the rightmost pixel is at rx
    void drawTextRight(int16_t rx, int16_t ly, const char* text,
                       uint16_t colour, uint8_t fontSize = 1);

    int16_t      _x, _y, _w, _h;   // bounding rect
    bool         _dirty;            // true → needs repaint this frame
    bool         _visible;
    ILI9341_t3n* _display;          // borrowed pointer — never freed here
};


// =============================================================================
// TFTButton — touchable label button
//
// Press flashes to buttonPress colour. Callback fires on release inside bounds
// (not on press) so a finger slide out cancels the action.
// =============================================================================
class TFTButton : public TFTWidget {
public:
    enum Style : uint8_t {
        STYLE_NORMAL  = 0,  // gTheme.buttonNormal
        STYLE_CONFIRM = 1,  // gTheme.keyConfirm  (green)
        STYLE_CANCEL  = 2,  // gTheme.keyCancel   (red)
    };

    using Callback = void (*)();

    TFTButton(int16_t x, int16_t y, int16_t w, int16_t h,
              const char* label, Style style = STYLE_NORMAL);

    // label must outlive the widget — pointer is stored, not copied
    void setLabel(const char* label);
    void setCallback(Callback cb);

    // Programmatic press (e.g. encoder mapped to button)
    void triggerCallback();

    bool onTouch(int16_t x, int16_t y)                override;
    void onTouchRelease(int16_t x, int16_t y)          override;

protected:
    void doDraw() override;

private:
    const char* _label;
    Style       _style;
    bool        _pressed;
    Callback    _callback;
};


// =============================================================================
// TFTRadioGroup — horizontal radio-button selector
//
// Per-cell dirty flags: only changed cells are repainted, minimising SPI traffic.
// =============================================================================
class TFTRadioGroup : public TFTWidget {
public:
    using Callback = void (*)(int index);

    TFTRadioGroup(int16_t x, int16_t y, int16_t w, int16_t h);

    // labels[] pointers must outlive this widget
    void setOptions(const char* const* labels, int count);

    // setSelected() marks only the two changed cells dirty
    // fireCallback: true → fire the callback as well as updating state
    void setSelected(int index, bool fireCallback = false);

    int  getSelected()     const;
    void setCallback(Callback cb);

    bool onTouch(int16_t x, int16_t y) override;

protected:
    void doDraw() override;

private:
    int         _numOptions;
    int         _selected;          // -1 = nothing selected yet
    const char* _labels[RADIO_MAX_OPTIONS];
    bool        _optionDirty[RADIO_MAX_OPTIONS]; // per-cell dirty flags
    Callback    _callback;
};


// =============================================================================
// TFTParamRow — single tappable parameter row
//
// Layout:
//   │  Name                         Value text  ›  │
//   │  ████████████░░░░░░░░░░░░  ← 12px value bar  │
//
// setValue() only marks dirty when value or text actually changes.
// Callback fires with the CC number so the parent can open an entry overlay.
// =============================================================================
class TFTParamRow : public TFTWidget {
public:
    using TapCallback = void (*)(uint8_t cc);

    // cc 255 = empty slot (draws "---", not tappable)
    TFTParamRow(int16_t x, int16_t y, int16_t w, int16_t h,
                uint8_t cc, const char* name, uint16_t colour);

    void setCallback(TapCallback cb);

    // Update displayed value; only repaints if something changed
    // text nullptr → format rawValue as integer
    void setValue(uint8_t rawValue, const char* text);

    // Reconfigure row for a different CC without reconstruction
    void configure(uint8_t cc, const char* name, uint16_t colour);

    void    setSelected(bool sel);
    bool    isSelected() const;
    uint8_t getCC()      const;

    bool onTouch(int16_t x, int16_t y) override;

protected:
    void doDraw() override;

private:
    uint8_t     _cc;
    uint16_t    _colour;
    bool        _selected;
    uint8_t     _rawValue;
    char        _name[PROW_NAME_LEN];
    char        _valText[PROW_VAL_LEN];
    TapCallback _onTap;
};


// =============================================================================
// TFTKnob — circular rotary knob widget
//
// Draws a filled arc indicator (270° sweep, -135° to +135°) around a circle.
// Label text below the knob circle; value text below the label.
// Touch anywhere in the bounding rect opens the entry overlay via callback.
//
// Visual (52px knob on ~70×80px bounding rect):
//   ┌──────────┐
//   │  ╭───╮   │
//   │  │ ● │   │  ← arc sweeps from 7 o'clock to 5 o'clock
//   │  ╰───╯   │
//   │  Label   │
//   │  Value   │
//   └──────────┘
// =============================================================================
class TFTKnob : public TFTWidget {
public:
    using TapCallback = void (*)(uint8_t cc);

    static constexpr int16_t KNOB_D   = 44;   // knob circle diameter (px)
    static constexpr int16_t KNOB_R   = KNOB_D / 2;
    static constexpr int16_t ARC_W    = 3;    // arc stroke width (px)

    // cc 255 = empty slot
    TFTKnob(int16_t x, int16_t y, int16_t w, int16_t h,
            uint8_t cc, const char* name, uint16_t colour);

    void setCallback(TapCallback cb);

    // Update value; repaints only when changed
    void setValue(uint8_t rawValue, const char* text);

    // Reconfigure for a different CC without reconstruction
    void configure(uint8_t cc, const char* name, uint16_t colour);

    void    setSelected(bool sel);
    bool    isSelected() const;
    uint8_t getCC()      const;

    bool onTouch(int16_t x, int16_t y) override;

protected:
    void doDraw() override;

private:
    void _drawArc(int16_t cx, int16_t cy, float angleDeg, uint16_t col);

    uint8_t     _cc;
    uint16_t    _colour;
    bool        _selected;
    uint8_t     _rawValue;
    char        _name[PROW_NAME_LEN];
    char        _valText[PROW_VAL_LEN];
    TapCallback _onTap;
};


// =============================================================================
// TFTSlider — horizontal slider widget
//
// Full-width horizontal bar with a draggable thumb. Suited to ADSR time/level
// parameters where 4 in a group should be comparable side-by-side.
//
// Layout (full screen width, ~44px tall):
//   │ Label  [████████░░░░░░░] Value │
//   y centred on track
//
// Touch on the track area adjusts value directly (touch-to-set).
// Tap on label area opens the numeric entry overlay.
// =============================================================================
class TFTSlider : public TFTWidget {
public:
    using TapCallback = void (*)(uint8_t cc);

    static constexpr int16_t LABEL_W  = 52;   // label column width (px)
    static constexpr int16_t VAL_W    = 52;   // value text column width (px)
    static constexpr int16_t TRACK_H  = 8;    // slider track height (px)
    static constexpr int16_t THUMB_W  = 6;    // thumb width (px)
    static constexpr int16_t THUMB_H  = 20;   // thumb height (px)

    TFTSlider(int16_t x, int16_t y, int16_t w, int16_t h,
              uint8_t cc, const char* name, uint16_t colour);

    void setCallback(TapCallback cb);

    void setValue(uint8_t rawValue, const char* text);
    void configure(uint8_t cc, const char* name, uint16_t colour);

    void    setSelected(bool sel);
    bool    isSelected() const;
    uint8_t getCC()      const;

    // Returns new rawValue if touch was on the track (0-127), -1 otherwise
    int16_t trackTouchValue(int16_t tx) const;

    bool onTouch(int16_t x, int16_t y) override;

protected:
    void doDraw() override;

private:
    int16_t _trackX() const;  // left edge of track region
    int16_t _trackW() const;  // width of track region

    uint8_t     _cc;
    uint16_t    _colour;
    bool        _selected;
    uint8_t     _rawValue;
    char        _name[PROW_NAME_LEN];
    char        _valText[PROW_VAL_LEN];
    TapCallback _onTap;
};


// =============================================================================
// TFTSectionTile — home-screen section tile
//
// Visual:
//   ┌────────────┐
//   ║ accent bar ║  ← 2 px accent colour
//   │  LABEL     │  ← section colour text
//   │  4p        │  ← page count hint
//   └────────────┘
//
// Fires callback on touch-release (same contract as TFTButton).
// =============================================================================
class TFTSectionTile : public TFTWidget {
public:
    using Callback = void (*)();

    TFTSectionTile(int16_t x, int16_t y, int16_t w, int16_t h,
                   const SectionDef& section);

    void setCallback(Callback cb);
    void activate();    // programmatic trigger (encoder press)

    bool onTouch(int16_t x, int16_t y)       override;
    void onTouchRelease(int16_t x, int16_t y) override;

protected:
    void doDraw() override;

private:
    const SectionDef& _section;
    bool              _pressed;
    Callback          _callback;
};


// =============================================================================
// TFTNumericEntry — full-screen value editor
//
// Two modes:
//   MODE_NUMBER — 10-key numeric pad with value display box
//   MODE_ENUM   — scrollable list picker
//
// isOpen() returns false once the user confirms or cancels.
// The callback fires ONLY on Confirm (not Cancel).
// =============================================================================
class TFTNumericEntry {
public:
    using Callback = void (*)(int value);

    enum Mode : uint8_t { MODE_CLOSED = 0, MODE_NUMBER, MODE_ENUM };

    TFTNumericEntry();

    void setDisplay(ILI9341_t3n* d);

    // Open numeric keypad
    // title, unit — must outlive the call (stored by pointer, not copied)
    void openNumeric(const char* title, const char* unit,
                     int minVal, int maxVal, int currentVal, Callback cb);

    // Open list picker
    // labels[] array must outlive the call
    void openEnum(const char* title, const char* const* labels, int count,
                  int currentIdx, Callback cb);

    // Call every frame while isOpen()
    void draw();

    // Route all touches here while isOpen() — consumes every touch
    bool onTouch(int16_t x, int16_t y);

    // Scroll the enum list by delta steps (positive = scroll down / later items).
    // No-op when mode is not MODE_ENUM.  Routes directly without going through
    // SectionScreen so the encoder works while the list is open.
    void onEncoderDelta(int delta);

    bool isOpen()  const;
    Mode getMode() const;
    void close();           // close without firing callback (Cancel)

private:
    // ---- Layout (320×240 screen) ----
    static constexpr int SW       = 320;
    static constexpr int SH       = 240;
    static constexpr int TB_Height     = 30;         // title bar height
    static constexpr int VB_Y     = TB_Height + 4;   // value box top
    static constexpr int VB_Height     = 36;         // value box height
    static constexpr int KP_Y     = VB_Y + VB_Height + 8;  // keypad top
    static constexpr int KP_X     = 10;
    static constexpr int KP_Width     = 300;
    static constexpr int KEY_Width    = 94;
    static constexpr int KEY_Height    = 36;
    static constexpr int KEY_GAP  = 4;
    static constexpr int BR_Y     = KP_Y + 3 * (KEY_Height + KEY_GAP);
    // Bottom row without sign key (minVal >= 0): [0:90] [<-:90] [OK:106]
    static constexpr int BR0_Width    = 90;
    static constexpr int BRBK_Width   = 90;
    static constexpr int BRCO_Width   = 106;
    // Bottom row with sign key (minVal < 0): [0:60] [±:60] [<-:72] [OK:96]
    static constexpr int BRS_0_Width  = 60;   // "0" when sign key present
    static constexpr int BRS_S_Width  = 60;   // "±" sign toggle key
    static constexpr int BRS_BK_Width = 72;   // "<-" backspace (sign row)
    static constexpr int BRS_CO_Width = 96;   // "OK" confirm (sign row)
    static constexpr int CANCEL_X = 240;
    static constexpr int CANCEL_Y = 4;
    static constexpr int CANCEL_Width = 75;
    static constexpr int CANCEL_Height = 22;
    static constexpr int EN_ROW_Height = 32;
    static constexpr int EN_ROWS  = (SH - TB_Height - 40) / EN_ROW_Height;
    static constexpr int EN_BTN_Y = SH - 36;

    // ---- Draw helpers ----
    void _drawFull();
    void _drawCancelButton(bool pressed);
    void _drawValueBox();
    void _drawKeypad();
    void _drawKey(int16_t kx, int16_t ky, int16_t kw, int16_t kh,
                  const char* label, uint16_t bgCol, bool pressed);
    static const char* _digitStr(int d);    // digit int → "0".."9"

    // ---- Touch handlers ----
    void _handleNumericTouch(int16_t x, int16_t y);
    void _handleEnumTouch(int16_t x, int16_t y);

    // ---- Numeric actions ----
    void _appendDigit(int d);
    void _backspace();
    void _toggleSign();   // flip negative flag; only active when minVal < 0
    void _confirm();

    // ---- Enum helpers ----
    void _drawEnumList();
    void _drawEnumButtons();
    void _scrollToSelection();

    // ---- Members ----
    ILI9341_t3n* _display;
    Mode         _mode;

    int  _minVal, _maxVal, _currentVal;
    char _digitBuf[ENTRY_MAX_DIGITS];
    int  _digitCount;
    bool _editing;      // false = showing hint; true = user has typed
    bool _negative;     // true when the entered value should be negated

    int         _selectedEnum;
    int         _numEnumOptions;
    const char* _enumLabels[ENTRY_MAX_ENUM];
    int         _scrollOffset;

    char     _titleBuf[ENTRY_TITLE_LEN];
    char     _unitBuf[ENTRY_UNIT_LEN];
    Callback _callback;

    bool _fullRedraw;
    bool _valueDirty;
};


// =============================================================================
// TFTScreen — widget container for one UI page
//
// Manages up to MAX_WIDGETS, routes touch events, propagates display pointer.
// draw() cost is proportional to dirty widget count, not total widget count.
// =============================================================================
class TFTScreen {
public:
    TFTScreen();

    void setDisplay(ILI9341_t3n* display);
    void setBackground(uint16_t colour);

    // Register widget — returns false if full
    bool addWidget(TFTWidget* widget);

    // Force all widgets to repaint next draw()
    void markAllDirty();

    // Clear screen then mark all dirty (use when switching to this screen)
    void clearAndRedraw();

    // Repaint all dirty widgets
    void draw();

    // Route touch to first widget that claims it
    bool onTouch(int16_t x, int16_t y);

    // Propagate release to all widgets (buttons restore normal appearance)
    void onTouchRelease(int16_t x, int16_t y);

    int numWidgets() const;

private:
    ILI9341_t3n* _display;
    TFTWidget*   _widgets[MAX_WIDGETS];
    int          _numWidgets;
    uint16_t     _bgColour;
};


// =============================================================================
// TFTScreenManager — navigation stack + numeric entry overlay
//
// push() opens a sub-screen; pop() returns to the previous one.
// Only the top screen draws and receives touches.
// TFTNumericEntry overlay is integrated: while open it takes priority over
// the stack.
// =============================================================================
class TFTScreenManager {
public:
    TFTScreenManager();

    void setDisplay(ILI9341_t3n* display);

    // Push / pop — return false on stack overflow/underflow
    bool push(TFTScreen* screen);
    bool pop();

    TFTScreen* topScreen();
    int        stackDepth() const;

    // Access integrated numeric entry overlay
    TFTNumericEntry& numericEntry();
    bool             isEntryOpen() const;

    // Call each frame: routes draw, touch, and release to active screen
    // or entry overlay, whichever takes priority
    void update(bool newTouch, int16_t tx, int16_t ty,
                bool newRelease = false, int16_t rx = 0, int16_t ry = 0);

private:
    ILI9341_t3n*    _display;
    TFTScreen*      _stack[SCREEN_STACK_DEPTH];
    int             _stackDepth;
    TFTNumericEntry _numericEntry; // one shared overlay; only one open at a time
};
