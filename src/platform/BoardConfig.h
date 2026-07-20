// =============================================================================
// BoardConfig.h — hardware selection for JT-8000 v2 (design brief §8)
// =============================================================================
// The ONLY file where build-flag conditionals live.  Logic code includes
// this and uses the constants; it never tests backend #defines itself —
// the v1 pattern of #ifdefs sprinkled through behaviour code is banned.
//
// Select a backend by building the matching PlatformIO environment:
//   pio run -e jt8000-pcm5102     (default hardware — MicroDexed board)
//   pio run -e jt8000-sgtl5000    (Teensy Audio Shield)
//   pio run -e jt8000-usbonly     (no DAC fitted; USB audio only)
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

// Exactly one backend must be selected by the environment's build_flags.
#if defined(JT_BACKEND_PCM5102) + defined(JT_BACKEND_SGTL5000) + \
    defined(JT_BACKEND_USBONLY) != 1
#error "Select exactly one backend: build a jt8000-* PlatformIO environment"
#endif

namespace JT {
namespace Board {

#if defined(JT_BACKEND_PCM5102)
// PCM5102A DAC on I2S1 (MicroDexed main board).  XSMT (soft-mute, active
// low) is wired to pin 34 — hold LOW until the I2S clocks are stable to
// avoid the power-on click, then release in setup().
inline constexpr int  kMutePin      = 34;
inline constexpr bool kHasI2sOutput = true;
inline constexpr const char* kBackendName = "PCM5102A (I2S1)";

#elif defined(JT_BACKEND_SGTL5000)
// Teensy Audio Shield: control port init handled in main.cpp; no mute pin.
inline constexpr int  kMutePin      = -1;
inline constexpr bool kHasI2sOutput = true;
inline constexpr const char* kBackendName = "SGTL5000 (Audio Shield)";

#else  // JT_BACKEND_USBONLY
inline constexpr int  kMutePin      = -1;
inline constexpr bool kHasI2sOutput = false;
inline constexpr const char* kBackendName = "USB audio only";
#endif

// CPU clock policy (decision F7): 600 MHz default.  Building with
// -D JT_CPU_720MHZ requests 720 MHz — KNOWN ISSUE: USB audio quality
// degrades at 720 MHz on this hardware (v1 field observation, unresolved).
// Do not combine the flag with USB-audio use until root-caused.

// -----------------------------------------------------------------------------
// Serial1 MIDI link (all backends) — Phase B' D3.
//
// ⚠ DEVIATION FROM v1 (flagged, D-1 in PHASE_B_IOHUB_SPEC.md §8): v1 ran
// Serial1 as standard DIN MIDI at 31250 baud (Jteensy8000.cpp:147).  In v2
// the Serial1 peer is the ESP32 controller (Teensy pins 0/1 <-> ESP32 17/18),
// both ends ours, so the link runs at 1 Mbaud: a full 140-parameter state
// push is ~17 ms instead of ~550 ms.  Serial1 is therefore NOT DIN-compatible
// any more — a future 5-pin DIN jack must use another UART at 31250.
// The ESP32 firmware must open its UART at this same constant (Phase C).
// -----------------------------------------------------------------------------
inline constexpr long kSerial1MidiBaud = 1000000;

} // namespace Board
} // namespace JT
