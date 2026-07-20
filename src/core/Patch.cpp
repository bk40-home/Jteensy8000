// =============================================================================
// Patch.cpp — implementation
// =============================================================================
// Format, rationale and API contracts live in Patch.h.  Implementation
// notes here cover only what the header cannot: byte-access strategy and
// the validate-before-apply ordering.
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "core/Patch.h"

#include "core/AudioConfig.h"   // JT_COLD

#include <string.h>   // memcpy — the sanctioned way to type-pun floats

#include "core/dsp/Crc32.h"
#include "core/dsp/Curves.h"

namespace JT {
namespace Patch {

// -----------------------------------------------------------------------------
// Byte-wise little-endian accessors.
//
// Entries sit at 6-byte stride, so every float32 in the image is misaligned.
// The Cortex-M7 tolerates unaligned word access, but relying on that is a
// portability trap (the ESP32 tools and stricter cores do not) — and memcpy
// of a known-size compiles to the optimal load/store on every target anyway.
// These four helpers are the ONLY code that touches raw image bytes.
// -----------------------------------------------------------------------------
namespace {

void put16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

uint16_t get16(const uint8_t* p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

void put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8)  & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

uint32_t get32(const uint8_t* p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

void putF32(uint8_t* p, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof bits);   // never (uint32_t&)v — strict aliasing
    put32(p, bits);
}

float getF32(const uint8_t* p)
{
    const uint32_t bits = get32(p);
    float v;
    memcpy(&v, &bits, sizeof v);
    return v;
}

// Printable-ASCII sanitiser for patch names — a name typed on the JUCE
// editor may contain anything; the TFT font and the SysEx name message
// only handle 32..126.
char sanitise(char c)
{
    return (c >= 32 && c <= 126) ? c : '_';
}

} // namespace

// -----------------------------------------------------------------------------
// save
// -----------------------------------------------------------------------------
JT_COLD size_t save(const ParameterStore& store,
            const char* name, uint8_t category,
            uint8_t* out, size_t cap)
{
    if (out == nullptr || cap < kMaxEncodedSize) return 0;

    // --- header ---
    memcpy(out, kMagic, sizeof kMagic);
    put16(out + 4, Params::kSchemaVersion);
    // entryCount written below once counted — keeps save() correct even if
    // a future table mixes scopes in new ways.
    for (size_t i = 0; i < kNameLen; ++i) {
        // Copy-with-pad in one pass: after the source string ends, NULs.
        const char c = (name != nullptr && name[0] != '\0') ? *name++ : '\0';
        out[8 + i] = (c == '\0') ? 0u : (uint8_t)sanitise(c);
        if (c == '\0') name = "";     // keep reading NULs, stop advancing
    }
    out[24] = category;
    out[25] = out[26] = out[27] = 0;  // reserved — always zero on write

    // --- entries: one per patch-scope parameter, in table (id) order ---
    uint8_t* p = out + kHeaderSize;
    uint16_t count = 0;
    for (size_t i = 0; i < Params::kParamCount; ++i) {
        const Params::ParamDesc& d = Params::kParams[i];
        if (d.scope != Params::Scope::Patch) continue;
        put16(p, d.id);
        // Engineering units on the wire — see the header's encoding decision.
        putF32(p + 2, Curves::toEngineering(d, store.getByIndex(i)));
        p += kEntrySize;
        ++count;
    }
    put16(out + 6, count);

    // --- CRC over everything written so far ---
    const size_t payload = (size_t)(p - out);
    put32(p, Crc32::compute(out, payload));

    return payload + kCrcSize;
}

// -----------------------------------------------------------------------------
// peekInfo — header-only; see Patch.h for why the CRC is skipped here.
// -----------------------------------------------------------------------------
JT_COLD bool peekInfo(const uint8_t* data, size_t len, Info& out)
{
    if (data == nullptr || len < kHeaderSize + kCrcSize) return false;
    if (memcmp(data, kMagic, sizeof kMagic) != 0)        return false;

    out.schemaVersion = get16(data + 4);
    out.entryCount    = get16(data + 6);
    memcpy(out.name, data + 8, kNameLen);
    out.name[kNameLen] = '\0';        // guaranteed display-safe termination
    out.category = data[24];
    return true;
}

// -----------------------------------------------------------------------------
// load — VALIDATE COMPLETELY, THEN APPLY.
//
// Ordering is the safety property: every reject path (short buffer, magic,
// CRC, version, size) runs before beginBulk(), so a corrupt file can never
// leave the store half-written.  Only a byte image that has passed every
// check gets to touch parameters, and then only inside one atomic bulk.
// -----------------------------------------------------------------------------
JT_COLD LoadResult load(const uint8_t* data, size_t len,
                ParameterStore& store, Origin origin,
                const Migration* migrations, size_t migrationCount)
{
    LoadResult r{};
    r.status = Status::TooShort;

    if (data == nullptr || len < kHeaderSize + kCrcSize) return r;

    if (memcmp(data, kMagic, sizeof kMagic) != 0) {
        r.status = Status::BadMagic;
        return r;
    }

    // CRC before anything else is parsed — do not interpret rotten bytes.
    const uint32_t stored   = get32(data + len - kCrcSize);
    const uint32_t computed = Crc32::compute(data, len - kCrcSize);
    if (stored != computed) {
        r.status = Status::BadCrc;
        return r;
    }

    (void)peekInfo(data, len, r.info);   // cannot fail past the checks above

    // A file written by NEWER firmware may use conventions this build has
    // never heard of — refuse loudly rather than half-load quietly.  Files
    // from OLDER firmware are fine: TLV entries are self-describing and the
    // migration table covers renames.
    if (r.info.schemaVersion > Params::kSchemaVersion) {
        r.status = Status::FutureVersion;
        return r;
    }

    // The declared entry count must account for the buffer exactly —
    // anything else means truncation or trailing garbage.
    const size_t expected = kHeaderSize
                          + kEntrySize * (size_t)r.info.entryCount
                          + kCrcSize;
    if (expected != len) {
        r.status = Status::SizeMismatch;
        return r;
    }

    // --- all checks passed: apply as ONE atomic bulk ---
    store.beginBulk();
    const uint8_t* p = data + kHeaderSize;
    for (uint16_t n = 0; n < r.info.entryCount; ++n, p += kEntrySize) {
        uint16_t id = get16(p);

        // Retired id?  Redirect to its successor (linear scan: the table
        // holds a handful of entries at most, and this is a patch load,
        // not a hot path).
        bool wasMigrated = false;
        for (size_t m = 0; m < migrationCount; ++m) {
            if (migrations[m].oldId == id) {
                id = migrations[m].newId;
                wasMigrated = true;
                break;
            }
        }

        const size_t idx = ParameterStore::indexOf(id);
        if (idx == ParameterStore::kInvalidIndex) {
            // Unknown id: a param this firmware doesn't have (newer file,
            // or retired without migration).  Skip it, count it, and let
            // the caller decide whether to log — the rest of the patch
            // still loads.  v1 would have rejected or misapplied here.
            ++r.skippedUnknown;
            continue;
        }

        // Engineering -> normalized through the CURRENT curve/range;
        // Curves clamps out-of-range and filters NaN, so even a value from
        // a wider-ranged future firmware degrades gracefully.
        const float norm = Curves::toNorm(Params::kParams[idx], getF32(p + 2));
        store.setByIndex(idx, norm, origin);
        ++r.applied;
        if (wasMigrated) ++r.migrated;
    }
    store.endBulk();

    r.status = Status::Ok;
    return r;
}

} // namespace Patch
} // namespace JT
