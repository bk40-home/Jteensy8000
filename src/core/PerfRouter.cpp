// =============================================================================
// PerfRouter.cpp — implementation
// =============================================================================
// The routing rules and the CPU contract live in PerfRouter.h.  This file's
// job is to keep the early-outs in the order the header promises.
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "core/PerfRouter.h"

#include "core/AudioConfig.h"     // JT_COLD
#include "core/dsp/Curves.h"

namespace JT {

JT_COLD PerfRouter::PerfRouter(const ParameterStore& store)
    : _store(store),
      _iMode     (ParameterStore::indexOf(Params::ID::PERF_MODE)),
      _iChA      (ParameterStore::indexOf(Params::ID::PERF_MIDI_CHANNEL_A)),
      _iChB      (ParameterStore::indexOf(Params::ID::PERF_MIDI_CHANNEL_B)),
      _iSplitNote(ParameterStore::indexOf(Params::ID::PERF_SPLIT_NOTE))
{
}

// perf.* are Scope::Performance — single-instance, so layer A's slot IS the
// value and the defaulted layer argument is correct everywhere below.

int PerfRouter::mode() const
{
    return Curves::toOptionIndex(Params::kParams[_iMode], _store.getByIndex(_iMode));
}

uint8_t PerfRouter::splitNote() const
{
    const float note = Curves::toEngineering(Params::kParams[_iSplitNote],
                                             _store.getByIndex(_iSplitNote));
    // The table clamps to 0..127 already; the cast is guarded anyway because
    // this value indexes nothing but is compared against a MIDI note.
    return (note <= 0.0f) ? 0u : (note >= 127.0f) ? 127u : static_cast<uint8_t>(note);
}

uint8_t PerfRouter::channelOf(size_t paramIndex) const
{
    // The 'midi_channel' option set is "Ch 1".."Ch 16", so option index + 1
    // is the wire channel.  No option means omni or off — the set has neither
    // today, so an out-of-range index folds to 0 and matches nothing rather
    // than aliasing onto channel 1.
    const int opt = Curves::toOptionIndex(Params::kParams[paramIndex],
                                          _store.getByIndex(paramIndex));
    return (opt >= 0 && opt < 16) ? static_cast<uint8_t>(opt + 1) : 0u;
}

LayerMask PerfRouter::forChannel(uint8_t channel1to16) const
{
    // SINGLE MODE IS OMNI, DELIBERATELY.
    //
    // There is no "Omni" entry in the midi_channel option set, and before
    // Performance existed the firmware ignored the receive channel entirely
    // (main.cpp's handlers took 'byte /*ch*/').  Honouring perf.midi_channel_a
    // in Single mode would therefore SILENTLY STOP any rig that plays the
    // synth on a channel other than 1 — a behaviour change disguised as a
    // feature.  Single mode accepts everything, exactly as it always has.
    // Channel routing engages only once the user asks for Layer or Split.
    const int m = mode();
    if (m == kModeSingle) return LayerMask::A;

    // Layer/Split: each layer answers to its own channel.  The two are
    // compared independently rather than as an if/else chain, because the
    // ordinary Layer setup assigns BOTH layers the same channel so that one
    // keyboard plays both — and that must return Both, not just A.
    const uint8_t chA = channelOf(_iChA);
    const uint8_t chB = channelOf(_iChB);

    LayerMask out = LayerMask::None;
    if (channel1to16 == chA) out = out | LayerMask::A;
    if (channel1to16 == chB) out = out | LayerMask::B;
    return out;
}

LayerMask PerfRouter::forNote(uint8_t note, uint8_t channel1to16) const
{
    const int m = mode();
    if (m == kModeSingle) return LayerMask::A;    // early-out: no split read

    const LayerMask byChannel = forChannel(channel1to16);
    if (m != kModeSplit) return byChannel;        // Layer: channel decides alone

    // Split: the keyboard divides the two layers, and the channel gate still
    // applies on top.  Intersecting (rather than returning the split result
    // outright) is what makes a Split performance on two DIFFERENT channels
    // behave: a note on B's channel below the split point belongs to neither
    // half and correctly sounds nothing.
    const LayerMask bySplit = (note < splitNote()) ? LayerMask::A : LayerMask::B;
    return byChannel & bySplit;
}

} // namespace JT
