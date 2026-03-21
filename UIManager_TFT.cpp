// UIManager_TFT.cpp
// =============================================================================
// Simplified UI manager: HOME (accordion) + SCOPE_FULL + BROWSER.
//
// Key changes from previous version:
//   - SECTION mode removed — HomeScreen is now the accordion section editor
//   - HomeScreen no longer has scope/tiles — it's a scrollable param list
//   - Preset browser: long-press Encoder R from HOME (was a tile tap)
//   - Full-screen scope: long-press Encoder L from HOME (unchanged)
//   - Scope waveform: orange, background-matched erase (no ghost artefacts)
//   - Entry overlay: managed by HomeScreen, UIManager routes input when open
// =============================================================================

#include "UIManager_TFT.h"
#include "MidiDrain.h"
#include "DebugTrace.h"
#include <math.h>

UIManager_TFT* UIManager_TFT::_instance = nullptr;

// =============================================================================
// Constructor
// =============================================================================
UIManager_TFT::UIManager_TFT()
    : _display(TFT_CS, TFT_DC, TFT_RST, TFT_MOSI, TFT_SCK, TFT_MISO)
    , _touchOk(false)
    , _mode(Mode::HOME)
    , _lastFrame(0)
    , _synthRef(nullptr)
    , _currentPresetIdx(0)
    , _scopeFullFirstFrame(true)
{
    memset(_fsPrevWave, 0, sizeof(_fsPrevWave));
}

// =============================================================================
// beginDisplay() — SPI + touch init. Call BEFORE AudioMemory().
// =============================================================================
void UIManager_TFT::beginDisplay() {
    _display.begin(SPI_CLOCK_HZ);
    _display.setRotation(3);       // landscape 320×240
    _display.invertDisplay(true);  // ILI9341 INVON → send INVOFF
    _display.fillScreen(COLOUR_BACKGROUND);

    // Touch controller over I2C
    _touchOk = _touch.begin();
    JT_LOGF("[UI] Touch: %s\n", _touchOk ? "OK" : "not found");

    // Boot splash
    _display.setTextSize(3);
    _display.setTextColor(COLOUR_ACCENT_ORANGE);
    _display.setCursor(60, 90);
    _display.print("JT.8000");

    _display.setTextSize(1);
    _display.setTextColor(COLOUR_TEXT_DIM);
    _display.setCursor(74, 124);
    _display.print("MicroDexed Edition");

    delay(800);
    _display.fillScreen(COLOUR_BACKGROUND);
}

// =============================================================================
// begin() — wire screens. Call AFTER AudioMemory() and synth init.
// =============================================================================
void UIManager_TFT::begin(SynthEngine& synth) {
    _synthRef = &synth;
    _instance = this;

    _home.begin(&_display, &synth);
    _home.markFullRedraw();
}

// =============================================================================
// updateDisplay() — rate-limited to FRAME_MS
// =============================================================================
void UIManager_TFT::updateDisplay(SynthEngine& synth) {
    _synthRef = &synth;
    const uint32_t now = millis();
    if ((now - _lastFrame) < FRAME_MS) return;
    _lastFrame = now;

    switch (_mode) {
        case Mode::HOME:
            _home.draw();
            break;

        case Mode::BROWSER:
            _browser.draw(_display);
            break;

        case Mode::SCOPE_FULL:
            _drawFullScope();
            break;
    }
}

// =============================================================================
// pollInputs() — high-frequency input poll (>= 100 Hz)
// =============================================================================
void UIManager_TFT::pollInputs(HardwareInterface_MicroDexed& hw, SynthEngine& synth) {
    _synthRef = &synth;

    // ---- Touch ----
    if (_touchOk) {
        _touch.update();
        _handleTouch(synth);
    }

    // ---- Encoders ----
    using HW = HardwareInterface_MicroDexed;
    const int  dL = hw.getEncoderDelta(HW::ENC_LEFT);
    const int  dR = hw.getEncoderDelta(HW::ENC_RIGHT);
    const auto bL = hw.getButtonPress(HW::ENC_LEFT);
    const auto bR = hw.getButtonPress(HW::ENC_RIGHT);

    switch (_mode) {

        case Mode::HOME:
            // If entry overlay is open, route L encoder to it for list scrolling
            if (_home.isEntryOpen()) {
                if (dL) _home.onEntryEncoderDelta(dL);
                // Any button press while entry is open — let HomeScreen handle
                // (entry has its own touch-based confirm/cancel)
                break;
            }

            // Normal navigation
            if (dL) _home.onEncoderLeft(dL);
            if (dR) _home.onEncoderRight(dR);

            if (bL == HW::PRESS_SHORT) _home.onEncoderLeftPress();
            if (bR == HW::PRESS_SHORT) _home.onEncoderRightPress();

            // Long-press L → scope
            if (bL == HW::PRESS_LONG) _setMode(Mode::SCOPE_FULL);

            // Long-press R → preset browser
            if (bR == HW::PRESS_LONG) _openBrowser();
            break;

        case Mode::BROWSER:
            if (dL) _browser.onEncoder(dL);
            if (bL == HW::PRESS_SHORT) {
                _browser.onEncoderPress();
                if (!_browser.isOpen()) _goHome();
            }
            // R press or L long → cancel browser
            if (bR == HW::PRESS_SHORT || bL == HW::PRESS_LONG) {
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
// syncFromEngine() — after preset load
// =============================================================================
void UIManager_TFT::syncFromEngine(SynthEngine& synth) {
    _synthRef = &synth;
    _home.syncFromEngine();
}

void UIManager_TFT::setCurrentPresetIdx(int idx)  { _currentPresetIdx = idx; }
int  UIManager_TFT::getCurrentPresetIdx()   const  { return _currentPresetIdx; }

// =============================================================================
// Private: mode switching
// =============================================================================
void UIManager_TFT::_setMode(Mode m) {
    if (m == _mode) return;
    _mode = m;
    _display.fillScreen(COLOUR_BACKGROUND);
    MidiDrain::poll();   // fillScreen blocks ~15 ms — drain MIDI immediately after
    if (m == Mode::HOME)       _home.markFullRedraw();
    if (m == Mode::SCOPE_FULL) {
        _scopeFullFirstFrame = true;
        memset(_fsPrevWave, 0, sizeof(_fsPrevWave));
    }
}

void UIManager_TFT::_goHome() {
    _setMode(Mode::HOME);
}

// =============================================================================
// Private: open preset browser
// =============================================================================
void UIManager_TFT::_openBrowser() {
    if (!_synthRef) return;

    _display.fillScreen(COLOUR_BACKGROUND);
    MidiDrain::poll();   // fillScreen blocks ~15 ms — drain MIDI immediately after

    _browser.open(_synthRef, _currentPresetIdx,
        [](int globalIdx) {
            if (!_instance || !_instance->_synthRef) return;
            Presets::presets_loadByGlobalIndex(*_instance->_synthRef, globalIdx);
            _instance->_currentPresetIdx = globalIdx;
            _instance->syncFromEngine(*_instance->_synthRef);
            _instance->_goHome();
        });
    _mode = Mode::BROWSER;
}

// =============================================================================
// Private: touch routing
// =============================================================================
void UIManager_TFT::_handleTouch(SynthEngine& synth) {
    const TouchInput::Gesture g  = _touch.getGesture();
    const TouchInput::Point   p  = _touch.getTouchPoint();
    const TouchInput::Point   gs = _touch.getGestureStart();

    switch (_mode) {

        case Mode::HOME:
            if (g == TouchInput::GESTURE_TAP) {
                _home.onTouch(p.x, p.y);
            }
            if (g == TouchInput::GESTURE_SWIPE_UP) {
                _home.onSwipe(-40);  // scroll content up (reveal below)
            }
            if (g == TouchInput::GESTURE_SWIPE_DOWN) {
                _home.onSwipe(40);   // scroll content down (reveal above)
            }
            if (g == TouchInput::GESTURE_HOLD) {
                // Long hold on touch → scope (alternative to encoder long-press)
                _setMode(Mode::SCOPE_FULL);
            }
            break;

        case Mode::BROWSER:
            if (g == TouchInput::GESTURE_TAP) {
                _browser.onTouch(p.x, p.y);
                if (!_browser.isOpen()) _goHome();
            }
            if (g == TouchInput::GESTURE_SWIPE_UP)
                _browser.onEncoder(-PBLayout::VISIBLE_ROWS);
            if (g == TouchInput::GESTURE_SWIPE_DOWN)
                _browser.onEncoder(+PBLayout::VISIBLE_ROWS);
            break;

        case Mode::SCOPE_FULL:
            if (g == TouchInput::GESTURE_TAP) _goHome();
            break;
    }
}

// =============================================================================
// Private: full-screen oscilloscope
//
// Changes from previous version:
//   - Waveform colour: COLOUR_SCOPE_WAVE (now orange via JT8000Colours.h)
//   - Zero line: COLOUR_SCOPE_ZERO (now dim orange)
//   - Background erase: COLOUR_SCOPE_BG (= COLOUR_BACKGROUND, no ghost artefacts)
//   - Pixel-erase technique: erase previous waveform points individually before
//     drawing new ones, avoiding expensive fillRect on the waveform band
// =============================================================================
void UIManager_TFT::_drawFullScope() {

    if (_scopeFullFirstFrame) {
        _scopeFullFirstFrame = false;

        // Static header
        _display.fillRect(0, 0, 320, 20, COLOUR_HEADER_BG);
        _display.setTextSize(1);
        _display.setTextColor(COLOUR_ACCENT_ORANGE, COLOUR_HEADER_BG);
        _display.setCursor(4, 6);
        _display.print("OSCILLOSCOPE");

        // Static footer
        _display.fillRect(0, 220, 320, 20, COLOUR_HEADER_BG);
        MidiDrain::poll();   // drain after header+footer fills (~2 ms combined)

        _display.setTextSize(1);
        _display.setTextColor(COLOUR_TEXT_DIM, COLOUR_HEADER_BG);
        _display.setCursor(4, 226);
        _display.print("TAP OR PRESS ANY BUTTON TO RETURN");

        // Clear waveform area to background colour (not black)
        _display.fillRect(0, 20, 320, 200, COLOUR_SCOPE_BG);
        MidiDrain::poll();   // drain after large area clear (~5 ms)
    }

    // CPU% in header
    {
        char cpuBuf[12];
        snprintf(cpuBuf, sizeof(cpuBuf), "CPU:%d%%", (int)AudioProcessorUsageMax());
        const int16_t cpuX = 320 - (int16_t)(strlen(cpuBuf) * 6) - 4;
        _display.fillRect(cpuX - 2, 2, 320 - cpuX + 2, 16, COLOUR_HEADER_BG);
        const int cpuPct = (int)AudioProcessorUsageMax();
        const uint16_t cpuCol = (cpuPct > 80) ? COLOUR_METER_RED :
                                (cpuPct > 50) ? COLOUR_METER_YELLOW :
                                                 COLOUR_METER_GREEN;
        _display.setTextColor(cpuCol, COLOUR_HEADER_BG);
        _display.setCursor(cpuX, 6);
        _display.print(cpuBuf);
    }

    // Scope parameters
    static constexpr int16_t wy = 22, wh = 196, ww = 286;
    static constexpr int16_t midY = wy + wh / 2;

    // Erase previous waveform pixels (draw them in background colour)
    // This is much cheaper than fillRect on the entire waveform band
    for (int col = 1; col < ww - 1; ++col) {
        if (_fsPrevWave[col] != midY || _fsPrevWave[col-1] != midY) {
            _display.drawLine(col, _fsPrevWave[col-1], col + 1, _fsPrevWave[col],
                              COLOUR_SCOPE_BG);
        }
    }

    MidiDrain::poll();   // drain after erase loop (~2-4 ms of drawLine calls)

    // Draw border + centre line
    _display.drawRect(0, wy, ww, wh, COLOUR_BORDER);
    _display.drawFastHLine(1, midY, ww - 2, COLOUR_SCOPE_ZERO);

    // Snapshot audio buffer
    static int16_t buf[512];
    const uint16_t n = scopeTap.snapshot(buf, 512);

    if (n >= 64) {
        // Rising zero-crossing trigger
        int trig = n / 4;
        for (int i = 4; i < (int)n / 2; ++i) {
            if (buf[i-1] <= 0 && buf[i] > 0) { trig = i; break; }
        }

        const int spp = ((int)n > ww) ? (n / ww) : 1;
        static constexpr float SCOPE_GAIN = 10.0f;

        // Calculate new waveform Y positions
        int16_t newWave[288];
        for (int col = 0; col < ww - 2; ++col) {
            const int base = trig + col * spp;
            if (base >= (int)n) { newWave[col] = midY; continue; }

            int32_t acc = 0; int cnt = 0;
            for (int s = 0; s < spp && (base + s) < (int)n; ++s) {
                acc += buf[base + s]; cnt++;
            }
            const int16_t samp = cnt ? (int16_t)(acc / cnt) : 0;
            int cy = midY - (int)((float)samp * (float)(wh / 2 - 2) * SCOPE_GAIN / 32767.0f);
            newWave[col] = (int16_t)constrain(cy, wy + 1, wy + wh - 1);
        }

        // Draw new waveform
        for (int col = 1; col < ww - 2; ++col) {
            _display.drawLine(col, newWave[col-1], col + 1, newWave[col],
                              COLOUR_SCOPE_WAVE);
        }

        // Store for next-frame erase
        memcpy(_fsPrevWave, newWave, sizeof(int16_t) * (ww - 2));
    } else {
        // No data — store flat line
        for (int col = 0; col < ww; ++col) _fsPrevWave[col] = midY;
    }
}
