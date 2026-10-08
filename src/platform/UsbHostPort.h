// =============================================================================
// UsbHostPort.h — the Teensy 4.1 USB host port: MIDI in/out plus audio out
// =============================================================================
//
// WHY THIS REPLACES USBHost_t36
//   USBHost_t36 has no isochronous transport, so it can carry MIDI from the
//   host port but never audio.  A-Dunstan's teensy4_usbhost has both.  The
//   two libraries drive the same EHCI controller and the same interrupt, so
//   they cannot coexist: this is a replacement, not an addition.
//
//   Everything the old port did is preserved — note, control change, pitch
//   bend and real-time handlers, and the outbound CC that ParamBroadcast
//   uses to mirror parameters back to a controller.  What is new is that the
//   instrument's own speakers become an output for the synth.
//
// PITCH BEND, ONCE MORE
//   The raw USB-MIDI packet carries the unsigned 0..16383 form.  Every other
//   transport in this firmware hands main.cpp the CENTRED form, and
//   onPitchBend adds +8192 to recover the raw value.  This adapter therefore
//   subtracts 8192 before calling the handler, so that ONE convention holds
//   on every port and main.cpp needs no special case.  See the warning block
//   above onPitchBend in main.cpp before changing this.
//
// THREADING
//   The USB host library runs its own thread.  Driver callbacks arrive there;
//   MIDI messages are queued and dispatched from poll() in loop() context,
//   which is the same context the old midiHost.read() dispatched in, so the
//   handlers see no change.  Audio is pushed from the audio ISR and pulled
//   on the USB thread through the lock-free Asrc.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

// Build switch for the USB-host AUDIO path.  Defined here rather than in
// main.cpp so that this header and its .cpp agree: with it set to 0 the audio
// driver is not merely disconnected from the graph but never constructed, so
// it never registers, never claims the streaming interface, and no
// isochronous callback ever runs.  That is what makes it a usable bisect —
// gating only the graph connection leaves the entire audio path alive.
#ifndef JT_USBHOST_AUDIO
#define JT_USBHOST_AUDIO 1
#endif

#if JT_USBHOST_AUDIO && \
    (defined(JT_BACKEND_PCM5102) || defined(JT_BACKEND_SGTL5000))
#define JT_USBHOST_AUDIO_BUILD 1
#else
#define JT_USBHOST_AUDIO_BUILD 0
#endif

namespace JT {

// Handler signatures, matching the shapes main.cpp already binds to the
// other two ports so the same functions can be reused verbatim.
typedef void (*UsbHostNoteOnFn)(uint8_t channel, uint8_t note, uint8_t velocity);
typedef void (*UsbHostNoteOffFn)(uint8_t channel, uint8_t note, uint8_t velocity);
typedef void (*UsbHostControlFn)(uint8_t channel, uint8_t cc, uint8_t value);

// Pitch bend is delivered CENTRED, value in [-8192, +8191], resting = 0.
typedef void (*UsbHostBendFn)(uint8_t channel, int value);

// One raw real-time status byte, as ExternalClock expects.
typedef void (*UsbHostRealtimeFn)(uint8_t status);

// Every accepted message, before type dispatch, for verbatim forwarding to
// another port.  Bytes are the MIDI message itself, length 1 to 3.
typedef void (*UsbHostForwardFn)(uint8_t cable, const uint8_t* data,
                                 uint8_t length);

// -----------------------------------------------------------------------------
// UsbHostPort — facade over the host stack.
//
// A facade rather than an object holding the drivers, because the host
// controller and every DMA buffer must live in OCRAM: those are file-scope
// statics in the .cpp, and this class carries no state that would drag the
// placement requirement into whatever declares it.
// -----------------------------------------------------------------------------
class UsbHostPort
{
public:
    // Starts the host stack.  Call once from setup(), after handlers are
    // registered so nothing arrives before there is somewhere to put it.
    void begin(void);

    // Drains inbound MIDI to empty, dispatches to the handlers, and starts
    // any queued outbound transfer.  Call once per loop() pass; it replaces
    // both myusb.Task() and the midiHost.read() drain.
    //
    // 'maxMessages' bounds the worst case exactly as kMaxMidiDrain does for
    // the other ports, so one flooding controller cannot starve loop().
    void poll(int maxMessages);

    void setHandleNoteOn(UsbHostNoteOnFn fn);
    void setHandleNoteOff(UsbHostNoteOffFn fn);
    void setHandleControlChange(UsbHostControlFn fn);
    void setHandlePitchChange(UsbHostBendFn fn);
    void setHandleRealTimeSystem(UsbHostRealtimeFn fn);
    void setHandleForward(UsbHostForwardFn fn);

    // Accepted cables, bit per cable, for any device without a vendor rule.
    // Defaults to accepting everything.
    void setCableMask(uint16_t mask);

    // Per-instrument override of the mask above, matched on USB vendor ID.
    // See UsbHostMidi::setCableMaskForVendor.  Call before begin().
    void setCableMaskForVendor(uint16_t vendorId, uint16_t mask);

    // Outbound CC for ParamBroadcast.  Queued, never blocking, and a no-op
    // when nothing is attached — the same contract the old sink relied on.
    void sendControlChange(uint8_t cc, uint8_t value, uint8_t channel);

    bool midiConnected(void) const;

#if JT_USBHOST_AUDIO_BUILD
    // Audio in, from the audio ISR.  Frames are the engine's block; they are
    // resampled to the device's rate and pulled by the USB thread.  Silently
    // does nothing until a device with a playback path has attached, so the
    // sink node can call it unconditionally.
    void pushAudio(const float* left, const float* right, size_t frames);

    bool audioStreaming(void) const;
#endif

    // One compact line for the 1 Hz bring-up status.  Prints nothing when no
    // device is attached.
    void printStatus(void) const;

};

extern UsbHostPort gUsbHostPort;

} // namespace JT