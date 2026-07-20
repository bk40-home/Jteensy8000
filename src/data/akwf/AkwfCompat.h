// =============================================================================
// AkwfCompat.h — PROGMEM plumbing for the auto-generated AKWF table headers
// =============================================================================
// The generated headers include this file for ONE reason: the PROGMEM macro
// on their table definitions.
//
//   * TEENSY BUILD: Arduino.h supplies the real PROGMEM
//     (__attribute__((section(".progmem")))), which is what keeps 510 KB of
//     wavetables in FLASH.  On the IMXRT1062, plain const/.rodata data is
//     COPIED INTO DTCM at boot — losing this attribute silently costs the
//     entire RAM1 (a real linker failure taught us this: the include below
//     was once mangled by a bulk sed into a self-include, PROGMEM expanded
//     empty, and every table moved to RAM with zero warnings).
//   * HOST BUILD: no Arduino.h exists; PROGMEM defines away and the tables
//     are ordinary const data in the test binary.
//
// SED-PROOFING: the Arduino include below is written `#  include` (spaces
// after the hash — legal preprocessor) precisely so the historical pattern
// `s|#include <Arduino.h>|…|` can never match this file again.  The
// `make attrcheck` placement gate verifies the outcome on every test run.
// =============================================================================
#pragma once
#if defined(ARDUINO)
#  include <Arduino.h>        /* real PROGMEM — see sed-proofing note */
#endif
#ifndef PROGMEM
#define PROGMEM               /* host build: tables are ordinary const data */
#endif
