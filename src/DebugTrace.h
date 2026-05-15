/* Audio Library for Teensy
 * Copyright (c) 2025, Kris Bishop, bishopkris40@hotmail.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
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

// =============================================================================
// JT_CC_LOG — separate gate for CC handler logs.
//
// CC handlers run on the MIDI handler stack (i.e. inside usbMIDI.read()) and
// the same handlers fire dozens of times per second when a JUCE-style editor
// is connected and dumping its session.  Each Serial.printf can block on the
// USB-CDC TX ring under flood, which stalls loop() and causes USB MIDI
// inbound buffers to back up — the documented "MIDI handler must not call
// Serial.print*" rule applies.
//
// Set JT_LOG_CC_HANDLERS = 0 (default) to compile out CC-handler logs while
// leaving lifecycle / structural JT_LOGFs intact.  Set to 1 only when actively
// debugging a CC-handler path.
// =============================================================================
#ifndef JT_LOG_CC_HANDLERS
#define JT_LOG_CC_HANDLERS 0
#endif

#if JT_DEBUG_TRACE && JT_LOG_CC_HANDLERS
  #define JT_CC_LOG(fmt, ...) \
      do { Serial.printf(fmt, ##__VA_ARGS__); } while (0)
#else
  #define JT_CC_LOG(fmt, ...) do {} while (0)
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
  // JT_CC_LOG already defined above as a no-op when JT_LOG_CC_HANDLERS == 0,
  // and JT_DEBUG_TRACE == 0 forces JT_LOG_CC_HANDLERS off via the inner test.
#endif
