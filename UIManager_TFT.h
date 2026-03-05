// UIManager_TFT.h
// =============================================================================
// Top-level UI manager for the JT-8000 TFT variant.
//
// Navigation flow:
//   HOME (scope + tiles)
//     → tap section tile      → SECTION (page tabs + param rows)
//       → tap row / hold-R    → ENTRY OVERLAY (keypad or list)
//     → tap PRESETS tile      → BROWSER (full-screen preset list)
//     → hold left encoder     → SCOPE_FULL (full-screen oscilloscope)
//
// Setup sequence (Jteensy8000.ino):
//   1. ui.beginDisplay()    — SPI init + boot splash. Call BEFORE AudioMemory().
//   2. ui.begin(synth)      — wire screens to engine. Call AFTER synth ready.
//   3. ui.syncFromEngine()  — after preset load to force initial repaint.
//   4. ui.pollInputs()      — call at >= 100 Hz in loop().
//   5. ui.updateDisplay()   — call at ~30 Hz in loop() (rate-limited internally).
//
// Why two init functions?
//   beginDisplay() initialises SPI1 before AudioMemory(). If SPI DMA and the
//   audio DMA scheduler both configure the same bus at startup, intermittent
//   hard-faults occur. begin(synth) needs a live SynthEngine reference so it
//   must come after synth initialisation.
//
// SPI clock:
//   ILI9341_t3n runs at 30 MHz (not the 50 MHz default). The 50 MHz clock
//   causes intermittent hard-faults on boards with longer SPI traces due to
//   signal-integrity issues. 30 MHz is safe and still delivers > 30 fps.
// =============================================================================

#pragma once
#include <Arduino.h>
#include "ILI9341_t3n.h"
#include "SynthEngine.h"
#include "HardwareInterface_MicroDexed.h"
#include "TouchInput.h"
#include "AudioScopeTap.h"
#include "HomeScreen.h"
#include "SectionScreen.h"
#include "JT8000_Sections.h"
#include "PresetBrowser.h"
#include "Presets.h"

// Defined in Jteensy8000.ino — shared scope tap object
extern AudioScopeTap scopeTap;

class UIManager_TFT {
public:
    // ---- SPI1 pin assignments (Teensy 4.1) ----
    static constexpr uint8_t  TFT_CS   = 41;
    static constexpr uint8_t  TFT_DC   = 37;
    static constexpr uint8_t  TFT_RST  = 24;
    static constexpr uint8_t  TFT_MOSI = 26;
    static constexpr uint8_t  TFT_SCK  = 27;
    static constexpr uint8_t  TFT_MISO = 39;

    // 30 MHz — safe on typical PCB trace lengths.
    // 50 MHz (ILI9341_t3n default) can cause hard-faults with marginal wiring.
    static constexpr uint32_t SPI_CLOCK_HZ = 30000000;

    // Frame rate cap: 33 ms = 30 fps.
    // Raise to 66 ms (15 fps) if TFT SPI causes audio glitches.
    static constexpr uint32_t FRAME_MS = 33;

    enum class Mode : uint8_t { HOME = 0, SECTION, SCOPE_FULL, BROWSER };

    UIManager_TFT();

    // -------------------------------------------------------------------------
    // beginDisplay() — hardware-only init. Call BEFORE AudioMemory().
    //   Initialises SPI1, sets display rotation, clears screen, shows splash,
    //   initialises the touch controller.
    // -------------------------------------------------------------------------
    void beginDisplay();

    // -------------------------------------------------------------------------
    // begin() — wire screens to engine. Call AFTER AudioMemory() and synth init.
    // -------------------------------------------------------------------------
    void begin(SynthEngine& synth);

    // -------------------------------------------------------------------------
    // updateDisplay() — call at ~30 Hz from loop().
    //   Rate-limited to FRAME_MS internally; calling more often is harmless.
    // -------------------------------------------------------------------------
    void updateDisplay(SynthEngine& synth);

    // -------------------------------------------------------------------------
    // pollInputs() — call at >= 100 Hz from loop().
    //   Reads touch controller and encoder deltas/buttons, routes to active mode.
    // -------------------------------------------------------------------------
    void pollInputs(HardwareInterface_MicroDexed& hw, SynthEngine& synth);

    // -------------------------------------------------------------------------
    // syncFromEngine() — force repaint after preset load.
    // -------------------------------------------------------------------------
    void syncFromEngine(SynthEngine& synth);

    void setCurrentPresetIdx(int idx);
    int  getCurrentPresetIdx() const;

    // ---- Compatibility stubs (match UIManager_MicroDexed API) ----
    void setPage(int)                        {}
    int  getCurrentPage()          const     { return 0; }
    void selectParameter(int)                {}
    int  getSelectedParameter()              { return 0; }
    void setParameterLabel(int, const char*) {}

private:
    static UIManager_TFT* _instance;   // singleton for static callbacks

    void _setMode(Mode m);
    void _goHome();
    void _openSection(int idx);
    void _handleTouch(SynthEngine& synth);
    void _drawFullScope(SynthEngine& synth);

    // ---- Diagnostic ----
    //   while (true) {}              // halt so you can read the screen
    //
    // Remove both lines once colours are confirmed correct.

    // ---- Members ----
    ILI9341_t3n   _display;
    TouchInput    _touch;
    bool          _touchOk;
    Mode          _mode;
    int           _activeSect;
    uint32_t      _lastFrame;
    SynthEngine*  _synthRef;
    HomeScreen    _home;
    SectionScreen _section;
    PresetBrowser _browser;
    int           _currentPresetIdx;
    bool          _scopeFullFirstFrame;   // true = draw static chrome this frame
    float         _fsPeakSmooth;          // full-screen scope peak (exponential decay)
    int16_t       _fsPrevWave[282];        // per-column previous Y for erase-before-draw
};
