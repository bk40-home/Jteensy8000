/*
  UsbAudioRate.cpp - see UsbAudioRate.h.
*/

#include "UsbAudioRate.h"

#include <cmath>

namespace {

/* A feedback reading is accepted only within this fraction of nominal.
   Expressed as a numerator over 4 to keep the test in integers. */
constexpr uint32_t kToleranceNumerator = 1u; // 1/4 = 25 percent

bool plausible(uint32_t candidateQ16, uint32_t nominalQ16) {
  if (nominalQ16 == 0u) {
    return false;
  }
  const uint32_t margin = nominalQ16 / (4u / kToleranceNumerator);
  return (candidateQ16 >= (nominalQ16 - margin)) &&
         (candidateQ16 <= (nominalQ16 + margin));
}

/* Feedback packets are little-endian and may be unaligned. */
uint32_t read24(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16);
}

uint32_t read32(const uint8_t* p) {
  return read24(p) | (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

UsbAudioFeedback usbAudioDecodeFeedback(const uint8_t* data, uint16_t length,
                                        uint32_t nominalSamplesPerFrameQ16) {
  UsbAudioFeedback result = {0u, false, UsbFeedbackFormat::None};
  if (data == nullptr || length < 3u) {
    return result;
  }

  const uint32_t raw24 = read24(data);

  /* Left-justified 10.10 within 24 bits, equivalently 10.14. The common
     case, and the one the NC2x uses: shift left by 2 to reach 16.16. */
  const uint32_t asQ10_14 = raw24 << 2;
  if (plausible(asQ10_14, nominalSamplesPerFrameQ16)) {
    result.samplesPerFrameQ16 = asQ10_14;
    result.valid = true;
    result.format = UsbFeedbackFormat::Q10_14;
    return result;
  }

  /* Right-justified 10.10: shift left by 6 instead. */
  const uint32_t asQ10_10 = raw24 << 6;
  if (plausible(asQ10_10, nominalSamplesPerFrameQ16)) {
    result.samplesPerFrameQ16 = asQ10_10;
    result.valid = true;
    result.format = UsbFeedbackFormat::Q10_10;
    return result;
  }

  /* 16.16 in four bytes, used by some full-speed devices despite the spec. */
  if (length >= 4u) {
    const uint32_t asQ16_16 = read32(data);
    if (plausible(asQ16_16, nominalSamplesPerFrameQ16)) {
      result.samplesPerFrameQ16 = asQ16_16;
      result.valid = true;
      result.format = UsbFeedbackFormat::Q16_16;
      return result;
    }
  }

  return result;
}

const char* usbFeedbackFormatName(UsbFeedbackFormat format) {
  switch (format) {
    case UsbFeedbackFormat::Q10_14: return "10.14";
    case UsbFeedbackFormat::Q10_10: return "10.10";
    case UsbFeedbackFormat::Q16_16: return "16.16";
    default: return "none";
  }
}

uint32_t usbAudioRateToQ16(uint32_t sampleRateHz) {
  /* Frames are 1 ms, so samples per frame is Hz/1000. The intermediate needs
     more than 32 bits for rates above roughly 65 kHz. */
  return static_cast<uint32_t>((static_cast<uint64_t>(sampleRateHz) << 16) /
                               1000u);
}

uint32_t usbAudioQ16ToRate(uint32_t samplesPerFrameQ16) {
  return static_cast<uint32_t>((static_cast<uint64_t>(samplesPerFrameQ16) *
                                1000u) >> 16);
}

void UsbAudioPacketPacer::reset(uint32_t samplesPerFrameQ16) {
  rateQ16 = samplesPerFrameQ16;
  /* Start half a sample in so the first frame is not systematically short. */
  accumulator = 0x8000u;
}

void UsbAudioPacketPacer::setRate(uint32_t samplesPerFrameQ16) {
  rateQ16 = samplesPerFrameQ16;
}

uint16_t UsbAudioPacketPacer::nextFrameSamples(uint16_t maxSamples) {
  accumulator += rateQ16;
  uint32_t samples = accumulator >> 16;
  accumulator &= 0xFFFFu;

  if (samples > maxSamples) {
    /* The endpoint cannot carry this frame. Dropping the excess rather than
       carrying it forward is deliberate: the surplus would otherwise queue
       up and every following frame would clip too. */
    samples = maxSamples;
  }
  if (samples == 0u) {
    samples = 1u;
  }
  return static_cast<uint16_t>(samples);
}

void usbAudioBuildSineTable(int32_t* table, uint16_t length, float amplitude) {
  if (table == nullptr || length == 0u) {
    return;
  }
  /* Full scale for a 24-bit signed sample. */
  const float peak = 8388607.0f * amplitude;
  const float step = 6.2831853f / static_cast<float>(length);
  for (uint16_t i = 0u; i < length; ++i) {
    const float phase = step * static_cast<float>(i);
    table[i] = static_cast<int32_t>(peak * sinf(phase));
  }
}
