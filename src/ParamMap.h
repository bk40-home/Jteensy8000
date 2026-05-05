// =============================================================================
// ParamMap.h — JT-8000 ParamID routing table (declarations)
//
// Maps the 14-bit ParamID address space to the firmware's existing CC dispatch
// system. Phase 1 strategy: ParamIDs that have a CC alias dispatch through the
// existing CC handler path (well-tested, lossy at the bucket level). The
// SysEx-only abstractions (0x0C80..0x0C87 from the Phase 0 map) have no CC
// alias and are handled inside SysExAdapter directly.
//
// Phase 3 will add direct float setters that bypass CC quantisation for the
// hot params (Cutoff, Resonance, Env Times, LFO Freq). For now this table
// only carries the CC alias + scope hint.
//
// Source of truth: JT8000_ParamMap_Phase0_v2.2.md
//
// © 2025 Kris Bishop — MIT licensed.
// =============================================================================

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace ParamMap {

// -----------------------------------------------------------------------------
// Scope hint — tells the SysEx adapter where this param lives.
//
// The adapter uses this to choose the right setter:
//   kPatch     -> route via LayerManager::setCCExplicit(layer, cc, value)
//                 (channel-bypass; honours explicit layer A/B/Both)
//   kPerf      -> route via LayerManager::handleControlChange(1, cc, value)
//                 (the perf-CC switch handles it locally)
//   kGlobalFx  -> route via LayerManager::handleControlChange(1, cc, value)
//                 (the global-FX-CC switch handles it locally)
//   kSysExOnly -> handled entirely inside SysExAdapter; no CC alias.
// -----------------------------------------------------------------------------
enum Scope : uint8_t {
    kPatch     = 0,
    kPerf      = 1,
    kGlobalFx  = 2,
    kSysExOnly = 3,
};

// -----------------------------------------------------------------------------
// Value-type hint — used by the adapter to convert incoming SysEx float into
// the CC byte the firmware's CC handler expects. Phase 1 uses a small set;
// Phase 3 will add fine-grained float setters that don't need this.
// -----------------------------------------------------------------------------
enum ValueType : uint8_t {
    kCont      = 0, // 0..1 normalised, mapped via param-specific curve in firmware
    kBipolar   = 1, // -1..+1; CC 64 = exact zero (firmware dead-zone)
    kNorm01    = 2, // 0..1 linear → CC 0..127
    kEnumN     = 3, // discrete index 0..N-1; bucket-midpoint encode to CC
    kToggle    = 4, // bool → CC 0 or 127 (post-Q4 standardisation: threshold >=64)
    kInt7      = 5, // raw 0..127 byte
    kSignedOff = 6, // -1..N (CC 0 = -1/OFF, 1..127 = scaled to 0..N)
    kSysEx     = 7, // SysEx-only, handled by adapter
};

// -----------------------------------------------------------------------------
// Routing entry — one per ParamID. Compact (4 bytes per row).
//
//   paramId : 14-bit ID in upper bits of uint16_t (0x0000..0x3FFF)
//   ccAlias : the CC number 0..127, or kNoCC (0xFF) for SysEx-only params
//   scope   : where the param is owned (Patch / Perf / GlobalFx / SysExOnly)
//   type    : how to convert SysEx float to a CC byte
//
// Entries are sorted by paramId to allow binary search.
// -----------------------------------------------------------------------------
struct Entry {
    uint16_t paramId;
    uint8_t  ccAlias;
    uint8_t  scope : 4;
    uint8_t  type  : 4;
};

static constexpr uint8_t kNoCC = 0xFF;

// -----------------------------------------------------------------------------
// Lookup — O(log N) binary search by ParamID. Returns nullptr if not found.
// -----------------------------------------------------------------------------
const Entry* find(uint16_t paramId);

// Index-based accessor — used by the SysEx adapter to walk the whole table
// for BANK_DUMP composition. Returns nullptr if i >= entryCount().
const Entry* entryAt(size_t i);

// Total number of entries in the table (for diagnostics / tests).
size_t entryCount();

// -----------------------------------------------------------------------------
// SysEx-only ParamID constants — referenced from SysExAdapter for the section
// 12b abstraction handling. Kept here so all ParamID literals live in one place.
// -----------------------------------------------------------------------------
namespace SysExOnlyIds {
    static constexpr uint16_t kFxModEnabled       = 0x0C80;
    static constexpr uint16_t kFxModVariation     = 0x0C81;
    static constexpr uint16_t kFxModFbOverride    = 0x0C82;
    static constexpr uint16_t kFxModFbValue       = 0x0C83;
    static constexpr uint16_t kFxDelayEnabled     = 0x0C84;
    static constexpr uint16_t kFxDelayVariation   = 0x0C85;
    static constexpr uint16_t kFxDelayFbOverride  = 0x0C86;
    static constexpr uint16_t kFxDelayFbValue     = 0x0C87;
}

} // namespace ParamMap
