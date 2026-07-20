// =============================================================================
// Crc32.cpp — implementation
// =============================================================================
// See Crc32.h for the role, algorithm choice and cost analysis.
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "core/dsp/Crc32.h"

#include "core/AudioConfig.h"   // JT_COLD

namespace JT {
namespace Crc32 {

// -----------------------------------------------------------------------------
// The 256-entry lookup table, generated AT COMPILE TIME.
//
// C++17 pattern worth knowing: a constexpr function cannot return a raw
// array, but it can return a struct that wraps one — so the classic
// runtime table-init loop becomes a compile-time computation and the whole
// table is baked into flash as constant data.  Zero boot cost, zero RAM,
// and the compiler proves the table correct on every build.
// -----------------------------------------------------------------------------
namespace {

struct Table { uint32_t entry[256]; };

constexpr Table makeTable()
{
    Table t{};
    for (uint32_t i = 0; i < 256u; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        t.entry[i] = c;
    }
    return t;
}

constexpr Table kTable JT_FLASH_DATA = makeTable();

// Compile-time self-check against the published test vector: the CRC of
// the ASCII string "123456789" is 0xCBF43926 in this variant.  A wrong
// polynomial or reflection bug becomes a build error, not a corrupt bank.
constexpr uint32_t selfTest()
{
    const uint8_t v[9] = { '1','2','3','4','5','6','7','8','9' };
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < 9; ++i)
        c = kTable.entry[(c ^ v[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static_assert(selfTest() == 0xCBF43926u, "CRC-32 table generation is wrong");

} // namespace

JT_COLD uint32_t begin()
{
    return 0xFFFFFFFFu;
}

JT_COLD uint32_t update(uint32_t state, const uint8_t* data, size_t len)
{
    // One table lookup + shift + XOR per byte.  ~4 cycles/byte on the M7 —
    // no point in the sliced 4/8-byte variants at patch-file sizes.
    for (size_t i = 0; i < len; ++i)
        state = kTable.entry[(state ^ data[i]) & 0xFFu] ^ (state >> 8);
    return state;
}

JT_COLD uint32_t end(uint32_t state)
{
    return state ^ 0xFFFFFFFFu;
}

JT_COLD uint32_t compute(const uint8_t* data, size_t len)
{
    return end(update(begin(), data, len));
}

} // namespace Crc32
} // namespace JT
