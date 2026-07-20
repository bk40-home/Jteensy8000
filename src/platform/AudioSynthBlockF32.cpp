// =============================================================================
// AudioSynthBlockF32.cpp — implementation (role in the header)
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "platform/AudioSynthBlockF32.h"

namespace JT {

void AudioSynthBlockF32::update()
{
#if defined(__IMXRT1062__)
    // DWT cycle counter: free-running 32-bit at CPU clock, enabled by the
    // Teensy core at boot.  Reading it is one load — the probe itself is
    // effectively free.
    const uint32_t t0 = ARM_DWT_CYCCNT;
#endif

    audio_block_f32_t* left  = AudioStream_F32::allocate_f32();
    if (left == nullptr) return;                     // pool exhausted: skip
    audio_block_f32_t* right = AudioStream_F32::allocate_f32();
    if (right == nullptr) {
        AudioStream_F32::release(left);              // never leak the pair
        return;
    }

    // The whole synthesizer happens on this line — same code as `make test`.
    _core.renderBlock(left->data, right->data, kBlockSize);

    AudioStream_F32::transmit(left,  0);
    AudioStream_F32::transmit(right, 1);
    AudioStream_F32::release(left);
    AudioStream_F32::release(right);

#if defined(__IMXRT1062__)
    const uint32_t dt = ARM_DWT_CYCCNT - t0;         // wrap-safe subtraction
    perfLastCycles = dt;
    if (dt > perfMaxCycles) perfMaxCycles = dt;
#endif
}

} // namespace JT
