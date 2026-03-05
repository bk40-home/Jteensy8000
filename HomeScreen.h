// HomeScreen.h
// =============================================================================
// JT-8000 home screen: oscilloscope + 8 section tile grid.
//
// Layout (320 × 240 px):
//   y=0    Header   22 px  — product name + CPU%
//   y=22   Scope    88 px  — waveform (WAVE_W px wide) + peak meter (METER_W px)
//   y=110  Tiles   124 px  — 4+4 section tiles
//   y=234  Footer    6 px  — hint text
//
// Scope rendering:
//   To prevent flicker the previous waveform is erased pixel-by-pixel using
//   _prevWave[] before each new frame is drawn.  This avoids a fillRect() on
//   the scope band (~60 000 pixels) every frame.
//
// Peak meter:
//   Exponential smoothing with PEAK_DECAY constant gives a VU-meter feel.
//   Attack = instant (raw peak wins).  Decay ~12 frames to reach silence.
//
// Tiles:
//   Repaint only on touch/encoder events (_tilesDirty flag).
//
// Header CPU%:
//   Rate-limited to HEADER_REDRAW_MS to reduce SPI overhead.
// =============================================================================

#pragma once
#include <Arduino.h>
#include "ILI9341_t3n.h"
#include "AudioScopeTap.h"
#include "SynthEngine.h"
#include "TFTWidgets.h"
#include "JT8000_Sections.h"

using SectionSelectedCallback = void (*)(int sectionIndex);

class HomeScreen {
public:
    // ---- Layout constants ----
    static constexpr int16_t SW              = 320;
    static constexpr int16_t SH              = 240;
    static constexpr int16_t HEADER_H        = 22;
    static constexpr int16_t SCOPE_H         = 88;
    static constexpr int16_t FOOTER_H        = 6;
    static constexpr int16_t SCOPE_Y         = HEADER_H;
    static constexpr int16_t TILE_Y          = HEADER_H + SCOPE_H;
    static constexpr int16_t TILE_H          = SH - TILE_Y - FOOTER_H;
    static constexpr int16_t ROW_H           = TILE_H / 2;
    static constexpr int16_t TILE_W          = 78;
    static constexpr int16_t TILE_GAP        = 2;
    static constexpr int16_t METER_W         = 26;
    static constexpr int16_t WAVE_W          = SW - METER_W - 4;

    // Waveform column count matches WAVE_W - 2 (border inset)
    static constexpr int16_t WAVE_COLS       = WAVE_W - 2;

    // ---- Timing constants ----
    /** Header CPU% redrawn at most every N ms to reduce SPI traffic. */
    static constexpr uint32_t HEADER_REDRAW_MS = 500;

    // ---- Peak meter smoothing ----
    /** Exponential decay coefficient per frame (~0.85 → ~12-frame fall to silence). */
    static constexpr float PEAK_DECAY = 0.85f;

    HomeScreen();

    // Wire display, scope tap, and section-selected callback. Call once.
    void begin(ILI9341_t3n* disp, AudioScopeTap* tap, SectionSelectedCallback cb);

    // Repaint the screen. Call every display frame.
    void draw(SynthEngine& synth);

    // Touch routing
    bool onTouch(int16_t x, int16_t y);
    void onTouchRelease(int16_t x, int16_t y);

    // Encoder: delta moves tile highlight; press fires highlighted tile
    void onEncoderDelta(int delta);
    void onEncoderPress();

    // One-shot flag: true if scope area was tapped since last call
    bool isScopeTapped();

    // Force full repaint on next draw() — call when returning from another screen
    void markFullRedraw();

private:
    static HomeScreen* _instance;   // singleton for tile lambda callbacks

    void _fire(int idx);

    void _drawHeader(SynthEngine& synth, bool fullRepaint);
    void _drawScope();
    void _drawAllTiles();
    void _drawFooter();

    ILI9341_t3n*            _display;
    AudioScopeTap*          _scopeTap;
    SectionSelectedCallback _onSection;

    int       _highlighted;     // keyboard-navigation cursor index
    bool      _fullRedraw;
    bool      _tilesDirty;
    bool      _scopeTapped;     // one-shot tap flag

    float     _peakSmooth;      // exponentially-smoothed peak level (0..1)
    uint32_t  _lastHeaderMs;    // timestamp of last CPU% redraw

    // Previous waveform Y positions for pixel-erase (no-flicker technique)
    // One entry per waveform column pixel.
    int16_t   _prevWave[WAVE_COLS];

    // Section tiles — one per kSections[] entry
    TFTSectionTile _t0, _t1, _t2, _t3;   // top row
    TFTSectionTile _t4, _t5, _t6, _t7;   // bottom row (kSections[7] = PRESETS)
    TFTSectionTile* _tiles[SECTION_COUNT];
};
