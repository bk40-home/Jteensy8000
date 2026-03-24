/**
 * Jteensy8000.ino — JT-8000 polyphonic synthesizer  (v6)
 *
 * Audio path:
 *   SynthEngine (8 voices) → FXChainBlock → mixerI2S{L/R} → I2S → PCM5102A
 *                                         → scopeTap      (waveform capture)
 *                                         → ampUSB{L/R}   → usbOut (DAW monitor)
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
//#include "UIPageLayout.h"
#include "HardwareInterface_MicroDexed.h"
#include "UIManager_TFT.h"
#include "Presets.h"
//#include "AudioScopeTap.h"
#include "BPMClockManager.h"

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
SynthEngine                  synth;
HardwareInterface_MicroDexed hw;
UIManager_TFT                ui;
BPMClockManager              bpmClock;

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

static void onCCHandled(uint8_t cc, uint8_t val) {
    // Echo CC changes back to USB Device MIDI for HTML editor sync.
    usbMIDI.sendControlChange(cc & 0x7F, val & 0x7F, 1);

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
        synth.noteOff(note);
    } else {
        synth.noteOn(note, velocity / 127.0f);
    }
}

static void onNoteOff(byte channel, byte note, byte /*velocity*/) {
    midiLog("MIDI", "NoteOff", note, 0);
    synth.noteOff(note);
}

static void onCC(byte channel, byte control, byte value) {
    midiLog("MIDI", "CC", control, value);
    synth.handleControlChange(channel, control, value);
}

// onPitchBend — MIDI pitch bend wheel callback.
// value = raw 14-bit pitch bend (0..16383, centre = 8192).
// Forwarded directly to SynthEngine which converts to semitones and applies
// to all voices via OscillatorBlock::setPitchModulation().
static void onPitchBend(byte channel, int value) {
    // Teensy MIDI libraries pass pitch bend as int (0..16383, centre 8192).
    synth.handlePitchBend(channel, (int16_t)value);
    JT_LOGF("[MIDI] PitchBend ch%u val=%d\n", (unsigned)channel, value);
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
// setup()
// ===========================================================================
void setup() {
    Serial.begin(115200);
    delay(200);   // Let power rail settle before touching SPI or I2C

    Serial.println("[JT8000] Boot start");

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
    // SynthEngine is a global object so its constructor ran before setup().
    // Any AudioConnection built before AudioMemory() is silently broken.
    synth.begin();

    // -------------------------------------------------------------------------
    // STEP 3: USB Host MIDI  (keyboard on host port)
    // -------------------------------------------------------------------------
    myusb.begin();

    midiHost.setHandleNoteOn(onNoteOn);
    midiHost.setHandleNoteOff(onNoteOff);
    midiHost.setHandleControlChange(onCC);
    midiHost.setHandlePitchChange(onPitchBend);    // pitch wheel
    midiHost.setHandleRealTimeSystem(onUSBHostRealTime);

    Serial.println("[JT8000] USB Host MIDI configured");

    // -------------------------------------------------------------------------
    // STEP 4: USB Device MIDI  (DAW/PC connected to Teensy micro-USB)
    // -------------------------------------------------------------------------
    usbMIDI.setHandleNoteOn(onNoteOn);
    usbMIDI.setHandleNoteOff(onNoteOff);
    usbMIDI.setHandleControlChange(onCC);
    usbMIDI.setHandlePitchChange(onPitchBend);    // pitch wheel
    usbMIDI.setHandleRealTimeSystem(onUSBHostRealTime);

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

    Serial.println("[JT8000] DIN MIDI (Serial1) configured");

    // -------------------------------------------------------------------------
    // STEP 6: Hardware encoders + synth engine
    // -------------------------------------------------------------------------
    hw.begin();
    ui.begin(synth);
    synth.setNotifier(onCCHandled);

    // Load init template BEFORE syncFromEngine so _ccState is populated.
    // Without this, all CC values are 0 at boot and the display shows wrong values
    // until the first preset is loaded.
    //Presets.loadInitTemplateByWave(synth, 1);


    ui.syncFromEngine(synth);

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
    synth.setBPMClock(&bpmClock);

    // -------------------------------------------------------------------------
    // STEP 9: Audio patch cords (AFTER AudioMemory)
    // -------------------------------------------------------------------------
    patchMixerI2SL = new AudioConnection(synth.getFXOutL(), 0, mixerI2SL, 0);
    patchMixerI2SR = new AudioConnection(synth.getFXOutR(), 0, mixerI2SR, 0);
    patchUSBInL    = new AudioConnection(usbIn, 0, mixerI2SL, 1);
    patchUSBInR    = new AudioConnection(usbIn, 1, mixerI2SR, 1);
    patchOutL      = new AudioConnection(mixerI2SL, 0, i2sOut, 0);
    patchOutR      = new AudioConnection(mixerI2SR, 0, i2sOut, 1);
    patchAmpUSBL   = new AudioConnection(synth.getFXOutL(), 0, ampUSBL, 0);
    patchAmpUSBR   = new AudioConnection(synth.getFXOutR(), 0, ampUSBR, 0);
    patchOutUSBL   = new AudioConnection(ampUSBL, 0, usbOut, 0);
    patchOutUSBR   = new AudioConnection(ampUSBR, 0, usbOut, 1);
    patchOutScope  = new AudioConnection(synth.getFXOutL(), 0, scopeTap, 0);

    // Gain settings
    mixerI2SL.gain(0, 1.0f);   // Synth → I2S L
    mixerI2SR.gain(0, 1.0f);   // Synth → I2S R
    mixerI2SL.gain(1, 0.4f);   // USB in → I2S L (lower so DAW audio doesn't overpower synth)
    mixerI2SR.gain(1, 0.4f);   // USB in → I2S R
    ampUSBL.gain(0.7f);         // USB output trim
    ampUSBR.gain(0.7f);

    Serial.println("[JT8000] Ready");


        synth.handlePitchBend(1, 8192);
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

    // Drain the MIDI log ring (safe outside handlers)
    midiLogFlush();

    // Synth update: voice management, LFO, etc.
    synth.update();

    // Encoder + button poll
    hw.update();

    // UI input: touch + encoders → actions
    ui.pollInputs(hw, synth);

    // UI display: rate-limited to ~30 fps internally
    ui.updateDisplay(synth);
}
