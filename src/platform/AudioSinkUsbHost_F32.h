// =============================================================================
// AudioSinkUsbHost_F32.h — audio-graph tap feeding the USB host port
// =============================================================================
//
// A pure sink: two F32 inputs, no outputs.  Its update() runs in the audio
// ISR, hands the block to UsbHostPort, and releases it.  Nothing else.
//
// WHY A NODE AND NOT A DIRECT CALL
//   AudioConnection_F32 has no destructor, so the graph is built once at
//   startup and never rewired.  Making this a node means the tap is a static
//   cable like every other, decided at compile time, with no dynamic
//   attachment anywhere.
//
// PLACEMENT (decision 4a): fed from the return mixers, so the instrument's
// speakers carry exactly what the PCM5102A carries, DAW return included —
// a monitor, not a second and subtly different mix.  The USBONLY backend has
// no return mixers, so this node is not built there.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

// Included first for JT_USBHOST_AUDIO_BUILD: the build system compiles every
// source file under src/, so this one must compile to nothing when the audio
// path is switched off rather than relying on nobody declaring the node.
#include "platform/UsbHostPort.h"

#if JT_USBHOST_AUDIO_BUILD

#include <AudioStream_F32.h>

namespace JT {

class AudioSinkUsbHost_F32 : public AudioStream_F32 {
public:
    AudioSinkUsbHost_F32(void) : AudioStream_F32(2, _inputQueue) {}

    virtual void update(void) override;

private:
    audio_block_f32_t* _inputQueue[2];
};

} // namespace JT

#endif // JT_USBHOST_AUDIO_BUILD