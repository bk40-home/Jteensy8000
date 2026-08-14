// =============================================================================
// ParameterStore.h — canonical parameter state for JT-8000 v2
// =============================================================================
//
// ROLE IN THE ARCHITECTURE (design brief §3, §4.3)
//   This class IS the synth's state.  Patches, MIDI, SysEx, the TFT UI and
//   the step sequencer all read and write here; the audio engine consumes
//   changes from here once per block.  It replaces v1's four overlapping
//   stores (Patch / PatchState / CCCache / SynthEngine members).
//
// THE LAYER DIMENSION (Performance mode)
//   v1 layered by owning TWO SynthEngine objects, because in v1 an engine WAS
//   a patch.  v2 inverted that — this store is the state — so a second engine
//   would give two of everything and one state.  Layering therefore lives
//   HERE, as a second set of storage slots, not as a second object.
//
//   Two vocabulary words, kept strictly apart everywhere below:
//
//     INDEX  0..kCount-1   WHICH PARAMETER.  The dense table index.  This is
//                          what the engine, the patch codec and every
//                          transport speak; it never mentions a layer.
//     SLOT   0..kSlots-1   WHICH PARAMETER, IN WHICH LAYER.  A storage
//                          address.  Only this class and the dirty bitsets
//                          use it.
//
//   The mapping is generated (Params::slotFor / paramOfSlot / layerOfSlot):
//
//     slot(i, A) = i                                 <- IDENTITY, load-bearing
//     slot(i, B) = kCount + kBankIndex[i]            <- banked params
//     slot(i, B) = i                                 <- shared params
//
//   Layer A's slot being identical to the table index is the whole reason
//   this is cheap: every cached index, every dirty-word position and the
//   publish() flip/ctz/fetch_or ordering keep their exact single-layer
//   meaning.  A build with Performance switched off does the same work,
//   in the same order, as before the layer dimension existed.
//
//   WHAT BANKS is derived from Scope in params.yaml, never listed here:
//   Scope::Patch banks (per-voice sound), everything else is shared — the
//   FX chain and step sequencer are singletons both layers feed.
//
// CONCURRENCY MODEL — READ THIS BEFORE TOUCHING ANYTHING
//   Two execution contexts, one CPU core (Teensy 4.1, Cortex-M7):
//
//     CONTROL PLANE  loop() context.  May be PREEMPTED at any instruction
//                    by the audio ISR.  Calls set/get/beginBulk/endBulk.
//     AUDIO PLANE    the audio ISR.  Runs to completion — it is NEVER
//                    preempted by the control plane.  Calls
//                    acquireSnapshot()/takeNextDirty() only.
//
//   The design is wait-free for the audio plane: no locks, no retries, no
//   AudioNoInterrupts().  Correctness argument, step by step:
//
//   * Values live in TWO buffers.  _front (atomic) names the one the audio
//     plane reads; the control plane only ever writes the other.  publish()
//     flips _front with release ordering, then re-syncs the new back buffer.
//     Because the ISR runs to completion, it can never be mid-read while the
//     control plane syncs — it observes either the complete old state or the
//     complete new state.  This is what makes a patch load appear ATOMIC to
//     the engine (v1 could expose half-loaded patches).
//
//   * Dirty flags are a bitset of atomic words.  The control plane ORs bits
//     in (in publish(), strictly AFTER the flip, so a set bit always refers
//     to an already-visible value); the audio plane clears bits with an
//     atomic AND as it consumes them.  Because control's read-modify-write
//     uses fetch_or, an ISR clearing a different bit mid-operation cannot be
//     lost.  Worst case under contention is one SPURIOUS recompute of a
//     parameter (never a missed one) and worst-case latency from set() to
//     engine application is one audio block (2.9 ms) — both harmless.
//
//   std::atomic is used rather than bare volatile: it is correct on the M7
//   (LDREX/STREX for the RMWs, plain loads/stores otherwise), free of cost
//   at -O2, and makes the host-side unit tests honest.
//
// CPU CONTRACT ("do not calculate if not required")
//   The audio plane's per-block cost when nothing changed is ONE atomic load
//   (the snapshot pointer) plus kDirtyWords (= 9 today) word loads that all
//   read zero — a handful of nanoseconds.  Work is only ever proportional to
//   the number of parameters that actually changed.
//
//   The layer dimension costs 4 extra zero-word loads per idle block and
//   about 1.0 KB of DTCM.  It does NOT cost per-parameter work: a layer-A
//   edit dirties exactly one bit, exactly as before.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <atomic>

#include "gen/ParamTable.h"

namespace JT {

// Where a change came from.  Transports use this for echo suppression
// (e.g. the MIDI transport must not echo a change back to the port that
// produced it) — replacing v1's ad-hoc _suppressEcho atomics and rate
// limiters with one queryable fact.
//
// Phase B' (D1): the single 'Midi' bucket became one value PER PORT.  The
// broadcaster needs to know WHICH port last wrote a parameter, or it cannot
// skip that port — suppression with a shared bucket would silence all three.
// Runtime-only: Origin is never serialized (the patch codec stores values,
// not provenance), so renumbering here cannot corrupt saved patches.
enum class Origin : uint8_t {
    Init,          // power-on defaults
    MidiUsbDev,    // NRPN / curated CC — USB device port (DAW / JUCE editor)
    MidiUsbHost,   // NRPN / curated CC — USB host port (controllers via hub)
    MidiSerial,    // NRPN / curated CC — Serial1 UART (ESP32 controller)
    SysEx,         // editor SET_PARAM / patch dump
    Ui,            // local TFT / panel
    PatchLoad,     // internal storage recall
    Sequencer,     // step sequencer / arpeggiator writes
};

class ParameterStore {
public:
    // Table geometry — fixed at compile time from the generated table.
    static constexpr size_t kCount      = Params::kParamCount;   // parameters
    static constexpr size_t kSlots      = Params::kStoreSlots;   // storage cells
    static constexpr size_t kLayers     = 2;                     // A and B
    static constexpr size_t kDirtyWords = (kSlots + 31u) / 32u;

    // "Not found" / "nothing left" sentinel.  It is kSlots, NOT kCount: once
    // layer B's slots exist, kCount is a perfectly valid slot number and
    // reusing it as a sentinel would make the first banked layer-B parameter
    // read as "clean" forever.  Callers compare against this constant, so
    // moving its value is safe; hard-coding kCount anywhere is not.
    static constexpr size_t kInvalidIndex = kSlots;

    // Constructor loads every parameter's default (converted from the
    // engineering-unit default in the table), publishes once, and leaves
    // EVERY parameter dirty on purpose: the engine's first audio block then
    // applies the complete state with no special "initial sync" code path.
    ParameterStore();

    // =========================================================================
    // CONTROL PLANE API — loop() context only.  Never call from the audio ISR.
    // =========================================================================

    // -------------------------------------------------------------------------
    // A NOTE ON THE 'layer' ARGUMENT
    //   Every accessor takes layer as a TRAILING DEFAULTED argument (0 = A).
    //   That is deliberate: it keeps the ~40 existing single-layer call sites
    //   compiling and meaning exactly what they meant before, so this change
    //   cannot alter behaviour anywhere it has not been reviewed.  The cost is
    //   that a site which SHOULD pass a layer and forgets will silently use A.
    //   Sites that must be layer-aware are converted explicitly, one subsystem
    //   at a time, and each conversion is its own reviewable change.
    //
    //   'layer' is masked, not asserted: out-of-range values fold to a valid
    //   layer rather than reading past the array.  A corrupt MIDI byte must
    //   not be able to index memory.
    // -------------------------------------------------------------------------

    // Set by permanent ParamID.  'normalized' is clamped to 0..1 (NaN -> 0).
    // Returns false (and changes nothing) for an unknown id — a malformed
    // NRPN number must not corrupt neighbouring state.
    bool set(uint16_t id, float normalized, Origin origin, uint8_t layer = 0);

    // Convenience: set in engineering units (Hz, ms, ...) via Curves.
    // The SysEx editor path speaks engineering floats, so it lands here.
    bool setEngineering(uint16_t id, float engineering, Origin origin,
                        uint8_t layer = 0);

    // Read the control plane's view — always the freshest value, including
    // writes not yet published to the audio plane (mid-bulk).  Unknown id
    // returns 0.0f; use indexOf() first when the id needs validating.
    float get(uint16_t id, uint8_t layer = 0) const;
    float getEngineering(uint16_t id, uint8_t layer = 0) const;

    // Origin of the most recent change to a parameter (Init until touched).
    Origin origin(uint16_t id, uint8_t layer = 0) const;

    // Bulk loads (patch recall).  Between beginBulk() and endBulk() nothing
    // is published — the audio plane keeps playing the OLD state, then sees
    // the ENTIRE new state at once.  Nestable; publication happens when the
    // outermost endBulk() runs.
    void beginBulk();
    void endBulk();

    // Reset every parameter to its table default (one bulk publish).
    void resetToDefaults(Origin origin);

    // ParamID -> dense table index (binary search over the id-sorted table;
    // ~8 steps for 140 params).  Returns kInvalidIndex when unknown.
    // Transports that touch one parameter repeatedly should cache this.
    static size_t indexOf(uint16_t id);

    // Fast-path accessors for callers that already hold a dense index
    // (skips the binary search).  Index must be < kCount.
    float getByIndex(size_t index, uint8_t layer = 0) const;
    void  setByIndex(size_t index, float normalized, Origin origin,
                     uint8_t layer = 0);

    // Last-writer origin by dense index — the broadcaster's suppression
    // fact, on the hot drain path, so no binary search (unlike origin()).
    Origin originByIndex(size_t index, uint8_t layer = 0) const;

    // True when a parameter has independent per-layer values.  Callers that
    // iterate layers use this to avoid visiting a shared parameter twice —
    // writing it once per layer is harmless but wasteful, and reading it
    // twice invites the reader to believe the two values could differ.
    static bool isBanked(size_t index)
    {
        return index < kCount && Params::isBanked(index);
    }

    // --- broadcast (tx) dirty tracking — Phase B' ---------------------------
    // A SECOND dirty bitset, set at the same publish point as the audio
    // one, consumed by ParamBroadcast to mirror every accepted change out
    // to the editor ports.  Pop one changed index (clearing its bit), or
    // kInvalidIndex when clean.
    //
    // CONCURRENCY NOTE — plain words, deliberately NOT atomic (deviation
    // from the Phase B' spec's "mirror _dirty" wording, flagged): every
    // store writer in this codebase is control-plane (verified: the
    // sequencer modulates via per-block accumulators, never store writes),
    // so producer (publish) and consumer (broadcast drain) share one
    // context and cannot preempt each other.  If an audio-plane writer is
    // ever added, promote these to std::atomic exactly like _dirty.
    size_t takeNextTxDirty();

    // Drop all pending broadcast bits.  Used once by the constructor (boot
    // must not spray 140 NRPNs at a DAW that just enumerated us — editors
    // pull state via the resync request instead) and by tests.
    void clearTxDirty();

    // =========================================================================
    // AUDIO PLANE API — audio ISR context only.
    // =========================================================================

    // Base pointer of the published value buffer, indexed by dense table
    // index, values normalized 0..1.  Grab ONCE at the top of each block;
    // the pointed-to buffer is guaranteed stable until the ISR returns
    // (the control plane never mutates the front buffer).
    const float* acquireSnapshot() const;

    // Pop one changed SLOT (not index) and clear its dirty bit, or
    // kInvalidIndex when nothing (else) changed.  The snapshot is indexed by
    // slot, so v[slot] is the right value; naming the PARAMETER again needs
    // Params::paramOfSlot().  Typical engine loop:
    //
    //   const float* v = store.acquireSnapshot();
    //   for (size_t s; (s = store.takeNextDirty()) != store.kInvalidIndex; ) {
    //       const size_t  i     = Params::paramOfSlot(s);
    //       const uint8_t layer = Params::layerOfSlot(s);
    //       applyParam(i, v[s], layer);   // convert + update coefficients
    //   }
    //
    // Returning the slot rather than (index, layer) keeps this function's
    // signature and its wait-free ctz walk untouched, and leaves the decode
    // — two compares — at the one call site that actually needs it.
    //
    // Cost when idle: kDirtyWords relaxed loads, all zero.
    size_t takeNextDirty();

#ifdef JT_TESTING
    // Host-test hook: called at the two interesting interruption points
    // inside publish() (after the flip / after the value sync).  Unit tests
    // install a fake "audio ISR" here to prove snapshot consistency at the
    // exact instants a real ISR could preempt publish().  Compiled out of
    // firmware builds entirely.
    void (*testPreemptHook)(void*) = nullptr;
    void* testPreemptCtx = nullptr;
#endif

private:
    // Fold any byte into a valid layer.  A layer arrives from MIDI in the
    // end, so it is masked rather than asserted — a corrupt wire byte must
    // cost a wrong-layer edit, never an out-of-bounds store.
    static constexpr uint8_t layerMask(uint8_t layer)
    {
        return static_cast<uint8_t>(layer & 0x01u);
    }

    // The single write primitive: every public setter resolves to a slot and
    // funnels through here, so the publish rule and the dirty-bit rule are
    // stated once rather than once per overload.
    void setBySlot(size_t slot, float normalized, Origin origin);

    // Make pending writes visible to the audio plane.  Control context only.
    // Ordering inside is load-bearing — see the .cpp walk-through.
    void publish();

#ifdef JT_TESTING
    void testPreempt() { if (testPreemptHook) testPreemptHook(testPreemptCtx); }
#else
    void testPreempt() {}
#endif

    // --- value storage -------------------------------------------------------
    // Two buffers; _front indexes the one the audio plane reads.  Plain
    // floats are safe: aligned 32-bit stores are single-copy-atomic on both
    // Cortex-M7 and the host, and the buffer being written is by
    // construction never the one being read.
    // Indexed by SLOT, so layer A occupies [0, kCount) exactly as it did
    // before layering existed and layer B's banked copies follow it.
    float _values[2][kSlots];
    std::atomic<uint32_t> _front;

    // --- dirty tracking ------------------------------------------------------
    // _pending: control-plane-private accumulation between publishes (plain
    // words — only the control plane touches them).
    // _dirty:   the audio-visible bitset (atomic — both planes RMW it).
    uint32_t              _pending[kDirtyWords];
    std::atomic<uint32_t> _dirty[kDirtyWords];
    uint32_t              _txDirty[kDirtyWords];   // broadcast-out bits (see
                                                   // takeNextTxDirty for why
                                                   // these are plain words)

    // --- metadata ------------------------------------------------------------
    uint8_t  _origin[kSlots];   // last Origin per SLOT (control-plane only)
    uint32_t _bulkDepth;        // beginBulk() nesting counter
};

} // namespace JT
