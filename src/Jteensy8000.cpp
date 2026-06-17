/* 
 * Copyright (c) 2025, Kris Bishop, bishopkris40@hotmail.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
 /**
 * Jteensy8000.ino — JT-8000 polyphonic synthesizer  (v7 — dual-layer)
 *
 * Audio path:
 *   LayerManager
 *     ├─ Layer A: SynthEngine (voices 0..N) → FXChainBlock ─┐
 *     └─ Layer B: SynthEngine (voices N..7) → FXChainBlock ─┤
 *                                                            ▼
 *     Performance Mixer (crossfade) → mixerI2S{L/R} → I2S → PCM5102A
 *                                   → scopeTap      (waveform capture)
 *                                   → ampUSB{L/R}   → usbOut (DAW monitor)
 *
 *   In SINGLE mode (default at boot), only Layer A is active with all 8
 *   voices — identical behaviour to the pre-layer v6 architecture.
 *
 * MIDI sources (all share the same handlers):
 *   1. usbMIDI      — USB device MIDI (PC/Mac DAW host)
 *   2. midiHost     — USB Host MIDI   (keyboard plugged into Teensy USB-Host port)
 *   3. Serial1      — DIN-5 hardware MIDI (31250 baud)
 *
 * CRITICAL RULES (hard-won lessons):
 *
 * [R1] UI must NEVER block the main loop long enough to starve MIDI reads.
 *      MicroDexed solved this by rate-limiting ALL display work to a short
 *      time-slice per loop() iteration.  We do the same via UIManager_TFT's
 *      internal FRAME_MS gate (33 ms = ~30 fps) and by keeping each SPI
 *      operation bounded.  fillScreen() is BANNED inside draw() hot-paths.
 *
 * [R2] MIDI handlers are called from xxx.read() inside loop() — they run on
 *      the main core, NOT in an ISR.  It is safe to call synth.noteOn/Off()
 *      from them because SynthEngine modifies voice state that the audio ISR
 *      reads; all shared state uses AudioNoInterrupts() guards inside the
 *      engine.  Do NOT call Serial.print* from handlers (USB-serial TX flood).
 *
 * [R3] Serial.print* in MIDI handlers was the original note-dropping culprit
 *      in MicroDexed (and still kills performance).  All serial logging below
 *      uses a rate-limited queue: MIDI_LOG() macro, printed in loop().
 *
 * [R4] USBHost_t36 midiHost requires myusb.Task() and midiHost.read() every
 *      loop() iteration — no rate-limiting.
 *
 * [R5] DIN MIDI (Serial1) must call midi1.read() every loop() too.  The
 *      Serial1 FIFO holds ~16 bytes at 31250 baud so missing even one frame
 *      (33 ms) loses a byte at 100 notes/sec.
 *
 * PCM5102A XSMT pin:
 *   Must be driven HIGH after I2S starts or the DAC stays hardware-muted.
 *   Wire XSMT → Teensy pin 34.
 *
 * Encoder pins (28-32) must NOT use attachInterrupt().
 *   GPIO6/7 ICR register overflow → memory corruption → crash.
 *   HardwareInterface_MicroDexed uses polled Gray-code decode instead.
 */

#include "Audio.h"
#include <Wire.h>
#include <MIDI.h>              // Teensy/FortySevenEffects MIDI Library
#include <usb_midi.h>
#include <USBHost_t36.h>
#include "SynthEngine.h"
#include "LayerManager.h"

//#include "UIPageLayout.h"
#include "HardwareInterface_MicroDexed.h"
#include "UIManager_TFT.h"
#include "Presets.h"
//#include "AudioScopeTap.h"
#include "BPMClockManager.h"
#include "SysExAdapter.h"       // Phase 1 — SysEx receive path for editor protocol
#include "SyxProtocol.h"       // SyxProto:: layer IDs used in onCCHandled echo
#include "ParamDefs.h"            // CC:: reverb constants used in onCCHandled scope check

// ---------------------------------------------------------------------------
// Teensy 4.x core symbols used for boot diagnostics.
//
// external_psram_size — set by Teensyduino startup.c when PSRAM chips are
//   detected on the QSPI pads.  0 = no PSRAM, 8 = one 8 MB chip, 16 = two
//   8 MB chips.  Marked weak in case a future Teensyduino drops it; if so
//   the address is null and the boot diagnostic just prints "0 MB".
//
// _heap_start / _heap_end / __brkval — linker / runtime symbols that
//   describe the bounds of the malloc heap.  __brkval is the current top
//   of the heap; nullptr means malloc has not yet been called, in which
//   case the top equals _heap_start.
//
// All declared at file scope (not inside setup()) so that the C++ name
// mangling rules pick up the C linkage correctly.
// ---------------------------------------------------------------------------
extern "C" uint8_t external_psram_size __attribute__((weak));
extern "C" unsigned long _heap_start;
extern "C" uint32_t set_arm_clock(uint32_t frequency);  // declared in Teensyduino core, not a header
extern "C" unsigned long _heap_end;
extern "C" char *__brkval;

// ---------------------------------------------------------------------------
// PCM5102A mute pin — wire to XSMT on DAC board
// ---------------------------------------------------------------------------
static constexpr uint8_t DAC_MUTE_PIN = 34;

// ---------------------------------------------------------------------------
// MIDI serial debug log — non-blocking ring, printed in loop() outside MIDI
// handlers so we never stall the UART FIFO or the USB serial TX buffer.
// ---------------------------------------------------------------------------
static constexpr uint8_t  MIDI_LOG_SIZE = 32;   // ring capacity (power of 2)
static char               midiLogBuf[MIDI_LOG_SIZE][48];
static volatile uint8_t   midiLogWrite = 0;
static uint8_t            midiLogRead  = 0;

/** Queue a short MIDI event string — call from MIDI handlers only. */
static void midiLog(const char* src, const char* type, uint8_t a, uint8_t b) {
    uint8_t next = (midiLogWrite + 1) & (MIDI_LOG_SIZE - 1);
    if (next == midiLogRead) return;  // ring full — drop (prefer audio over logging)
    snprintf(midiLogBuf[midiLogWrite], 48, "[%s] %-7s %3u %3u", src, type, a, b);
    midiLogWrite = next;
}

/** Drain the log ring — call once per loop() iteration, outside handlers. */
static void midiLogFlush() {
    while (midiLogRead != midiLogWrite) {
        Serial.println(midiLogBuf[midiLogRead]);
        midiLogRead = (midiLogRead + 1) & (MIDI_LOG_SIZE - 1);
    }
}

// ---------------------------------------------------------------------------
// DIN MIDI via Serial1 (31250 baud, standard DIN-5 connector)
// ---------------------------------------------------------------------------
MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, midi1);

// ---------------------------------------------------------------------------
// Audio endpoints
// ---------------------------------------------------------------------------
AudioOutputI2S  i2sOut;    // I2S1: BCK=21, LRCK=20, DATA=7 → PCM5102A
AudioInputUSB   usbIn;     // USB audio in  (DAW loopback)
AudioOutputUSB  usbOut;    // USB audio out (DAW monitor)
AudioScopeTap   scopeTap;  // Waveform capture for home screen scope

// Post-FX signal split: one copy goes to I2S (hardware), one to USB (DAW)
AudioMixer4    mixerI2SL;
AudioMixer4    mixerI2SR;
AudioAmplifier ampUSBL;    // Independent gain trim for USB output
AudioAmplifier ampUSBR;

// Patch cords — heap-allocated, live for program lifetime
AudioConnection* patchMixerI2SL = nullptr;
AudioConnection* patchMixerI2SR = nullptr;
AudioConnection* patchUSBInL    = nullptr;
AudioConnection* patchUSBInR    = nullptr;
AudioConnection* patchOutL      = nullptr;
AudioConnection* patchOutR      = nullptr;
AudioConnection* patchAmpUSBL   = nullptr;
AudioConnection* patchAmpUSBR   = nullptr;
AudioConnection* patchOutUSBL   = nullptr;
AudioConnection* patchOutUSBR   = nullptr;
AudioConnection* patchOutScope  = nullptr;

// ---------------------------------------------------------------------------
// Core objects
// ---------------------------------------------------------------------------
LayerManager                 layers;
// Convenience pointer: tracks the active edit-target engine.
// Refreshed every loop() so UI always references the correct layer.
static SynthEngine*          synth = nullptr;
HardwareInterface_MicroDexed hw;
UIManager_TFT                ui;
BPMClockManager              bpmClock;

// Phase 1 — SysEx adapter bridges the editor protocol to LayerManager.
// Must be after `layers` because the constructor takes a reference to it.
SysExAdapter                 syxAdapter(layers);

// ---------------------------------------------------------------------------
// USB Host MIDI  (keyboard → Teensy USB-A host port)
// ---------------------------------------------------------------------------
USBHost    myusb;
USBHub     hub1(myusb);
MIDIDevice midiHost(myusb);

// ---------------------------------------------------------------------------
// USB Host device connection tracking
//
// USBHost_t36 does not provide connect/disconnect callbacks for MIDIDevice.
// We poll midiHost.idVendor() each loop: non-zero means a device is claimed.
// On transition we print VID/PID/strings to Serial for debug.
//
// WHY THIS HELPS:
//   If the host is "unreliable" it usually means:
//   1. The device is not being claimed (VID/PID will show 0000:0000)
//   2. myusb.Task() is being starved (check loop timing)
//   3. The device is being claimed and released rapidly (shows repeated
//      connect/disconnect messages at ~1 Hz)
// ---------------------------------------------------------------------------
static bool    usbHostConnected = false;  // tracks last known state
static uint8_t usbHostPollDiv   = 0;      // divides loop() for slow polling

/** Print USB host device info — call when connection state changes. */
static void printUSBDeviceInfo(bool connected) {
    if (connected) {
        // idVendor/idProduct are valid once the device is claimed
        Serial.printf("[USB-HOST] Device CONNECTED: VID=%04X PID=%04X\n",
                      midiHost.idVendor(), midiHost.idProduct());
        // manufacturer() and product() return const char* from USB string descriptors.
        // They may be nullptr if the device does not supply them.
        // reinterpret_cast: USBHost_t36 returns uint8_t* for string data.
        const char* mfr  = reinterpret_cast<const char*>(midiHost.manufacturer());
        const char* prod = reinterpret_cast<const char*>(midiHost.product());
        const char* ser  = reinterpret_cast<const char*>(midiHost.serialNumber());
        Serial.printf("[USB-HOST]   Manufacturer : %s\n", mfr  ? mfr  : "(none)");
        Serial.printf("[USB-HOST]   Product      : %s\n", prod ? prod : "(none)");
        Serial.printf("[USB-HOST]   Serial       : %s\n", ser  ? ser  : "(none)");
        Serial.printf("[USB-HOST]   MIDI class   : claimed OK\n");
    } else {
        Serial.println("[USB-HOST] Device DISCONNECTED");
    }
}

// ---------------------------------------------------------------------------
// CC echo suppression — prevents feedback loop when DAW mirrors CCs back.
// Set true while we are sending a CC echo; if a CC arrives while true,
// it came from our own echo and must not be re-processed.
//
// IMPORTANT BEHAVIOUR NOTE (May 2026 — JUCE feedback investigation):
//   The _suppressEcho flag is true only during the few microseconds while
//   usbMIDI.sendControlChange() queues bytes.  The DAW's echo of that CC
//   arrives milliseconds later, by which time _suppressEcho is already
//   false again — so the flag does NOT actually suppress DAW echoes.
//
//   When a JUCE editor is connected and configured to echo received CCs
//   back to the synth (a common default for parameter mirroring), this
//   creates a fast feedback storm: every parameter change ping-pongs
//   between Teensy and JUCE, with each round multiplying the work.
//   Symptom: synth crashes within seconds of JUCE connect, runs for
//   hours on DIN MIDI alone.
//
//   The proper fix is to thread a CCSource tag through the dispatch
//   chain and only echo to USB Device when the source was NOT USB Device.
//   That refactor is pending; for now CC echo to USB Device is DISABLED
//   below, breaking the loop entirely.  The cost is that a JUCE editor
//   no longer sees Teensy-side encoder/touch changes mirrored back —
//   acceptable for testing the hypothesis.
// ---------------------------------------------------------------------------
static volatile bool _suppressEcho = false;

static void onCCHandled(uint8_t cc, uint8_t val) {
    // ---- SysEx PARAM_VALUE echo (firmware → editor) -------------------------
    // Replaces the old raw-CC echo (which was disabled to prevent feedback).
    // The JUCE plugin's handleIncomingSysEx already parses PARAM_VALUE and
    // updates APVTS with _suppressEcho, so no bounce-back occurs.
    //
    // Layer routing:
    //   Performance CCs (140..146)        → kLayerPerf
    //   Global FX CCs (reverb 70-98)      → kLayerGlobalFx
    //   Patch CCs (everything else)       → current edit target (A/B/Both)
    //
    // notifyLocalCC no-ops silently if the CC is not in ParamMap (e.g.
    // internal-only CCs above the map range), so this is always safe to call.
    {
        uint8_t layer;
        // Performance CCs live above MIDI range (140+)
        if (cc >= 140 && cc <= 146) {
            layer = SyxProto::kLayerPerf;
        }
        // Global FX (reverb) CCs — match the same predicate LayerManager uses
        else if (cc == CC::FX_REVERB_SIZE   || cc == CC::FX_REVERB_DAMP    ||
                 cc == CC::FX_REVERB_LODAMP || cc == CC::FX_REVERB_MIX     ||
                 cc == CC::FX_REVERB_BYPASS || cc == CC::FX_REVERB_SHIMMER ||
                 cc == CC::FX_REVERB_FREEZE || cc == CC::FX_REVERB_LOWPASS ||
                 cc == CC::FX_REVERB_HIPASS) {
            layer = SyxProto::kLayerGlobalFx;
        }
        // Patch-scope — use current edit target
        else {
            switch (layers.getEditTarget()) {
                case EditTarget::LAYER_A: layer = SyxProto::kLayerA;    break;
                case EditTarget::LAYER_B: layer = SyxProto::kLayerB;    break;
                case EditTarget::BOTH:    layer = SyxProto::kLayerBoth; break;
                default:                  layer = SyxProto::kLayerA;    break;
            }
        }
        syxAdapter.notifyLocalCC(layer, cc, val);
    }

    // Tell TFT to repaint the matching control (if visible).
    // This is just a dirty-flag set — no drawing happens here.
    ui.notifyCC(cc);
}


// ===========================================================================
// MIDI event handlers
//
// RULES (see [R2], [R3] above):
//   - Call synth.noteOn/Off/handleCC → safe (engine guards with AudioNoInterrupts)
//   - Use midiLog() for debug output — NEVER Serial.print* directly here
//   - Keep execution under ~10 µs — no loops, no allocations
// ===========================================================================

/** Fired by all three MIDI sources (USB device, USB Host, DIN). */
static void onNoteOn(byte channel, byte note, byte velocity) {
    midiLog("MIDI", "NoteOn", note, velocity);
    if (velocity == 0) {
        // Velocity-0 NoteOn is a NoteOff (running status optimisation)
        layers.noteOff(channel, note);
    } else {
        layers.noteOn(channel, note, velocity / 127.0f);
    }
}

static void onNoteOff(byte channel, byte note, byte /*velocity*/) {
    midiLog("MIDI", "NoteOff", note, 0);
    layers.noteOff(channel, note);
}

static void onCC(byte channel, byte control, byte value) {
    // Drop CCs that are echoes of our own sendControlChange (feedback prevention)
    if (_suppressEcho) return;
    midiLog("MIDI", "CC", control, value);
    layers.handleControlChange(channel, control, value);
}

// onPitchBend — MIDI pitch bend wheel callback.
// value = raw 14-bit pitch bend (0..16383, centre = 8192).
// Forwarded to LayerManager which routes to active layer(s).
//
// MIDI handler — must NOT call Serial.print* (rule [R3] above and the
// note in DebugTrace.h).  Pitch bend can stream at >100 messages/sec
// during a wheel sweep from a JUCE plugin; printf would block on the
// USB-CDC TX ring and stall loop().  Use the midiLog() ring instead,
// which is bounded and drained outside handler context.
static void onPitchBend(byte channel, int value) {
    // Teensy MIDI libraries pass pitch bend as int (0..16383, centre 8192).
    layers.handlePitchBend(channel, (int16_t)value);
    // Optional: enqueue a rate-limited record into midiLog().  Don't call
    // Serial.printf here.
}

// Real-time clock messages — forwarded to BPMClockManager only (no logging —
// these fire up to 24× per beat and would flood the ring).
static void onMIDIClock()    { bpmClock.handleMIDIClock();    }
static void onMIDIStart()    { bpmClock.handleMIDIStart();    midiLog("MIDI","Start",0,0); }
static void onMIDIStop()     { bpmClock.handleMIDIStop();     midiLog("MIDI","Stop",0,0);  }
static void onMIDIContinue() { bpmClock.handleMIDIContinue(); midiLog("MIDI","Cont",0,0);  }

/** USB Host real-time byte dispatcher (USBHost_t36 API). */
static void onUSBHostRealTime(uint8_t byte) {
    switch (byte) {
        case 0xF8: onMIDIClock();    break;
        case 0xFA: onMIDIStart();    break;
        case 0xFC: onMIDIStop();     break;
        case 0xFB: onMIDIContinue(); break;
        default: break;
    }
}

// ===========================================================================
// SysEx handler — Phase 1 editor protocol
//
// Fired by usbMIDI, midiHost, and midi1 when a complete SysEx message
// arrives.  Delegates to SysExAdapter which validates the JT-8000 envelope
// and dispatches SET_PARAM / GET_PARAM / BANK_DUMP_REQUEST / BANK_DUMP.
//
// IMPORTANT: Teensy MIDI libraries pass the FULL message including F0 and F7.
// Length includes both framing bytes.
//
// Each MIDI library has a slightly different callback signature:
//   usbMIDI:     void(uint8_t *data, unsigned int size)
//   USBHost_t36: void(const uint8_t *data, uint16_t length, bool complete)
//   MIDI lib:    void(byte *data, unsigned size)
// ===========================================================================

// USB Device MIDI callback (JUCE plugin connects here)
static void onSysEx(uint8_t* data, unsigned int len) {
    syxAdapter.handleSysEx(data, (size_t)len);
}

// USB Host MIDI callback (external controller on USB-A host port)
static void onSysExHost(const uint8_t* data, uint16_t len, bool /*complete*/) {
    syxAdapter.handleSysEx(data, (size_t)len);
}

// DIN MIDI callback (FortySevenEffects library, hardware serial port)
static void onSysExDIN(byte* data, unsigned len) {
    syxAdapter.handleSysEx(data, (size_t)len);
}

// SysEx reply sender — registered with SysExAdapter so it can respond to
// GET_PARAM and BANK_DUMP_REQUEST without knowing which port to use.
// Sends via USB Device MIDI (the same port the JUCE plugin connects to).
//
// usbMIDI.sendSysEx expects (length, data, hasF0F7):
//   hasF0F7=true means the data array already contains F0 at start and F7
//   at end — sendSysEx transmits them verbatim. SysExAdapter always builds
//   complete F0..F7 messages, so we pass true.
static void syxSend(const uint8_t* data, size_t len) {
    usbMIDI.sendSysEx((unsigned int)len, data, /*hasBeginEnd=*/true);
}

// ===========================================================================
// setup()
// ===========================================================================
void setup() {
    Serial.begin(115200);
    delay(200);   // Let power rail settle before touching SPI or I2C

    // -------------------------------------------------------------------------
    // DIAGNOSTICS — print before any other setup() work so they are visible
    // even if a later step blocks or crashes.
    // -------------------------------------------------------------------------

    // CrashReport persists across reboot on Teensy 4.x and captures hard
    // faults, watchdog timeouts, and stack overflows.  Print before clearing
    // so the next reboot loop is debuggable.  Cleared at the END of setup()
    // (search for "CrashReport.clear" near the bottom of this function) so a
    // crash mid-setup leaves the report intact for the following boot.
    if (CrashReport) {
        Serial.println(F("======== CRASH REPORT ========"));
        Serial.print(CrashReport);
        Serial.println(F("=============================="));
    }

    // Cortex-M7 FPU: enable Flush-To-Zero (FZ, bit 24) and Default-NaN
    // (DN, bit 25).  By default the M7 traps subnormal float operations to
    // microcode at ~50× normal cost.  Audio DSP filter state (Moog ladder
    // integrators, IIR feedback paths) decays into subnormal range during
    // long silent gaps; FZ flushes those to +0.0 in hardware, eliminating
    // the slowdown.  Audibly indistinguishable; CPU win is large.
    {
        uint32_t fpscr;
        __asm__ volatile ("vmrs %0, fpscr" : "=r"(fpscr));
        fpscr |= (1u << 24) | (1u << 25);
        __asm__ volatile ("vmsr fpscr, %0" : : "r"(fpscr));
    }

    // Restore the project's design CPU clock.  Must precede AudioMemory()
    // so the audio ISR's first dispatch is at the intended rate.
    //set_arm_clock(816000000);

    Serial.println("[JT8000] Boot start");
    Serial.printf("[JT8000] CPU = %lu MHz\n",
                  (unsigned long)F_CPU_ACTUAL / 1000000UL);

    // Memory diagnostics — confirm PSRAM presence and heap headroom before
    // any further allocation.  The new performance branch allocates ~1.2 MB
    // of PSRAM at global construction time (2× JPFX delay buffers + 1×
    // reverb pool); if no PSRAM chip is installed those fall back to RAM
    // heap and may fail silently.
    {
        Serial.printf("[JT8000] PSRAM size = %u MB\n",
                      (unsigned)external_psram_size);

        const uintptr_t heapTop = (__brkval == nullptr)
                                ? (uintptr_t)&_heap_start
                                : (uintptr_t)__brkval;
        const uintptr_t heapFree = (uintptr_t)&_heap_end - heapTop;
        Serial.printf("[JT8000] Heap free  = %u bytes\n", (unsigned)heapFree);
    }

    // -------------------------------------------------------------------------
    // STEP 1: Display (SPI) — BEFORE AudioMemory to avoid DMA bus conflicts.
    // -------------------------------------------------------------------------
    ui.beginDisplay();
    Serial.println("[JT8000] Display OK");

    // -------------------------------------------------------------------------
    // COLOUR DIAGNOSTIC  —  enable to identify display channel mapping
    // -------------------------------------------------------------------------
    // Step 1: uncomment BOTH lines below and upload.
    //         The screen will show 6 colour bars.  Note what colour each bar
    //         ACTUALLY appears as on the hardware, then report back.
    //   while (true) {}   // halt — synth does not start
    // #endif
    // -------------------------------------------------------------------------

    // -------------------------------------------------------------------------
    // STEP 2: Audio memory pool.
    // 200 blocks = 51200 bytes DMAMEM.  256 was marginal under heavy SPI.
    // -------------------------------------------------------------------------
    AudioMemory(200);

    // CRITICAL: Create all internal AudioConnections NOW — after AudioMemory().
    // LayerManager calls begin() on both SynthEngine instances internally.
    // Both engines are global objects so their constructors ran before setup().
    layers.begin();

    // -------------------------------------------------------------------------
    // STEP 3: USB Host MIDI  (keyboard on host port)
    // -------------------------------------------------------------------------
    myusb.begin();

    midiHost.setHandleNoteOn(onNoteOn);
    midiHost.setHandleNoteOff(onNoteOff);
    midiHost.setHandleControlChange(onCC);
    midiHost.setHandlePitchChange(onPitchBend);    // pitch wheel
    midiHost.setHandleRealTimeSystem(onUSBHostRealTime);
    midiHost.setHandleSysEx(onSysExHost);            // Phase 1 — editor SysEx

    Serial.println("[JT8000] USB Host MIDI configured");
    delay(200);  // Let USB host stack settle before polling

    set_arm_clock(720000000);

    Serial.println("[JT8000] Boot start");
    Serial.printf("[JT8000] CPU = %lu MHz\n",
                  (unsigned long)F_CPU_ACTUAL / 1000000UL);

    // -------------------------------------------------------------------------
    // STEP 4: USB Device MIDI  (DAW/PC connected to Teensy micro-USB)
    // -------------------------------------------------------------------------
    usbMIDI.setHandleNoteOn(onNoteOn);
    usbMIDI.setHandleNoteOff(onNoteOff);
    usbMIDI.setHandleControlChange(onCC);
    usbMIDI.setHandlePitchChange(onPitchBend);    // pitch wheel
    usbMIDI.setHandleRealTimeSystem(onUSBHostRealTime);
    usbMIDI.setHandleSystemExclusive(onSysEx);     // Phase 1 — editor SysEx

    Serial.println("[JT8000] USB Device MIDI configured");

    // -------------------------------------------------------------------------
    // STEP 5: DIN MIDI via Serial1 (hardware 5-pin DIN connector, 31250 baud)
    // -------------------------------------------------------------------------
    midi1.begin(MIDI_CHANNEL_OMNI);   // listen on all channels
    midi1.setHandleNoteOn(onNoteOn);
    midi1.setHandleNoteOff(onNoteOff);
    midi1.setHandleControlChange(onCC);
    midi1.setHandlePitchBend(onPitchBend);        // pitch wheel (MIDI lib uses different name)
    midi1.setHandleClock(onMIDIClock);
    midi1.setHandleStart(onMIDIStart);
    midi1.setHandleStop(onMIDIStop);
    midi1.setHandleContinue(onMIDIContinue);
    midi1.turnThruOff();  // disable software MIDI-thru (would re-send to Serial1)
    midi1.setHandleSystemExclusive(onSysExDIN);    // Phase 1 — editor SysEx

    Serial.println("[JT8000] DIN MIDI (Serial1) configured");

    // -------------------------------------------------------------------------
    // STEP 6: Hardware encoders + synth engine
    // -------------------------------------------------------------------------
    hw.begin();
    synth = &layers.activeEngine();   // set convenience pointer for UI
    ui.begin(*synth, &layers);

    // Phase 1 — connect SysExAdapter CC snoop to LayerManager BEFORE preset
    // load so the cache is populated as the init patch CCs flow through.
    // The snoop only reads — it never sends, so this is safe pre-enumeration.
    layers.setSysExSnoop(&syxAdapter);
    Serial.println("[JT8000] SysEx adapter snoop wired");

    // Load init preset to both engines BEFORE syncFromEngine so _ccState is
    // fully populated. Without this the display shows zero for every parameter
    // until the user manually loads a preset.
    //
    // Index 0 = "Init Sine" — the canonical blank-slate starting point.
    // Both Layer A and B are initialised so dual-layer mode starts clean.
    // loadFactoryPatch() sends all 95 CCs atomically under AudioNoInterrupts(),
    // so it is safe to call here with the audio ISR already running.
    //
    // IMPORTANT: the notifier and SysEx sender are NOT yet registered. This
    // prevents the ~90 init CCs from flooding usbMIDI.sendSysEx() before USB
    // has fully enumerated (which caused watchdog resets).
    Presets::presets_loadByGlobalIndex(layers.layerA(), 0, /*midiCh=*/1);
    Presets::presets_loadByGlobalIndex(layers.layerB(), 0, /*midiCh=*/1);

    ui.syncFromEngine(*synth);

    // NOW register the notifier and SysEx sender — preset load is done, USB
    // is enumerated, no more CC floods.  From this point on, every CC change
    // (TFT, encoder, incoming MIDI) triggers a PARAM_VALUE echo to the editor
    // and a TFT repaint via ui.notifyCC.
    layers.setNotifier(onCCHandled);
    syxAdapter.setSender(syxSend);
    Serial.println("[JT8000] SysEx sender + notifier active");

    // -------------------------------------------------------------------------
    // STEP 7: Unmute PCM5102A — LAST, after I2S DMA is running.
    // -------------------------------------------------------------------------
    pinMode(DAC_MUTE_PIN, OUTPUT);
    digitalWrite(DAC_MUTE_PIN, HIGH);
    Serial.println("[JT8000] DAC unmuted");

    // -------------------------------------------------------------------------
    // STEP 8: BPM clock
    // -------------------------------------------------------------------------
    bpmClock.setInternalBPM(120.0f);
    bpmClock.setClockSource(CLOCK_INTERNAL);
    layers.setBPMClock(&bpmClock);

    // -------------------------------------------------------------------------
    // STEP 9: Audio patch cords (AFTER AudioMemory)
    // -------------------------------------------------------------------------
    // Audio output comes from LayerManager's performance mixer which combines
    // both layers' stereo outputs with crossfade balance control.
    patchMixerI2SL = new AudioConnection(layers.getPerfOutL(), 0, mixerI2SL, 0);
    patchMixerI2SR = new AudioConnection(layers.getPerfOutR(), 0, mixerI2SR, 0);
    patchUSBInL    = new AudioConnection(usbIn, 0, mixerI2SL, 1);
    patchUSBInR    = new AudioConnection(usbIn, 1, mixerI2SR, 1);
    patchOutL      = new AudioConnection(mixerI2SL, 0, i2sOut, 0);
    patchOutR      = new AudioConnection(mixerI2SR, 0, i2sOut, 1);
    patchAmpUSBL   = new AudioConnection(layers.getPerfOutL(), 0, ampUSBL, 0);
    patchAmpUSBR   = new AudioConnection(layers.getPerfOutR(), 0, ampUSBR, 0);
    patchOutUSBL   = new AudioConnection(ampUSBL, 0, usbOut, 0);
    patchOutUSBR   = new AudioConnection(ampUSBR, 0, usbOut, 1);
    patchOutScope  = new AudioConnection(layers.getPerfOutL(), 0, scopeTap, 0);

    // Gain settings
    mixerI2SL.gain(0, 1.0f);   // Synth → I2S L
    mixerI2SR.gain(0, 1.0f);   // Synth → I2S R
    mixerI2SL.gain(1, 0.4f);   // USB in → I2S L (lower so DAW audio doesn't overpower synth)
    mixerI2SR.gain(1, 0.4f);   // USB in → I2S R
    ampUSBL.gain(0.7f);         // USB output trim
    ampUSBR.gain(0.7f);

    Serial.println("[JT8000] Ready");

    // Setup completed without crashing — clear the crash report so the
    // next boot doesn't re-print this stale fault.  If we crash before
    // reaching here, the report stays intact for the following boot.
    CrashReport.clear();
    Serial.println("[JT8000] Setup complete");

    ARM_DEMCR    |= ARM_DEMCR_TRCENA;        // enable DWT
    ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;  // enable cycle counter

        
}

// ===========================================================================
// loop()
//
// ORDERING MATTERS:
//   1. Service all MIDI sources FIRST — highest priority, smallest latency.
//   2. USB host task (required by USBHost_t36 every iteration).
//   3. Drain serial MIDI log (non-blocking, safe here).
//   4. Synth update (voice management, envelope clocking).
//   5. Hardware poll (encoders, buttons) — feeds UI.
//   6. UI input poll (touch + encoders → screen actions).
//   7. UI display update (rate-limited inside UIManager to ~30 fps).
//
// DO NOT put any delay() or long-running operations in this loop.
// The display SPI operations in updateDisplay() are the longest single
// operation (~2-8 ms for a full row redraw); the FRAME_MS gate keeps them
// to one repaint per 33 ms frame, so MIDI is only stalled for a single SPI
// transaction (~200 µs max per drawLine call).
// ===========================================================================
void loop() {
    // [R4/R5] Service all MIDI sources — must happen every iteration
    myusb.Task();           // USB Host stack pump — drives enumeration & data
    while (midiHost.read()) {}   // USB Host MIDI messages
    while (usbMIDI.read()) {}    // USB Device MIDI messages
    midi1.read();                // DIN MIDI (MIDI library reads one message per call)

    
    // ---- USB Host connection state polling ----
    // USBHost_t36 does not fire a connect callback for MIDIDevice, so we poll.
    // Run every 256 loops (~every 2-5 ms at typical loop rate) to keep overhead
    // negligible.  idVendor() returns 0 when no device is claimed.
    if (++usbHostPollDiv == 0) {  // wraps at 256
        const bool nowConnected = (midiHost.idVendor() != 0);
        if (nowConnected != usbHostConnected) {
            usbHostConnected = nowConnected;
            printUSBDeviceInfo(nowConnected);
        }
    }

    // ── Serial Diagnostics (comment out this entire block to disable) ────────
{
    static uint32_t lastDiag = 0;
    static uint32_t loopCount = 0;
    loopCount++;
    const uint32_t now = millis();
    if ((now - lastDiag) >= 10000) {
        if (Serial.availableForWrite() > 100) {
            Serial.printf("[DIAG] CPU:%.1f%% pk:%.1f%% | mem:%d max:%d | lps:%lu\n",
                AudioProcessorUsage(), AudioProcessorUsageMax(),
                AudioMemoryUsage(), AudioMemoryUsageMax(), loopCount);
            // Reset peaks so the NEXT window reports its own fresh maximum.
            AudioProcessorUsageMaxReset();
            AudioMemoryUsageMaxReset();
        }
        lastDiag = now;
        loopCount = 0;
    }
}
// ── End Serial Diagnostics ───────────────────────────────────────────────

    // Drain the MIDI log ring (safe outside handlers)
    midiLogFlush();

    // Synth update: voice management, LFO, etc. (both layers if dual mode)
    layers.update();
    synth = &layers.activeEngine();   // refresh convenience pointer

    // Encoder + button poll
    hw.update();

    // UI input: touch + encoders → actions
    ui.pollInputs(hw, *synth);

    // UI display: rate-limited to ~30 fps internally
    ui.updateDisplay(*synth);
}