// =============================================================================
// Crc32.h — IEEE 802.3 CRC-32 for JT-8000 v2
// =============================================================================
//
// ROLE
//   Integrity check for everything that leaves RAM: patch files (Patch.h),
//   and later performance files and SysEx bulk dumps.  One implementation,
//   one polynomial, everywhere — v1 had no integrity checking at all, so a
//   single flipped bit in a JSON patch loaded silently as a wrong sound.
//
// ALGORITHM
//   Standard reflected CRC-32 (polynomial 0xEDB88320, init 0xFFFFFFFF,
//   final XOR 0xFFFFFFFF) — the zlib/PNG/Ethernet variant, chosen because
//   every desktop tool can verify it (`python3 -c "import zlib; ..."`).
//
// CPU / MEMORY
//   Table-driven: one byte per step, 256 × 4 B lookup table.  The table is
//   built at COMPILE TIME (constexpr) so it lives in flash and costs zero
//   boot cycles and zero RAM.  A patch file (~900 B) checks in ~4 µs at
//   600 MHz — control-plane cost only, never in the audio ISR.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace JT {
namespace Crc32 {

// One-shot: CRC of a complete buffer.  This is the common case (patch
// save/load, where the whole image is in RAM anyway).
uint32_t compute(const uint8_t* data, size_t len);

// Streaming variant for data that arrives in pieces — a SysEx bulk dump
// spans many USB packets and should be checksummed as it lands rather than
// buffered twice.  Usage:
//     uint32_t s = Crc32::begin();
//     s = Crc32::update(s, chunk, chunkLen);   // repeat per chunk
//     uint32_t crc = Crc32::end(s);
uint32_t begin();
uint32_t update(uint32_t state, const uint8_t* data, size_t len);
uint32_t end(uint32_t state);

} // namespace Crc32
} // namespace JT
