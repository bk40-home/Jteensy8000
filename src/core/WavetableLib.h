// =============================================================================
// WavetableLib.h — arbitrary-wavetable access for JT-8000 v2 (AKWF banks)
// =============================================================================
//
// ROLE
//   The engine-facing door to the AKWF data: give it a bank and wave index,
//   get back a bounds-safe table pointer for OscCore::setArbTable().
//
// WHY THIS FILE IS SO THIN (a deliberate build-hygiene decision)
//   The AKWF catalogue is ~2.1 MB of generated headers.  ONLY WavetableLib
//   .cpp includes them — every other translation unit sees just these four
//   prototypes, so a change to SynthCore never recompiles two megabytes of
//   tables, and host test builds stay fast.  This is the standard
//   "firewall header" pattern for large generated data.
//
// V1 SEMANTICS PRESERVED (verified in SynthEngine's CC handlers)
//   * Bank select buckets the normalized knob evenly across the 10 banks:
//     bank = floor(norm × 10), clamped.
//   * Index buckets against the CURRENT bank's own count (banks differ in
//     size), clamped to count-1 — so a bank switch with a high index lands
//     on the new bank's last wave instead of nullptr.
//   * The underlying per-bank accessors already return nullptr + len 0 for
//     impossible indices; OscCore's Arb mode then falls back to the naive
//     saw exactly as v1's arbdata guard did.  Defence in depth.
//
// MEMORY (Teensy 4.1)
//   All tables are const and live in flash (PROGMEM is a no-op on the
//   IMXRT1062); zero RAM cost.  Flash reads through the cache are fast
//   enough for the linear-interp lookup in OscCore.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>

namespace JT {
namespace WavetableLib {

inline constexpr int kNumBanks = 10;

// Bounds-clamped fetch: any int is safe to pass.  Returns the table pointer
// and writes its length; never returns a dangling pointer.  A nullptr
// return (only possible for an empty bank) is legal input to setArbTable.
const int16_t* akwfTable(int bank, int index, uint16_t& lenOut);

// Number of waves in a bank (0 for an out-of-range bank).
uint16_t bankCount(int bank);

// v1 bucketing laws for the two normalized knobs (see header note).
int bankFromNorm(float norm01);
int indexFromNorm(float norm01, int bank);

} // namespace WavetableLib
} // namespace JT
