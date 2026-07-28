// =============================================================================
// main.cpp — JT-8000 v2 Teensy 4.1 firmware entry point (Phase 1)
// =============================================================================
//
// WHAT THIS FILE IS ALLOWED TO DO (design brief §9): construct objects,
// wire connections, route MIDI events to their owners, print status.
// No DSP, no parameter logic, no protocol parsing lives here.
//
// SIGNAL PATH (F1b — all F32, all cables static for life):
//   AudioSynthBlockF32 ──► AudioOutputI2S_F32          (PCM5102A/SGTL5000)
//                      └─► AudioConvert_F32toI16 ──► AudioOutputUSB
//
// MIDI ROUTING CONTRACT (brief §5.1 + Phase B' spec §3/§6):
//   THREE ports, each with its own MidiParamTransport instance (per-port
//   Origin = echo suppression identity):
//     usbMIDI   USB device — DAW / JUCE editor        Origin::MidiUsbDev
//     midiHost  USB host   — controllers via hub      Origin::MidiUsbHost
//     midi1     Serial1    — ESP32 controller, 1 Mbaud Origin::MidiSerial
//   ControlChange ─► that port's MidiParamTransport first (NRPN + curated
//   CCs); what it declines routes to the shared fallthrough: 64 sustain,
//   120 all-sound-off, 123 all-notes-off, 121 reset-controllers.  A DAW
//   panic REALLY panics.
//   OUTBOUND: every accepted parameter change is re-emitted as NRPN by
//   ParamBroadcast to every port except its producer — each editor is a
//   live view of the one ParameterStore (Phase B').
//
// BRING-UP MARKERS: the [S3.x] serial prints match OFFLINE_TESTING.md —
// each one appearing tells you which stage of the boot succeeded.
//
// HARDWARE-VERIFICATION STATUS: written against the OpenAudio API as used
// in v1; not compiled against real Teensy headers in this pass.  See
// OFFLINE_TESTING.md S2 before first flash.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include <Arduino.h>
#include <Audio.h>                     // stock lib: AudioOutputUSB + int16 pool
#include <OpenAudio_ArduinoLibrary.h>  // F32: AudioOutputI2S_F32, converters
#include <MIDI.h>                      // FortySevenEffects — Serial1 link
#include <USBHost_t36.h>               // USB host port (controllers via hub)

#include "core/ParameterStore.h"
#include "core/MidiParamTransport.h"
#include "core/ParamBroadcast.h"
#include "platform/AudioSynthBlockF32.h"
#include "platform/BoardConfig.h"

// -----------------------------------------------------------------------------
// Serial1 MIDI — the ESP32 controller link at 1 Mbaud (Board::kSerial1MidiBaud;
// deviation D-1 is documented there).  Custom settings override only the baud.
// -----------------------------------------------------------------------------

// Teensy core's PSRAM auto-detect result (MB; 0 = no chip found).  Defined in
// the core's startup.c but not declared in any header — declaring it here is
// the documented PJRC pattern.
extern "C" uint8_t external_psram_size;

struct JTSerialMidiSettings : public midi::DefaultSettings {
    static const long BaudRate = JT::Board::kSerial1MidiBaud;
};
MIDI_CREATE_CUSTOM_INSTANCE(HardwareSerial, Serial1, midi1, JTSerialMidiSettings);

// -----------------------------------------------------------------------------
// USB host — controllers plugged into the Teensy's host port, via hub.
//
// ⚠ HARDWARE WARNING (two boards have died to this): before running a
// bus-powered controller from the host port with the Teensy also on USB,
// power the host port from EXTERNAL 5 V and CUT THE VUSB->VIN TRACE.
// Powering a device like a Launchkey straight from VUSB stresses the
// VUSB->VIN path beyond what it survives.
// -----------------------------------------------------------------------------
static USBHost    myusb;
static USBHub     hub1(myusb);
static MIDIDevice midiHost(myusb);

// -----------------------------------------------------------------------------
// Audio objects — constructed once, wired once, never re-patched (F32 cables
// have no destructor; dynamic graphs crash — v1 lesson, now a hard rule).
// -----------------------------------------------------------------------------
static AudioSettings_F32       audioSettings(44100.0f, 128);
static JT::ParameterStore      gStore;
// Feedback-comb delay lines (14.1 KB): OCRAM via DMAMEM — sequential
// once-per-sample access is the ideal cached-RAM2 pattern, and DTCM is
// the scarce resource (see JT_COLD in AudioConfig.h for the memory model).
DMAMEM static float            gCombPool[JT::SynthCore::kCombPoolFloats];
// Global-reverb delay lines (~155 KB): PSRAM via EXTMEM.  Random-access delay
// taps tolerate PSRAM latency far better than DTCM can spare the space, and the
// input diffusers (the latency-sensitive part) stay in DTCM inside PlateReverb.
// Requires a PSRAM chip fitted on the Teensy 4.1; without one this pool fails to
// place and the reverb should be left bypassed (its default state anyway).
EXTMEM static float            gReverbPool[JT::SynthCore::kReverbPoolFloats];
// Per-patch FX-chain delay/mod lines (~534 KB): PSRAM via EXTMEM.  Sized for
// v1's 1500 ms max delay + 50 ms mod, stereo.  Also requires a PSRAM chip fitted
// on the Teensy 4.1; without one this pool fails to place and the FX chain stays
// inert (FxChain::begin sees a null-equivalent and processBlock bails).
EXTMEM static float            gFxPool[JT::SynthCore::kFxPoolFloats];
static JT::AudioSynthBlockF32  gSynth(gStore, gCombPool, gReverbPool, gFxPool);
// One transport per port; the Origin passed here IS the suppression identity
// ParamBroadcast keys on (Phase B' D1).
static JT::MidiParamTransport  gTransportUsbDev (gStore, JT::Origin::MidiUsbDev);
static JT::MidiParamTransport  gTransportUsbHost(gStore, JT::Origin::MidiUsbHost);
static JT::MidiParamTransport  gTransportSerial (gStore, JT::Origin::MidiSerial);

static JT::ParamBroadcast      gBroadcast(gStore);

// Audio graph — constructed once, wired once, never re-patched (F32 cables have
// no destructor; dynamic graphs crash — v1 lesson, now a hard rule).
//
// v2 restores v1's BIDIRECTIONAL USB audio ("synth as soundcard"):
//
//   OUT (record path): synth F32 -> int16 -> gUsbOut -> DAW.  Fed by the SYNTH
//     ONLY.  The DAW therefore never records its own return signal — this is
//     the structural fix for v1's loopback: the loop cannot form because the
//     return is not wired into gUsbOut.  (v1's USB-audio overclock only made
//     the loop audible/unstable; the real cure is topological, not clock-based.)
//
//   IN (monitor path): DAW -> gUsbIn (int16) -> I16toF32 -> return mixer ch1 ->
//     I2S out ONLY.  You hear the DAW return on the physical output alongside
//     the synth, so you can record the synth in the DAW while playing another
//     generated sound back out through the same box.
//
//   SIGNAL GRAPH (I2S backends):
//     gSynth -+-> gRetMixL/R ch0 -> gI2sOut        (synth + return, heard)
//             +-> gUsbConvL/R    -> gUsbOut        (synth only, recorded)
//     gUsbIn ---> gUsbInConvL/R  -> gRetMixL/R ch1 -> gI2sOut
//
// DECLARATION-ORDER RULE: AudioConnection_F32 captures its endpoints by
// reference at construction, so every node must be declared BEFORE the cable
// that names it.  The order below is deliberate — mixers before the I2S cables
// that read them, converters before the mixer cables that read them.
// -----------------------------------------------------------------------------

#if defined(JT_BACKEND_SGTL5000)
static AudioControlSGTL5000    gCodec;
#endif

// -- record path (unchanged behaviour): synth -> DAW --------------------------
// F32 -> int16 at the very edge.  This path is intentionally independent of the
// return mixer so the recorded stream is the pure synth output.
static AudioConvert_F32toI16   gUsbConvL;
static AudioConvert_F32toI16   gUsbConvR;
static AudioOutputUSB          gUsbOut;
static AudioConnection_F32     cUsbConvL(gSynth, 0, gUsbConvL, 0);
static AudioConnection_F32     cUsbConvR(gSynth, 1, gUsbConvR, 0);
static AudioConnection         cUsbL(gUsbConvL, 0, gUsbOut, 0);
static AudioConnection         cUsbR(gUsbConvR, 0, gUsbOut, 1);

// -- monitor path: DAW -> Teensy.  Stock AudioInputUSB is int16 stereo; convert
//    to F32 so it can enter the F32 return mixer.  Mirrors the outbound convert.
static AudioInputUSB           gUsbIn;
static AudioConvert_I16toF32   gUsbInConvL;
static AudioConvert_I16toF32   gUsbInConvR;
static AudioConnection         cUsbInL(gUsbIn, 0, gUsbInConvL, 0);
static AudioConnection         cUsbInR(gUsbIn, 1, gUsbInConvR, 0);

#if defined(JT_BACKEND_PCM5102) || defined(JT_BACKEND_SGTL5000)
// -- return mixers: only exist when there is a physical output to hear them on.
//    JT_BACKEND_USBONLY has no I2S sink, so the DAW return has nowhere to go and
//    these are omitted (the record path above still functions on USBONLY).
//    ch0 = synth (unity, fixed); ch1 = DAW return (gain polled from the store).
static AudioMixer4_F32         gRetMixL;
static AudioMixer4_F32         gRetMixR;
static AudioConnection_F32     cRetSynthL(gSynth,      0, gRetMixL, 0);
static AudioConnection_F32     cRetSynthR(gSynth,      1, gRetMixR, 0);
static AudioConnection_F32     cRetDawL  (gUsbInConvL, 0, gRetMixL, 1);
static AudioConnection_F32     cRetDawR  (gUsbInConvR, 0, gRetMixR, 1);

// -- physical output: fed by the return mixers (synth + DAW return), NOT by the
//    synth directly.  gUsbOut stays upstream of this sum, which is what keeps
//    the record path loop-free.
static AudioOutputI2S_F32      gI2sOut(audioSettings);
static AudioConnection_F32     cI2sL(gRetMixL, 0, gI2sOut, 0);
static AudioConnection_F32     cI2sR(gRetMixR, 0, gI2sOut, 1);
#endif
// -----------------------------------------------------------------------------
// MIDI handlers — thin routing only.
// -----------------------------------------------------------------------------
static void onNoteOn(byte /*ch*/, byte note, byte vel)
{
    gSynth.core().noteOn(note, vel);
}

static void onNoteOff(byte /*ch*/, byte note, byte /*vel*/)
{
    gSynth.core().noteOff(note);
}

// Pitch bend wheel (Phase 4).
//
// ⚠ THE RECURRING PITCH-BEND BUG — read before touching:  Teensy's usbMIDI
// (and USBHost_t36 setHandlePitchChange, and the FortySevenEffects lib) all
// deliver the wheel ALREADY CENTRED ON ZERO: value ∈ [-8192, +8191], with a
// RESTING wheel = 0.  v1's comments claimed "0..16383, centre 8192" — that was
// WRONG for every transport, and each time an AI trusted the comment it
// "corrected" the maths and broke a working wheel (resting wheel jumped by a
// whole bend range).  Do NOT reintroduce that.
//
// SynthCore::pitchBend takes the RAW unsigned form (0..16383, centre 8192) so
// its own maths is unambiguous; we convert the centred callback value with a
// single +8192 here.  Resting wheel: 0 + 8192 = 8192 → 0 semitones.  This is
// numerically identical to v1's (value/8192) applied to the centred value.
//
// Thin routing only — no Serial: bend streams fast (v1 Jteensy8000.cpp note).
// The +8192 conversion applies IDENTICALLY on every port: usbMIDI,
// USBHost_t36 and FortySevenEffects all deliver the centred form.
static void onPitchBend(byte /*ch*/, int value)
{
    gSynth.core().pitchBend((uint16_t)(value + 8192));
}

// Shared CC routing, parameterised by the receiving port's transport —
// the split (parameters vs standard meanings) is identical on all three.
static void onControlChangeFor(JT::MidiParamTransport& t, byte cc, byte value)
{
    // Parameter traffic (NRPN cluster + curated CCs) is consumed here...
    if (t.handleControlChange(cc, value)) return;

    // ...everything else has a standard MIDI meaning and its own owner.
    switch (cc) {
        case 64:  gSynth.core().sustain(value >= 64);   break;
        case 120: gSynth.core().allSoundOff();          break;   // panic
        case 123: gSynth.core().allNotesOff();          break;
        case 121: t.resetState();                       break;   // reset ctrls
        default: /* mod wheel etc: mod matrix, Phase 3 */ break;
    }
}

// Per-port trampolines — the MIDI libraries take bare function pointers
// with no user-data argument, so the port binding has to be spelled out.
static void onCCUsbDev (byte /*ch*/, byte cc, byte v) { onControlChangeFor(gTransportUsbDev,  cc, v); }
static void onCCUsbHost(byte /*ch*/, byte cc, byte v) { onControlChangeFor(gTransportUsbHost, cc, v); }
static void onCCSerial (byte /*ch*/, byte cc, byte v) { onControlChangeFor(gTransportSerial,  cc, v); }

// -----------------------------------------------------------------------------
// Outbound sinks — thin NrpnSink adapters over each port's raw sender.
// Channel 1 throughout: our editors ignore channel on NRPN, and a single
// fixed channel keeps DAW monitoring legible.
// -----------------------------------------------------------------------------
struct UsbDevSink : JT::NrpnSink {
    void sendCC(uint8_t cc, uint8_t v) override { usbMIDI.sendControlChange(cc, v, 1); }
};
struct UsbHostSink : JT::NrpnSink {
    // USBHost_t36 no-ops safely when no device is attached.
    void sendCC(uint8_t cc, uint8_t v) override { midiHost.sendControlChange(cc, v, 1); }
};
struct SerialSink : JT::NrpnSink {
    void sendCC(uint8_t cc, uint8_t v) override { midi1.sendControlChange(cc, v, 1); }
};
static UsbDevSink  gSinkUsbDev;
static UsbHostSink gSinkUsbHost;
static SerialSink  gSinkSerial;

// -----------------------------------------------------------------------------
void setup()
{
    Serial.begin(115200);

#if defined(JT_BACKEND_PCM5102)
    // Keep the DAC muted through clock start-up (XSMT active low).
    pinMode(JT::Board::kMutePin, OUTPUT);
    digitalWrite(JT::Board::kMutePin, LOW);
#endif

    // Two pools: F32 blocks for the synth path, a small int16 pool for the
    // stock USB output object.  Sizes per brief §10 — edges only.
    AudioMemory(16);   // +4: AudioInputUSB (int16 stereo) needs its own blocks
    AudioMemory_F32(20, audioSettings);

    // Return mixer init (Region D): synth at unity, DAW return at its stored
    // default.  Set once here; loop() tracks the return level thereafter.
#if defined(JT_BACKEND_PCM5102) || defined(JT_BACKEND_SGTL5000)
    gRetMixL.gain(0, 1.0f);  gRetMixR.gain(0, 1.0f);
    gRetMixL.gain(1, gStore.get(0x0801));
    gRetMixR.gain(1, gStore.get(0x0801));
#endif

#if defined(JT_BACKEND_SGTL5000)
    gCodec.enable();
    gCodec.volume(0.7f);
#endif
#if defined(JT_BACKEND_PCM5102)
    delay(50);                                   // clocks stable -> unmute
    digitalWrite(JT::Board::kMutePin, HIGH);
#endif

    // --- PSRAM guard (bench-earned, 2026-07-11) -----------------------------
    // EXTMEM symbols get a "valid" address whether or not a PSRAM chip is
    // detected; with none, begin()'s memset vanishes into unbacked bus space
    // and every FX/reverb wet read returns garbage — dry path intact,
    // delay/mod pure noise.  Detach the pools instead: silent FX and a loud
    // warning beat noise every time.  (external_psram_size is set by the
    // core's startup BEFORE C++ static ctors, so gSynth saw the pools first;
    // detaching here overrides that cleanly via the engines' null-pool mode.)
    if (external_psram_size == 0) {
        gSynth.core().disableExtmemPools();
        Serial.println("!! PSRAM NOT DETECTED — reverb + FX chain DISABLED.");
        Serial.println("!! Check chip fitting, or PIO core version vs Arduino");
        Serial.println("!! IDE (pio pkg update) — see psramMB in status line.");
    }

    // --- USB device port (DAW / JUCE editor) --------------------------------
    usbMIDI.setHandleNoteOn(onNoteOn);
    usbMIDI.setHandleNoteOff(onNoteOff);
    usbMIDI.setHandleControlChange(onCCUsbDev);
    usbMIDI.setHandlePitchChange(onPitchBend);

    // --- USB host port (controllers via hub) --------------------------------
    myusb.begin();
    midiHost.setHandleNoteOn(onNoteOn);
    midiHost.setHandleNoteOff(onNoteOff);
    midiHost.setHandleControlChange(onCCUsbHost);
    midiHost.setHandlePitchChange(onPitchBend);

    // --- Serial1 port (ESP32 controller, 1 Mbaud) ---------------------------
    midi1.begin(MIDI_CHANNEL_OMNI);
    midi1.turnThruOff();   // CRITICAL: soft-thru would re-send the ESP32's own
                           // traffic straight back to it, defeating the echo
                           // suppression this whole phase exists to provide
                           // (v1 did the same for its reason: Jteensy8000.cpp:551)
    midi1.setHandleNoteOn(onNoteOn);
    midi1.setHandleNoteOff(onNoteOff);
    midi1.setHandleControlChange(onCCSerial);
    midi1.setHandlePitchBend(onPitchBend);

    // --- outbound: each sink registered under its port's Origin -------------
    gBroadcast.addSink(gSinkUsbDev,  JT::Origin::MidiUsbDev);
    gBroadcast.addSink(gSinkUsbHost, JT::Origin::MidiUsbHost);
    gBroadcast.addSink(gSinkSerial,  JT::Origin::MidiSerial);

    Serial.println("[S3.1] JT-8000 v2 boot");
    Serial.print  ("[S3.2] backend: ");
    Serial.println(JT::Board::kBackendName);
    Serial.print  ("[S3.3] params: ");
    Serial.print  ((int)JT::Params::kParamCount);
    Serial.print  (", schema v");
    Serial.println((int)JT::Params::kSchemaVersion);
    Serial.println("[S3.4] audio graph up — send MIDI notes");
}

// -----------------------------------------------------------------------------
void loop()
{
    // Drain every inbound port; handlers above fire from these calls.
    usbMIDI.read();
    myusb.Task();        // USBHost housekeeping — required every pass ([R4])
    midiHost.read();
    midi1.read();        // 1 Mbaud: FIFO headroom is ~32x the v1 DIN case,
                         // but once per loop() remains the rule ([R5])

    // An editor asked for the full state (reserved NRPN, spec D4)?
    if (gTransportUsbDev.consumeResyncRequest()  ||
        gTransportUsbHost.consumeResyncRequest() ||
        gTransportSerial.consumeResyncRequest())
        gBroadcast.requestFullResync();

    // Mirror this pass's accepted changes out to the other ports (paced).
    gBroadcast.drain();

    // USB-return monitor level (Region C): poll the store and push into the
    // return mixer's ch1 gain.  Control-plane cadence (~1 kHz here), never the
    // audio ISR.  Unconditional store each pass is a single float write — cheaper
    // than a change-detect branch would be.  ch0 (synth) is fixed at unity.
#if defined(JT_BACKEND_PCM5102) || defined(JT_BACKEND_SGTL5000)
    {
        const float retLevel = gStore.get(0x0801);   // master.usb_return_level 0..1
        gRetMixL.gain(1, retLevel);
        gRetMixR.gain(1, retLevel);
    }
#endif

    // Status feed (Phase F5): voice-activity dots, SEQ playhead and run flag
    // for the controller. One compare when nothing changed; a 6-CC send when
    // something did (~20 Hz worst case at extreme sequencer rates). The 1 s
    // heartbeat makes an IDLE engine still prove the wire is alive — the
    // controller's LINK dot goes dark within ~1.5 s of a real cable fault.
    {
        static uint32_t lastStatusBeatMs = 0;
        const uint32_t nowMs = millis();
        if (nowMs - lastStatusBeatMs >= 1000) {
            lastStatusBeatMs = nowMs;
            gBroadcast.invalidateStatus();
        }
    }
    gBroadcast.sendStatusIfChanged(gSynth.core().statusWord());

    // 1 Hz status line — everything the bring-up guide asks you to watch.
    static uint32_t lastStatus = 0;
    const uint32_t now = millis();
    if (now - lastStatus >= 1000) {
        lastStatus = now;
        // CPU% of the audio budget: cycles / (cycles available per block).
        const float budget = (float)F_CPU * (128.0f / 44100.0f);
        const float pctMax = 100.0f * (float)gSynth.perfMaxCycles / budget;
        Serial.print("[S3.5] voices=");
        Serial.print((int)gSynth.core().activeVoices());
        Serial.print(" synthCPUmax=");
        Serial.print(pctMax, 1);
        Serial.print("% nrpnApplied=");
        Serial.print(gTransportUsbDev.appliedCount());
        Serial.print("/");
        Serial.print(gTransportUsbHost.appliedCount());
        Serial.print("/");
        Serial.print(gTransportSerial.appliedCount());   // dev/host/serial
        Serial.print(" nrpnUnknown=");
        Serial.print(gTransportUsbDev.unknownIdCount()
                     + gTransportUsbHost.unknownIdCount()
                     + gTransportSerial.unknownIdCount());
        Serial.print(" txSent=");
        Serial.print(gBroadcast.sentCount());
        // --- FX-noise triage probe (bench diagnostic, cheap enough to keep) --
        // psramMB: Teensy core's detected external RAM size in MB.  0 means
        // the EXTMEM pools (reverb/FX delay lines) point at UNBACKED bus
        // space — begin()'s memset vanished and every wet-path read returns
        // garbage: dry signal intact, delay/mod pure noise.
        // cpuMax: worst-case audio ISR load since the last line.  Pegged
        // near 100 only when FX engage = PSRAM access cost overrunning the
        // block budget instead.
        Serial.print(" psramMB=");
        Serial.print(external_psram_size);
        Serial.print(" cpuMax=");
        Serial.println(AudioProcessorUsageMax());
        AudioProcessorUsageMaxReset();
        gSynth.perfReset();
    }
}