/*
  UsbMidiPacket.h - USB-MIDI 1.0 event packet decoding.

  A USB MIDI endpoint carries a stream of fixed 4-byte event packets rather
  than a raw MIDI byte stream. Each packet is:

    byte 0: cable number in the high nibble, Code Index Number in the low
    bytes 1-3: the MIDI message, padded with zeros

  The Code Index Number, not the MIDI status byte, determines how many of the
  following bytes are meaningful. Decoding on the status byte instead works
  for channel messages and then quietly corrupts system exclusive traffic.

  Free of Arduino and teensy4_usbhost dependencies so it can be host-compiled
  and tested. All lookups are table-driven: no branching per packet, and no
  arithmetic beyond a nibble mask.
*/

#ifndef JT8000_USB_MIDI_PACKET_H
#define JT8000_USB_MIDI_PACKET_H

#include <cstdint>

/* Code Index Numbers, as defined by the USB MIDI 1.0 class specification. */
enum : uint8_t {
  kUsbMidiCinMisc = 0x0u,           // reserved
  kUsbMidiCinCableEvent = 0x1u,     // reserved
  kUsbMidiCinSysCommon2 = 0x2u,     // 2-byte system common
  kUsbMidiCinSysCommon3 = 0x3u,     // 3-byte system common
  kUsbMidiCinSysExStart = 0x4u,     // SysEx start or continue
  kUsbMidiCinSysExEnd1 = 0x5u,      // SysEx ending with one byte
  kUsbMidiCinSysExEnd2 = 0x6u,      // SysEx ending with two bytes
  kUsbMidiCinSysExEnd3 = 0x7u,      // SysEx ending with three bytes
  kUsbMidiCinNoteOff = 0x8u,
  kUsbMidiCinNoteOn = 0x9u,
  kUsbMidiCinPolyPressure = 0xAu,
  kUsbMidiCinControlChange = 0xBu,
  kUsbMidiCinProgramChange = 0xCu,
  kUsbMidiCinChannelPressure = 0xDu,
  kUsbMidiCinPitchBend = 0xEu,
  kUsbMidiCinSingleByte = 0xFu
};

/* One decoded event. The Code Index Number is retained because it is the only
   way to tell a SysEx continuation fragment, which carries no status byte,
   from a channel message. */
struct UsbMidiMessage {
  uint8_t cable;   // 0-15, identifies the physical or virtual port
  uint8_t cin;     // Code Index Number
  uint8_t length;  // meaningful bytes in data, 1 to 3
  uint8_t data[3];
};

/* Meaningful byte count for a Code Index Number. Returns 0 for the two
   reserved values, which callers should discard. */
uint8_t usbMidiCinLength(uint8_t cin);

/* Decode one 4-byte packet. Returns false for reserved Code Index Numbers
   and for the all-zero packets that pad a short bulk transfer, so a false
   return means "skip this packet", not "error". */
bool usbMidiDecodePacket(const uint8_t* packet, UsbMidiMessage& message);

/* True when the message is a channel voice message, meaning data[0] holds a
   status byte whose low nibble is the MIDI channel. */
bool usbMidiIsChannelMessage(const UsbMidiMessage& message);

/* MIDI channel, 1-16, or 0 when the message is not a channel message. */
uint8_t usbMidiChannel(const UsbMidiMessage& message);

/* Short human-readable name for the message type. */
const char* usbMidiMessageName(const UsbMidiMessage& message);

/* Build a message from a MIDI status byte and up to two data bytes. Derives
   the Code Index Number and length from the status, so callers never have to
   know the packet format. Returns false for a status byte this encoder does
   not handle, which currently means system exclusive: SysEx spans several
   packets and needs a streaming encoder rather than a one-shot call.

   Note that channel is carried in the low nibble of 'status', matching how
   MIDI itself works, rather than as a separate argument. */
bool usbMidiBuildFromStatus(uint8_t cable, uint8_t status, uint8_t data1,
                            uint8_t data2, UsbMidiMessage& message);

/* Write a message into a 4-byte USB-MIDI event packet. Returns false if the
   message has no valid length, so a caller cannot silently transmit
   rubbish. The packet is always fully written, with unused bytes zeroed,
   because a device is entitled to look at all four. */
bool usbMidiEncodePacket(const UsbMidiMessage& message, uint8_t* packet);

/* Write a note name such as "C#4" into buffer. Uses the convention that note
   60 is C4, which is what Roland instruments display. Buffer needs 5 bytes. */
void usbMidiNoteName(uint8_t note, char* buffer, uint8_t bufferSize);

#endif // JT8000_USB_MIDI_PACKET_H
