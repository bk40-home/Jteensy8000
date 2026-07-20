// =============================================================================
// test_patch.cpp — proofs for core/Patch and core/dsp/Crc32
// =============================================================================
// Strategy: build REAL images with save(), then attack them — flip bits,
// lie about counts, forge versions, plant unknown and retired ids — and
// prove load() either applies everything atomically or touches nothing.
// =============================================================================
#include "doctest.h"

#include <string.h>
#include <cmath>

#include "core/Patch.h"
#include "core/ParameterStore.h"
#include "core/dsp/Crc32.h"
#include "core/dsp/Curves.h"
#include "gen/ParamTable.h"

using namespace JT;
using namespace JT::Params;

namespace {

// Deterministic value generator (LCG) — reproducible "random" patches
// without dragging <random> variability into CI.
struct Lcg {
    uint32_t s;
    explicit Lcg(uint32_t seed) : s(seed) {}
    float next01() { s = s * 1664525u + 1013904223u; return (float)(s >> 8) / 16777216.0f; }
};

// Fill every patch-scope param with a deterministic value; return count.
size_t scramblePatchParams(ParameterStore& st, uint32_t seed)
{
    Lcg rng(seed);
    size_t n = 0;
    st.beginBulk();
    for (size_t i = 0; i < kParamCount; ++i) {
        if (kParams[i].scope != Scope::Patch) continue;
        st.setByIndex(i, rng.next01(), Origin::Ui);
        ++n;
    }
    st.endBulk();
    return n;
}

void drain(ParameterStore& s)
{
    while (s.takeNextDirty() != ParameterStore::kInvalidIndex) {}
}

size_t drainCount(ParameterStore& s)
{
    size_t n = 0;
    while (s.takeNextDirty() != ParameterStore::kInvalidIndex) ++n;
    return n;
}

// Re-seal an image after a deliberate header/payload edit so only the field
// under test is wrong (otherwise every attack just reads as BadCrc).
void reseal(uint8_t* img, size_t len)
{
    const uint32_t crc = Crc32::compute(img, len - Patch::kCrcSize);
    img[len - 4] = (uint8_t)(crc & 0xFFu);
    img[len - 3] = (uint8_t)((crc >> 8) & 0xFFu);
    img[len - 2] = (uint8_t)((crc >> 16) & 0xFFu);
    img[len - 1] = (uint8_t)((crc >> 24) & 0xFFu);
}

} // namespace

// =============================================================================
// CRC-32
// =============================================================================

TEST_CASE("CRC-32 matches published vectors and streams identically")
{
    // "123456789" -> 0xCBF43926 is THE check value for this variant
    // (also proven at compile time inside Crc32.cpp — belt and braces).
    const uint8_t v[] = { '1','2','3','4','5','6','7','8','9' };
    CHECK(Crc32::compute(v, 9) == 0xCBF43926u);

    // Empty input -> 0 for the zlib variant.
    CHECK(Crc32::compute(v, 0) == 0x00000000u);

    // Chunked streaming must equal one-shot regardless of split points —
    // the property the SysEx bulk path will rely on.
    uint32_t s = Crc32::begin();
    s = Crc32::update(s, v, 3);
    s = Crc32::update(s, v + 3, 1);
    s = Crc32::update(s, v + 4, 5);
    CHECK(Crc32::end(s) == 0xCBF43926u);
}

// =============================================================================
// Save / load round trip
// =============================================================================

TEST_CASE("round trip: every patch-scope value survives, nothing else moves")
{
    ParameterStore src;
    const size_t nPatch = scramblePatchParams(src, 0xC0FFEE);
    CHECK(nPatch == Patch::patchParamCount());

    uint8_t img[Patch::kMaxEncodedSize];
    const size_t len = Patch::save(src, "Init Scrambled", 3, img, sizeof img);
    // Every patch-scope param is always saved, so a full save IS the
    // worst case — the constant and reality must agree exactly.
    CHECK(len == Patch::kMaxEncodedSize);

    ParameterStore dst;                      // boots to defaults
    drain(dst);
    const Patch::LoadResult r = Patch::load(img, len, dst);

    REQUIRE(r.status == Patch::Status::Ok);
    CHECK(r.applied == nPatch);
    CHECK(r.skippedUnknown == 0);
    CHECK(r.migrated == 0);
    CHECK(strcmp(r.info.name, "Init Scrambled") == 0);
    CHECK(r.info.category == 3);

    // Exactly the applied params are dirty — the engine recomputes the
    // patch, not the world.
    CHECK(drainCount(dst) == nPatch);

    for (size_t i = 0; i < kParamCount; ++i) {
        const ParamDesc& d = kParams[i];
        CAPTURE(d.key);
        if (d.scope == Scope::Patch) {
            // Value went norm -> engineering (float32) -> norm; allow only
            // the curve round-trip tolerance established in test_curves.
            CHECK(dst.getByIndex(i) == doctest::Approx(src.getByIndex(i)).epsilon(2e-4));
            CHECK(dst.origin(d.id) == Origin::PatchLoad);
        } else {
            // Performance/global params must be exactly the boot defaults —
            // a patch load may NEVER leak outside its scope.
            CHECK(dst.getByIndex(i) == doctest::Approx(Curves::toNorm(d, d.def)));
        }
    }
}

TEST_CASE("save: undersized buffer is refused outright")
{
    ParameterStore s;
    uint8_t img[Patch::kMaxEncodedSize];
    CHECK(Patch::save(s, "X", 0, img, Patch::kMaxEncodedSize - 1) == 0);
    CHECK(Patch::save(s, "X", 0, nullptr, Patch::kMaxEncodedSize) == 0);
}

TEST_CASE("names: truncated to 16, control bytes sanitised, always terminated")
{
    ParameterStore s;
    uint8_t img[Patch::kMaxEncodedSize];

    // 20 chars with an embedded tab — must come back as 16 printable chars.
    const size_t len = Patch::save(s, "SuperSaw\tLead 12345X", 9, img, sizeof img);
    REQUIRE(len > 0);

    Patch::Info info;
    REQUIRE(Patch::peekInfo(img, len, info));
    CHECK(strlen(info.name) == 16);
    CHECK(strncmp(info.name, "SuperSaw_Lead 12", 16) == 0);
    CHECK(info.category == 9);
    CHECK(info.schemaVersion == kSchemaVersion);
    CHECK(info.entryCount == Patch::patchParamCount());
}

// =============================================================================
// Corruption — every reject path leaves the store untouched
// =============================================================================

TEST_CASE("corruption: magic, truncation, bit rot, forged count, future version")
{
    ParameterStore src;
    scramblePatchParams(src, 0xBEEF);
    uint8_t good[Patch::kMaxEncodedSize];
    const size_t len = Patch::save(src, "Victim", 0, good, sizeof good);
    REQUIRE(len == Patch::kMaxEncodedSize);

    ParameterStore dst;
    drain(dst);

    // Snapshot of dst's state to prove "untouched" after each attack.
    float before[ParameterStore::kCount];
    for (size_t i = 0; i < ParameterStore::kCount; ++i)
        before[i] = dst.getByIndex(i);

    auto checkUntouched = [&]() {
        for (size_t i = 0; i < ParameterStore::kCount; ++i)
            REQUIRE(dst.getByIndex(i) == before[i]);
        REQUIRE(drainCount(dst) == 0);           // and no stray dirty flags
    };

    uint8_t img[Patch::kMaxEncodedSize];

    SUBCASE("bad magic") {
        memcpy(img, good, len);
        img[0] = 'X';
        CHECK(Patch::load(img, len, dst).status == Patch::Status::BadMagic);
        checkUntouched();
    }
    SUBCASE("too short") {
        CHECK(Patch::load(good, Patch::kHeaderSize + 3, dst).status
              == Patch::Status::TooShort);
        checkUntouched();
    }
    SUBCASE("single flipped bit anywhere payload -> BadCrc") {
        // Probe a spread of offsets: name, count, first entry id, a value
        // byte mid-file, and the last payload byte.
        const size_t offsets[] = { 9, 6, Patch::kHeaderSize,
                                   Patch::kHeaderSize + 3, len - 5 };
        for (size_t off : offsets) {
            CAPTURE(off);
            memcpy(img, good, len);
            img[off] ^= 0x40u;
            CHECK(Patch::load(img, len, dst).status == Patch::Status::BadCrc);
            checkUntouched();
        }
    }
    SUBCASE("truncated copy -> BadCrc (CRC no longer at the end)") {
        memcpy(img, good, len);
        CHECK(Patch::load(img, len - Patch::kEntrySize, dst).status
              == Patch::Status::BadCrc);
        checkUntouched();
    }
    SUBCASE("forged entryCount with valid CRC -> SizeMismatch") {
        memcpy(img, good, len);
        img[6] = (uint8_t)(img[6] + 1);          // count lies by one
        reseal(img, len);                        // ...with a VALID checksum
        CHECK(Patch::load(img, len, dst).status == Patch::Status::SizeMismatch);
        checkUntouched();
    }
    SUBCASE("future schemaVersion -> FutureVersion") {
        memcpy(img, good, len);
        img[4] = (uint8_t)((kSchemaVersion + 1) & 0xFFu);
        img[5] = (uint8_t)((kSchemaVersion + 1) >> 8);
        reseal(img, len);
        CHECK(Patch::load(img, len, dst).status == Patch::Status::FutureVersion);
        checkUntouched();
    }
}

// =============================================================================
// Forward compatibility: unknown ids and migrations
// =============================================================================

TEST_CASE("unknown ParamID is skipped and counted; the rest still applies")
{
    ParameterStore src;
    scramblePatchParams(src, 0xFACE);
    uint8_t img[Patch::kMaxEncodedSize];
    const size_t len = Patch::save(src, "Fwd", 0, img, sizeof img);

    // Overwrite the FIRST entry's id with one no firmware will ever have
    // (0x3FFF is inside the 14-bit space but unassigned) and re-seal.
    img[Patch::kHeaderSize + 0] = 0xFF;
    img[Patch::kHeaderSize + 1] = 0x3F;
    reseal(img, len);

    ParameterStore dst;
    drain(dst);
    const Patch::LoadResult r = Patch::load(img, len, dst);
    REQUIRE(r.status == Patch::Status::Ok);
    CHECK(r.skippedUnknown == 1);
    CHECK(r.applied == Patch::patchParamCount() - 1);
    CHECK(drainCount(dst) == r.applied);
}

TEST_CASE("migration: a retired id redirects to its successor with its value")
{
    // Craft: save a patch, then rewrite FILTER_CUTOFF's entry to carry a
    // fictitious RETIRED id, as if written by older firmware.
    ParameterStore src;
    drain(src);
    src.setEngineering(ID::FILTER_CUTOFF, 1234.0f, Origin::Ui);

    uint8_t img[Patch::kMaxEncodedSize];
    const size_t len = Patch::save(src, "Old", 0, img, sizeof img);

    const uint16_t kRetired = 0x3F00;            // never a real id
    uint8_t* p = img + Patch::kHeaderSize;
    bool rewrote = false;
    for (uint16_t n = 0; n < Patch::patchParamCount(); ++n, p += Patch::kEntrySize) {
        if ((uint16_t)(p[0] | (p[1] << 8)) == ID::FILTER_CUTOFF) {
            p[0] = (uint8_t)(kRetired & 0xFFu);
            p[1] = (uint8_t)(kRetired >> 8);
            rewrote = true;
            break;
        }
    }
    REQUIRE(rewrote);
    reseal(img, len);

    const Patch::Migration table[] = { { kRetired, ID::FILTER_CUTOFF } };

    SUBCASE("without the table the value is lost (skipped, counted)") {
        ParameterStore dst;
        const Patch::LoadResult r = Patch::load(img, len, dst);
        REQUIRE(r.status == Patch::Status::Ok);
        CHECK(r.skippedUnknown == 1);
        CHECK(r.migrated == 0);
    }
    SUBCASE("with the table the value lands on the successor") {
        ParameterStore dst;
        const Patch::LoadResult r = Patch::load(img, len, dst,
                                                Origin::PatchLoad, table, 1);
        REQUIRE(r.status == Patch::Status::Ok);
        CHECK(r.skippedUnknown == 0);
        CHECK(r.migrated == 1);
        CHECK(dst.getEngineering(ID::FILTER_CUTOFF)
              == doctest::Approx(1234.0f).epsilon(0.001));
    }
}

TEST_CASE("peekInfo: rejects garbage, never needs a store")
{
    uint8_t junk[64] = { 0 };
    Patch::Info info;
    CHECK_FALSE(Patch::peekInfo(junk, sizeof junk, info));      // bad magic
    CHECK_FALSE(Patch::peekInfo(junk, 8, info));                // too short
    CHECK_FALSE(Patch::peekInfo(nullptr, 1000, info));
}
