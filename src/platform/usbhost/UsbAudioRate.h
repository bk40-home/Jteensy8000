/*
  UsbAudioRate.h - rate and format arithmetic for USB Audio Class streaming.

  Kept free of Arduino and teensy4_usbhost dependencies so the value laws can
  be host-compiled and tested. Everything on the per-frame path is integer:
  the only floating point is the one-off construction of the tone table.

  Sample rates are carried as 16.16 fixed point samples per USB frame, which
  is both the natural unit for choosing packet sizes and the native format of
  a high-speed feedback endpoint. At 48 kHz full speed the nominal value is
  48.0, or 0x00300000.
*/

#ifndef JT8000_USB_AUDIO_RATE_H
#define JT8000_USB_AUDIO_RATE_H

#include <cstdint>

/* How a feedback packet turned out to be encoded. Recorded so that a device
   can be characterised once rather than guessed at on every reading. */
enum class UsbFeedbackFormat : uint8_t {
  None = 0u,
  Q10_14 = 1u, /* 3 bytes, 10.10 left-justified in the 24-bit field */
  Q10_10 = 2u, /* 3 bytes, 10.10 right-justified */
  Q16_16 = 3u  /* 4 bytes */
};

/* Result of decoding an isochronous feedback packet. */
struct UsbAudioFeedback {
  uint32_t samplesPerFrameQ16; // 16.16 samples per frame
  bool valid;                  // false when no plausible reading was found
  UsbFeedbackFormat format;
};

/* Decode a feedback packet.

   USB 2.0 specifies 10.10 format in three bytes for full-speed endpoints and
   16.16 in four bytes for high speed, but the three-byte case is ambiguous
   in practice: most devices left-justify the 10.10 value within the 24-bit
   field, making the payload effectively 10.14, while some right-justify it.
   A Studiologic NC2x reporting 48.000 kHz sends 0x0C0000, which is
   48 x 2^14, so it is one of the left-justifying majority.

   All three interpretations are tried and the one that lands near nominal
   wins. The orderings cannot collide: a right-justified value decodes to a
   quarter of nominal under the left-justified rule, and a left-justified one
   decodes to sixteen times nominal under the other, so both are rejected on
   plausibility. A reading more than 25 percent from nominal is discarded
   rather than acted on, since following a bad rate empties or floods the
   device's buffer within a second. */
UsbAudioFeedback usbAudioDecodeFeedback(const uint8_t* data, uint16_t length,
                                        uint32_t nominalSamplesPerFrameQ16);

/* Convert a sample rate in Hz to 16.16 samples per 1 ms USB frame. */
uint32_t usbAudioRateToQ16(uint32_t sampleRateHz);

/* Convert 16.16 samples per frame back to Hz, for reporting. */
uint32_t usbAudioQ16ToRate(uint32_t samplesPerFrameQ16);

/* Chooses how many samples to put in each frame so that the long-run average
   matches the requested rate. A 44.1 kHz stream becomes the classic
   45,44,44,... pattern with no floating point and no lookup table. */
class UsbAudioPacketPacer {
public:
  void reset(uint32_t samplesPerFrameQ16);

  /* Change the target rate without disturbing the fractional accumulator. */
  void setRate(uint32_t samplesPerFrameQ16);

  uint32_t rate(void) const { return rateQ16; }

  /* Samples for the next frame, clamped to what the endpoint can carry. */
  uint16_t nextFrameSamples(uint16_t maxSamples);

private:
  uint32_t rateQ16 = 0u;
  uint32_t accumulator = 0u;
};

/* Write one 24-bit sample in little-endian order. The USB wire format is
   packed, so there is no padding byte between samples. */
inline void usbAudioPack24(uint8_t* destination, int32_t sample) {
  destination[0] = static_cast<uint8_t>(sample & 0xFFu);
  destination[1] = static_cast<uint8_t>((sample >> 8) & 0xFFu);
  destination[2] = static_cast<uint8_t>((sample >> 16) & 0xFFu);
}

/* Build one cycle of a sine wave as 24-bit signed samples. Called once at
   startup: this is the only floating point in the module, and choosing a
   table length that divides the sample rate exactly means the tone can be
   played back by indexing alone, with no phase accumulator arithmetic and no
   discontinuity at the wrap. */
void usbAudioBuildSineTable(int32_t* table, uint16_t length, float amplitude);

/* Human-readable feedback format, for reporting. */
const char* usbFeedbackFormatName(UsbFeedbackFormat format);

#endif // JT8000_USB_AUDIO_RATE_H
