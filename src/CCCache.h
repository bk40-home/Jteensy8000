// =============================================================================
// CCCache.h — last-CC-sent cache (Phase 2 + Phase 4 foundation)
//
// Records the most recent CC value seen for every controllable CC, per scope.
// Provides the "current state" view that GET_PARAM and BANK_DUMP need.
//
// Layout:
//   - Per-layer arrays (A and B) of 128 bytes each, indexed by patch CC number
//     (0..127). Patch-scope CCs only.
//   - One shared array of 16 bytes for GlobalFX CCs, indexed by lookup table
//     (CCs 70/71/75/93/94/95/96/97/98 are sparse so we pack them).
//   - One shared array of 7 bytes for Performance CCs (CCs 140..146 → indices 0..6).
//
// The Layer A/B arrays are indexed directly by the CC byte for O(1) reads.
// 90 of those 128 entries are actually used (the patchable CCs); the rest are
// wasted bytes — trivial cost, dramatically simpler than a hash map.
//
// Initial state: zero. Boot patch load through CC dispatch fills the cache as
// part of normal applyTo() — no separate init pass needed.
//
// Mutation: a single chokepoint, LayerManager::cacheCC(), keeps the cache in
// sync regardless of whether the CC came from MIDI, internal patch load,
// SysEx, or UI.
//
// Phase 4 will replace this byte-cache with a native float cache. The shape
// of the read API stays the same — only the storage type changes.
//
// © 2025 Kris Bishop — MIT licensed.
// =============================================================================

#pragma once

#include <stdint.h>
#include <string.h>     // memset

namespace CCCache {

// -----------------------------------------------------------------------------
// Sentinel returned for "not in cache yet" reads. 0xFF is safe because no real
// MIDI CC value ever equals 0xFF (CCs are 7-bit, 0..127). Callers that need
// strict "is the slot populated?" semantics should check for 0xFF.
// -----------------------------------------------------------------------------
static constexpr uint8_t kUnset = 0xFF;

// -----------------------------------------------------------------------------
// GlobalFX CC packing — 9 sparse CCs packed into 9 indices for compact storage.
// kGfxIndex returns 0..8 for valid GlobalFX CCs, or 0xFF if the CC isn't a
// GlobalFX CC.
//
// The order matches Phase 0 v2.2 map section 16: Size, HiDamp, LoDamp, Mix,
// Bypass, Shimmer, Freeze, Lowpass, Hipass.
// -----------------------------------------------------------------------------
inline uint8_t gfxIndex(uint8_t cc) {
    switch (cc) {
        case 70: return 0; // FX_REVERB_SIZE
        case 71: return 1; // FX_REVERB_DAMP (Hi)
        case 93: return 2; // FX_REVERB_LODAMP
        case 75: return 3; // FX_REVERB_MIX
        case 94: return 4; // FX_REVERB_BYPASS
        case 95: return 5; // FX_REVERB_SHIMMER
        case 96: return 6; // FX_REVERB_FREEZE
        case 97: return 7; // FX_REVERB_LOWPASS
        case 98: return 8; // FX_REVERB_HIPASS
        default: return 0xFF;
    }
}
static constexpr uint8_t kGfxCount = 9;

// -----------------------------------------------------------------------------
// Performance CCs are CCs 140..146 in the firmware. Pack to 0..6.
// -----------------------------------------------------------------------------
inline uint8_t perfIndex(uint8_t cc) {
    return (cc >= 140 && cc <= 146) ? (uint8_t)(cc - 140) : 0xFF;
}
static constexpr uint8_t kPerfCount = 7;
static constexpr uint8_t kPerfCcBase = 140;   // CC 140 = index 0

// -----------------------------------------------------------------------------
// The cache itself — small enough to live as a struct member of LayerManager.
// Allocates ~272 bytes total. RAM is not the constraint here.
// -----------------------------------------------------------------------------
struct Storage {
    uint8_t patchA[128];
    uint8_t patchB[128];
    uint8_t gfx   [kGfxCount];
    uint8_t perf  [kPerfCount];

    Storage() { reset(); }

    // Wipe everything to kUnset. Called from LayerManager::begin() before
    // the boot patch is applied.
    void reset() {
        memset(patchA, kUnset, sizeof(patchA));
        memset(patchB, kUnset, sizeof(patchB));
        memset(gfx,    kUnset, sizeof(gfx));
        memset(perf,   kUnset, sizeof(perf));
    }
};

} // namespace CCCache
