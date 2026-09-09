// =============================================================================
// test_psram_arena.cpp — PsramArena carve-up, alignment and exhaustion
// =============================================================================
// The arena hands out the memory every PSRAM-backed engine runs on, and its
// failure mode is silent by design (nullptr -> engine goes inert).  These tests
// exist so that "silent" never becomes "unnoticed": the alignment invariant,
// the non-overlap guarantee and the exhaustion path are all checked explicitly.
// =============================================================================

#include "doctest.h"
#include "core/PsramArena.h"

#include <string>
#include <vector>

using namespace JT;

TEST_CASE("psram arena: no region -> inert, every allocation fails")
{
    PsramArena a;
    CHECK_FALSE(a.ok());
    CHECK(a.allocate(16, "x") == nullptr);

    // Explicitly attaching nothing must behave the same as never attaching —
    // this is the no-PSRAM-chip boot path.
    a.begin(nullptr, 1024);
    CHECK_FALSE(a.ok());
    CHECK(a.capacity() == 0u);
    CHECK(a.allocate(16, "x") == nullptr);
    // begin() re-carves from scratch, so it also clears the diagnostics — the
    // miss counted before the attach is deliberately not carried across.
    CHECK(a.failedCount() == 1u);
}

TEST_CASE("psram arena: allocations are 32-byte aligned and never overlap")
{
    std::vector<float> region(4096, 0.0f);
    PsramArena a;
    a.begin(region.data(), region.size());
    REQUIRE(a.ok());

    // Deliberately awkward sizes: none is a multiple of kAlignFloats, so every
    // one exercises the round-up.
    float* p0 = a.allocate(1,   "one");
    float* p1 = a.allocate(9,   "nine");
    float* p2 = a.allocate(100, "hundred");
    REQUIRE(p0 != nullptr);
    REQUIRE(p1 != nullptr);
    REQUIRE(p2 != nullptr);

    const size_t align = PsramArena::kAlignFloats;
    CHECK((size_t)(p0 - region.data()) % align == 0u);
    CHECK((size_t)(p1 - region.data()) % align == 0u);
    CHECK((size_t)(p2 - region.data()) % align == 0u);

    // Non-overlap: each block starts at or after the end of the previous one.
    CHECK(p1 >= p0 + 1);
    CHECK(p2 >= p1 + 9);

    // used() counts the PADDED footprint, recordSize() the raw request — the
    // status line reports what a pool asked for, not what alignment cost.
    // 1 -> 8, 9 -> 16 (a 9-float request crosses into a second cache line),
    // 100 -> 104.  The 9 case is the one worth spelling out: alignment rounds
    // the FOOTPRINT up, it does not pack two requests into one line.
    CHECK(a.used() == 8u + 16u + 104u);
    CHECK(a.recordCount() == 3u);
    CHECK(a.recordSize(1) == 9u);
    CHECK(std::string(a.recordTag(2)) == "hundred");
    CHECK(a.failedCount() == 0u);
}

TEST_CASE("psram arena: writes to one block do not disturb another")
{
    std::vector<float> region(512, 0.0f);
    PsramArena a;
    a.begin(region.data(), region.size());

    float* p0 = a.allocate(11, "a");
    float* p1 = a.allocate(11, "b");
    REQUIRE(p0 != nullptr);
    REQUIRE(p1 != nullptr);

    for (int i = 0; i < 11; ++i) p0[i] = 1.0f;
    for (int i = 0; i < 11; ++i) p1[i] = 2.0f;
    for (int i = 0; i < 11; ++i) CHECK(p0[i] == 1.0f);
    for (int i = 0; i < 11; ++i) CHECK(p1[i] == 2.0f);
}

TEST_CASE("psram arena: exhaustion returns nullptr and is counted, not fatal")
{
    std::vector<float> region(64, 0.0f);
    PsramArena a;
    a.begin(region.data(), region.size());

    CHECK(a.allocate(60, "big") != nullptr);   // padded to 64 -> exactly fits
    CHECK(a.remaining() == 0u);

    // The over-large request must fail WITHOUT disturbing what is already
    // carved — a later tenant failing cannot be allowed to move an earlier one.
    const size_t usedBefore = a.used();
    CHECK(a.allocate(8, "toolate") == nullptr);
    CHECK(a.used() == usedBefore);
    CHECK(a.failedCount() == 1u);

    // A zero-length request is a caller bug, not a shortage; it must be
    // visible rather than returning a pointer to nothing.
    CHECK(a.allocate(0, "zero") == nullptr);
    CHECK(a.failedCount() == 2u);
}

TEST_CASE("psram arena: reset re-carves from the top")
{
    std::vector<float> region(256, 0.0f);
    PsramArena a;
    a.begin(region.data(), region.size());

    float* first = a.allocate(32, "first");
    a.reset();
    CHECK(a.used() == 0u);
    CHECK(a.recordCount() == 0u);
    CHECK(a.allocate(32, "again") == first);
}

TEST_CASE("psram arena: diagnostics saturate instead of overflowing")
{
    std::vector<float> region(4096, 0.0f);
    PsramArena a;
    a.begin(region.data(), region.size());

    // More tenants than kMaxRecords: allocation must keep working, only the
    // bookkeeping stops.  A dropped status line is acceptable; a corrupted
    // audio pool is not.
    for (int i = 0; i < PsramArena::kMaxRecords + 5; ++i)
        CHECK(a.allocate(8, "t") != nullptr);

    CHECK(a.recordCount() == PsramArena::kMaxRecords);
    CHECK(a.recordTag(PsramArena::kMaxRecords) == nullptr);
    CHECK(a.recordSize(PsramArena::kMaxRecords) == 0u);
}
