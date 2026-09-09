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
//                      ├─► AudioConvert_F32toI16 ──► AudioOutputUSB
//                      └─► AudioSinkUsbHost_F32   (USB host port, 4a tap:
//                          actually fed from gRetMix, see below)
//
// MIDI ROUTING CONTRACT (brief §5.1 + Phase B' spec §3/§6):
//   THREE ports, each with its own MidiParamTransport instance (per-port
//   Origin = echo suppression identity):
//     usbMIDI   USB device — DAW / JUCE editor        Origin::MidiUsbDev
//     hostPort  USB host   — controllers/keyboards    Origin::MidiUsbHost
//     midi1     Serial1    — ESP32 controller, 1 Mbaud Origin::MidiSerial
//
//   USB HOST PORT, CHANGED: USBHost_t36 has been replaced by
//   platform/UsbHostPort (A-Dunstan's teensy4_usbhost).  The old library has
//   no isochronous transport, so it could carry MIDI from the host port but
//   never audio; both drive the same EHCI controller, so this is a swap and
//   not an addition.  Every handler the old port bound is bound here, and
//   ParamBroadcast still mirrors CCs outbound.  What is new: the attached
//   instrument's own speakers are now an output for the synth.
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
// USB host port.  NOT USBHost_t36 — see the routing contract above.

#include "core/ParameterStore.h"
#include "core/PsramArena.h"
#include "core/MidiParamTransport.h"
#include "core/PerfRouter.h"
#include "core/ParamBroadcast.h"
#include "platform/ExternalClock.h"
#include "platform/AudioSynthBlockF32.h"
#include "platform/BoardConfig.h"
// JT_USBHOST_AUDIO is defined in platform/UsbHostPort.h, which also decides
// whether the audio driver is built at all.  Override it from build_flags
// (-D JT_USBHOST_AUDIO=0) to bisect: MIDI keeps working, and nothing of the
// audio path is compiled, registered or executed.

#include "platform/IsrStack.h"
#include "platform/UsbHostPort.h"
#if defined(JT_BACKEND_PCM5102) || defined(JT_BACKEND_SGTL5000)
#if JT_USBHOST_AUDIO_BUILD
#include "platform/AudioSinkUsbHost_F32.h"
#endif
#endif

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
// The host stack, its two drivers and the resampler are file-scope statics
// inside UsbHostPort.cpp: the controller and every DMA buffer must live in
// OCRAM, and keeping that requirement in one translation unit means nothing
// here has to care.  JT::gUsbHostPort is the facade.
//
// Cables accepted from the host port.  The Studiologic NC2x mirrors one
// keypress onto cable 0 (its internal sound module) AND cable 1 (its MIDI
// mode output), often at different transpositions, so accepting both sounds
// every note twice.  Bit 1 alone takes the MIDI-mode stream.  Set to 0xFFFF
// for a controller that uses a single cable.
static constexpr uint16_t kHostCableMask = 0x0002u;

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
// ---- PSRAM: ONE region, carved by PsramArena ------------------------------
// Every PSRAM consumer used to declare its own EXTMEM array here.  That does
// not scale past two: each pool is resident whether or not its feature is on,
// a 16 MB board behaves exactly like an 8 MB one, and the only failure mode
// available is the all-or-nothing SynthCore::disableExtmemPools().  One region
// plus a bump allocator fixes all three — see core/PsramArena.h.
//
// SIZE (6.00 MB of the 8 MB minimum chip).  Current tenants:
//     reverb tank      39707 floats  =  0.15 MB   (PlateReverb::kPoolFloats)
//     FX delay + mod  886418 floats  =  3.38 MB   (FxChain::kPoolFloats, 10 s)
//     ------------------------------------------
//     carved           926136 floats =  3.53 MB   (after 32-byte alignment)
// The remaining ~2.5 MB is deliberate headroom for the next tenants (tape
// delay, multi-tap) so adding one does not mean re-sizing this array.  The
// 2 MB left outside the arena on an 8 MB board is reserve — raise
// kPsramArenaFloats before spending it, never allocate around the arena.
static constexpr size_t        kPsramArenaFloats = 1572864u;   // 6 MB / sizeof(float)
EXTMEM static float            gPsramRegion[kPsramArenaFloats];
static JT::PsramArena          gArena;

// Carving has to happen BEFORE gSynth's constructor runs, because gSynth takes
// the pools as constructor arguments.  Static initialisation inside one
// translation unit is strictly top-to-bottom, and external_psram_size is set by
// the core's startup BEFORE any C++ static constructor (the same fact the old
// PSRAM guard below relied on) — so this struct is a legal place to do it.
//
// ORDER MATTERS: reverb is requested first so that on a chip smaller than the
// arena expects, the small tenant survives and only the 3.38 MB one fails.
// Real hardware is 8 or 16 MB and cannot hit this, but the ordering costs
// nothing and makes the degradation sensible rather than arbitrary.
struct PsramPools {
    float* reverb = nullptr;
    float* fx     = nullptr;

    PsramPools()
    {
        const size_t availFloats =
            (size_t)external_psram_size * (1024u * 1024u / sizeof(float));
        const size_t cap = (availFloats < kPsramArenaFloats) ? availFloats
                                                             : kPsramArenaFloats;
        // No chip -> begin(nullptr, 0): every allocate() returns nullptr and
        // both engines take their documented inert path.  This REPLACES the
        // old detach-after-the-fact guard: the pools are never handed out in
        // the first place, so there is no window in which the ISR could touch
        // unbacked bus space.
        gArena.begin((external_psram_size > 0) ? gPsramRegion : nullptr, cap);

        reverb = gArena.allocate(JT::SynthCore::kReverbPoolFloats, "reverb");
        fx     = gArena.allocate(JT::SynthCore::kFxPoolFloats,     "fx");
    }
};
static PsramPools              gPools;
static JT::AudioSynthBlockF32  gSynth(gStore, gCombPool, gPools.reverb, gPools.fx);
// One transport per port; the Origin passed here IS the suppression identity
// ParamBroadcast keys on (Phase B' D1).  The router is what lets a curated CC
// (74 cutoff, ...) reach layer B — NRPN carries its layer in the address and
// does not consult it.
// The engine owns the one PerfRouter (it owns the store the router reads), so
// the transports and the note handlers cannot disagree about where a channel
// goes — two routers would be two answers to a question with one right one.
static JT::MidiParamTransport  gTransportUsbDev (gStore, JT::Origin::MidiUsbDev,  &gSynth.core().router());
static JT::MidiParamTransport  gTransportUsbHost(gStore, JT::Origin::MidiUsbHost, &gSynth.core().router());
static JT::MidiParamTransport  gTransportSerial (gStore, JT::Origin::MidiSerial,  &gSynth.core().router());

static JT::ParamBroadcast      gBroadcast(gStore);

// External MIDI clock (Phase 9): measures 24-PPQN into a BPM and forwards
// transport to the core.  micros() is injected so ExternalClock.h stays
// Arduino-free and host-testable; the bridge is the only Arduino coupling.
static uint32_t jtMicros() { return micros(); }
static JT::ExternalClock       gExtClock(gSynth.core(), jtMicros);

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

// -- USB host audio: the same signal the DAC gets (decision 4a), so an
//    attached instrument's speakers act as a monitor rather than carrying a
//    second and subtly different mix.  Costs one branch per block while
//    nothing is plugged in.  Not built on USBONLY: there are no return
//    mixers there to tap.
#if JT_USBHOST_AUDIO_BUILD
static JT::AudioSinkUsbHost_F32 gHostAudioOut;
static AudioConnection_F32     cHostL(gRetMixL, 0, gHostAudioOut, 0);
static AudioConnection_F32     cHostR(gRetMixR, 0, gHostAudioOut, 1);
#endif
#endif
// -----------------------------------------------------------------------------
// MIDI handlers — thin routing only.
// -----------------------------------------------------------------------------
// ===========================================================================
// NOTE-KILL DEBUG INSTRUMENTATION (temporary — remove once the ESP32 note
// cut-off bug is closed).  Set to 1 to trace WHICH event stops a held note
// and WHICH port it arrived on.  Changes no synth behaviour: the counters are
// bumped alongside the unchanged noteOn/noteOff/CC calls, and printed on the
// existing 1 Hz status line.
//
// WHAT WE ARE TRYING TO SEPARATE:
//   * a spurious NoteOff parsed on the Serial1 (ESP32) port — the signature of
//     UART byte-corruption at 1 Mbaud turning a benign byte into an 0x8n
//     status.  This is the prime suspect: midi1 has note handlers wired
//     (setup below), so a corrupted off KILLS a note that arrived on any
//     port.  Watch for offSerial climbing while you play.
//   * a channel-mode CC (120 allSoundOff / 123 allNotesOff) or a stuck
//     sustain (64) leaking through onControlChangeFor — watch killCc*.
//
// Read the 1 Hz line while slowly playing ONE key with the ESP32 on: the
// counter that ticks at the moment the note dies is the culprit.
// ===========================================================================
// ===========================================================================
// JT_DEBUG_NOTEKILL now lives in core/JtDebugFlags.h (included via the headers
// above) so MidiParamTransport.h and this file share one definition.  Set it
// to 0 THERE to compile out all bring-up instrumentation.
// ===========================================================================

#if JT_DEBUG_NOTEKILL
// Per-port note tallies.  A healthy single keypress is +1 on and +1 off on the
// SAME port; an extra off on a DIFFERENT port than the on is the smoking gun.
static volatile uint32_t gDbgOnUsbDev  = 0, gDbgOffUsbDev  = 0;
static volatile uint32_t gDbgOnUsbHost = 0, gDbgOffUsbHost = 0;
static volatile uint32_t gDbgOnSerial  = 0, gDbgOffSerial  = 0;
// Channel-mode kills, tagged by the port their CC arrived on (0=dev,1=host,2=ser).
static volatile uint32_t gDbgKillCc120[3] = {0,0,0};
static volatile uint32_t gDbgKillCc123[3] = {0,0,0};
static volatile uint32_t gDbgKillCc64Off[3] = {0,0,0};
// Raw Serial1 (ESP32 link) RX probe: bytes seen waiting vs messages parsed.
static volatile uint32_t gDbgSer1RawBytes = 0;
static volatile uint32_t gDbgSer1Msgs     = 0;
#endif

// Core note entry points — behaviour UNCHANGED.  Keeping one implementation
// each means the synth path is identical whether or not debug is on.
static inline void doNoteOn(byte ch, byte note, byte vel)
{
    gSynth.core().noteOn(note, vel, ch);
}

static inline void doNoteOff(byte ch, byte note)
{
    gSynth.core().noteOff(note, ch);
}

#if JT_DEBUG_NOTEKILL
// Port-tagged trampolines: used ONLY so the debug build can attribute a note
// event to the port it arrived on.  Each tags a counter and forwards to the
// unchanged core call — no logic difference.
static void onNoteOnUsbDev (byte ch, byte note, byte vel){ ++gDbgOnUsbDev;  doNoteOn(ch,note,vel); }
static void onNoteOnUsbHost(byte ch, byte note, byte vel){ ++gDbgOnUsbHost; doNoteOn(ch,note,vel); }
static void onNoteOnSerial (byte ch, byte note, byte vel){ ++gDbgOnSerial;  doNoteOn(ch,note,vel); }

static void onNoteOffUsbDev (byte ch, byte note, byte){ ++gDbgOffUsbDev;  doNoteOff(ch,note); }
static void onNoteOffUsbHost(byte ch, byte note, byte){ ++gDbgOffUsbHost; doNoteOff(ch,note); }
static void onNoteOffSerial (byte ch, byte note, byte){ ++gDbgOffSerial;  doNoteOff(ch,note); }
#endif

// Shared handlers, still used when debug is OFF (single wiring, no port tag).
static void onNoteOn(byte ch, byte note, byte vel)
{
    doNoteOn(ch, note, vel);
}

static void onNoteOff(byte ch, byte note, byte /*vel*/)
{
    doNoteOff(ch, note);
}

// Pitch bend wheel (Phase 4).
//
// ⚠ THE RECURRING PITCH-BEND BUG — read before touching:  Teensy's usbMIDI
// (and the FortySevenEffects lib) deliver the wheel ALREADY CENTRED ON ZERO: value ∈ [-8192, +8191], with a
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
// The +8192 conversion applies IDENTICALLY on every port.  usbMIDI and
// FortySevenEffects deliver the centred form natively; the USB host port
// carries the RAW form on the wire and UsbHostPort subtracts 8192 in its
// adapter so that this one convention still holds everywhere.
static void onPitchBend(byte /*ch*/, int value)
{
    gSynth.core().pitchBend((uint16_t)(value + 8192));
}

// Shared CC routing, parameterised by the receiving port's transport —
// the split (parameters vs standard meanings) is identical on all three.
// MIDI source port, passed to onControlChangeFor.  Used by the note-kill
// guard (Option C) in every build, and by the debug counters when enabled.
enum : uint8_t { kPortUsbDev = 0, kPortUsbHost = 1, kPortSerial = 2 };

static void onControlChangeFor(JT::MidiParamTransport& t, byte ch, byte cc, byte value,
                               uint8_t port)
{
    // Parameter traffic (NRPN cluster + curated CCs) is consumed here.  The
    // channel matters only to the curated-CC branch: NRPN carries its layer
    // in the address instead, so an editor can keep using any channel.
    if (t.handleControlChange(cc, value, ch)) return;

    // The Serial port is the ESP32 control surface — never a keyboard.  It has
    // no business commanding all-notes-off / all-sound-off, yet a DAW patched
    // through the ESP32's USB relay used to stream CC 123 down this link and
    // panic held notes ~2x/sec.  The ESP32 side no longer forwards these
    // (Option A), but this end refuses them too so no future relay, thru, or
    // stray byte on the control link can silence the voices.  Kept narrow:
    // only the two voice-killers, and only on Serial — 120/123 from a real
    // keyboard on the USB ports still work.
    if (port == kPortSerial && (cc == 120 || cc == 123))
        return;

    // ...everything else has a standard MIDI meaning and its own owner.
    switch (cc) {
        case 1:   gSynth.core().modWheel(value, ch);    break;   // vibrato
        case 64:  gSynth.core().sustain(value >= 64, ch); break;
        case 120: gSynth.core().allSoundOff();          break;   // panic
        case 123: gSynth.core().allNotesOff();          break;
        case 121: t.resetState();                       break;   // reset ctrls
        default: /* unassigned: mod matrix will claim these */ break;
    }

#if JT_DEBUG_NOTEKILL
    // Tag channel-mode kills that reached here.  The Option C guard above
    // returns first for Serial 120/123, so after this fix cc123(s) stays FLAT
    // — that flat counter is the confirmation the relay is no longer panicking
    // the voices.  USB-port 120/123 and any sustain-release (64 falling) still
    // count, so a genuine keyboard panic is still visible.
    if (port < 3) {
        if      (cc == 120) ++gDbgKillCc120[port];
        else if (cc == 123) ++gDbgKillCc123[port];
        else if (cc == 64 && value < 64) ++gDbgKillCc64Off[port];
    }
#endif
}

// Per-port trampolines — the MIDI libraries take bare function pointers
// with no user-data argument, so the port binding has to be spelled out.
static void onCCUsbDev (byte ch, byte cc, byte v) { onControlChangeFor(gTransportUsbDev,  ch, cc, v, kPortUsbDev);  }
static void onCCUsbHost(byte ch, byte cc, byte v) { onControlChangeFor(gTransportUsbHost, ch, cc, v, kPortUsbHost); }

#if JT_DEBUG_NOTEKILL
// Per-CC Serial NRPN trace (fault-2 hunt).  Wraps the Serial CC handler: reads
// the applied/unknown counters before and after, and prints what the assembler
// did with THIS byte plus its state afterwards.  Reading the trace of one knob
// turn tells us exactly where a cluster breaks:
//   * a clean edit is  99(sel=1) 98(sel=1) 06 ->APPLIED  38 ->APPLIED
//   * CC6 with sel=0/None or addr=127/127 -> "swallow" and neither counter
//     moves == the "ser1msg climbs, nrpnApplied flat" signature.
//   * CC99/98 never appearing before CC6 -> the address never gets set ->
//     every data byte is a null-selection swallow.
// Rate-limited to the first N events after boot so a knob turn is legible and
// the console is not flooded; bump kTraceMax if you need a longer window.
static uint32_t gDbgTraceCount = 0;
static void onCCSerial(byte ch, byte cc, byte v)
{
    constexpr uint32_t kTraceMax = 80;
    const uint32_t a0 = gTransportSerial.appliedCount();
    const uint32_t u0 = gTransportSerial.unknownIdCount();

    onControlChangeFor(gTransportSerial, ch, cc, v, kPortSerial);

    if (gDbgTraceCount < kTraceMax) {
        ++gDbgTraceCount;
        const uint32_t da = gTransportSerial.appliedCount()   - a0;
        const uint32_t du = gTransportSerial.unknownIdCount() - u0;
        Serial.print("[CCTRACE] cc=");   Serial.print(cc);
        Serial.print(" v=");             Serial.print(v);
        Serial.print(" sel=");           Serial.print(gTransportSerial.dbgSelected());
        Serial.print(" addr=");          Serial.print(gTransportSerial.dbgNrpnMsb());
        Serial.print('/');               Serial.print(gTransportSerial.dbgNrpnLsb());
        Serial.print(" dV=");            Serial.print(gTransportSerial.dbgDataMsbValid() ? 1 : 0);
        if      (da) Serial.print(" ->APPLIED");
        else if (du) Serial.print(" ->UNKNOWN");
        else         Serial.print(" ->swallow");
        Serial.println();
    }
}
#else
static void onCCSerial (byte ch, byte cc, byte v) { onControlChangeFor(gTransportSerial,  ch, cc, v, kPortSerial); }
#endif

// -----------------------------------------------------------------------------
// External MIDI clock — real-time byte handlers (Phase 9).
//
// The three libraries expose DIFFERENT real-time APIs, so the bindings differ:
//   * usbMIDI (Teensy core) and FortySevenEffects (midi1): no-arg per-message
//     handlers — setHandleClock/Start/Stop/Continue — bound to these trampolines.
//   * UsbHostPort (host port): ONE setHandleRealTimeSystem(uint8_t) that receives
//     the raw status byte — routed through gExtClock.onRealtimeByte(), which
//     decodes 0xF8/0xFA/0xFB/0xFC itself.
// All ports share the one gExtClock (transport + tempo are global).  These fire
// in loop() polling context (same as the note handlers), so calling into the
// core's lock-free producers is the identical, safe context the note path uses.
static void onClockPulse() { gExtClock.onClockPulse(); }
static void onClockStart() { gExtClock.onStart();      }
static void onClockStop()  { gExtClock.onStop();       }
static void onClockCont()  { gExtClock.onContinue();   }
// Raw-byte adapter, used by the USB host port.
static void onHostRealtime(uint8_t status) { gExtClock.onRealtimeByte(status); }

// -----------------------------------------------------------------------------
// Outbound sinks — thin NrpnSink adapters over each port's raw sender.
// Channel 1 throughout: our editors ignore channel on NRPN, and a single
// fixed channel keeps DAW monitoring legible.
// -----------------------------------------------------------------------------
struct UsbDevSink : JT::NrpnSink {
    void sendCC(uint8_t cc, uint8_t v) override { usbMIDI.sendControlChange(cc, v, 1); }
};
// NOTE: there is deliberately NO UsbHostSink.
//
// The old firmware registered one, so ParamBroadcast mirrored every parameter
// edit AND the periodic status feed (0x3FFF voice/step word, 0x3FFE arp
// playhead) out of the USB host port.  That made sense when the port might
// hold an editor.  It holds a keyboard, which has no use for telemetry, and
// the cost was two NRPNs per second — twelve control changes — transmitted
// forever at a device that ignores them.
//
// It also broke the port: with bulk OUT traffic running, bulk IN transfers on
// the same device stopped completing altogether, so the keyboard's own notes
// never arrived.  Removing the sink removes the traffic and the fault with it.
//
// UsbHostPort::sendControlChange still exists and still works.  To mirror
// parameters to a USB-host controller in future, add a sink here and register
// it below; nothing else has to change.
struct SerialSink : JT::NrpnSink {
    void sendCC(uint8_t cc, uint8_t v) override { midi1.sendControlChange(cc, v, 1); }
};
static UsbDevSink  gSinkUsbDev;
static SerialSink  gSinkSerial;

// Host-port forwarding (routing decision 6a).  Everything that passes the
// cable filter is re-sent verbatim on the USB device port, so a DAW sees the
// keyboard exactly as it played — both parts, on their own channels, as
// separate tracks.  Deliberately verbatim: no channel remap, no filtering.
//
// This cannot loop.  Traffic only ever moves host -> device here; nothing
// forwards the device port back to the host.
static void onHostForward(uint8_t /*cable*/, const uint8_t* data, uint8_t length)
{
    if (length == 0u) { return; }

    const uint8_t status  = data[0];
    const uint8_t type    = static_cast<uint8_t>(status & 0xF0u);
    const uint8_t channel = static_cast<uint8_t>((status & 0x0Fu) + 1u);

    switch (type) {
        case 0x80u: usbMIDI.sendNoteOff(data[1], data[2], channel);        break;
        case 0x90u: usbMIDI.sendNoteOn(data[1], data[2], channel);         break;
        case 0xA0u: usbMIDI.sendAfterTouchPoly(data[1], data[2], channel); break;
        case 0xB0u: usbMIDI.sendControlChange(data[1], data[2], channel);  break;
        case 0xC0u: usbMIDI.sendProgramChange(data[1], channel);           break;
        case 0xD0u: usbMIDI.sendAfterTouch(data[1], channel);              break;
        case 0xE0u:
            // usbMIDI.sendPitchBend takes the CENTRED form; the wire carries
            // the raw 14-bit.  Same conversion, same reason, as everywhere
            // else in this file.
            usbMIDI.sendPitchBend(
                (static_cast<int>(data[1]) |
                 (static_cast<int>(data[2]) << 7)) - 8192, channel);
            break;
        default:
            // Real-time and system common are not forwarded: the DAW has its
            // own transport, and echoing clock into it causes more trouble
            // than it solves.
            break;
    }
}

// -----------------------------------------------------------------------------
void setup()
{
    // FIRST STATEMENT, and it must stay first.  TeensyAtomThreads points MSP
    // at a 2 KB buffer during startup, and on Cortex-M every exception runs on
    // MSP — including the software interrupt this synth renders from.  This
    // installs a larger stack in its place.
    //
    // ORDERING MATTERS AND IS EASY TO GET WRONG: it must run before the audio
    // graph is started and before gUsbHostPort.begin(), because that starts the
    // host thread and enumeration begins immediately on it.  Repointing MSP
    // underneath a running USB host stack breaks enumeration.
    JT::gIsrStack.install();

    Serial.begin(115200);

#if defined(JT_BACKEND_PCM5102)
    // Keep the DAC muted through clock start-up (XSMT active low).
    pinMode(JT::Board::kMutePin, OUTPUT);
    digitalWrite(JT::Board::kMutePin, LOW);
#endif

    // Two pools: F32 blocks for the synth path, a small int16 pool for the
    // stock USB output object.  Sizes per brief §10 — edges only.
    AudioMemory(16);   // +4: AudioInputUSB (int16 stereo) needs its own blocks
    // F32 pool.  Back to the original 20 after measurement: peak usage with
    // the USB host sink connected and a chord held was well inside it.  It was
    // briefly raised to 28 while chasing a suspected block shortage; that
    // turned out not to be the fault, and the headroom was never justified.
    // Watch f32Blk on the status line if the graph grows.
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

    // --- PSRAM arena report (bench-earned guard, 2026-07-11; arena 2026-09) --
    // EXTMEM symbols get a "valid" address whether or not a PSRAM chip is
    // detected; with none, begin()'s memset vanishes into unbacked bus space
    // and every FX/reverb wet read returns garbage — dry path intact,
    // delay/mod pure noise.  The ARENA now prevents that at the source: with
    // no chip it holds no region, every allocate() returned nullptr during
    // static init, and both engines are already inert.  Nothing to detach
    // here any more — this block only REPORTS what happened, which is why the
    // per-pool lines matter: a partial failure is now possible and visible.
    if (!gArena.ok()) {
        Serial.println("!! PSRAM NOT DETECTED — reverb + FX chain DISABLED.");
        Serial.println("!! Check chip fitting, or PIO core version vs Arduino");
        Serial.println("!! IDE (pio pkg update) — see psramMB in status line.");
    } else {
        Serial.print("[S3.0] PSRAM arena ");
        Serial.print((unsigned long)(gArena.used() * sizeof(float) / 1024u));
        Serial.print(" KB used / ");
        Serial.print((unsigned long)(gArena.capacity() * sizeof(float) / 1024u));
        Serial.println(" KB");
        for (uint8_t i = 0; i < gArena.recordCount(); ++i) {
            Serial.print("       pool ");
            Serial.print(gArena.recordTag(i));
            Serial.print(" = ");
            Serial.print((unsigned long)(gArena.recordSize(i) * sizeof(float) / 1024u));
            Serial.println(" KB");
        }
        // Non-zero means a tenant asked for more than was left: that feature is
        // silently inert, so say so rather than let it look like a DSP fault.
        if (gArena.failedCount() > 0) {
            Serial.print("!! PSRAM arena EXHAUSTED — ");
            Serial.print((int)gArena.failedCount());
            Serial.println(" pool(s) unallocated and INERT.");
        }
    }

    // --- USB device port (DAW / JUCE editor) --------------------------------
#if JT_DEBUG_NOTEKILL
    usbMIDI.setHandleNoteOn(onNoteOnUsbDev);
    usbMIDI.setHandleNoteOff(onNoteOffUsbDev);
#else
    usbMIDI.setHandleNoteOn(onNoteOn);
    usbMIDI.setHandleNoteOff(onNoteOff);
#endif
    usbMIDI.setHandleControlChange(onCCUsbDev);
    usbMIDI.setHandlePitchChange(onPitchBend);
    // External clock (Phase 9): no-arg real-time handlers.
    usbMIDI.setHandleClock(onClockPulse);
    usbMIDI.setHandleStart(onClockStart);
    usbMIDI.setHandleContinue(onClockCont);
    usbMIDI.setHandleStop(onClockStop);

    // --- USB host port (keyboards and controllers) --------------------------
    // Handlers are bound BEFORE begin(): the host stack enumerates on its own
    // thread and a device can attach before setup() returns.
#if JT_DEBUG_NOTEKILL
    JT::gUsbHostPort.setHandleNoteOn(onNoteOnUsbHost);
    JT::gUsbHostPort.setHandleNoteOff(onNoteOffUsbHost);
#else
    JT::gUsbHostPort.setHandleNoteOn(onNoteOn);
    JT::gUsbHostPort.setHandleNoteOff(onNoteOff);
#endif
    JT::gUsbHostPort.setHandleControlChange(onCCUsbHost);
    // Delivered CENTRED, exactly as every other port delivers it, so
    // onPitchBend needs no special case.  The adapter does the conversion
    // from the wire's raw form; see UsbHostPort.h.
    JT::gUsbHostPort.setHandlePitchChange(onPitchBend);
    // External clock (Phase 9): one raw real-time byte, same shape as the
    // old adapter, so onHostRealtime is unchanged.
    JT::gUsbHostPort.setHandleRealTimeSystem(onHostRealtime);
    JT::gUsbHostPort.setHandleForward(onHostForward);
    JT::gUsbHostPort.setCableMask(kHostCableMask);
    JT::gUsbHostPort.begin();

    // --- Serial1 port (ESP32 controller, 1 Mbaud) ---------------------------
    midi1.begin(MIDI_CHANNEL_OMNI);
    midi1.turnThruOff();   // CRITICAL: soft-thru would re-send the ESP32's own
                           // traffic straight back to it, defeating the echo
                           // suppression this whole phase exists to provide
                           // (v1 did the same for its reason: Jteensy8000.cpp:551)
#if JT_DEBUG_NOTEKILL
    midi1.setHandleNoteOn(onNoteOnSerial);
    midi1.setHandleNoteOff(onNoteOffSerial);
#else
    midi1.setHandleNoteOn(onNoteOn);
    midi1.setHandleNoteOff(onNoteOff);
#endif
    midi1.setHandleControlChange(onCCSerial);
    midi1.setHandlePitchBend(onPitchBend);
    // External clock (Phase 9): FortySevenEffects no-arg real-time handlers.
    midi1.setHandleClock(onClockPulse);
    midi1.setHandleStart(onClockStart);
    midi1.setHandleContinue(onClockCont);
    midi1.setHandleStop(onClockStop);

    // --- outbound: each sink registered under its port's Origin -------------
    gBroadcast.addSink(gSinkUsbDev,  JT::Origin::MidiUsbDev);
    // No host-port sink: see the note above UsbDevSink.  Origin::MidiUsbHost
    // is still a valid origin for INBOUND edits, and gTransportUsbHost still
    // resyncs; only the outbound mirror is gone.
    gBroadcast.addSink(gSinkSerial,  JT::Origin::MidiSerial);

    // Teensy 4.x keeps fault state across a reboot.  If the last run ended in
    // a hard fault this prints the faulting address, the fault type and the
    // stack — the difference between diagnosing a crash and guessing at one.
    // Prints nothing after a clean start or a fresh upload.
    if (CrashReport) {
        Serial.println("[S3.0] previous run ended in a fault:");
        Serial.print(CrashReport);
    }

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
    // Drain every inbound port to EMPTY each pass; handlers above fire from
    // these calls.
    //
    // WHY A while-DRAIN, NOT ONE read() PER PORT (regression fix, Aug 2026):
    //   The old code read exactly one message per port per loop().  That kept
    //   up only while loop() spun fast.  With the ESP32 controller live, two
    //   things arrive at once: NRPN clusters stream in on Serial1, AND
    //   gBroadcast.drain() (below) mirrors ESP32-originated edits back OUT the
    //   usbMIDI device port — the same port the keyboard plays through.  A
    //   single usbMIDI.read() then serviced one inbound packet per loop while
    //   the port was also busy transmitting mirror CCs, so note-on/off packets
    //   queued and were parsed a full cycle late: notes arrived cut short, and
    //   once the endpoint buffer overflowed, bytes were lost mid-message and
    //   whole notes dropped.  Draining to empty clears a burst in the pass it
    //   arrives, so playing is never held behind parameter traffic.
    //
    // WHY THE CAP:  an unbounded while would let one flooding port (e.g. the
    //   ESP32 mid-resync dumping ~140 params) spin here and starve the other
    //   ports plus the status/return housekeeping further down.  The cap
    //   bounds worst-case loop() latency — the intent of the old
    //   "once per loop" rule ([R5]) — while giving enough headroom to clear a
    //   normal note+param burst in a single pass.  32 messages ≈ five 6-CC
    //   NRPN clusters plus notes; at 1 Mbaud that drains in well under the
    //   audio block period, so the DSP ISR is never at risk.
    static constexpr int kMaxMidiDrain = 32;

    for (int i = 0; i < kMaxMidiDrain && usbMIDI.read();  ++i) { }

    // Host port: drains inbound to empty under the same cap as the other
    // ports, dispatches to the handlers in this same loop() context, and
    // starts any queued outbound transfer.  Replaces both myusb.Task() and
    // the midiHost.read() drain — the library services itself on its own
    // thread, so there is no housekeeping call to make here.
    JT::gUsbHostPort.poll(kMaxMidiDrain);

#if JT_DEBUG_NOTEKILL
    // ONE-SHOT raw byte capture.  Before the MIDI parser ever touches Serial1,
    // grab the first bytes verbatim and print them as hex, ONCE.  This shows
    // the literal bytes on the ESP32 link so we can read the framing directly:
    //   * clean NRPN clusters look like  B0 63 xx  B0 62 xx  B0 06 xx  B0 26 xx
    //     B0 65 7F  B0 64 7F   (status B0 = CC ch1; 63/62=NRPN MSB/LSB;
    //     06/26=data; 65/64=RPN park).
    //   * garbage / half-speed framing shows random-looking bytes with no B0
    //     structure -> baud or level problem on the ESP32's TX.
    //   * a repeating 1-byte value (e.g. FE FE FE) -> active sensing / stuck.
    // After the dump we STOP intercepting and hand the port to the parser for
    // good, so normal operation is unaffected.
    {
        static bool  s_capDone = false;
        static uint8_t s_cap[48];
        static uint8_t s_capN = 0;
        if (!s_capDone) {
            while (s_capN < sizeof(s_cap) && Serial1.available()) {
                s_cap[s_capN++] = (uint8_t)Serial1.read();
            }
            if (s_capN >= sizeof(s_cap)) {
                Serial.print("[SER1RAW]");
                for (uint8_t i = 0; i < s_capN; ++i) {
                    Serial.print(' ');
                    if (s_cap[i] < 0x10) { Serial.print('0'); }
                    Serial.print(s_cap[i], HEX);
                }
                Serial.println();
                s_capDone = true;
            }
        }
    }

    // RAW Serial1 RX probe.  midi1.read() consumes the bytes, so we cannot read
    // Serial1 ourselves without stealing them.  Instead PEEK the count waiting
    // in the RX buffer just before draining (non-consuming), and separately
    // count complete messages the parser returns.  Comparing the two localises
    // the ESP32->Teensy failure:
    //   rawRx climbs, msgs flat -> bytes ARRIVE at the pin but the parser
    //     rejects them -> framing / baud / status problem (software/UART).
    //   rawRx flat              -> nothing reaches the UART buffer despite the
    //     ESP32 comms LED -> Teensy RX1 pin / Serial1 not actually receiving
    //     (wrong pins, pinMode conflict, level issue on THIS pin only).
    //   both climb, nrpnApplied flat -> parse OK, apply fails -> store/ID path.
    gDbgSer1RawBytes += (uint32_t)Serial1.available();
    for (int i = 0; i < kMaxMidiDrain && midi1.read(); ++i) { ++gDbgSer1Msgs; }
#else
    for (int i = 0; i < kMaxMidiDrain && midi1.read();    ++i) { }
#endif

    // An editor asked for the full state (reserved NRPN, spec D4)?
    // Each port is consumed separately, not short-circuited: || would skip the
    // later transports' flags and leave a second editor's request pending for
    // a whole poll cycle.  The layer comes from whichever port asked — an
    // editor showing layer B gets layer B's values.
    if (gTransportUsbDev.consumeResyncRequest())
        gBroadcast.requestFullResync(gTransportUsbDev.resyncLayer());
    if (gTransportUsbHost.consumeResyncRequest())
        gBroadcast.requestFullResync(gTransportUsbHost.resyncLayer());
    if (gTransportSerial.consumeResyncRequest())
        gBroadcast.requestFullResync(gTransportSerial.resyncLayer());

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

    // Arp playhead, on its own reserved address (0x3FFE). The sequencer
    // word above is already 13 of its 14 bits, so the arp could not be
    // packed alongside it — without this second feed the controller drew
    // the SEQUENCER's playhead on the arp lane, or none at all whenever
    // the sequencer was stopped. Same change-only + 1 s heartbeat rules:
    // invalidateStatus() above clears both words together.
    gBroadcast.sendArpStatusIfChanged(gSynth.core().arpStatusWord());

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
        Serial.print(AudioProcessorUsageMax());

        // F32 block pool: 'now' and 'worst since boot'.  If the worst reaches
        // the pool size the graph has been starved, which shows up as silence
        // on every output and a CPU figure near zero — not as an error.
        Serial.print(" f32Blk=");
        Serial.print(AudioMemoryUsage_F32());
        Serial.print("/");
        Serial.print(AudioMemoryUsageMax_F32());

        // Deepest interrupt-stack excursion since boot, against the stack we
        // installed.  Reaching capacity means the pattern is gone entirely and
        // the stack has overflowed: expect a hard fault at a nonsense address.
        if (JT::gIsrStack.valid()) {
            Serial.print(" isrStk=");
            Serial.print(JT::gIsrStack.used());
            Serial.print("/");
            Serial.print(JT::gIsrStack.capacity());
        }

        // USB host port: silent until something is attached.  Read fill and
        // ratio together — a fill that walks away from ~512 while the ratio
        // sits at a clamp means the servo has saturated, and ur/or rising
        // means it lost the race.
        JT::gUsbHostPort.printStatus();

        // --- Phase 9 external-clock sync triage ------------------------------
        // Flip JT_DEBUG_CLOCK on to walk the chain in one glance:
        //   clkPulses rising  -> 0xF8 bytes ARE reaching the handler
        //   extBpm  non-zero   -> measurement produced a tempo (needs >=2 beats)
        //   srcExt=1           -> the drainExternalClock gate is OPEN
        //   bpm follows extBpm -> the shared clock actually moved
        //   arpRate>=1         -> arp is on a synced division (0=kFree ignores tempo,
        //                         -1 = arp disabled)
        // The usual culprit: srcExt=0 (clock source still Internal) — set
        // CLOCK_CLOCK_SOURCE to External from any editor and it should flip to 1.
#ifdef JT_DEBUG_CLOCK
        Serial.print(" | clkPulses=");
        Serial.print(gExtClock.debugPulseCount());
        Serial.print(" extBpm=");
        Serial.print(gExtClock.debugLastBpm(), 1);
        Serial.print(" run=");
        Serial.print(gExtClock.debugRunning() ? 1 : 0);
        Serial.print(" srcExt=");
        Serial.print(gSynth.core().debugClockSourceExternal() ? 1 : 0);
        Serial.print(" bpm=");
        Serial.print(gSynth.core().debugClockBpm(), 1);
        Serial.print(" arpRate=");
        Serial.print(gSynth.core().debugArpRateMode());
#endif

        // --- filter drive triage (walk the chain left to right) -------------
        // Turn JT_DEBUG_DRIVE on and turn the drive knob. Read the line in
        // this order; the FIRST thing that does not move is the fault:
        //   nrpnApplied rising / nrpnUnknown flat (above) -> the NRPN arrived
        //     and the firmware recognised 0x018B.  If nrpnUnknown is the one
        //     rising instead, the flashed ParamTable.h predates filter.drive:
        //     reflash the Teensy, not just the ESP32.
        //   drvStore -> the value the ParameterStore holds, in engineering
        //     units (1.000 .. 4.000).  Flat while the panel moves means the
        //     ESP32 is displaying a value it never actually transmitted.
        //   drv -> what FilterSection ACTUALLY holds. Store moving but this
        //     flat means applyParam never reached the voices.
        //   drvOn -> 0 while drv > 1.0 would mean setDrive's neutral test is
        //     wrong; it should be impossible and is printed to prove it.
        //   va -> 0 means the OBXa engine is selected and drive is INERT BY
        //     DESIGN. This is the expected answer when everything else looks
        //     right and nothing is audible.
        //   vaType -> 8 (Diode) and 4 (SVF AP) are excluded from the
        //     saturator, so drive there changes level and not timbre. 5
        //     (Moog LP4) is the loudest test case at roughly +9 dB.
        // Any probe reading -1 means layer A currently owns no voices.
#ifdef JT_DEBUG_DRIVE
        Serial.print(" | drvStore=");
        Serial.print(gStore.getEngineering(JT::Params::ID::FILTER_DRIVE), 3);
        Serial.print(" drv=");
        Serial.print(gSynth.core().debugFilterDrive(), 3);
        Serial.print(" drvOn=");
        Serial.print(gSynth.core().debugFilterDriveActive() ? 1 : 0);
        Serial.print(" va=");
        Serial.print(gSynth.core().debugFilterEngineIsVa() ? 1 : 0);
        Serial.print(" vaType=");
        Serial.print(gSynth.core().debugFilterVaType());
#endif
        Serial.println();

#if JT_DEBUG_NOTEKILL
        // Note-kill trace.  on/off per port (dev/host/ser); a single clean
        // keypress on the DEVICE port is on(dev) +1, off(dev) +1 with off(ser)
        // and off(host) FLAT.  If off(ser) climbs while you hold a note, the
        // ESP32 link is injecting spurious NoteOffs (UART corruption at 1
        // Mbaud).  If killCc123/120/64off climb, a channel-mode CC is leaking
        // through the NRPN parser.  Whichever moves at the instant the note
        // dies is the cause.
        Serial.print("[NKILL] on(d/h/s)=");
        Serial.print(gDbgOnUsbDev);  Serial.print('/');
        Serial.print(gDbgOnUsbHost); Serial.print('/');
        Serial.print(gDbgOnSerial);
        Serial.print(" off(d/h/s)=");
        Serial.print(gDbgOffUsbDev);  Serial.print('/');
        Serial.print(gDbgOffUsbHost); Serial.print('/');
        Serial.print(gDbgOffSerial);
        Serial.print(" cc123(d/h/s)=");
        Serial.print(gDbgKillCc123[0]); Serial.print('/');
        Serial.print(gDbgKillCc123[1]); Serial.print('/');
        Serial.print(gDbgKillCc123[2]);
        Serial.print(" cc120(d/h/s)=");
        Serial.print(gDbgKillCc120[0]); Serial.print('/');
        Serial.print(gDbgKillCc120[1]); Serial.print('/');
        Serial.print(gDbgKillCc120[2]);
        Serial.print(" cc64off(d/h/s)=");
        Serial.print(gDbgKillCc64Off[0]); Serial.print('/');
        Serial.print(gDbgKillCc64Off[1]); Serial.print('/');
        Serial.print(gDbgKillCc64Off[2]);
        // ser1rx = raw bytes seen in the Serial1 RX buffer (ESP32->Teensy);
        // ser1msg = complete MIDI messages the parser returned from that port.
        Serial.print(" ser1rx=");
        Serial.print(gDbgSer1RawBytes);
        Serial.print(" ser1msg=");
        Serial.print(gDbgSer1Msgs);
        Serial.println();
#endif

        AudioProcessorUsageMaxReset();
        gSynth.perfReset();
    }
}