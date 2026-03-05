// UIManager_TFT.cpp
// =============================================================================
// Implementation of UIManager_TFT — see UIManager_TFT.h for design notes.
// =============================================================================

#include "UIManager_TFT.h"
#include "DebugTrace.h"
#include <math.h>

// Static singleton pointer — set in begin()
UIManager_TFT* UIManager_TFT::_instance = nullptr;

// =============================================================================
// Constructor
// =============================================================================
UIManager_TFT::UIManager_TFT()
    : _display(TFT_CS, TFT_DC, TFT_RST, TFT_MOSI, TFT_SCK, TFT_MISO)
    , _touchOk(false)
    , _mode(Mode::HOME)
    , _activeSect(-1)
    , _lastFrame(0)
    , _synthRef(nullptr)
    , _currentPresetIdx(0)
    , _scopeFullFirstFrame(true)
    , _fsPeakSmooth(0.0f)
{}

// =============================================================================
// beginDisplay() — hardware-only: SPI init, splash, touch init.
// Call BEFORE AudioMemory() to avoid DMA bus conflicts at startup.
// =============================================================================
void UIManager_TFT::beginDisplay() {
    _display.begin(SPI_CLOCK_HZ);
    _display.setRotation(3);       // landscape 320×240

    // The ILI9341 power-on default is INVON (colour inversion active).
    // Without this call every colour would display as its complement.
    // invertDisplay(false) sends the INVOFF (0x20) command once.
    _display.invertDisplay(true);

    _display.fillScreen(0x0000);

    // Touch controller init over I2C — safe before AudioMemory
    _touchOk = _touch.begin();
    JT_LOGF("[UI] Touch: %s\n", _touchOk ? "OK" : "not found");

    // Boot splash — confirms the display is working before audio starts
    _display.setTextSize(3);
    _display.setTextColor(COLOUR_SYSTEXT);   // amber (BGR-corrected)
    _display.setCursor(60, 90);
    _display.print("JT.8000");

    _display.setTextSize(1);
    _display.setTextColor(COLOUR_TEXT_DIM);  // steel grey (BGR-corrected)
    _display.setCursor(74, 124);
    _display.print("MicroDexed Edition");

    delay(800);   // long enough to read; short enough not to delay boot
    _display.fillScreen(0x0000);
}

// =============================================================================
// begin() — wire screens. Call AFTER AudioMemory() and synth initialisation.
// =============================================================================
void UIManager_TFT::begin(SynthEngine& synth) {
    _synthRef = &synth;
    _instance = this;

    // HomeScreen: display pointer, scope tap, and tile-tap callback
    _home.begin(&_display, &scopeTap,
        [](int idx) { if (_instance) _instance->_openSection(idx); });

    // SectionScreen: display pointer and back-button callback
    _section.begin(&_display);
    _section.setBackCallback(
        []() { if (_instance) _instance->_goHome(); });

    _home.markFullRedraw();
}

// =============================================================================
// updateDisplay() — rate-limited to FRAME_MS; safe to call every loop iteration.
// =============================================================================
void UIManager_TFT::updateDisplay(SynthEngine& synth) {
    _synthRef = &synth;
    const uint32_t now = millis();
    if ((now - _lastFrame) < FRAME_MS) return;
    _lastFrame = now;

    switch (_mode) {

        case Mode::HOME:
            _home.draw(synth);
            break;

        case Mode::SECTION:
            // syncFromEngine(): read CC values → update rows (cheap, no draw)
            // draw(): repaint only rows whose dirty flag is set
            _section.syncFromEngine();
            _section.draw();
            break;

        case Mode::BROWSER:
            _browser.draw(_display);
            break;

        case Mode::SCOPE_FULL:
            _drawFullScope(synth);
            break;
    }
}

// =============================================================================
// pollInputs() — high-frequency (>= 100 Hz) input poll.
// =============================================================================
void UIManager_TFT::pollInputs(HardwareInterface_MicroDexed& hw, SynthEngine& synth) {
    _synthRef = &synth;

    // Touch input — I2C poll
    if (_touchOk) {
        _touch.update();
        _handleTouch(synth);
    }

    // Encoder deltas and button presses
    using HW = HardwareInterface_MicroDexed;
    const int  dL = hw.getEncoderDelta(HW::ENC_LEFT);
    const int  dR = hw.getEncoderDelta(HW::ENC_RIGHT);
    const auto bL = hw.getButtonPress(HW::ENC_LEFT);
    const auto bR = hw.getButtonPress(HW::ENC_RIGHT);

    switch (_mode) {

        case Mode::HOME:
            if (dL)                    _home.onEncoderDelta(dL);
            if (bL == HW::PRESS_SHORT) _home.onEncoderPress();
            if (bL == HW::PRESS_LONG)  _setMode(Mode::SCOPE_FULL);
            break;

        case Mode::SECTION:
            // When entry overlay is open, left encoder scrolls the enum list.
            // SectionScreen.onEncoderLeft() handles the isEntryOpen check internally,
            // routing delta to TFTNumericEntry.onEncoderDelta() when appropriate.
            if (dL) _section.onEncoderLeft(dL);
            // Right encoder only adjusts CC values when no entry is open
            if (dR && !_section.isEntryOpen()) _section.onEncoderRight(dR);
            if (bL == HW::PRESS_SHORT) _section.onBackPress();
            if (bR == HW::PRESS_LONG)  _section.onEditPress();
            break;

        case Mode::BROWSER:
            if (dL)                    _browser.onEncoder(dL);
            if (bL == HW::PRESS_SHORT) _browser.onEncoderPress();
            // Left long or right short → cancel browser, return HOME
            if (bL == HW::PRESS_LONG || bR == HW::PRESS_SHORT) {
                _browser.close();
                _goHome();
            }
            break;

        case Mode::SCOPE_FULL:
            // Any button press returns to HOME
            if (bL != HW::PRESS_NONE || bR != HW::PRESS_NONE) _goHome();
            break;
    }
}

// =============================================================================
// syncFromEngine() — force repaint after preset load
// =============================================================================
void UIManager_TFT::syncFromEngine(SynthEngine& synth) {
    if (_mode == Mode::SECTION) _section.syncFromEngine();
    _home.markFullRedraw();
}

void UIManager_TFT::setCurrentPresetIdx(int idx)  { _currentPresetIdx = idx; }
int  UIManager_TFT::getCurrentPresetIdx()   const  { return _currentPresetIdx; }

// =============================================================================
// Private: mode switch
// =============================================================================
void UIManager_TFT::_setMode(Mode m) {
    if (m == _mode) return;
    _mode = m;
    _display.fillScreen(0x0000);
    if (m == Mode::HOME)       _home.markFullRedraw();
    if (m == Mode::SCOPE_FULL) { _scopeFullFirstFrame = true; }
}

void UIManager_TFT::_goHome() {
    _activeSect = -1;
    _setMode(Mode::HOME);
}

// =============================================================================
// Private: _openSection()
// PRESETS tile opens the browser; all other tiles open the section screen.
// =============================================================================
void UIManager_TFT::_openSection(int idx) {
    if (idx < 0 || idx >= SECTION_COUNT || !_synthRef) return;
    _activeSect = idx;
    const SectionDef& sect = kSections[idx];

    _display.fillScreen(0x0000);

    if (sectionIsBrowser(sect)) {
        // PRESETS tile — open the full-screen preset browser
        _browser.open(_synthRef, _currentPresetIdx,
            [](int globalIdx) {
                if (!_instance) return;
                Presets::presets_loadByGlobalIndex(*_instance->_synthRef, globalIdx);
                _instance->_currentPresetIdx = globalIdx;
                _instance->syncFromEngine(*_instance->_synthRef);
                _instance->_goHome();
            });
        _setMode(Mode::BROWSER);
    } else {
        // Normal synth section — open parameter screen
        _section.open(sect, *_synthRef);
        _setMode(Mode::SECTION);
    }
}

// =============================================================================
// Private: touch routing
// =============================================================================
void UIManager_TFT::_handleTouch(SynthEngine& synth) {
    const TouchInput::Gesture g  = _touch.getGesture();
    const TouchInput::Point   p  = _touch.getTouchPoint();      // lift position
    const TouchInput::Point   gs = _touch.getGestureStart();    // touch-down position

    switch (_mode) {

        case Mode::HOME:
            if (g == TouchInput::GESTURE_TAP) {
                _home.onTouch(p.x, p.y);
                if (_home.isScopeTapped()) _setMode(Mode::SCOPE_FULL);
            }
            if (g == TouchInput::GESTURE_HOLD)  _setMode(Mode::SCOPE_FULL);
            if (!_touch.isTouched())             _home.onTouchRelease(p.x, p.y);
            break;

        case Mode::SECTION:
            // TAP: open entry / select row  (use lift position — finger didn't move)
            if (g == TouchInput::GESTURE_TAP)        _section.onTouch(p.x, p.y);
            // SWIPE LEFT: back to home
            if (g == TouchInput::GESTURE_SWIPE_LEFT) _section.onBackPress();
            // SWIPE UP/DOWN: adjust the CC at the ROW WHERE THE FINGER STARTED.
            // We use gestureStart (not liftPoint) because the swipe gesture
            // begins on the parameter row and ends above/below it.  Using the
            // lift position would miss the row completely for fast swipes.
            if (g == TouchInput::GESTURE_SWIPE_UP)
                _section.onSwipeAdjust(gs.x, gs.y, +1);
            if (g == TouchInput::GESTURE_SWIPE_DOWN)
                _section.onSwipeAdjust(gs.x, gs.y, -1);
            break;

        case Mode::BROWSER:
            if (g == TouchInput::GESTURE_TAP) {
                _browser.onTouch(p.x, p.y);
                if (!_browser.isOpen()) _goHome();
            }
            // Swipe up = earlier items (lower index), swipe down = later items
            if (g == TouchInput::GESTURE_SWIPE_UP)   _browser.onEncoder(-PBLayout::VISIBLE_ROWS);
            if (g == TouchInput::GESTURE_SWIPE_DOWN) _browser.onEncoder(+PBLayout::VISIBLE_ROWS);
            break;

        case Mode::SCOPE_FULL:
            // Any tap on the full-screen scope returns home.
            if (g == TouchInput::GESTURE_TAP) {
                _goHome();
            }
            break;
    }
}

// =============================================================================
// Private: full-screen oscilloscope
//
// Performance: static chrome (header/footer text) is drawn only once on mode
// entry via _scopeFullFirstFrame. Only the waveform band (y=20..219) is cleared
// each frame. This saves ~100 000 SPI bytes/frame vs fillScreen().
// =============================================================================
void UIManager_TFT::_drawFullScope(SynthEngine& /*synth*/) {

    if (_scopeFullFirstFrame) {
        _scopeFullFirstFrame = false;

        // Static header
        _display.fillRect(0, 0, 320, 20, COLOUR_HEADER_BG);
        _display.setTextSize(1);
        _display.setTextColor(COLOUR_SYSTEXT, COLOUR_HEADER_BG);
        _display.setCursor(4, 6);
        _display.print("OSCILLOSCOPE");

        // Static footer
        _display.fillRect(0, 220, 320, 20, COLOUR_HEADER_BG);
        _display.setTextSize(1);
        _display.setTextColor(COLOUR_TEXT_DIM, COLOUR_HEADER_BG);
        _display.setCursor(4, 226);
        _display.print("TAP OR PRESS ANY BUTTON TO RETURN");
    }

    // CPU% in header — update every frame (small region)
    {
        char cpuBuf[12];
        snprintf(cpuBuf, sizeof(cpuBuf), "CPU:%d%%", (int)AudioProcessorUsageMax());
        const int16_t cpuX = 320 - (int16_t)(strlen(cpuBuf) * 6) - 4;
        _display.fillRect(cpuX - 2, 2, 320 - cpuX + 2, 16, COLOUR_HEADER_BG);
        _display.setTextColor(COLOUR_TEXT_DIM, COLOUR_HEADER_BG);
        _display.setCursor(cpuX, 6);
        _display.print(cpuBuf);
    }

    // Clear only the waveform band — not the whole screen
    _display.fillRect(0, 20, 320, 200, 0x0000);

    // Full-screen scope: 512 samples gives ~11 ms window.
    // After trig offset (n/4 = 128) we have 384 samples for 286 columns — fills width.
    static int16_t buf[512];
    const uint16_t n  = scopeTap.snapshot(buf, 512);
    const int16_t  wy = 22, wh = 198, ww = 288;

    _display.drawRect(0, wy, ww, wh, COLOUR_BORDER);

    if (n >= 64) {
        // Rising zero-crossing trigger — search first half of buffer
        int trig = n / 4;
        for (int i = 4; i < (int)n / 2; ++i) {
            if (buf[i-1] <= 0 && buf[i] > 0) { trig = i; break; }
        }

        const int16_t midY = wy + wh / 2;
        const int     spp  = ((int)n > ww) ? (n / ww) : 1;
        int16_t px = 1, py = midY;

        for (int col = 0; col < ww - 2; ++col) {
            const int base = trig + col * spp;
            if (base >= (int)n) break;

            // Box-filter average per pixel column — reduces aliasing
            int32_t acc = 0; int cnt = 0;
            for (int s = 0; s < spp && (base + s) < (int)n; ++s) {
                acc += buf[base + s]; cnt++;
            }
            const int16_t samp = cnt ? (int16_t)(acc / cnt) : 0;

            // ×10 gain (+20 dB) — same as home scope for consistent appearance.
            // Full-screen scope uses drawLine to connect adjacent points smoothly.
            static constexpr float SCOPE_GAIN = 10.0f;
            int cy = midY - (int)((float)samp * (float)(wh / 2 - 2) * SCOPE_GAIN / 32767.0f);
            cy = constrain(cy, wy + 1, wy + wh - 1);

            if (col > 0) _display.drawLine(px, py, col + 1, cy, COLOUR_SCOPE_WAVE);
            px = col + 1; py = cy;
        }
        _display.drawFastHLine(1, midY, ww - 2, COLOUR_SCOPE_ZERO);
    }
}

