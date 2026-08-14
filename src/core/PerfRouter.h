// =============================================================================
// PerfRouter.h — "which layer does this MIDI event belong to?" for JT-8000 v2
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   Performance mode (Single / Layer / Split) is entirely a ROUTING question:
//   given a MIDI channel, and for notes a key number, which of the two layers
//   should receive the event?  That question has exactly one correct answer at
//   any instant, it depends only on the four perf.* parameters, and it is
//   needed by several unrelated callers (note handlers, the pitch wheel, the
//   curated-CC path).  So it lives here — one small, pure, host-testable
//   object — rather than being re-derived, slightly differently, at each site.
//
//   This class does NOT route parameter EDITS.  Editor traffic carries its
//   layer explicitly in the NRPN address (bit 13); see MidiParamTransport.
//   Only live performance traffic is channel-routed.  Keeping those two
//   mechanisms apart is deliberate: in Layer mode both layers are routinely
//   assigned the SAME channel so one keyboard plays both, and if edits were
//   channel-routed too they would become unaddressable the moment a user did
//   the most ordinary thing in the world.
//
// CPU CONTRACT ("do not calculate if not required")
//   Parameter indices are resolved ONCE in the constructor, so no call here
//   ever performs the store's binary search.  Single mode — the default, and
//   the common case — reads exactly ONE parameter and returns; it never looks
//   at the channels or the split point.  A note-on in Layer/Split mode costs
//   three or four reads of already-published floats.  Nothing here runs per
//   sample or per block.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "core/ParameterStore.h"

namespace JT {

// A destination SET, not a single layer: in Layer mode one key legitimately
// sounds both layers, and with no channel match it sounds neither.  A bitmask
// says all four of those things without a special case.
enum class LayerMask : uint8_t {
    None = 0,
    A    = 1u << 0,
    B    = 1u << 1,
    Both = A | B,
};

inline constexpr LayerMask operator&(LayerMask a, LayerMask b)
{
    return static_cast<LayerMask>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}
inline constexpr LayerMask operator|(LayerMask a, LayerMask b)
{
    return static_cast<LayerMask>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline constexpr bool has(LayerMask set, LayerMask which)
{
    return (static_cast<uint8_t>(set) & static_cast<uint8_t>(which)) != 0u;
}

class PerfRouter {
public:
    // perf.mode option indices — these mirror the 'perf_mode' option set in
    // params.yaml.  Named here so the routing logic below reads as prose
    // rather than as magic numbers; the generator's option ORDER is what
    // makes them true, and appending never changes existing indices.
    enum : int { kModeSingle = 0, kModeLayer = 1, kModeSplit = 2 };

    explicit PerfRouter(const ParameterStore& store);

    // Live performance traffic with no key number: pitch bend, mod wheel,
    // curated CCs, sustain.
    LayerMask forChannel(uint8_t channel1to16) const;

    // Note on/off.  Applies the channel gate first, THEN the keyboard split,
    // and returns the intersection.  Doing it in that order matters when both
    // layers share a channel: the channel gate says "both", and the split is
    // what actually picks one.
    LayerMask forNote(uint8_t note, uint8_t channel1to16) const;

    // True when Performance is off.  Callers that only need "is anything
    // layered at all?" use this instead of paying for a full route.
    bool isSingle() const { return mode() == kModeSingle; }

    // Current perf.mode option index.
    int mode() const;

    // Current split key.  Notes strictly BELOW it belong to layer A; the
    // split key itself and everything above belong to layer B.
    uint8_t splitNote() const;

private:
    // 1..16 as the wire and the UI both speak it; 0 means "no such channel"
    // and matches nothing.
    uint8_t channelOf(size_t paramIndex) const;

    const ParameterStore& _store;

    // Resolved once — see the CPU contract above.
    size_t _iMode;
    size_t _iChA;
    size_t _iChB;
    size_t _iSplitNote;
};

} // namespace JT
