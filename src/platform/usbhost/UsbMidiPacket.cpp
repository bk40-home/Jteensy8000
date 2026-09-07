/*
  UsbMidiPacket.cpp - see UsbMidiPacket.h.
*/

#include "UsbMidiPacket.h"

namespace {

/* Meaningful bytes per Code Index Number, indexed by the CIN itself. Zero
   marks the two reserved values. A table costs 16 bytes and replaces a
   switch that would otherwise run on every packet. */
const uint8_t kCinLength[16] = {
    0u, /* 0x0 reserved */
    0u, /* 0x1 reserved */
    2u, /* 0x2 two-byte system common */
    3u, /* 0x3 three-byte system common */
    3u, /* 0x4 SysEx start or continue */
    1u, /* 0x5 SysEx end, one byte */
    2u, /* 0x6 SysEx end, two bytes */
    3u, /* 0x7 SysEx end, three bytes */
    3u, /* 0x8 note off */
    3u, /* 0x9 note on */
    3u, /* 0xA polyphonic key pressure */
    3u, /* 0xB control change */
    2u, /* 0xC program change */
    2u, /* 0xD channel pressure */
    3u, /* 0xE pitch bend */
    1u  /* 0xF single byte */
};

/* Note names using sharps. Flats are the same pitches and the choice is
   cosmetic; sharps match how most instruments label their displays. */
const char* const kNoteNames[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                    "F#", "G",  "G#", "A",  "A#", "B"};

/* System realtime and system common status bytes worth naming, since these
   arrive as single-byte or system-common packets rather than channel ones. */
const char* systemName(uint8_t status) {
  switch (status) {
    case 0xF0u: return "sysex start";
    case 0xF1u: return "MTC quarter frame";
    case 0xF2u: return "song position";
    case 0xF3u: return "song select";
    case 0xF6u: return "tune request";
    case 0xF7u: return "sysex end";
    case 0xF8u: return "clock";
    case 0xFAu: return "start";
    case 0xFBu: return "continue";
    case 0xFCu: return "stop";
    case 0xFEu: return "active sensing";
    case 0xFFu: return "reset";
    default: return "system";
  }
}

} // namespace

uint8_t usbMidiCinLength(uint8_t cin) { return kCinLength[cin & 0x0Fu]; }

bool usbMidiDecodePacket(const uint8_t* packet, UsbMidiMessage& message) {
  if (packet == nullptr) {
    return false;
  }

  const uint8_t cin = packet[0] & 0x0Fu;
  const uint8_t length = kCinLength[cin];
  if (length == 0u) {
    /* Reserved Code Index Number, or the zero padding that fills the tail of
       a short bulk transfer. Either way there is nothing to deliver. */
    return false;
  }

  message.cable = static_cast<uint8_t>(packet[0] >> 4);
  message.cin = cin;
  message.length = length;
  message.data[0] = packet[1];
  message.data[1] = packet[2];
  message.data[2] = packet[3];
  return true;
}

bool usbMidiIsChannelMessage(const UsbMidiMessage& message) {
  return (message.cin >= kUsbMidiCinNoteOff) &&
         (message.cin <= kUsbMidiCinPitchBend);
}

uint8_t usbMidiChannel(const UsbMidiMessage& message) {
  if (!usbMidiIsChannelMessage(message)) {
    return 0u;
  }
  return static_cast<uint8_t>((message.data[0] & 0x0Fu) + 1u);
}

const char* usbMidiMessageName(const UsbMidiMessage& message) {
  switch (message.cin) {
    case kUsbMidiCinNoteOff: return "note off";
    case kUsbMidiCinNoteOn:
      /* A note on with zero velocity is a note off in disguise, and every
         keyboard that uses running status sends it that way. */
      return (message.data[2] == 0u) ? "note off (vel 0)" : "note on";
    case kUsbMidiCinPolyPressure: return "poly pressure";
    case kUsbMidiCinControlChange: return "control change";
    case kUsbMidiCinProgramChange: return "program change";
    case kUsbMidiCinChannelPressure: return "channel pressure";
    case kUsbMidiCinPitchBend: return "pitch bend";
    case kUsbMidiCinSysExStart: return "sysex data";
    case kUsbMidiCinSysExEnd1:
    case kUsbMidiCinSysExEnd2:
    case kUsbMidiCinSysExEnd3: return "sysex end";
    case kUsbMidiCinSysCommon2:
    case kUsbMidiCinSysCommon3:
    case kUsbMidiCinSingleByte: return systemName(message.data[0]);
    default: return "unknown";
  }
}

bool usbMidiBuildFromStatus(uint8_t cable, uint8_t status, uint8_t data1,
                            uint8_t data2, UsbMidiMessage& message) {
  if (status < 0x80u) {
    /* Not a status byte at all. Running status has no place on a USB MIDI
       endpoint: every packet carries its own status. */
    return false;
  }

  uint8_t cin;
  if (status >= 0xF8u) {
    /* System realtime: one byte, and it may legally interrupt anything. */
    cin = kUsbMidiCinSingleByte;
  } else if (status >= 0xF0u) {
    switch (status) {
      case 0xF1u:                       /* MTC quarter frame  */
      case 0xF3u: cin = kUsbMidiCinSysCommon2; break;  /* song select */
      case 0xF2u: cin = kUsbMidiCinSysCommon3; break;  /* song position */
      case 0xF6u: cin = kUsbMidiCinSingleByte; break;  /* tune request */
      default: return false;            /* 0xF0 / 0xF7 need a stream encoder */
    }
  } else {
    /* Channel voice: the Code Index Number is the status high nibble, which
       is the one place the two numbering schemes deliberately coincide. */
    cin = static_cast<uint8_t>(status >> 4);
  }

  const uint8_t length = kCinLength[cin];
  if (length == 0u) {
    return false;
  }

  message.cable = static_cast<uint8_t>(cable & 0x0Fu);
  message.cin = cin;
  message.length = length;
  message.data[0] = status;
  message.data[1] = data1;
  message.data[2] = data2;
  return true;
}

bool usbMidiEncodePacket(const UsbMidiMessage& message, uint8_t* packet) {
  if (packet == nullptr) {
    return false;
  }
  const uint8_t cin = message.cin & 0x0Fu;
  if (kCinLength[cin] == 0u) {
    return false;
  }

  packet[0] = static_cast<uint8_t>((message.cable << 4) | cin);
  /* Bytes beyond the message length are zeroed rather than left as whatever
     the buffer held: some devices inspect all four regardless. */
  packet[1] = (message.length > 0u) ? message.data[0] : 0u;
  packet[2] = (message.length > 1u) ? message.data[1] : 0u;
  packet[3] = (message.length > 2u) ? message.data[2] : 0u;
  return true;
}

void usbMidiNoteName(uint8_t note, char* buffer, uint8_t bufferSize) {
  if (buffer == nullptr || bufferSize < 5u) {
    return;
  }
  const uint8_t index = static_cast<uint8_t>(note % 12u);
  /* Note 60 is C4: dividing by twelve gives 5, so one is subtracted. The
     octave can be -1 for the lowest notes, hence the sign handling. */
  const int8_t octave = static_cast<int8_t>(static_cast<int>(note / 12u) - 1);

  uint8_t at = 0u;
  buffer[at++] = kNoteNames[index][0];
  if (kNoteNames[index][1] != '\0') {
    buffer[at++] = kNoteNames[index][1];
  }
  if (octave < 0) {
    buffer[at++] = '-';
    buffer[at++] = static_cast<char>('0' - octave);
  } else {
    buffer[at++] = static_cast<char>('0' + octave);
  }
  buffer[at] = '\0';
}
