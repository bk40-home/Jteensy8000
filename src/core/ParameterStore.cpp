// =============================================================================
// ParameterStore.cpp — implementation
// =============================================================================
// The concurrency model and correctness argument live in ParameterStore.h.
// This file's job is to keep the publish() ordering exactly as documented.
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "core/ParameterStore.h"

#include "core/AudioConfig.h"   // JT_COLD
#include "core/dsp/Curves.h"

namespace JT {

// -----------------------------------------------------------------------------
// Compile-time guarantees.
//
// indexOf() binary-searches the generated table, which is only valid if the
// generator emitted it sorted by id.  It does (sections ascending, indices
// ascending), but that is a property of a *different* file — so prove it
// here, at compile time, instead of trusting it.  A future generator bug
// becomes a build error, not a corrupted-lookup mystery.
// -----------------------------------------------------------------------------
static constexpr bool tableSortedById()
{
    for (size_t i = 1; i < Params::kParamCount; ++i)
        if (Params::kParams[i - 1].id >= Params::kParams[i].id) return false;
    return true;
}
static_assert(tableSortedById(),
              "gen/ParamTable.h must be sorted by id — regenerate it");

// The audio-plane wait-free claim requires genuinely lock-free atomics.
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "uint32 atomics must be lock-free on this target");

// -----------------------------------------------------------------------------
// Construction: load defaults, publish once, leave everything dirty so the
// engine's first block applies the full state (see header).
// -----------------------------------------------------------------------------
ParameterStore::ParameterStore()
    : _front(0), _bulkDepth(0)
{
    for (size_t w = 0; w < kDirtyWords; ++w) {
        _pending[w] = 0;
        _dirty[w].store(0, std::memory_order_relaxed);
        _txDirty[w] = 0;
    }

    // Walk SLOTS, not indices: layer B's banked copies need the same default
    // as layer A, or a Single->Layer switch would drop B in at zero.  Shared
    // parameters have one slot and are therefore written exactly once — which
    // is why this loops over slots rather than (layer, index) pairs.
    beginBulk();
    for (size_t slot = 0; slot < kSlots; ++slot) {
        const Params::ParamDesc& d = Params::kParams[Params::paramOfSlot(slot)];
        setBySlot(slot, Curves::toNorm(d, d.def), Origin::Init);
    }
    endBulk();   // one flip; every slot now published AND flagged dirty

    // The initial publish also marked every param broadcast-dirty.  Drop
    // those bits: boot must not transmit the whole table to whatever DAW
    // happens to be listening.  Editors that want the full state ask for
    // it (NRPN resync request -> ParamBroadcast::requestFullResync).
    clearTxDirty();
}

// -----------------------------------------------------------------------------
// Lookup: binary search over the id-sorted table.  O(log n): ~8 compares for
// 140 params — cheap at control rate, and callers with a hot parameter cache
// the returned index (see header).
// -----------------------------------------------------------------------------
JT_COLD size_t ParameterStore::indexOf(uint16_t id)
{
    size_t lo = 0, hi = kCount;              // half-open [lo, hi)
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const uint16_t v = Params::kParams[mid].id;
        if (v == id) return mid;
        if (v < id)  lo = mid + 1;
        else         hi = mid;
    }
    return kInvalidIndex;
}

// -----------------------------------------------------------------------------
// Control-plane writes
// -----------------------------------------------------------------------------

// The single write primitive.  Everything else resolves to a slot and calls
// this, so the publish rule and the dirty-bit rule exist in exactly one place.
JT_COLD void ParameterStore::setBySlot(size_t slot, float normalized, Origin origin)
{
    if (slot >= kSlots) return;              // defensive: never trust callers

    const float v = Curves::clamp01(normalized);

    // Write goes into the BACK buffer only (1 - front).  The audio plane
    // never reads that buffer, so a plain store is race-free by construction.
    const uint32_t back = 1u - _front.load(std::memory_order_relaxed);
    _values[back][slot] = v;

    _origin[slot] = static_cast<uint8_t>(origin);
    _pending[slot / 32u] |= (1u << (slot % 32u));

    // Outside a bulk load every set publishes immediately — a knob turn
    // reaches the engine on the next block.  Inside a bulk load publication
    // waits for the outermost endBulk() so the patch appears atomically.
    if (_bulkDepth == 0) publish();
}

JT_COLD void ParameterStore::setByIndex(size_t index, float normalized,
                                        Origin origin, uint8_t layer)
{
    if (index >= kCount) return;
    setBySlot(Params::slotFor(index, layerMask(layer)), normalized, origin);
}

JT_COLD bool ParameterStore::set(uint16_t id, float normalized, Origin origin,
                                 uint8_t layer)
{
    const size_t i = indexOf(id);
    if (i == kInvalidIndex) return false;
    setByIndex(i, normalized, origin, layer);
    return true;
}

JT_COLD bool ParameterStore::setEngineering(uint16_t id, float engineering,
                                            Origin origin, uint8_t layer)
{
    const size_t i = indexOf(id);
    if (i == kInvalidIndex) return false;
    setByIndex(i, Curves::toNorm(Params::kParams[i], engineering), origin, layer);
    return true;
}

JT_COLD void ParameterStore::beginBulk() { ++_bulkDepth; }

JT_COLD void ParameterStore::endBulk()
{
    if (_bulkDepth == 0) return;             // unmatched endBulk: ignore
    if (--_bulkDepth == 0) publish();
}

JT_COLD void ParameterStore::resetToDefaults(Origin origin)
{
    // Slots, for the same reason as the constructor: BOTH layers reset, and
    // shared parameters are written once rather than twice.
    beginBulk();
    for (size_t slot = 0; slot < kSlots; ++slot) {
        const Params::ParamDesc& d = Params::kParams[Params::paramOfSlot(slot)];
        setBySlot(slot, Curves::toNorm(d, d.def), origin);
    }
    endBulk();
}

// -----------------------------------------------------------------------------
// publish() — the only function where the two planes meet.  ORDER MATTERS:
//
//   1. FLIP _front (release store).  From this instant any ISR sees the new,
//      fully written values.  Release ordering guarantees every back-buffer
//      store above is visible before the flip is.
//   2. SYNC the new back buffer: copy each changed value across so future
//      writes start from current state.  Only dirty indices are copied —
//      one knob turn costs one float copy, not a 560-byte memcpy.
//   3. RAISE dirty bits (fetch_or) — strictly AFTER the flip, so a raised
//      bit always refers to an already-visible value.  An ISR landing
//      between 1 and 3 simply picks the change up next block (≤ 2.9 ms).
// -----------------------------------------------------------------------------
JT_COLD void ParameterStore::publish()
{
    // Cheap early-out: nothing pending (e.g. endBulk after a read-only bulk).
    uint32_t any = 0;
    for (size_t w = 0; w < kDirtyWords; ++w) any |= _pending[w];
    if (any == 0) return;

    // (1) flip — new values become the audio plane's world.
    const uint32_t newFront = 1u - _front.load(std::memory_order_relaxed);
    _front.store(newFront, std::memory_order_release);

    testPreempt();   // host tests fake an ISR here (compiled out on target)

    // (2) sync changed values into the new back buffer.
    const uint32_t newBack = 1u - newFront;
    for (size_t w = 0; w < kDirtyWords; ++w) {
        uint32_t bits = _pending[w];
        while (bits) {
            // Count-trailing-zeros walks set bits directly — cost is
            // proportional to the number of CHANGES, never to kCount.
            const unsigned b = (unsigned)__builtin_ctz(bits);
            bits &= bits - 1u;               // clear lowest set bit
            const size_t i = w * 32u + b;
            _values[newBack][i] = _values[newFront][i];
        }
    }

    testPreempt();   // second modelled preemption point

    // (3) hand the dirty bits to the audio plane and clear pending.
    for (size_t w = 0; w < kDirtyWords; ++w) {
        if (_pending[w]) {
            _dirty[w].fetch_or(_pending[w], std::memory_order_release);
            _txDirty[w] |= _pending[w];   // broadcast mirror — same publish
                                          // point, so a tx bit always refers
                                          // to an already-visible value
            _pending[w] = 0;
        }
    }
}

// -----------------------------------------------------------------------------
// Control-plane reads — always the back buffer: the freshest state,
// including writes not yet published (mid-bulk).
// -----------------------------------------------------------------------------

float ParameterStore::getByIndex(size_t index, uint8_t layer) const
{
    if (index >= kCount) return 0.0f;
    const uint32_t back = 1u - _front.load(std::memory_order_relaxed);
    return _values[back][Params::slotFor(index, layerMask(layer))];
}

JT_COLD float ParameterStore::get(uint16_t id, uint8_t layer) const
{
    const size_t i = indexOf(id);
    return (i == kInvalidIndex) ? 0.0f : getByIndex(i, layer);
}

JT_COLD float ParameterStore::getEngineering(uint16_t id, uint8_t layer) const
{
    const size_t i = indexOf(id);
    if (i == kInvalidIndex) return 0.0f;
    return Curves::toEngineering(Params::kParams[i], getByIndex(i, layer));
}

JT_COLD Origin ParameterStore::origin(uint16_t id, uint8_t layer) const
{
    const size_t i = indexOf(id);
    return (i == kInvalidIndex)
             ? Origin::Init
             : static_cast<Origin>(_origin[Params::slotFor(i, layerMask(layer))]);
}

// -----------------------------------------------------------------------------
// Audio-plane reads
// -----------------------------------------------------------------------------

const float* ParameterStore::acquireSnapshot() const
{
    // Acquire pairs with the release flip in publish(): once the ISR sees
    // the new front index, it is guaranteed to see the values written
    // before the flip.
    return _values[_front.load(std::memory_order_acquire)];
}

size_t ParameterStore::takeNextDirty()
{
    for (size_t w = 0; w < kDirtyWords; ++w) {
        const uint32_t bits = _dirty[w].load(std::memory_order_relaxed);
        if (bits == 0) continue;             // the common, near-free case

        const unsigned b = (unsigned)__builtin_ctz(bits);
        // Atomic clear of exactly this bit.  fetch_and (LDREX/STREX on the
        // M7) means a concurrent control-plane fetch_or on the same word
        // cannot lose either side's bits.
        _dirty[w].fetch_and(~(1u << b), std::memory_order_acq_rel);

        const size_t i = w * 32u + b;
        // Bound is kSlots now, not kCount: the top dirty word has padding
        // bits above the last SLOT, and layer-B slots live above kCount.
        return (i < kSlots) ? i : kInvalidIndex;
    }
    return kInvalidIndex;
}


// -----------------------------------------------------------------------------
// Broadcast (tx) drain — Phase B'.  Control-plane consumer (ParamBroadcast),
// control-plane producer (publish); one execution context, so plain word
// operations are race-free (full argument at the header declaration).
// Same ctz walk as takeNextDirty: cost proportional to CHANGES, near-zero idle.
// -----------------------------------------------------------------------------
JT_COLD size_t ParameterStore::takeNextTxDirty()
{
    for (size_t w = 0; w < kDirtyWords; ++w) {
        const uint32_t bits = _txDirty[w];
        if (bits == 0) continue;             // the common, near-free case

        const unsigned b = (unsigned)__builtin_ctz(bits);
        _txDirty[w] &= ~(1u << b);

        const size_t i = w * 32u + b;
        // Bound is kSlots now, not kCount: the top dirty word has padding
        // bits above the last SLOT, and layer-B slots live above kCount.
        return (i < kSlots) ? i : kInvalidIndex;
    }
    return kInvalidIndex;
}

JT_COLD void ParameterStore::clearTxDirty()
{
    for (size_t w = 0; w < kDirtyWords; ++w) _txDirty[w] = 0;
}

JT_COLD Origin ParameterStore::originByIndex(size_t index, uint8_t layer) const
{
    return (index < kCount)
             ? static_cast<Origin>(_origin[Params::slotFor(index, layerMask(layer))])
             : Origin::Init;
}

} // namespace JT
