#pragma once
// =============================================================================
// VoicePool.h — shared MAX_VOICES-voice pool for multi-engine (layer) setups
// =============================================================================
//
// Owns the single array of VoiceBlock objects that SynthEngine instances share
// (hard-partitioned by range, Option A). Before the layering refactor each
// SynthEngine owned its own `new VoiceBlock[MAX_VOICES]`; with two engines
// that approach would have doubled the per-voice DSP cost even when a voice
// is silent (Teensy AudioStream::update() is called unconditionally on every
// registered object). This class centralises ownership so we always have
// exactly MAX_VOICES voices registered with the Audio Library, regardless of
// how many engines slice into them.
//
// USAGE:
//   VoicePool pool;                           // held by LayerManager
//   _engineA.setVoicePool(&pool);             // ctor-time or pre-begin()
//   _engineB.setVoicePool(&pool);
//   _engineA.setVoiceRange(0, 4);
//   _engineB.setVoiceRange(4, 4);
//   _engineA.begin();   _engineB.begin();     // wires both to SAME voices
//
// LIFETIME:
//   The pool must outlive every SynthEngine that references it. In practice
//   LayerManager owns it as a direct member and the engines are also direct
//   members, so construction order (pool before engines) is guaranteed by
//   LayerManager's member declaration order.
// =============================================================================

#include "VoiceBlock.h"     // brings in MAX_VOICES via SynthEngine.h is NOT safe
                             // (circular). The definition we need is the raw
                             // constant — guard it here if not yet seen.

#ifndef MAX_VOICES
#define MAX_VOICES 8   // must match SynthEngine.h
#endif

class VoicePool {
public:
    // No dynamic allocation — VoiceBlock instances live directly in the pool.
    // This keeps audio objects in fast RAM1 (same region as other globals)
    // instead of the general heap.
    VoicePool() = default;

    // Access by absolute index (0..MAX_VOICES-1). Caller is responsible for
    // bounds; engines use _firstVoice/_voiceCount to stay within their slice.
    inline VoiceBlock&       voice(uint8_t idx)       { return _voices[idx]; }
    inline const VoiceBlock& voice(uint8_t idx) const { return _voices[idx]; }

    // Raw pointer for engines that prefer pointer arithmetic (matches the
    // existing SynthEngine._voices access pattern, one-for-one swap).
    inline VoiceBlock*       data()       { return _voices; }
    inline const VoiceBlock* data() const { return _voices; }

    // Capacity — always MAX_VOICES in this project; accessor provided for
    // forward-compatibility if the constant ever becomes a runtime value.
    static constexpr uint8_t size() { return MAX_VOICES; }

private:
    VoiceBlock _voices[MAX_VOICES];
};
