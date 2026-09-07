// =============================================================================
// AudioSinkUsbHost_F32.cpp — see AudioSinkUsbHost_F32.h.
// =============================================================================
#include "platform/AudioSinkUsbHost_F32.h"

#if JT_USBHOST_AUDIO_BUILD

#include <string.h>

namespace JT {

namespace {

// Substituted for a channel that produced no block this update.  A missing
// block means silence, not "reuse whatever was there last time", which would
// be a repeating fragment at the block rate.
float g_silence[AUDIO_BLOCK_SAMPLES];
bool  g_silenceReady = false;

} // namespace

void AudioSinkUsbHost_F32::update(void)
{
    if (!g_silenceReady) {
        memset(g_silence, 0, sizeof(g_silence));
        g_silenceReady = true;
    }

    audio_block_f32_t* left  = receiveReadOnly_f32(0);
    audio_block_f32_t* right = receiveReadOnly_f32(1);

    // Blocks are released on every path below, including the early return:
    // leaking one starves the F32 pool and the whole graph stops within
    // seconds.
    if (left != nullptr || right != nullptr) {
        const float* l = (left  != nullptr) ? left->data  : g_silence;
        const float* r = (right != nullptr) ? right->data : g_silence;
        const uint16_t frames = (left != nullptr) ? left->length
                                                  : right->length;

        // Returns immediately when no playback device is attached, so the
        // cost while nothing is plugged in is one branch.
        gUsbHostPort.pushAudio(l, r, frames);
    }

    if (left  != nullptr) { AudioStream_F32::release(left);  }
    if (right != nullptr) { AudioStream_F32::release(right); }
}

} // namespace JT

#endif // JT_USBHOST_AUDIO_BUILD