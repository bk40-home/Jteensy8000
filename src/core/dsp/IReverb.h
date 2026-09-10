// =============================================================================
// IReverb.h — common interface for switchable reverb algorithms
// =============================================================================
//
// WHY AN INTERFACE AND NOT A SWITCH STATEMENT
//   The dispatch happens ONCE PER BLOCK (128 samples), not per sample, so the
//   indirect call is ~1/128th of one branch — unmeasurable.  In exchange every
//   algorithm is a self-contained file that cannot accidentally reach into
//   SynthCore, and ReverbRack's switching logic is written once instead of
//   once per algorithm.  Signed off as option A1.
//
//   The rule this must NOT break: nothing virtual may appear INSIDE a
//   per-sample loop.  An algorithm's inner DSP structs stay header-inline
//   concrete types exactly as they are today.
//
// WHY THE SETTERS ARE THE PLATE'S SETTERS
//   Option P1: the nine existing `reverb.*` parameters are reused and each
//   algorithm maps them onto its own internals — size becomes decay in one and
//   room dimension in another.  No per-algorithm parameter explosion; the
//   panels hide what does not apply via `visible_when`.  An algorithm that has
//   no meaning for a control implements the setter as an empty body rather
//   than pretending, so "does nothing" is visible in the source.
//
// THE MIX RAMP
//   processBlock takes mixStart AND mixEnd so ReverbRack can crossfade an
//   algorithm out and the next one in without a zipper (option S2).  At steady
//   state the two are equal, the per-sample increment is exactly 0.0f, and the
//   arithmetic is bit-identical to a scalar mix — which is what lets the plate
//   keep its render baseline through this refactor.
//
// POOL OWNERSHIP
//   Unchanged from PlateReverb: the caller owns the memory.  poolFloats() is
//   how ReverbRack knows what to ask the PsramArena for, and it must be
//   answerable BEFORE begin(), so every implementation also exposes a static
//   constant that the virtual simply returns.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace JT {

class IReverb {
public:
    virtual ~IReverb() = default;

    // How many floats this algorithm needs.  Queried by ReverbRack before
    // begin(), so it must not depend on any state begin() sets up.
    virtual uint32_t poolFloats() const = 0;

    // Short name for the boot report and diagnostics.  Must be a string
    // literal or other object outliving the instance.
    virtual const char* name() const = 0;

    // Attach the caller-owned pool (poolFloats() floats).  A null pool leaves
    // the algorithm inert and processBlock must return immediately — the
    // documented no-PSRAM path.
    virtual void begin(float* pool) = 0;

    // ---- the nine shared reverb.* controls, all normalised 0..1 -------------
    // An algorithm without a meaning for one of these implements it empty.
    virtual void setSize(float n)    = 0;   // decay / room dimension
    virtual void setHiDamp(float n)  = 0;   // in-tail HF damping
    virtual void setLoDamp(float n)  = 0;   // in-tail LF damping
    virtual void setLowpass(float n) = 0;   // post master LPF (wet only)
    virtual void setHipass(float n)  = 0;   // post master HPF (wet only)
    virtual void setShimmer(float n) = 0;   // octave-up feed, Shimmer only
    virtual void setFreeze(bool on)  = 0;   // infinite hold

    // Process one stereo block IN PLACE, blending against the dry input with a
    // mix that ramps linearly from mixStart to mixEnd across the block.
    virtual void processBlock(float* left, float* right, size_t n,
                              float mixStart, float mixEnd) = 0;

    // Drop the tail without re-carving the pool.  ReverbRack calls this on the
    // algorithm it has just faded OUT, so a later switch back does not
    // resurrect a stale tail from minutes ago.  Cheaper than begin(): it
    // clears filter and feedback state, not the whole PSRAM region.
    virtual void clearTail() = 0;
};

} // namespace JT
