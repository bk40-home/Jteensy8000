// =============================================================================
// PsramArena.cpp — see PsramArena.h for the design rationale.
// =============================================================================

#include "core/PsramArena.h"

#include "core/AudioConfig.h"   // JT_COLD

namespace JT {

// Every function here runs once at boot.  JT_COLD keeps them out of the hot
// instruction path so they do not compete for ITCM with the audio code.

size_t PsramArena::alignUp(size_t n)
{
    // kAlignFloats is a power of two, so the mask form is exact and branchless.
    // Guarding the assumption here rather than trusting a future edit of the
    // constant: a non-power-of-two would silently corrupt every offset.
    static_assert((kAlignFloats & (kAlignFloats - 1u)) == 0u,
                  "kAlignFloats must be a power of two for the mask below");
    return (n + (kAlignFloats - 1u)) & ~(kAlignFloats - 1u);
}

JT_COLD void PsramArena::begin(float* base, size_t capacityFloats)
{
    // A null base with a non-zero capacity would let allocate() hand out
    // offsets from address zero.  Normalise the pair to the inert state
    // instead of trusting every future caller to keep them consistent.
    if (base == nullptr) capacityFloats = 0u;

    _base     = base;
    _capacity = capacityFloats;
    reset();
}

JT_COLD void PsramArena::reset()
{
    _used        = 0u;
    _recordCount = 0u;
    _failed      = 0u;
    for (uint8_t i = 0; i < kMaxRecords; ++i) {
        _tags [i] = nullptr;
        _sizes[i] = 0u;
    }
}

JT_COLD float* PsramArena::allocate(size_t nFloats, const char* tag)
{
    // A zero-length request is a caller bug rather than a memory shortage, but
    // returning a valid pointer to zero floats invites an out-of-bounds write.
    // Treat it as a miss so it shows up in failedCount() at boot.
    if (!ok() || nFloats == 0u) { ++_failed; return nullptr; }

    const size_t start = _used;                 // already aligned by invariant
    const size_t step  = alignUp(nFloats);

    // Overflow-safe capacity test.  Writing it as `start + step > _capacity`
    // could wrap on an absurd nFloats and pass a check it should fail; the
    // subtraction cannot wrap because start <= _capacity always holds.
    if (step > _capacity - start) { ++_failed; return nullptr; }

    _used = start + step;

    if (_recordCount < kMaxRecords) {
        _tags [_recordCount] = tag;
        _sizes[_recordCount] = nFloats;         // request, not the padded step
        ++_recordCount;
    }

    return _base + start;
}

JT_COLD const char* PsramArena::recordTag(uint8_t i) const
{
    return (i < _recordCount) ? _tags[i] : nullptr;
}

JT_COLD size_t PsramArena::recordSize(uint8_t i) const
{
    return (i < _recordCount) ? _sizes[i] : 0u;
}

}  // namespace JT
