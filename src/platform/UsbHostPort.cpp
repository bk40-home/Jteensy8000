// =============================================================================
// UsbHostPort.cpp — see UsbHostPort.h.
// =============================================================================
#include "UsbHostPort.h"

#include <Arduino.h>

#include <atomic>

#include "core/dsp/Asrc.h"
#if JT_USBHOST_AUDIO_BUILD
#include "platform/usbhost/UsbAudioOut.h"
#endif
#include "platform/usbhost/UsbHostMidi.h"
#include "platform/usbhost/UsbMidiPacket.h"

namespace JT {

namespace {

// The host controller and both drivers.  File-scope statics because the
// controller wants OCRAM and the drivers register themselves with the
// library's factory the moment they are constructed — one place, one order,
// no surprises.
DMAMEM TeensyUSBHost2 g_host;
UsbHostMidi           g_midi;

#if JT_USBHOST_AUDIO_BUILD
UsbAudioOut g_audio;

// Rate conversion between the engine and whatever crystal the instrument
// runs on.  The servo is updated from pushAudio(), which is the steadiest
// clock available on this side.
Asrc      g_asrc;
AsrcServo g_servo;

// Set once the device's format is known, so pushAudio() can tell a running
// path from one that has not started yet.
// Written in loop(), read in the audio ISR: atomic so the ISR cannot observe
// a half-initialised resampler while poll() is starting one up.
std::atomic_bool g_asrcReady{false};
uint32_t g_asrcRateHz  = 0u;
#endif

// Handlers, all null until main.cpp binds them.
UsbHostNoteOnFn    g_onNoteOn    = nullptr;
UsbHostNoteOffFn   g_onNoteOff   = nullptr;
UsbHostControlFn   g_onControl   = nullptr;
UsbHostBendFn      g_onBend      = nullptr;
UsbHostRealtimeFn  g_onRealtime  = nullptr;
UsbHostForwardFn   g_onForward   = nullptr;

#if JT_USBHOST_AUDIO_BUILD
// Pull callback for the audio driver.  Runs on the USB host thread.
void audioFill(void* context, int32_t* interleaved, uint16_t frames,
               uint8_t channels)
{
    (void)context;
    g_asrc.pull(interleaved, frames, channels);
}
#endif

#if JT_USBHOST_TRACE
// Breadcrumbs either side of a handler call.  Flushed, because an unflushed
// line is exactly the one lost when the fault reboots the board.
void traceEnter(const char* what, const UsbMidiMessage& m)
{
    Serial.print("[HOST] cin=");
    Serial.print(m.cin, HEX);
    Serial.print(" d=");
    Serial.print(m.data[0], HEX);
    Serial.print(",");
    Serial.print(m.data[1], HEX);
    Serial.print(",");
    Serial.print(m.data[2], HEX);
    Serial.print(" -> ");
    Serial.println(what);
    Serial.flush();
}

void traceLeave(const char* what)
{
    Serial.print("[HOST] ");
    Serial.print(what);
    Serial.println(" returned");
    Serial.flush();
}
#define JT_TRACE_ENTER(w, m) traceEnter(w, m)
#define JT_TRACE_LEAVE(w)    traceLeave(w)
#else
#define JT_TRACE_ENTER(w, m) ((void)0)
#define JT_TRACE_LEAVE(w)    ((void)0)
#endif

// Dispatch one decoded message to the handlers.  Runs in loop() context.
void dispatch(const UsbMidiMessage& message)
{
#if !JT_USBHOST_DISPATCH
    // Read and counted, but delivered nowhere.  Reduces the firmware's use of
    // the host port to exactly what the bring-up demo did.
    (void)message;
    return;
#else
#if JT_USBHOST_FORWARD
    // Forwarding sees everything that passed the cable filter, before any
    // interpretation, so a DAW receives exactly what the instrument sent.
    if (g_onForward != nullptr) {
        JT_TRACE_ENTER("forward", message);
        g_onForward(message.cable, message.data, message.length);
        JT_TRACE_LEAVE("forward");
    }
#endif

    // Channel is 1-16 here, matching what usbMIDI and the FortySevenEffects
    // library hand the other two ports.
    const uint8_t channel = static_cast<uint8_t>((message.data[0] & 0x0Fu) + 1u);

    switch (message.cin) {
        case kUsbMidiCinNoteOn:
#if !JT_USBHOST_NOTES
            break;
#else
            // Velocity zero is a note off.  The other transports pass it
            // through as a note on and SynthCore copes, but translating here
            // keeps the two spellings from reaching the allocator at all.
            if (message.data[2] == 0u) {
                if (g_onNoteOff != nullptr) {
                    g_onNoteOff(channel, message.data[1], 0u);
                }
            } else if (g_onNoteOn != nullptr) {
                JT_TRACE_ENTER("noteOn", message);
                g_onNoteOn(channel, message.data[1], message.data[2]);
                JT_TRACE_LEAVE("noteOn");
            }
            break;
#endif

        case kUsbMidiCinNoteOff:
#if !JT_USBHOST_NOTES
            break;
#else
            if (g_onNoteOff != nullptr) {
                JT_TRACE_ENTER("noteOff", message);
                g_onNoteOff(channel, message.data[1], message.data[2]);
                JT_TRACE_LEAVE("noteOff");
            }
            break;
#endif

        case kUsbMidiCinControlChange:
#if !JT_USBHOST_CC
            break;
#else
            if (g_onControl != nullptr) {
                JT_TRACE_ENTER("controlChange", message);
                g_onControl(channel, message.data[1], message.data[2]);
                JT_TRACE_LEAVE("controlChange");
            }
            break;
#endif

        case kUsbMidiCinPitchBend: {
#if !JT_USBHOST_BEND
            break;
#else
            if (g_onBend == nullptr) { break; }
            // Raw 14-bit from the wire, converted to the centred form every
            // other port delivers.  See the header, and the warning block
            // above onPitchBend in main.cpp.
            const int raw = static_cast<int>(message.data[1]) |
                            (static_cast<int>(message.data[2]) << 7);
            JT_TRACE_ENTER("pitchBend", message);
            g_onBend(channel, raw - 8192);
            JT_TRACE_LEAVE("pitchBend");
            break;
#endif
        }

        case kUsbMidiCinSingleByte:
#if !JT_USBHOST_REALTIME
            break;
#else
            // System real-time arrives as a one-byte packet.  Anything below
            // 0xF8 in this form is system common, which the clock ignores.
            if (g_onRealtime != nullptr && message.data[0] >= 0xF8u) {
                JT_TRACE_ENTER("realtime", message);
                g_onRealtime(message.data[0]);
                JT_TRACE_LEAVE("realtime");
            }
            break;
#endif

        default:
            // System common and SysEx fragments: nothing here consumes them.
            break;
    }
#endif // JT_USBHOST_DISPATCH
}

} // namespace

UsbHostPort gUsbHostPort;

void UsbHostPort::begin(void)
{
#if JT_USBHOST_AUDIO_BUILD
    g_audio.setSource(audioFill, nullptr);
#endif
    g_host.begin();
}

void UsbHostPort::setHandleNoteOn(UsbHostNoteOnFn fn)            { g_onNoteOn = fn; }
void UsbHostPort::setHandleNoteOff(UsbHostNoteOffFn fn)          { g_onNoteOff = fn; }
void UsbHostPort::setHandleControlChange(UsbHostControlFn fn)    { g_onControl = fn; }
void UsbHostPort::setHandlePitchChange(UsbHostBendFn fn)         { g_onBend = fn; }
void UsbHostPort::setHandleRealTimeSystem(UsbHostRealtimeFn fn)  { g_onRealtime = fn; }
void UsbHostPort::setHandleForward(UsbHostForwardFn fn)          { g_onForward = fn; }

void UsbHostPort::setCableMask(uint16_t mask) { g_midi.setCableMask(mask); }

bool UsbHostPort::midiConnected(void) const { return g_midi.isConnected(); }

void UsbHostPort::sendControlChange(uint8_t cc, uint8_t value, uint8_t channel)
{
#if JT_USBHOST_TX
    // Queued regardless of connection state: send() refuses cleanly when the
    // queue is full and the queue is simply never drained while detached, so
    // ParamBroadcast keeps its "no-op when nothing is attached" contract.
    (void)g_midi.sendControlChange(channel, cc, value);
#else
    (void)cc;
    (void)value;
    (void)channel;
#endif
}

void UsbHostPort::poll(int maxMessages)
{
    UsbMidiMessage message;
    for (int i = 0; i < maxMessages && g_midi.read(message); ++i) {
        dispatch(message);
    }

#if JT_USBHOST_TX
    // Starts a transfer if one is not already running, and picks up anything
    // queued in the window where a completion callback found the queue empty.
    g_midi.flushOutput();
#endif

#if JT_USBHOST_AUDIO_BUILD
    // The device's format is only known once its descriptors have been read
    // and the stream has started, which happens asynchronously well after
    // begin().  Latch it the first time it appears.
    if (!g_asrcReady.load(std::memory_order_relaxed) && g_audio.isStreaming()) {
        const uint32_t deviceRate = g_audio.deviceSampleRate();
        if (deviceRate != 0u) {
            // Nominal ratio: input frames consumed per output frame.  The
            // engine's true rate is the I2S-derived AUDIO_SAMPLE_RATE_EXACT,
            // not the nominal 44100 in AudioConfig.h — using the exact figure
            // starts the servo close to its answer instead of 400 ppm out.
            const float ratio = AUDIO_SAMPLE_RATE_EXACT /
                                static_cast<float>(deviceRate);
            const uint32_t ratioQ16 =
                static_cast<uint32_t>((ratio * 65536.0f) + 0.5f);
            g_asrc.begin(ratioQ16);
            g_servo.begin(ratioQ16);
            g_asrcRateHz = deviceRate;
            // Release: publishes the initialised ring to the audio ISR.
            g_asrcReady.store(true, std::memory_order_release);
        }
    }
    // Prints the plan and the four setup results ONCE when a device attaches,
    // then a stats line every interval.  Without this the SET_INTERFACE,
    // SET_CUR rate and clear-mute results are invisible in the firmware, and
    // a device that enumerated but refused its format looks identical to one
    // that is working.
    g_audio.report(10000u);

    if (g_asrcReady.load(std::memory_order_relaxed) && !g_audio.isStreaming()) {
        // Device unplugged: stop feeding a ring nobody drains.
        g_asrcReady.store(false, std::memory_order_release);
    }
#endif
}

#if JT_USBHOST_AUDIO_BUILD

void UsbHostPort::pushAudio(const float* left, const float* right, size_t frames)
{
    if (!g_asrcReady.load(std::memory_order_acquire)) {
        return;
    }

    (void)g_asrc.push(left, right, frames);

    // One servo update per block.  Integer only, a handful of instructions,
    // and the block rate is the steadiest tick available on this side.
    g_asrc.setRatio(g_servo.update(g_asrc.fill()));
}

bool UsbHostPort::audioStreaming(void) const { return g_audio.isStreaming(); }

#endif

void UsbHostPort::printBuildConfig(void) const
{
    Serial.print(F("[S3.1b] host port build: audio="));
    Serial.print(JT_USBHOST_AUDIO_BUILD);
    Serial.print(F(" tx="));
    Serial.print(JT_USBHOST_TX);
    Serial.print(F(" fwd="));
    Serial.print(JT_USBHOST_FORWARD);
    Serial.print(F(" dispatch="));
    Serial.print(JT_USBHOST_DISPATCH);
    Serial.print(F(" notes="));
    Serial.print(JT_USBHOST_NOTES);
    Serial.print(F(" cc="));
    Serial.print(JT_USBHOST_CC);
    Serial.print(F(" bend="));
    Serial.print(JT_USBHOST_BEND);
    Serial.print(F(" rt="));
    Serial.print(JT_USBHOST_REALTIME);
    Serial.print(F(" trace="));
    Serial.println(JT_USBHOST_TRACE);
}

void UsbHostPort::printStatus(void) const
{
    if (!g_midi.isConnected()
#if JT_USBHOST_AUDIO_BUILD
        && !g_audio.isStreaming()
#endif
    ) {
        return;
    }

    Serial.print(" | host midi=");
    Serial.print(g_midi.isConnected() ? 1 : 0);
    /* xfer/rxb are the receive path BEFORE any decoding: if these stay at
       zero while keys are played, no bulk IN transfer ever completed and the
       fault is below the packet layer.  If they climb while rx stays zero,
       data is arriving and the decoder is rejecting it. */
    Serial.print(" xfer=");
    Serial.print(g_midi.transfersCompleted());
    Serial.print(" rxb=");
    Serial.print(g_midi.bytesReceived());
    Serial.print(" rerr=");
    Serial.print(g_midi.transferErrors());
    Serial.print(" lerr=");
    Serial.print(g_midi.lastTransferError());
    Serial.print(" rx=");
    Serial.print(g_midi.messagesDecoded());
    Serial.print(" filt=");
    Serial.print(g_midi.messagesFiltered());
    Serial.print(" drop=");
    Serial.print(g_midi.messagesDropped());
    Serial.print(" tx=");
    Serial.print(g_midi.messagesSent());

#if JT_USBHOST_AUDIO_BUILD
    Serial.print(" audio=");
    Serial.print(g_audio.isStreaming() ? 1 : 0);
    if (g_asrcReady.load(std::memory_order_relaxed)) {
        Serial.print(" rate=");
        Serial.print(g_asrcRateHz);
        Serial.print(" fill=");
        Serial.print(static_cast<uint32_t>(g_asrc.fill()));
        Serial.print(" ratio=");
        Serial.print(g_asrc.getRatio());
        Serial.print(" ur=");
        Serial.print(g_asrc.underruns());
        Serial.print(" or=");
        Serial.print(g_asrc.overruns());
#if JT_ASRC_PEAK
        // Levels either side of the conversion, peak since the last line.
        //   inPk 0 and outPk 0 -> the engine is handing the sink silence;
        //                        the fault is upstream, in the graph.
        //   inPk > 0, outPk 0  -> signal is arriving and being lost in the
        //                        resampler.
        //   both > 0           -> real audio is reaching the wire and the
        //                        fault is beyond us: format, mute or volume
        //                        on the instrument itself.
        Serial.print(" inPk=");
        Serial.print(g_asrc.peakIn(), 4);
        Serial.print(" outPk=");
        Serial.print(g_asrc.peakOut());
#endif
    }
    Serial.print(" aerr=");
    Serial.print(g_audio.transferErrors());
#endif
}

} // namespace JT