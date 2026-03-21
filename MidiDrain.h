/**
 * MidiDrain.h — Non-blocking MIDI buffer drain for JT-8000
 *
 * PURPOSE:
 *   Any long-running operation (display fillScreen, section expand redraw,
 *   preset load, SD card access) risks overflowing the USB MIDI input buffer
 *   because loop() is stalled.  Call MidiDrain::poll() at natural break
 *   points inside those operations to keep MIDI flowing.
 *
 * HOW IT WORKS:
 *   MidiDrain::poll() calls the same three MIDI read functions as loop():
 *     - myusb.Task()        (USB host stack pump)
 *     - midiHost.read()     (USB host MIDI)
 *     - usbMIDI.read()      (USB device MIDI)
 *     - midi1.read()        (DIN MIDI)
 *
 *   Registered callbacks fire normally — noteOn/Off/CC are dispatched to
 *   SynthEngine exactly as they would from loop().
 *
 * COST:
 *   ~5 µs when no messages pending (just checks three empty FIFOs).
 *   ~2 µs per message processed (callback overhead).
 *
 * USAGE:
 *   In setup():  MidiDrain::begin(&myusb, &midiHost, &midi1obj);
 *   Anywhere:    MidiDrain::poll();   // safe to call from any context
 *
 * This replaces the need for a full framebuffer + DMA rewrite of the display
 * library.  By draining MIDI inside the existing blocking SPI operations,
 * we prevent buffer overflow without changing the rendering architecture.
 *
 * REFERENCE: MicroDexed-touch uses the same pattern — MIDI is read between
 * display update chunks inside their draw loops.
 */

#pragma once

#include <Arduino.h>
#include <USBHost_t36.h>
#include <MIDI.h>
#include <usb_midi.h>

namespace MidiDrain {

    // Call once in setup() after all MIDI handlers are registered.
    void begin(USBHost* usbHost, MIDIDevice* midiHostDev,
               midi::MidiInterface<midi::SerialMIDI<HardwareSerial>>* dinMidi);

    // Drain all pending MIDI from all three sources.
    // Safe to call from anywhere — no side effects beyond dispatching
    // MIDI callbacks that are already registered.
    //
    // Call this inside any operation that blocks loop() for > 1 ms:
    //   - fillScreen / fillRect clears
    //   - Section expand (full content redraw)
    //   - Preset load (bulk CC dispatch)
    //   - SD card operations
    void poll();

}  // namespace MidiDrain
