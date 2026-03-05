// DebugTrace.h
// =============================================================================
// JT-8000 unified serial logging system.
//
// ALL serial output in the project MUST use these macros. Direct calls to
// Serial.print/printf/println are forbidden outside this header.
//
// Compile-time control:
//   JT_DEBUG_TRACE  0  — strip ALL log code (zero overhead in release builds)
//   JT_DEBUG_TRACE  1  — enable logging (default during development)
//
// Rate-limited variant:
//   JT_LOGF_RATE(interval_ms, fmt, ...) — logs at most once per interval.
//   Use this for parameters that change continuously (e.g. filter cutoff,
//   LFO frequency) to avoid flooding the serial bus.
//
// IMPORTANT: Never call Serial.printf() from audio ISR context or from
// MIDI handlers. Use the midiLog() ring buffer in the main .ino for those.
// These macros are for engine/UI code running in the main loop only.
// =============================================================================
#pragma once
#include <Arduino.h>

#ifndef JT_DEBUG_TRACE
#define JT_DEBUG_TRACE 1   // Set to 0 to strip all logs at compile time
#endif

#if JT_DEBUG_TRACE
  // Standard log — immediate serial output
  #define JT_LOGF(fmt, ...) \
      do { Serial.printf(fmt, ##__VA_ARGS__); } while (0)

  #define JT_LOGNL() \
      do { Serial.println(); } while (0)

  // Rate-limited log — fires at most once per interval_ms milliseconds.
  // Uses a static local variable per call site to track timing.
  #define JT_LOGF_RATE(interval_ms, fmt, ...) \
      do { \
          static uint32_t _jt_last_log_ms = 0; \
          const uint32_t _jt_now = millis(); \
          if ((_jt_now - _jt_last_log_ms) >= (interval_ms)) { \
              _jt_last_log_ms = _jt_now; \
              Serial.printf(fmt, ##__VA_ARGS__); \
          } \
      } while (0)

  // Conditional set-with-log — only writes and logs when value actually changes.
  // Useful for parameters updated every frame but rarely changing.
  #define JT_SETF_WITH_LOG(var, newval, label) \
      do { \
          float __old = (var); \
          float __nv = (newval); \
          if (fabsf(__old - __nv) > 1e-6f) { \
              (var) = __nv; \
              Serial.printf("[ENG] %s: %.6f -> %.6f\n", (label), __old, __nv); \
          } \
      } while (0)

#else
  // All logging compiled out — zero overhead
  #define JT_LOGF(...)                    do {} while (0)
  #define JT_LOGNL()                      do {} while (0)
  #define JT_LOGF_RATE(interval_ms, ...)  do {} while (0)
  #define JT_SETF_WITH_LOG(v, n, l)       do { (v) = (n); } while (0)
#endif
