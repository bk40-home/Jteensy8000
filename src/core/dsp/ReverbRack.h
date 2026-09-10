// =============================================================================
// ReverbRack.h — holds every reverb algorithm, runs exactly one
// =============================================================================
//
// WHAT THIS SOLVES
//   Only one algorithm may run at a time (CPU), but switching must not click
//   and must not stall the audio ISR.  Three facts shape the design:
//
//   1. PSRAM IS NO LONGER SCARCE.  Since PsramArena landed there is ~2.5 MB
//      spare and each algorithm is 90-125 KB, so every algorithm gets its OWN
//      permanent pool.  Switching is then a pointer change: no memset, no
//      re-carve, no re-init.  The alternative (one shared pool re-carved on
//      switch) needed a ~90 KB PSRAM memset — about 2 ms against a 2.9 ms
//      block budget.  Signed off as option R2.
//
//   2. INACTIVE ALGORITHMS COST NOTHING.  processBlock is only called on the
//      active one.  An idle tank is memory, not cycles.
//
//   3. PARAMETERS GO TO ALL OF THEM.  Every setter fans out to every instance,
//      not just the active one, so a switch lands on an algorithm that already
//      has the patch's settings rather than boot defaults.  This is control
//      plane only — a handful of float stores per parameter change.
//
// THE CROSSFADE (option S2)
//   A hard switch cuts a ringing tail dead.  Instead the rack ramps the active
//   algorithm's mix down to zero over kFadeBlocks, swaps, then ramps the new
//   one up.  Only ONE algorithm is ever processed, so the fade costs nothing
//   extra — this is why option S3 (run both, crossfade) was rejected: it would
//   have doubled CPU at exactly the moment the point is to save it.
//
//   The old algorithm gets clearTail() as it goes silent, so switching back
//   later does not resurrect a tail from minutes ago.
//
// WHY NOT std::variant / placement new
//   The instances are plain members.  They are cheap (a few hundred bytes of
//   control state each; the buffers are all in the arena), constructed once,
//   and never destroyed — the same static-lifetime discipline the audio graph
//   follows.  Nothing here allocates at runtime.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/dsp/IReverb.h"
#include "core/dsp/PlateReverb.h"
#include "core/dsp/RoomReverb.h"
#include "core/dsp/HallReverb.h"

namespace JT {

class PsramArena;

class ReverbRack {
public:
    // ORDER IS LOAD-BEARING: these are the option indices of the `reverb_algo`
    // set in params.yaml.  Append new algorithms, never insert — a select
    // stores its option index and inserting would re-point saved patches.
    // (Same rule as timing_mode; see ledger D-13.)
    enum Algo : int {
        kPlate = 0,
        kShimmer,
        kRoom,          // appended, never inserted — see the note above
        kHall,
        kNumAlgos
    };

    // Crossfade length in BLOCKS.  8 blocks at 128 samples is ~23 ms — long
    // enough that a decaying tail is not chopped, short enough that turning a
    // panel selector still feels immediate.
    static constexpr uint8_t kFadeBlocks = 8;

    // Total memory for ALL algorithms.  Every algorithm keeps its own pool
    // permanently, which is what makes a switch a pointer change rather than a
    // ~90 KB PSRAM memset inside the audio path (option R2).
    //   plate    23418 floats =  91.5 KB
    //   shimmer  31610 floats = 123.5 KB
    //   room      4558 floats =  17.8 KB
    //   hall     14542 floats =  56.8 KB
    //   total    74128 floats = 289.6 KB  (one bare plate was 39707 / 155.1 KB)
    static constexpr uint32_t kTotalPoolFloats =
        PlateReverb::kPoolFloats + ShimmerReverb::kPoolFloats
        + RoomReverb::kPoolFloats + HallReverb::kPoolFloats;

    ReverbRack() = default;

    // Attach ONE contiguous region of kTotalPoolFloats and slice it in Algo
    // order.  Taking a single base keeps SynthCore's constructor contract
    // unchanged — it still receives one reverb pool and knows nothing about
    // how many algorithms exist or how they are laid out.
    // A null base leaves every algorithm inert, the documented no-PSRAM path.
    void begin(float* base);

    // Select an algorithm.  Out-of-range values are ignored rather than
    // clamped: a bad index almost always means a stale editor, and silently
    // landing on a neighbouring algorithm hides that.
    void setAlgorithm(int algo);
    int  algorithm() const { return _active; }

    // ---- the nine shared reverb.* controls, fanned out to every instance ----
    void setSize(float n);
    void setHiDamp(float n);
    void setLoDamp(float n);
    void setLowpass(float n);
    void setHipass(float n);
    void setShimmer(float n);
    void setFreeze(bool on);

    // Process one stereo block IN PLACE.  Caller guarantees this is only
    // invoked when NOT bypassed (SynthCore holds that decision), exactly as it
    // did when it called PlateReverb directly.
    void processBlock(float* left, float* right, size_t n, float mix);

    // ---- diagnostics --------------------------------------------------------
    bool        ready()      const { return _ready; }
    bool        fading()     const { return _fade > 0; }
    const char* activeName() const;
    uint8_t     failedCount() const { return _failed; }

private:
    IReverb* instance(int algo);

    PlateReverb   _plate;
    ShimmerReverb _shimmer;
    RoomReverb    _room;
    HallReverb    _hall;

    int     _active  = kPlate;
    int     _pending = kPlate;

    // Fade counter, in blocks.  0 == steady state.  Counts DOWN through the
    // fade-out, hits the swap at the midpoint, then counts down the fade-in.
    uint8_t _fade    = 0;
    bool    _fadeOut = false;

    bool    _ready   = false;
    uint8_t _failed  = 0;

    // Last applied mix, needed so the ramp starts where the previous block
    // ended rather than jumping.
    float   _lastMix = 0.0f;
};

} // namespace JT
