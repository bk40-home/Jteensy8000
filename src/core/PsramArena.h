// =============================================================================
// PsramArena.h — single-owner bump allocator for the external PSRAM region
// =============================================================================
//
// WHY THIS EXISTS
// ---------------
// Before this class every PSRAM consumer declared its own `EXTMEM static float
// pool[N]` in main.cpp.  That worked for two consumers and stops working at
// four or five: every pool is resident and memset at boot whether the feature
// is enabled or not, a 16 MB board behaves identically to an 8 MB one, and the
// only failure handling available is the all-or-nothing SynthCore::
// disableExtmemPools().  One arena replaces all of that: the owner declares a
// single EXTMEM region, hands it here once, and each subsystem asks for the
// floats it needs.  A subsystem whose request cannot be met gets nullptr and
// goes inert on its own — which is already the documented null-pool mode for
// both PlateReverb::begin and FxChain::begin.
//
// WHY A BUMP ALLOCATOR AND NOT A HEAP
// -----------------------------------
// Every allocation in this system happens once, at boot, and lives for the
// life of the firmware — exactly like the audio graph itself (see the "F32
// cables have no destructor" rule in main.cpp).  A bump pointer is therefore
// not a compromise, it is the correct shape: no free list, no fragmentation,
// no metadata per block, allocation is one add and one compare.  reset() winds
// the pointer back to zero and exists ONLY for boot-time re-carving and host
// tests — calling it while the audio ISR is running would hand the same
// addresses to a second consumer while the first is still reading them.
//
// ALIGNMENT — WHY 32 BYTES
// ------------------------
// The M7's D-cache line is 32 bytes.  Two pools sharing a cache line means a
// write to one can evict the other's line, and it makes any future per-pool
// arm_dcache_flush_delete() maintenance unsafe (the maintenance range would
// straddle a neighbour's data).  Aligning every allocation up to a 32-byte
// boundary costs at most 7 wasted floats per pool and removes both problems.
//
// ZEROING IS THE CONSUMER'S JOB
// -----------------------------
// This class deliberately does NOT zero what it hands out.  PlateReverb::begin
// and FxChain::begin already memset their pools as part of carving them, and
// zeroing 3.4 MB of PSRAM twice at boot costs roughly 85 ms at the ~40 MB/s the
// FlexSPI2 bus delivers.  A future consumer that needs zeroed memory must
// memset what it received, exactly as the existing two do.
//
// NO ARDUINO DEPENDENCY
// ---------------------
// This file is pure C++ and lives under core/ so the host test harness builds
// it unchanged (see the Makefile CORE list).  The EXTMEM placement itself is
// the owner's concern: on Teensy main.cpp declares an EXTMEM array, on the
// host a test allocates a std::vector and passes .data().  Same carve-up, same
// code path — the PlateReverb pattern.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace JT {

class PsramArena {
public:
    // Cache-line alignment applied to every allocation, expressed in floats.
    // 32 bytes / sizeof(float) == 8.
    static constexpr size_t kAlignFloats = 8u;

    // Most allocations we will ever record for diagnostics.  Exceeding it does
    // not fail the allocation — the arena simply stops recording, so the status
    // line under-reports while the audio path stays correct.  Raise it if the
    // consumer count ever passes this.
    static constexpr uint8_t kMaxRecords = 12u;

    PsramArena() = default;

    // Attach the owner's region.  `base` may be null and `capacityFloats` may
    // be zero — that is the documented "no PSRAM fitted" state, in which every
    // allocate() returns nullptr and every consumer goes inert.  Safe to call
    // more than once; each call re-carves from scratch.
    void begin(float* base, size_t capacityFloats);

    // Carve `nFloats` from the region, 32-byte aligned.  Returns nullptr when
    // the request does not fit (or when the arena has no region), and counts
    // the miss so the boot status line can report it.  `tag` is stored by
    // POINTER for diagnostics and must therefore be a string literal or other
    // object that outlives the arena — never a stack buffer.
    float* allocate(size_t nFloats, const char* tag);

    // --- state ---------------------------------------------------------------

    // True when a region is attached and has room; false means every consumer
    // will be inert.  Cheaper and clearer at call sites than comparing pointers.
    bool   ok()        const { return _base != nullptr && _capacity > 0u; }
    size_t capacity()  const { return _capacity; }
    size_t used()      const { return _used; }
    size_t remaining() const { return _capacity - _used; }

    // Wind the bump pointer back to zero.  BOOT-TIME AND TESTS ONLY — see the
    // header comment.  Does not touch the memory itself.
    void reset();

    // --- diagnostics ---------------------------------------------------------
    // Enough to print a per-pool boot report and to make a failed allocation
    // visible rather than silent.  All const, all cold-path.

    uint8_t     recordCount()          const { return _recordCount; }
    const char* recordTag (uint8_t i)  const;
    size_t      recordSize(uint8_t i)  const;   // floats actually carved
    uint8_t     failedCount()          const { return _failed; }

private:
    // Round `n` up to the next kAlignFloats boundary.  Kept private and static
    // because it is an implementation detail of the bump step, not a service.
    static size_t alignUp(size_t n);

    float*  _base        = nullptr;
    size_t  _capacity    = 0u;   // floats
    size_t  _used        = 0u;   // floats, always a multiple of kAlignFloats
    uint8_t _recordCount = 0u;
    uint8_t _failed      = 0u;

    const char* _tags [kMaxRecords] = {};
    size_t      _sizes[kMaxRecords] = {};
};

}  // namespace JT
