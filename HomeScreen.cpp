// HomeScreen.cpp
// =============================================================================
// JT-8000 home screen: oscilloscope + 8 section tiles.
//
// PERFORMANCE NOTES:
//   - Scope area is cleared/redrawn each frame.  To prevent flicker the old
//     waveform pixels are erased by re-drawing them in COLOUR_SCOPE_BG before
//     the new waveform is painted in COLOUR_SCOPE_WAVE.  This avoids a full
//     fillRect() on the scope band (which is ~60 000 SPI pixels wide).
//   - The peak meter uses exponential averaging (_peakSmooth) rather than
//     readPeakAndClear() raw.  This gives a smooth VU-meter feel.
//   - The header CPU% is only redrawn once per HEADER_REDRAW_MS to reduce SPI
//     traffic — the text is small but fillRect + print is ~300 µs each call.
//   - Tiles only repaint on touch/encoder events (_tilesDirty flag).
// =============================================================================

#include "HomeScreen.h"
#include "JT8000Colours.h"
#include <math.h>

// Static singleton pointer — set in begin()
HomeScreen* HomeScreen::_instance = nullptr;

// =============================================================================
// Constructor
// =============================================================================
HomeScreen::HomeScreen()
    : _display(nullptr), _scopeTap(nullptr), _onSection(nullptr)
    , _highlighted(0), _redrawStep(1), _tilesDirty(true), _scopeTapped(false)
    , _peakSmooth(0.0f)
    , _lastHeaderMs(0)
    , _lastMeterFill(-1)          // -1 forces meter redraw on first frame
    , _lastPolyMode(PolyMode::POLY)
    , _lastCpuPct(-1)             // -1 forces CPU% redraw on first frame
    // Tile positions computed from layout constants
    , _t0(1 + 0*(TILE_W+TILE_GAP), TILE_Y+2, TILE_W, ROW_H-3, kSections[0])
    , _t1(1 + 1*(TILE_W+TILE_GAP), TILE_Y+2, TILE_W, ROW_H-3, kSections[1])
    , _t2(1 + 2*(TILE_W+TILE_GAP), TILE_Y+2, TILE_W, ROW_H-3, kSections[2])
    , _t3(1 + 3*(TILE_W+TILE_GAP), TILE_Y+2, TILE_W, ROW_H-3, kSections[3])
    , _t4(1 + 0*(TILE_W+TILE_GAP), TILE_Y+ROW_H+1, TILE_W, ROW_H-3, kSections[4])
    , _t5(1 + 1*(TILE_W+TILE_GAP), TILE_Y+ROW_H+1, TILE_W, ROW_H-3, kSections[5])
    , _t6(1 + 2*(TILE_W+TILE_GAP), TILE_Y+ROW_H+1, TILE_W, ROW_H-3, kSections[6])
    , _t7(1 + 3*(TILE_W+TILE_GAP), TILE_Y+ROW_H+1, TILE_W, ROW_H-3, kSections[7])
{
    _tiles[0] = &_t0; _tiles[1] = &_t1; _tiles[2] = &_t2; _tiles[3] = &_t3;
    _tiles[4] = &_t4; _tiles[5] = &_t5; _tiles[6] = &_t6; _tiles[7] = &_t7;

    // Zero previous-waveform buffer (used for erase-before-redraw)
    memset(_prevWave, 0, sizeof(_prevWave));
    // All voices start inactive — LED strip will draw as dark on first frame
    memset(_prevVoiceActive, 0, sizeof(_prevVoiceActive));
}

// =============================================================================
// begin()
// =============================================================================
void HomeScreen::begin(ILI9341_t3n* disp, AudioScopeTap* tap,
                       SectionSelectedCallback cb) {
    _display   = disp;
    _scopeTap  = tap;
    _onSection = cb;
    _instance  = this;

    for (int i = 0; i < SECTION_COUNT; ++i) _tiles[i]->setDisplay(disp);

    // Lambda callbacks — capture _instance (static, safe)
    _t0.setCallback([]{ if (_instance) _instance->_fire(0); });
    _t1.setCallback([]{ if (_instance) _instance->_fire(1); });
    _t2.setCallback([]{ if (_instance) _instance->_fire(2); });
    _t3.setCallback([]{ if (_instance) _instance->_fire(3); });
    _t4.setCallback([]{ if (_instance) _instance->_fire(4); });
    _t5.setCallback([]{ if (_instance) _instance->_fire(5); });
    _t6.setCallback([]{ if (_instance) _instance->_fire(6); });
    _t7.setCallback([]{ if (_instance) _instance->_fire(7); });

    markFullRedraw();
}

void HomeScreen::_fire(int idx) {
    if (_onSection) _onSection(idx);
}

// =============================================================================
// draw()  — called every display frame (~30 fps)
//
// Full repaint is split across 5 consecutive calls so MIDI reads can run
// between each step.  See SectionScreen::draw() for the design rationale.
//
//   Step 1 = fillScreen
//   Step 2 = _drawHeader (full)
//   Step 3 = _drawScope + _drawVoiceBar
//   Step 4 = _drawAllTiles
//   Step 5 = _drawFooter -> idle
// =============================================================================
void HomeScreen::draw(SynthEngine& synth) {
    if (!_display) return;

    // ---- Phased full-screen redraw ----
    if (_redrawStep > 0) {
        switch (_redrawStep) {
            case 1:
                _display->fillScreen(COLOUR_BACKGROUND);
                break;
            case 2:
                _drawHeader(synth, true);
                break;
            case 3:
                _drawScope();
                _drawVoiceBar(synth);
                break;
            case 4:
                _drawAllTiles();
                break;
            case 5:
                _drawFooter();
                _redrawStep = 0;
                _tilesDirty = false;
                return;
        }
        _redrawStep++;
        return;  // yield — MIDI reads run before next step
    }

    // ---- Idle path: incremental updates ----

    // Scope and voice bar run every frame (live data, cheap pixel-diff)
    _drawScope();
    _drawVoiceBar(synth);

    // Header: only when values change, rate-limited to HEADER_REDRAW_MS
    const uint32_t now = millis();
    if ((now - _lastHeaderMs) >= HEADER_REDRAW_MS) {
        _drawHeader(synth, false);
        _lastHeaderMs = now;
    }

    // Tiles only on touch/encoder events
    if (_tilesDirty) {
        for (int i = 0; i < SECTION_COUNT; ++i) _tiles[i]->draw();
        _tilesDirty = false;
    }
}

// =============================================================================
// Touch input
// =============================================================================
bool HomeScreen::onTouch(int16_t x, int16_t y) {
    if (y >= SCOPE_Y && y < SCOPE_Y + SCOPE_H) {
        _scopeTapped = true;
        return true;
    }
    for (int i = 0; i < SECTION_COUNT; ++i) {
        if (_tiles[i]->onTouch(x, y)) {
            _tilesDirty = true;
            return true;
        }
    }
    return false;
}

void HomeScreen::onTouchRelease(int16_t x, int16_t y) {
    for (int i = 0; i < SECTION_COUNT; ++i) _tiles[i]->onTouchRelease(x, y);
    _tilesDirty = true;
}

// =============================================================================
// Encoder input
// =============================================================================
void HomeScreen::onEncoderDelta(int delta) {
    if (!delta) return;
    _highlighted = (_highlighted + delta + SECTION_COUNT) % SECTION_COUNT;
    _tilesDirty  = true;
}

void HomeScreen::onEncoderPress() { _fire(_highlighted); }

bool HomeScreen::isScopeTapped() {
    bool v       = _scopeTapped;
    _scopeTapped = false;
    return v;
}

void HomeScreen::markFullRedraw() {
    _redrawStep = 1;
    memset(_prevWave, 0, sizeof(_prevWave));
    // Force all LEDs to repaint on next frame
    memset(_prevVoiceActive, 0xFF, sizeof(_prevVoiceActive));
    // Invalidate caches so header and meter fully repaint
    _lastMeterFill = -1;
    _lastCpuPct    = -1;
}

// =============================================================================
// _drawHeader()  — product name + poly mode badge + CPU%
//   fullRepaint=true: draw background + static name (done on full redraws only).
//   fullRepaint=false: update badge + CPU% only when their values have changed.
//
// PERFORMANCE: fillRect + print is ~300 µs per call over SPI.  Caching the
// last-drawn values means we skip the write when nothing changed — at 30 Hz
// with a static poly mode and steady CPU this costs zero SPI bandwidth.
// =============================================================================
void HomeScreen::_drawHeader(SynthEngine& synth, bool fullRepaint) {
    if (fullRepaint) {
        _display->fillRect(0, 0, SW, HEADER_H, COLOUR_HEADER_BG);
        _display->drawFastHLine(0, HEADER_H - 1, SW, COLOUR_BORDER);

        _display->setTextSize(1);
        _display->setTextColor(COLOUR_SYSTEXT, COLOUR_HEADER_BG);
        _display->setCursor(4, 7);
        _display->print("JT.8000");

        // Invalidate caches so the badge and CPU% paint on this frame
        _lastPolyMode = (PolyMode)255;  // invalid sentinel
        _lastCpuPct   = -1;
    }

    // ---- Poly mode badge: "POLY" / "MONO" / "UNI" ----
    // Only repaint if the mode has changed since the last draw.
    {
        const PolyMode mode = synth.getPolyMode();
        if (mode != _lastPolyMode) {
            _lastPolyMode = mode;
            const char* modeStr;
            uint16_t    modeCol;
            switch (mode) {
                case PolyMode::MONO:
                    modeStr = "MONO"; modeCol = COLOUR_LFO;      break;
                case PolyMode::UNISON:
                    modeStr = "UNI";  modeCol = COLOUR_FILTER;   break;
                default:
                    modeStr = "POLY"; modeCol = COLOUR_TEXT_DIM; break;
            }
            // Clear badge region then redraw text
            _display->fillRect(52, 2, 28, HEADER_H - 4, COLOUR_HEADER_BG);
            _display->setTextColor(modeCol, COLOUR_HEADER_BG);
            _display->setCursor(52, 7);
            _display->print(modeStr);
        }
    }

    // ---- CPU% — right-justified, only repaint when value changes ----
    {
        const int cpuNow = (int)AudioProcessorUsageMax();
        if (cpuNow != _lastCpuPct) {
            _lastCpuPct = cpuNow;
            const int16_t cpuX = SW - 64;
            _display->fillRect(cpuX, 2, 62, HEADER_H - 4, COLOUR_HEADER_BG);
            char buf[12];
            snprintf(buf, sizeof(buf), "CPU:%d%%", cpuNow);
            _display->setTextSize(1);
            _display->setTextColor(COLOUR_TEXT_DIM, COLOUR_HEADER_BG);
            _display->setCursor(cpuX, 7);
            _display->print(buf);
        }
    }
}

// =============================================================================
// _drawVoiceBar()  — 8-voice activity LED strip
//
// Draws one small coloured dot per voice in a thin horizontal band between
// the oscilloscope and the section tiles.  Only redraws dots that changed
// state since the last frame — typically 0-2 redraws per frame, very cheap.
//
// Colours:
//   Active voice   : COLOUR_OSC  (cyan)  — in UNISON mode: COLOUR_FILTER (amber)
//   Inactive voice : dark background slot
//
// Layout: 8 dots × LED_W px wide with LED_GAP spacing, centred in the strip.
// =============================================================================
void HomeScreen::_drawVoiceBar(SynthEngine& synth) {
    static constexpr int16_t LED_W   = 16;   // LED dot width (px)
    static constexpr int16_t LED_H   = 6;    // LED dot height (px)
    static constexpr int16_t LED_GAP = 4;    // gap between dots
    static constexpr int16_t TOTAL_W = 8 * LED_W + 7 * LED_GAP;   // = 156px
    static constexpr int16_t START_X = (SW - TOTAL_W) / 2;         // centred

    const bool isUnison = (synth.getPolyMode() == PolyMode::UNISON);
    const uint16_t activeCol = isUnison ? COLOUR_FILTER : COLOUR_OSC;

    for (int v = 0; v < 8; ++v) {
        const bool active = synth.isVoiceActive((uint8_t)v);

        // Skip redraw if nothing changed — saves SPI bandwidth
        if (active == _prevVoiceActive[v]) continue;
        _prevVoiceActive[v] = active;

        const int16_t lx = START_X + v * (LED_W + LED_GAP);
        const int16_t ly = VOICE_BAR_Y + 1;   // 1px inset from strip top

        if (active) {
            // Filled dot with a 1px darker border for depth
            _display->fillRect(lx, ly, LED_W, LED_H, activeCol);
        } else {
            // Dark slot — shows the LED is present but off
            _display->fillRect(lx, ly, LED_W, LED_H, COLOUR_BORDER);
        }
    }
}

// =============================================================================
// _drawScope()  -- oscilloscope waveform + peak meter
//
// Flicker prevention:
//   Each column's previous Y is stored in _prevWave[].  The old pixel is
//   erased in COLOUR_SCOPE_BG before the new one is drawn -- avoids fillRect().
//
// Peak meter caching (_lastMeterFill):
//   The meter bar (3x fillRect) is only redrawn when the fill height changes.
//   At steady audio level this saves ~900 us of SPI per frame.
//   dB scale labels sit just outside the waveform/meter area and are redrawn
//   alongside the meter when it updates (they'd be cleared by the fillRect).
// =============================================================================
void HomeScreen::_drawScope() {
    if (!_scopeTap) return;

    const int16_t midY = SCOPE_Y + SCOPE_H / 2;

    // ---- Capture audio snapshot (512 samples = 11.6 ms @ 44.1 kHz) ----
    static int16_t buf[512];
    const uint16_t n = _scopeTap->snapshot(buf, 512);

    if (n >= 64) {
        // Rising zero-crossing trigger -- search first quarter of buffer
        int trig = n / 4;
        for (int i = 4; i < (int)n / 2; ++i) {
            if (buf[i - 1] <= 0 && buf[i] > 0) { trig = i; break; }
        }

        const int spp = ((int)n > WAVE_W) ? (n / WAVE_W) : 1;

        for (int col = 0; col < WAVE_W - 2; ++col) {
            const int base = trig + col * spp;
            if (base >= (int)n) break;

            // Box-filter average across spp samples
            int32_t acc = 0; int cnt = 0;
            for (int s = 0; s < spp && (base + s) < (int)n; ++s) {
                acc += buf[base + s]; cnt++;
            }
            const int16_t samp = cnt ? (int16_t)(acc / cnt) : 0;

            // Map amplitude to screen Y.
            // SCOPE_GAIN=10 (+20 dB): -12 dBFS patch fills scope visibly.
            static constexpr float SCOPE_GAIN = 10.0f;
            int cy = midY - (int)((float)samp * (float)(SCOPE_H / 2 - 2) * SCOPE_GAIN / 32767.0f);
            cy = constrain(cy, SCOPE_Y + 2, SCOPE_Y + SCOPE_H - 2);

            const int16_t px = col + 1;

            // Erase 3-px old stroke, draw 3-px new stroke.
            // 3-px line is more visible than drawPixel at high frequencies.
            if (_prevWave[col] != 0) {
                _display->drawFastVLine(px, _prevWave[col] - 1, 3, COLOUR_SCOPE_BG);
            }
            _display->drawFastVLine(px, cy - 1, 3, COLOUR_SCOPE_WAVE);
            _prevWave[col] = (int16_t)cy;
        }

        // Zero-reference line -- cheap single HLine drawn after waveform
        _display->drawFastHLine(1, midY, WAVE_W - 2, COLOUR_SCOPE_ZERO);

    } else {
        // Not enough data -- clear and show hint
        memset(_prevWave, 0, sizeof(_prevWave));
        _display->fillRect(1, SCOPE_Y + 2, WAVE_W - 2, SCOPE_H - 4, COLOUR_SCOPE_BG);
        _display->setTextSize(1);
        _display->setTextColor(COLOUR_TEXT_DIM, COLOUR_SCOPE_BG);
        _display->setCursor(10, SCOPE_Y + SCOPE_H / 2 - 4);
        _display->print("arming...");
    }

    // ---- Peak meter ----
    // Attack instant, decay ~12 frames @ 30 Hz (~400 ms)
    const float rawPeak = _scopeTap->readPeakAndClear();
    _peakSmooth = (rawPeak > _peakSmooth) ? rawPeak : (_peakSmooth * PEAK_DECAY);

    const int16_t mx = WAVE_W + 4;
    const int16_t mh = SCOPE_H - 6;

    // dBFS -- floor at -60 dB
    const float db   = (_peakSmooth > 0.001f) ? 20.0f * log10f(_peakSmooth) : -60.0f;
    const int16_t fill = (int16_t)((constrain(db, -60.0f, 0.0f) + 60.0f) / 60.0f * (float)mh);

    // Only repaint when fill height changes -- eliminates ~900 us SPI per frame
    // when audio level is steady.
    if (fill != _lastMeterFill) {
        _lastMeterFill = fill;

        _display->fillRect(mx, SCOPE_Y + 3, METER_W - 2, mh, 0x0000);
        _display->drawRect(mx, SCOPE_Y + 3, METER_W - 2, mh, COLOUR_BORDER);

        if (fill > 0) {
            // -18 dB = green->yellow, -6 dB = yellow->red
            const int16_t g18 = (int16_t)((float)mh * 42.0f / 60.0f);
            const int16_t g6  = (int16_t)((float)mh * 54.0f / 60.0f);
            const int16_t gf  = min(fill, g18);
            const int16_t yf  = max((int16_t)0, (int16_t)(min(fill, g6)  - g18));
            const int16_t rf  = max((int16_t)0, (int16_t)(fill - g6));

            if (gf > 0) _display->fillRect(mx+2, SCOPE_Y+3+mh-gf,         METER_W-4, gf, COLOUR_METER_GREEN);
            if (yf > 0) _display->fillRect(mx+2, SCOPE_Y+3+mh-g18-yf,     METER_W-4, yf, COLOUR_METER_YELLOW);
            if (rf > 0) _display->fillRect(mx+2, SCOPE_Y+3+mh-g18-yf-rf,  METER_W-4, rf, COLOUR_METER_RED);
        }

        // dB scale labels -- redrawn here because fillRect above clears them.
        // They live just right of the meter bar, outside the waveform area.
        _display->setTextSize(1);
        _display->setTextColor(COLOUR_TEXT_DIM, COLOUR_BACKGROUND);
        _display->setCursor(mx + METER_W, SCOPE_Y + 3);          _display->print("0");
        _display->setCursor(mx + METER_W, SCOPE_Y + 3 + mh / 2); _display->print("-30");
    }
}

// =============================================================================
// _drawAllTiles()
// =============================================================================
void HomeScreen::_drawAllTiles() {
    _display->fillRect(0, TILE_Y, SW, TILE_H, COLOUR_BACKGROUND);
    for (int i = 0; i < SECTION_COUNT; ++i) {
        _tiles[i]->markDirty();
        _tiles[i]->draw();
    }
}

// =============================================================================
// _drawFooter()
// =============================================================================
void HomeScreen::_drawFooter() {
    const int16_t fy = SH - FOOTER_H;
    _display->fillRect(0, fy, SW, FOOTER_H, COLOUR_BACKGROUND);
    _display->setTextSize(1);
    _display->setTextColor(COLOUR_TEXT_DIM, COLOUR_BACKGROUND);
    _display->setCursor(4, fy + 1);
    _display->print("TAP SECTION  HOLD-L:FULLSCOPE");
}
