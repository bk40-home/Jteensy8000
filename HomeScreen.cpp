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
    , _highlighted(0), _fullRedraw(true), _tilesDirty(true), _scopeTapped(false)
    , _peakSmooth(0.0f)
    , _lastHeaderMs(0)
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
// =============================================================================
void HomeScreen::draw(SynthEngine& synth) {
    if (!_display) return;

    if (_fullRedraw) {
        // Repaint everything from scratch
        _display->fillScreen(COLOUR_BACKGROUND);
        _drawHeader(synth, true);
        _drawScope();
        _drawAllTiles();
        _drawFooter();
        _fullRedraw = false;
        _tilesDirty = false;
        return;
    }

    // Scope redraws every frame (live waveform)
    _drawScope();

    // Header CPU% — rate-limited to avoid excess SPI
    const uint32_t now = millis();
    if ((now - _lastHeaderMs) >= HEADER_REDRAW_MS) {
        _drawHeader(synth, false);
        _lastHeaderMs = now;
    }

    // Tiles only on events
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
    _fullRedraw = true;
    memset(_prevWave, 0, sizeof(_prevWave)); // force erase on next draw
}

// =============================================================================
// _drawHeader()  — product name + CPU%
//   fullRepaint=true: draw background + name (done on full redraws only).
//   fullRepaint=false: update CPU% text region only.
// =============================================================================
void HomeScreen::_drawHeader(SynthEngine& /*synth*/, bool fullRepaint) {
    if (fullRepaint) {
        _display->fillRect(0, 0, SW, HEADER_H, COLOUR_HEADER_BG);
        _display->drawFastHLine(0, HEADER_H - 1, SW, COLOUR_BORDER);

        _display->setTextSize(1);
        _display->setTextColor(COLOUR_SYSTEXT, COLOUR_HEADER_BG);
        _display->setCursor(4, 7);
        _display->print("JT.4000");
    }

    // CPU% — erase previous text region then redraw
    // Region: rightmost 60 px of header (enough for "CPU:100%")
    const int16_t cpuX = SW - 64;
    _display->fillRect(cpuX, 2, 62, HEADER_H - 4, COLOUR_HEADER_BG);

    char buf[12];
    snprintf(buf, sizeof(buf), "CPU:%d%%", (int)AudioProcessorUsageMax());
    _display->setTextSize(1);
    _display->setTextColor(COLOUR_TEXT_DIM, COLOUR_HEADER_BG);
    _display->setCursor(cpuX, 7);
    _display->print(buf);
}

// =============================================================================
// _drawScope()  — oscilloscope waveform + peak meter
//
// Flicker prevention:
//   Each column's previous Y coordinate is stored in _prevWave[].
//   Before drawing the new waveform, the previous pixel is erased by drawing
//   a vertical 1-px line in COLOUR_SCOPE_BG at the old position.  This avoids
//   clearing the entire scope band with fillRect() every frame.
//
// Peak meter smoothing:
//   Raw peak is blended with an exponential decay (_peakSmooth) so the meter
//   falls gradually rather than snapping.  Attack is instant; decay ~12 frames.
// =============================================================================
void HomeScreen::_drawScope() {
    if (!_scopeTap) return;

    const int16_t midY = SCOPE_Y + SCOPE_H / 2;

    // ---- Capture audio snapshot ----
    // 512 samples @ 44100 Hz = 11.6 ms window.
    // After the trigger offset (n/4 = 128 samples) we have 384 samples
    // available for 288 columns at spp=1, filling the full scope width.
    // Old value of 256 only gave 192 usable columns — right ~30% was blank.
    static int16_t buf[512];
    const uint16_t n = _scopeTap->snapshot(buf, 512);

    // Draw scope border once per full repaint; here just clear inner area top/bot
    // (border persists; we only erase the waveform band)

    if (n >= 64) {
        // ---- Rising zero-crossing trigger ----
        // Search the first quarter of the buffer to allow a full period to display.
        // If no crossing found, use n/4 as a fallback (stable enough for synth tones).
        int trig = n / 4;
        for (int i = 4; i < (int)n / 2; ++i) {
            if (buf[i - 1] <= 0 && buf[i] > 0) { trig = i; break; }
        }

        // Samples per display column (integer downsampling, no aliasing for synth)
        const int spp = ((int)n > WAVE_W) ? (n / WAVE_W) : 1;

        // ---- Erase previous waveform + draw new ----
        for (int col = 0; col < WAVE_W - 2; ++col) {
            const int base = trig + col * spp;
            if (base >= (int)n) break;

            // Box-filter average across spp samples
            int32_t acc = 0; int cnt = 0;
            for (int s = 0; s < spp && (base + s) < (int)n; ++s) {
                acc += buf[base + s]; cnt++;
            }
            const int16_t samp = cnt ? (int16_t)(acc / cnt) : 0;

            // ---- Map sample to screen Y ----
            // SCOPE_GAIN amplifies the display — does not affect audio.
            // × 10 (+20 dB): a typical synth patch at -12 dBFS (amplitude ≈ 0.25)
            // maps to 0.25 × 10 = 2.5 × half-height → fills and clips visibly.
            // This makes quiet signals large and loud signals clearly clip at the border.
            // Adjust if your patches are much louder or quieter than -12 dBFS.
            static constexpr float SCOPE_GAIN = 10.0f;
            int cy = midY - (int)((float)samp * (float)(SCOPE_H / 2 - 2) * SCOPE_GAIN / 32767.0f);
            cy = constrain(cy, SCOPE_Y + 2, SCOPE_Y + SCOPE_H - 2);

            const int16_t px = col + 1; // screen X within scope area

            // ---- Erase previous stroke, draw new stroke (3 px tall) ----
            // Drawing a 3-pixel vertical line per column gives a much more visible
            // waveform than single drawPixel, especially at high frequencies where
            // adjacent columns can be many pixels apart vertically.
            // We erase the OLD 3-px stroke before drawing the new one to avoid
            // accumulation of green pixels drifting across the scope band.
            if (_prevWave[col] != 0) {
                _display->drawFastVLine(px, _prevWave[col] - 1, 3, COLOUR_SCOPE_BG);
            }
            _display->drawFastVLine(px, cy - 1, 3, COLOUR_SCOPE_WAVE);
            _prevWave[col] = (int16_t)cy;
        }

        // Zero-reference line (always present — just overwrite once per frame;
        // individual waveform pixels will overdraw it correctly)
        _display->drawFastHLine(1, midY, WAVE_W - 2, COLOUR_SCOPE_ZERO);

    } else {
        // Not enough data yet — clear wave buffer and show hint
        memset(_prevWave, 0, sizeof(_prevWave));
        _display->fillRect(1, SCOPE_Y + 2, WAVE_W - 2, SCOPE_H - 4, COLOUR_SCOPE_BG);
        _display->setTextSize(1);
        _display->setTextColor(COLOUR_TEXT_DIM, COLOUR_SCOPE_BG);
        _display->setCursor(10, SCOPE_Y + SCOPE_H / 2 - 4);
        _display->print("arming...");
    }

    // ---- Peak meter ----
    // Exponential decay: new = max(raw, prev * decay)
    // Attack: instant (raw peak dominates); Decay: ~0.85^n frames → ~12-frame fall
    const float rawPeak   = _scopeTap->readPeakAndClear();
    _peakSmooth = (rawPeak > _peakSmooth) ? rawPeak : (_peakSmooth * PEAK_DECAY);

    const int16_t mx  = WAVE_W + 4;
    const int16_t mh  = SCOPE_H - 6;

    // dBFS conversion — floor at -60 dB
    const float db  = (_peakSmooth > 0.001f) ? 20.0f * log10f(_peakSmooth) : -60.0f;
    const float dbc = constrain(db, -60.0f, 0.0f);
    const int16_t fill = (int16_t)(((dbc + 60.0f) / 60.0f) * (float)mh);

    // Clear and redraw meter bar
    _display->fillRect(mx, SCOPE_Y + 3, METER_W - 2, mh, 0x0000);
    _display->drawRect(mx, SCOPE_Y + 3, METER_W - 2, mh, COLOUR_BORDER);

    if (fill > 0) {
        // Thresholds: -18 dB = green→yellow, -6 dB = yellow→red
        const int16_t g18 = (int16_t)((float)mh * 42.0f / 60.0f);
        const int16_t g6  = (int16_t)((float)mh * 54.0f / 60.0f);
        const int16_t gf  = min(fill, g18);
        const int16_t yf  = max((int16_t)0, (int16_t)(min(fill, g6)  - g18));
        const int16_t rf  = max((int16_t)0, (int16_t)(fill - g6));

        if (gf > 0) _display->fillRect(mx+2, SCOPE_Y+3+mh-gf,         METER_W-4, gf, COLOUR_METER_GREEN);
        if (yf > 0) _display->fillRect(mx+2, SCOPE_Y+3+mh-g18-yf,     METER_W-4, yf, COLOUR_METER_YELLOW);
        if (rf > 0) _display->fillRect(mx+2, SCOPE_Y+3+mh-g18-yf-rf,  METER_W-4, rf, COLOUR_METER_RED);
    }

    // dB scale labels (right of meter, static positions)
    _display->setTextSize(1);
    _display->setTextColor(COLOUR_TEXT_DIM, COLOUR_BACKGROUND);
    _display->setCursor(mx + METER_W, SCOPE_Y + 3);          _display->print("0");
    _display->setCursor(mx + METER_W, SCOPE_Y + 3 + mh / 2); _display->print("-30");
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
