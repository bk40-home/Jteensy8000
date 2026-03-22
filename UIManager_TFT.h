// UIManager_TFT.h
// =============================================================================
// Top-level UI manager for the JT-8000 TFT — scrollable accordion edition.
//
// Navigation flow:
//   HOME (scrollable accordion sections)
//     → Encoder L:       navigate sections / controls
//     → Encoder L press: expand/collapse section, or open entry overlay
//     → Encoder R:       adjust selected control value
//     → Encoder R press: open entry overlay on selected control
//     → Encoder L long:  full-screen oscilloscope
//     → Encoder R long:  full-screen preset browser
//     → Touch:           tap headers/controls, swipe to scroll
//
//   SCOPE_FULL (full-screen oscilloscope, orange waveform)
//     → any button/tap:  return to HOME
//
//   BROWSER (full-screen preset list)
//     → Encoder L:       scroll presets
//     → Encoder L press: confirm selection + return HOME
//     → Encoder R press: cancel + return HOME
//     → Touch:           tap row to select, tap cancel to close
//
// Setup sequence (Jteensy8000.ino — unchanged from previous version):
//   1. ui.beginDisplay()    — SPI init + splash. Call BEFORE AudioMemory().
//   2. ui.begin(synth)      — wire screens. Call AFTER synth ready.
//   3. ui.syncFromEngine()  — after preset load.
//   4. ui.pollInputs()      — call >= 100 Hz in loop().
//   5. ui.updateDisplay()   — call at ~30 Hz (rate-limited internally).
// =============================================================================

#pragma once
#include <Arduino.h>
#include "ILI9341_t3n.h"
#include "SynthEngine.h"
#include "HardwareInterface_MicroDexed.h"
#include "TouchInput.h"
#include "AudioScopeTap.h"
#include "HomeScreen.h"
#include "PresetBrowser.h"
#include "Presets.h"
#include "JT8000Colours.h"

// Defined in Jteensy8000.ino
extern AudioScopeTap scopeTap;

class UIManager_TFT {
public:
    // ---- SPI1 pin assignments (unchanged) ----
    static constexpr uint8_t  TFT_CS   = 41;
    static constexpr uint8_t  TFT_DC   = 37;
    static constexpr uint8_t  TFT_RST  = 24;
    static constexpr uint8_t  TFT_MOSI = 26;
    static constexpr uint8_t  TFT_SCK  = 27;
    static constexpr uint8_t  TFT_MISO = 39;
    static constexpr uint32_t SPI_CLOCK_HZ = 30000000;

    // Frame rate cap
    static constexpr uint32_t FRAME_MS = 33;   // ~30 fps

    // Three modes (down from four — SECTION mode removed)
    enum class Mode : uint8_t { HOME = 0, SCOPE_FULL, BROWSER };

    UIManager_TFT();

    // ---- Two-phase init (same API as before) ----
    void beginDisplay();
    void begin(SynthEngine& synth);

    // ---- Main loop calls (same API as before) ----
    void updateDisplay(SynthEngine& synth);
    void pollInputs(HardwareInterface_MicroDexed& hw, SynthEngine& synth);

    // ---- Sync after preset load (same API as before) ----
    void syncFromEngine(SynthEngine& synth);

    // ---- Notify single CC change (for MIDI input live update) ----
    void notifyCC(uint8_t cc);

    void setCurrentPresetIdx(int idx);
    int  getCurrentPresetIdx() const;

    // ---- Compatibility stubs (keep .ino compiling without changes) ----
    void setPage(int)                        {}
    int  getCurrentPage()          const     { return 0; }
    void selectParameter(int)                {}
    int  getSelectedParameter()              { return 0; }
    void setParameterLabel(int, const char*) {}

private:
    static UIManager_TFT* _instance;

    void _setMode(Mode m);
    void _goHome();
    void _openBrowser();
    void _handleTouch(SynthEngine& synth);
    void _drawFullScope();

    // ---- Members ----
    ILI9341_t3n   _display;
    TouchInput    _touch;
    bool          _touchOk;
    Mode          _mode;
    uint32_t      _lastFrame;
    SynthEngine*  _synthRef;

    // Screens
    HomeScreen    _home;
    PresetBrowser _browser;
    int           _currentPresetIdx;

    // Full-screen scope state
    bool          _scopeFullFirstFrame;
    int16_t       _fsPrevWave[288];   // previous waveform Y for erase-before-draw
};
