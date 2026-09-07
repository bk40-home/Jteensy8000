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

// Bisect switches for the two things this port does that the standalone
// bring-up demo never exercised.  Both default on.
//
//   JT_USBHOST_TX=0       ParamBroadcast's outbound CCs are dropped instead
//                         of queued, so no bulk OUT transfer is ever
//                         submitted on the host device.
//   JT_USBHOST_FORWARD=0  Inbound messages are not re-sent on the USB device
//                         port, so nothing calls usbMIDI from the host
//                         dispatch path.
//
// Inbound MIDI still reaches the synth with either set to 0; only the extra
// traffic disappears.
#ifndef JT_USBHOST_TX
#define JT_USBHOST_TX 1
#endif
#ifndef JT_USBHOST_FORWARD
#define JT_USBHOST_FORWARD 1
#endif

//   JT_USBHOST_DISPATCH=0  Messages are read, decoded and counted, but no
//                          handler is called at all: nothing reaches the
//                          synth, the clock or the device port.  With this
//                          set to 0 the firmware uses the host stack exactly
//                          as the standalone bring-up demo did.  If it still
//                          faults, the fault is not in this port's glue but
//                          in the host library coexisting with the engine.
#ifndef JT_USBHOST_DISPATCH
#define JT_USBHOST_DISPATCH 1
#endif

// Finer splits within dispatch, for isolating which handler faults.  Each
// defaults on; set any to 0 to decode and count that message type but deliver
// it nowhere.  They are independent, so a single build can rule out several.
//
//   JT_USBHOST_NOTES=0     no noteOn/noteOff reaches the synth
//   JT_USBHOST_CC=0        no control change reaches the NRPN transport
//   JT_USBHOST_BEND=0      no pitch bend reaches the synth
//   JT_USBHOST_REALTIME=0  no clock byte reaches ExternalClock
#ifndef JT_USBHOST_NOTES
#define JT_USBHOST_NOTES 1
#endif
#ifndef JT_USBHOST_CC
#define JT_USBHOST_CC 1
#endif
#ifndef JT_USBHOST_BEND
#define JT_USBHOST_BEND 1
#endif
#ifndef JT_USBHOST_REALTIME
#define JT_USBHOST_REALTIME 1
#endif

// Dispatch tracing.  With this on, every message prints its bytes and the
// handler about to receive it, then prints again once that handler returns,
// flushing both so nothing is lost to buffering when the board resets.
//
// The last line before a reboot therefore names the message and the handler
// that did not return — which is one build instead of one per handler.
//
// It prints per message and calls Serial.flush(), so it is far too slow to
// leave on: bring-up only.
#ifndef JT_USBHOST_TRACE
#define JT_USBHOST_TRACE 0
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

    // Accepted cables, bit per cable.  Instruments that mirror one keypress
    // onto several cables need this narrowed or every note sounds twice; the
    // Studiologic NC2x is one.  Defaults to accepting everything.
    void setCableMask(uint16_t mask);

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

    // Prints which build switches are active.  Called once at boot so that
    // every log identifies the binary that produced it: a flag that was not
    // actually passed to the compiler looks identical, in the output, to a
    // feature that ran and did nothing.
    void printBuildConfig(void) const;
};

extern UsbHostPort gUsbHostPort;

} // namespace JT