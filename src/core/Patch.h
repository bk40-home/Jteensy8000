// =============================================================================
// Patch.h — versioned binary patch format for JT-8000 v2
// =============================================================================
//
// ROLE IN THE ARCHITECTURE (design brief §7)
//   Serialises the patch-scope slice of a ParameterStore to a byte image and
//   back.  This module is a pure CODEC: it never touches a filesystem — the
//   platform storage layer (LittleFS/SD, Phase 5) hands it buffers and
//   handles atomic write/rename.  That split is what makes the format fully
//   unit-testable on the host.
//
// WHY THIS REPLACES v1'S FORMAT
//   v1 patches were JSON strings of CC NUMBERS with a "very light parser":
//   no checksum (bit rot loads silently), no migration path (any CC remap
//   corrupts every old patch), 7-bit values only.  v2 patches are keyed to
//   PERMANENT ParamIDs, carry a CRC-32, a schema version, and a migration
//   table for retired IDs — old patches survive firmware evolution.
//
// VALUE ENCODING DECISION (signed off, July 2026)
//   Entries store ENGINEERING UNITS (Hz, ms, option index...) as float32,
//   not normalized 0..1.  ParamIDs are permanent but RANGES may legitimately
//   be retuned (v1's gain staging history proves they will be) — a stored
//   "1000 Hz" still means 1000 Hz after the cutoff range widens, where a
//   stored "0.62" would silently shift pitch.  Out-of-range values clamp on
//   load via Curves, so a range NARROWING degrades gracefully too.
//
// WIRE FORMAT (all multi-byte fields little-endian, packed, no padding
// assumptions — every access goes through byte-wise helpers so the image
// can sit at any alignment):
//
//   offset size  field
//   0      4     magic "JTP1"
//   4      2     schemaVersion   (from the generated table)
//   6      2     entryCount      N
//   8      16    name            (printable ASCII, NUL-padded, no
//                                 terminator required at 16 chars)
//   24     1     category        (free-form tag for the browser)
//   25     3     reserved        (zero — future flags without a new magic)
//   28     6*N   entries         { uint16 ParamID, float32 value }
//   28+6N  4     CRC-32          over every byte before this field
//
// CONCURRENCY
//   Control plane only.  apply() wraps the whole load in one
//   beginBulk()/endBulk(), so the audio plane hears the OLD patch until the
//   instant the ENTIRE new one is published — the half-loaded-patch bug of
//   v1 is structurally impossible (proven in test_parameter_store.cpp).
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "core/ParameterStore.h"
#include "gen/ParamTable.h"

namespace JT {
namespace Patch {

// --- format constants (frozen — changing any of these needs a new magic) ---
inline constexpr uint8_t  kMagic[4]   = { 'J', 'T', 'P', '1' };
inline constexpr size_t   kNameLen    = 16;
inline constexpr size_t   kHeaderSize = 28;
inline constexpr size_t   kEntrySize  = 6;    // uint16 id + float32 value
inline constexpr size_t   kCrcSize    = 4;

// Number of patch-scope parameters, counted from the generated table at
// compile time — so buffer bounds below are constants, not runtime queries.
constexpr size_t patchParamCount()
{
    size_t n = 0;
    for (size_t i = 0; i < Params::kParamCount; ++i)
        if (Params::kParams[i].scope == Params::Scope::Patch) ++n;
    return n;
}

// Worst-case encoded size — callers size their buffers with this single
// constant.  (Today: 28 + 6*N + 4; grows automatically with params.yaml.)
inline constexpr size_t kMaxEncodedSize =
    kHeaderSize + kEntrySize * patchParamCount() + kCrcSize;

// -----------------------------------------------------------------------------
// Migration of retired ParamIDs (design brief §4.2).
//
// When a parameter is retired in params.yaml, patches in the field still
// contain its old id.  A Migration row redirects that id to its successor;
// the value carries across unchanged (both sides are engineering units, and
// Curves clamps if the successor's range differs).  The REAL table will be
// emitted by the generator once retired_ids gains its first entry — today it
// is empty, and load() accepts an injected table so the mechanism stays
// tested from day one rather than rotting until first needed.
// -----------------------------------------------------------------------------
struct Migration {
    uint16_t oldId;    // retired id found in old patch files
    uint16_t newId;    // current id that inherits the value
};

// --- load diagnostics --------------------------------------------------------
enum class Status : uint8_t {
    Ok,
    TooShort,        // buffer smaller than header + CRC
    BadMagic,        // not a JT-8000 v2 patch
    BadCrc,          // bit rot / truncated copy — NOTHING was applied
    FutureVersion,   // written by newer firmware — refuse rather than guess
    SizeMismatch,    // entryCount disagrees with the buffer length
};

struct Info {
    char     name[kNameLen + 1];   // always NUL-terminated for display
    uint8_t  category;
    uint16_t schemaVersion;
    uint16_t entryCount;
};

struct LoadResult {
    Status   status;
    Info     info;             // valid when status == Ok
    uint16_t applied;          // entries written into the store
    uint16_t migrated;         // subset of 'applied' that went via a Migration
    uint16_t skippedUnknown;   // ids not in this firmware's table (logged
                               // by the caller — core has no Serial)
};

// -----------------------------------------------------------------------------
// API — all control-plane, all heap-free, all bounds-checked.
// -----------------------------------------------------------------------------

// Serialise the store's patch-scope parameters.  'name' is truncated to 16
// chars and sanitised to printable ASCII (TFT and SysEx both choke on
// control bytes).  Returns bytes written, or 0 if 'cap' is too small
// (size buffers with kMaxEncodedSize and this cannot happen).
size_t save(const ParameterStore& store,
            const char* name, uint8_t category,
            uint8_t* out, size_t cap);

// Validate and apply a patch image.  On any status other than Ok the store
// is COMPLETELY untouched — validation happens before the bulk write starts.
// 'migrations' is normally the generator-emitted table (empty today);
// tests inject their own.
LoadResult load(const uint8_t* data, size_t len,
                ParameterStore& store,
                Origin origin = Origin::PatchLoad,
                const Migration* migrations = nullptr,
                size_t migrationCount = 0);

// Header-only read for the preset browser: name/category/version without
// touching a store.  Checks magic and length but NOT the CRC (the browser
// lists hundreds of files; full-image CRC happens on the one being loaded).
bool peekInfo(const uint8_t* data, size_t len, Info& out);

} // namespace Patch
} // namespace JT
