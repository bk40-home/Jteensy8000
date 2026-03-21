/**
 * MidiDrain.cpp — MIDI buffer drain implementation
 *
 * Stores pointers to the three MIDI sources set up in Jteensy8000.ino.
 * poll() drains all pending messages through the already-registered callbacks.
 */

#include "MidiDrain.h"

static USBHost*    sUsbHost   = nullptr;
static MIDIDevice* sMidiHost  = nullptr;
static midi::MidiInterface<midi::SerialMIDI<HardwareSerial>>* sDinMidi = nullptr;

void MidiDrain::begin(USBHost* usbHost, MIDIDevice* midiHostDev,
                      midi::MidiInterface<midi::SerialMIDI<HardwareSerial>>* dinMidi) {
    sUsbHost  = usbHost;
    sMidiHost = midiHostDev;
    sDinMidi  = dinMidi;
}

void MidiDrain::poll() {
    // USB Host stack pump — must be called for midiHost.read() to work.
    // Cost: ~2 µs when idle, handles enumeration and data transfers.
    if (sUsbHost)  sUsbHost->Task();

    // Drain USB Host MIDI (external controllers on USB-A port).
    // Registered callbacks fire inside read().
    if (sMidiHost) { while (sMidiHost->read()) {} }

    // Drain USB Device MIDI (DAW connected via micro-USB).
    while (usbMIDI.read()) {}

    // Drain DIN MIDI (hardware 5-pin connector on Serial1).
    // MIDI library reads one message per call, so one call is sufficient
    // at the rate we're polling (every ~1 ms during display operations).
    if (sDinMidi) sDinMidi->read();
}
