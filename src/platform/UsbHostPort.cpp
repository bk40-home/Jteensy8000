// =============================================================================
// UsbHostPort.cpp — see UsbHostPort.h.
// =============================================================================
#include "UsbHostPort.h"

#include <Arduino.h>

#include <atomic>

#include "core/AudioConfig.h"   // JT::kSampleRate — ASRC ratio seed
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

// Dispatch one decoded message to the handlers.  Runs in loop() context.
void dispatch(const UsbMidiMessage& message)
{
    // Forwarding sees everything that passed the cable filter, before any
    // interpretation, so a DAW receives exactly what the instrument sent.
    if (g_onForward != nullptr) {
        g_onForward(message.cable, message.data, message.length);
    }

    // Channel is 1-16 here, matching what usbMIDI and the FortySevenEffects
    // library hand the other two ports.
    const uint8_t channel = static_cast<uint8_t>((message.data[0] & 0x0Fu) + 1u);

    switch (message.cin) {
        case kUsbMidiCinNoteOn:
            // Velocity zero is a note off.  The other transports pass it
            // through as a note on and SynthCore copes, but translating here
            // keeps the two spellings from reaching the allocator at all.
            if (message.data[2] == 0u) {
                if (g_onNoteOff != nullptr) {
                    g_onNoteOff(channel, message.data[1], 0u);
                }
            } else if (g_onNoteOn != nullptr) {
                g_onNoteOn(channel, message.data[1], message.data[2]);
            }
            break;

        case kUsbMidiCinNoteOff:
            if (g_onNoteOff != nullptr) {
                g_onNoteOff(channel, message.data[1], message.data[2]);
            }
            break;

        case kUsbMidiCinControlChange:
            if (g_onControl != nullptr) {
                g_onControl(channel, message.data[1], message.data[2]);
            }
            break;

        case kUsbMidiCinPitchBend: {
            if (g_onBend == nullptr) { break; }
            // Raw 14-bit from the wire, converted to the centred form every
            // other port delivers.  See the header, and the warning block
            // above onPitchBend in main.cpp.
            const int raw = static_cast<int>(message.data[1]) |
                            (static_cast<int>(message.data[2]) << 7);
            g_onBend(channel, raw - 8192);
            break;
        }

        case kUsbMidiCinSingleByte:
            // System real-time arrives as a one-byte packet.  Anything below
            // 0xF8 in this form is system common, which the clock ignores.
            if (g_onRealtime != nullptr && message.data[0] >= 0xF8u) {
                g_onRealtime(message.data[0]);
            }
            break;

        default:
            // System common and SysEx fragments: nothing here consumes them.
            break;
    }
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
    // Queued regardless of connection state: send() refuses cleanly when the
    // queue is full and the queue is simply never drained while detached, so
    // ParamBroadcast keeps its "no-op when nothing is attached" contract.
    (void)g_midi.sendControlChange(channel, cc, value);
}

void UsbHostPort::poll(int maxMessages)
{
    UsbMidiMessage message;
    for (int i = 0; i < maxMessages && g_midi.read(message); ++i) {
        dispatch(message);
    }

    // Starts a transfer if one is not already running, and picks up anything
    // queued in the window where a completion callback found the queue empty.
    g_midi.flushOutput();

#if JT_USBHOST_AUDIO_BUILD
    // The device's format is only known once its descriptors have been read
    // and the stream has started, which happens asynchronously well after
    // begin().  Latch it the first time it appears.
    if (!g_asrcReady.load(std::memory_order_relaxed) && g_audio.isStreaming()) {
        const uint32_t deviceRate = g_audio.deviceSampleRate();
        if (deviceRate != 0u) {
            // Nominal ratio: input frames consumed per output frame.  Seed it
            // from the ENGINE's rate, JT::kSampleRate — push() runs on the
            // audio update, so the input stream is engine-rate frames, and the
            // I2S clock is derived from this same figure via AudioSettings_F32
            // in main.cpp.  (Was AUDIO_SAMPLE_RATE_EXACT ≈ 44117: correct only
            // while the engine ran at 44.1 kHz.  After the 48 kHz port that
            // seed was ~8 % off — far beyond the servo's ppm trim authority and
            // the ring's ~21 ms depth — so the ring over/under-ran continuously
            // and the USB-host output garbled.)  The residual PLL/crystal offset
            // between nominal kSampleRate and the true SAI rate is ppm-scale,
            // which is exactly what AsrcServo exists to trim out.
            const float ratio = JT::kSampleRate /
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
    }
    Serial.print(" aerr=");
    Serial.print(g_audio.transferErrors());
#endif
}

} // namespace JT