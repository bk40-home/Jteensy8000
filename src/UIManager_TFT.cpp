// UIManager_TFT.cpp
// =============================================================================
// UI manager with ILI9341_t3n ASYNC DMA FRAMEBUFFER.
//
// HOW THIS SOLVES MIDI NOTE DROPS:
//   OLD: Every fillRect/drawLine/print call blocked the CPU on SPI for
//        milliseconds.  During that time, MIDI buffers overflowed.
//   NEW: All draw calls write to a 153 KB RAM buffer (microseconds).
//        Once per frame, updateScreenAsync() kicks a DMA transfer that
//        sends the buffer to the display IN THE BACKGROUND.
//        The CPU returns immediately and loop() keeps polling MIDI.
// =============================================================================

#include "UIManager_TFT.h"
#include "DebugTrace.h"
#include <math.h>

UIManager_TFT* UIManager_TFT::_instance = nullptr;

// Static framebuffer in DMAMEM — 320x240x2 = 153,600 bytes in RAM2.
DMAMEM uint16_t UIManager_TFT::_fb[320 * 240];

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
// beginDisplay() — SPI init + splash (blocking), then enable framebuffer.
// =============================================================================
void UIManager_TFT::beginDisplay() {
    _display.begin(SPI_CLOCK_HZ);
    _display.setRotation(3);
    _display.invertDisplay(true);

    // Splash drawn in direct/blocking mode (framebuffer not yet active)
    _display.fillScreen(COLOUR_BACKGROUND);
    _display.setTextSize(3);
    _display.setTextColor(COLOUR_ACCENT_ORANGE);
    _display.setCursor(60, 90);
    _display.print("JT.8000");
    _display.setTextSize(1);
    _display.setTextColor(COLOUR_TEXT_DIM);
    _display.setCursor(74, 124);
    _display.print("MicroDexed Edition");

    delay(800);

    // ---- ENABLE FRAMEBUFFER MODE ----
    // After this, ALL draw calls write to _fb[] in RAM (microseconds).
    // Nothing hits SPI until updateScreenAsync() is called.
    _display.setFrameBuffer(_fb);
    _display.useFrameBuffer(true);
    _display.fillScreen(COLOUR_BACKGROUND);
    _display.updateScreen();   // push cleared screen (blocking, just once)

    _touchOk = _touch.begin();
    JT_LOGF("[UI] Touch: %s\n", _touchOk ? "OK" : "not found");
    JT_LOGF("[UI] Framebuffer: ENABLED (async DMA)\n");
}

// =============================================================================
// begin() — wire screens.  Call AFTER AudioMemory() and synth init.
// =============================================================================
void UIManager_TFT::begin(SynthEngine& synth) {
    _synthRef = &synth;
    _instance = this;
    _home.begin(&_display, &synth);
    _home.markFullRedraw();
}

// =============================================================================
// updateDisplay() — draw to framebuffer, then kick async DMA.
//
//   1. If DMA still sending previous frame → return immediately (~0 µs).
//   2. If frame interval not elapsed → return.
//   3. Draw current mode to framebuffer (RAM writes, ~0.1–0.5 ms).
//   4. Kick async DMA (~10 µs setup, transfer runs in background).
// =============================================================================
void UIManager_TFT::updateDisplay(SynthEngine& synth) {
    _synthRef = &synth;

    // If previous DMA transfer still running, skip this frame entirely.
    // The CPU never waits — it returns to loop() and polls MIDI.
    if (_display.asyncUpdateActive()) return;

    const uint32_t now = millis();
    if ((now - _lastFrame) < FRAME_MS) return;
    _lastFrame = now;

    // All draws below write to the RAM framebuffer (microseconds per call)
    switch (_mode) {
        case Mode::HOME:       _home.draw();            break;
        case Mode::BROWSER:    _browser.draw(_display);  break;
        case Mode::SCOPE_FULL: _drawFullScope();         break;
    }

    // Kick async DMA — CPU returns in ~10 µs, DMA streams pixels to screen.
    _display.updateScreenAsync();
}

// =============================================================================
// pollInputs() — UNCHANGED from previous version
// =============================================================================
void UIManager_TFT::pollInputs(HardwareInterface_MicroDexed& hw, SynthEngine& synth) {
    _synthRef = &synth;

    if (_touchOk) {
        _touch.update();
        _handleTouch(synth);
    }

    using HW = HardwareInterface_MicroDexed;
    const int  dL = hw.getEncoderDelta(HW::ENC_LEFT);
    const int  dR = hw.getEncoderDelta(HW::ENC_RIGHT);
    const auto bL = hw.getButtonPress(HW::ENC_LEFT);
    const auto bR = hw.getButtonPress(HW::ENC_RIGHT);

    switch (_mode) {
        case Mode::HOME:
            if (_home.isEntryOpen()) {
                if (dL) _home.onEntryEncoderDelta(dL);
                break;
            }
            if (dL) _home.onEncoderLeft(dL);
            if (dR) _home.onEncoderRight(dR);
            if (bL == HW::PRESS_SHORT) _home.onEncoderLeftPress();
            if (bR == HW::PRESS_SHORT) _home.onEncoderRightPress();
            if (bL == HW::PRESS_LONG) _setMode(Mode::SCOPE_FULL);
            if (bR == HW::PRESS_LONG) _openBrowser();
            break;

        case Mode::BROWSER:
            if (dL) _browser.onEncoder(dL);
            if (bL == HW::PRESS_SHORT) {
                _browser.onEncoderPress();
                if (!_browser.isOpen()) _goHome();
            }
            if (bR == HW::PRESS_SHORT || bL == HW::PRESS_LONG) {
                _browser.close();
                _goHome();
            }
            break;

        case Mode::SCOPE_FULL:
            if (bL != HW::PRESS_NONE || bR != HW::PRESS_NONE) _goHome();
            break;
    }
}

// =============================================================================
// syncFromEngine / notifyCC / accessors — UNCHANGED
// =============================================================================
void UIManager_TFT::syncFromEngine(SynthEngine& synth) {
    _synthRef = &synth;
    _home.syncFromEngine();
}

void UIManager_TFT::notifyCC(uint8_t cc) { _home.notifyCC(cc); }
void UIManager_TFT::setCurrentPresetIdx(int idx) { _currentPresetIdx = idx; }
int  UIManager_TFT::getCurrentPresetIdx() const   { return _currentPresetIdx; }

// =============================================================================
// Private: mode switching — fillScreen is now RAM memset (~0.3 ms)
// =============================================================================
void UIManager_TFT::_setMode(Mode m) {
    if (m == _mode) return;
    _mode = m;
    _display.fillScreen(COLOUR_BACKGROUND);
    if (m == Mode::HOME)       _home.markFullRedraw();
    if (m == Mode::SCOPE_FULL) {
        _scopeFullFirstFrame = true;
        memset(_fsPrevWave, 0, sizeof(_fsPrevWave));
    }
}

void UIManager_TFT::_goHome() { _setMode(Mode::HOME); }

// =============================================================================
// Private: open preset browser
// =============================================================================
void UIManager_TFT::_openBrowser() {
    if (!_synthRef) return;
    _display.fillScreen(COLOUR_BACKGROUND);
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
// Private: touch routing — UNCHANGED
// =============================================================================
void UIManager_TFT::_handleTouch(SynthEngine& synth) {
    const TouchInput::Gesture g  = _touch.getGesture();
    const TouchInput::Point   p  = _touch.getTouchPoint();

    switch (_mode) {
        case Mode::HOME:
            if (g == TouchInput::GESTURE_TAP) _home.onTouch(p.x, p.y);
            if (g == TouchInput::GESTURE_SWIPE_UP) _home.onSwipe(-40);
            if (g == TouchInput::GESTURE_SWIPE_DOWN) _home.onSwipe(40);
            if (g == TouchInput::GESTURE_HOLD) _setMode(Mode::SCOPE_FULL);
            break;
        case Mode::BROWSER:
            if (g == TouchInput::GESTURE_TAP) {
                _browser.onTouch(p.x, p.y);
                if (!_browser.isOpen()) _goHome();
            }
            if (g == TouchInput::GESTURE_SWIPE_UP) _browser.onEncoder(-PBLayout::VISIBLE_ROWS);
            if (g == TouchInput::GESTURE_SWIPE_DOWN) _browser.onEncoder(+PBLayout::VISIBLE_ROWS);
            break;
        case Mode::SCOPE_FULL:
            if (g == TouchInput::GESTURE_TAP) _goHome();
            break;
    }
}

// =============================================================================
// Private: full-screen oscilloscope — IDENTICAL drawing logic.
// All operations now write to RAM framebuffer (microseconds).
// =============================================================================
void UIManager_TFT::_drawFullScope() {
    if (_scopeFullFirstFrame) {
        _scopeFullFirstFrame = false;
        _display.fillRect(0, 0, 320, 20, COLOUR_HEADER_BG);
        _display.setTextSize(1);
        _display.setTextColor(COLOUR_ACCENT_ORANGE, COLOUR_HEADER_BG);
        _display.setCursor(4, 6);
        _display.print("OSCILLOSCOPE");
        _display.fillRect(0, 220, 320, 20, COLOUR_HEADER_BG);
        _display.setTextSize(1);
        _display.setTextColor(COLOUR_TEXT_DIM, COLOUR_HEADER_BG);
        _display.setCursor(4, 226);
        _display.print("TAP OR PRESS ANY BUTTON TO RETURN");
        _display.fillRect(0, 20, 320, 200, COLOUR_SCOPE_BG);
    }

    // CPU%
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

    static constexpr int16_t wy = 22, wh = 196, ww = 286;
    static constexpr int16_t midY = wy + wh / 2;

    for (int col = 1; col < ww - 1; ++col) {
        if (_fsPrevWave[col] != midY || _fsPrevWave[col-1] != midY) {
            _display.drawLine(col, _fsPrevWave[col-1], col + 1, _fsPrevWave[col],
                              COLOUR_SCOPE_BG);
        }
    }

    _display.drawRect(0, wy, ww, wh, COLOUR_BORDER);
    _display.drawFastHLine(1, midY, ww - 2, COLOUR_SCOPE_ZERO);

    static int16_t buf[512];
    const uint16_t n = scopeTap.snapshot(buf, 512);

    if (n >= 64) {
        int trig = n / 4;
        for (int i = 4; i < (int)n / 2; ++i) {
            if (buf[i-1] <= 0 && buf[i] > 0) { trig = i; break; }
        }
        const int spp = ((int)n > ww) ? (n / ww) : 1;
        static constexpr float SCOPE_GAIN = 10.0f;

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
        for (int col = 1; col < ww - 2; ++col) {
            _display.drawLine(col, newWave[col-1], col + 1, newWave[col],
                              COLOUR_SCOPE_WAVE);
        }
        memcpy(_fsPrevWave, newWave, sizeof(int16_t) * (ww - 2));
    } else {
        for (int col = 0; col < ww; ++col) _fsPrevWave[col] = midY;
    }
}
