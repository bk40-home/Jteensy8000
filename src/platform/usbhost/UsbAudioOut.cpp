/*
  UsbAudioOut.cpp - see UsbAudioOut.h.
*/

#include "UsbAudioOut.h"

#include <Arduino.h>
#include <cstring>

namespace {

constexpr uint8_t kClassAudio = 0x01u;
constexpr uint8_t kSubclassAudioStreaming = 0x02u;

constexpr uint8_t kAudioSetCur = 0x01u;
constexpr uint16_t kSamplingFreqControl = 0x0100u;
constexpr uint16_t kMuteControl = 0x0100u;

constexpr uint8_t kReqTypeEndpointClassSet = USB_CTRLTYPE_DIR_HOST2DEVICE |
                                             USB_CTRLTYPE_TYPE_CLASS |
                                             USB_CTRLTYPE_REC_ENDPOINT;
constexpr uint8_t kReqTypeInterfaceClassSet = USB_CTRLTYPE_DIR_HOST2DEVICE |
                                              USB_CTRLTYPE_TYPE_CLASS |
                                              USB_CTRLTYPE_REC_INTERFACE;

/* Buffers the USB controller reads or writes live in OCRAM: DMA cannot reach
   the tightly coupled memory that ordinary globals occupy on a Teensy 4.x.
   The 32-byte alignment keeps the library's clean-and-invalidate off cache
   lines belonging to anything else. */
DMAMEM __attribute__((aligned(32))) uint8_t
    g_slotBuffer[kAudioSlotCount][kAudioFramesPerSlot * kAudioMaxFrameBytes];
DMAMEM __attribute__((aligned(32))) uint8_t g_feedbackBuffer[32];
DMAMEM __attribute__((aligned(32))) uint8_t g_controlBuffer[32];
DMAMEM __attribute__((aligned(32))) uint8_t g_configBuffer[kAudioConfigBufferSize];

/* The survey is nearly 700 bytes and the USB host thread has a 1 kB stack. */
UsbDescriptorSurvey g_survey;

/* Scratch for one frame of samples pulled from the source, kept off the USB
   host thread's stack for the same reason. */
int32_t g_scratch[kAudioMaxSamplesPerFrame * 2u];

void printResult(const char* label, int32_t result) {
  Serial.print(label);
  if (result == kAudioResultPending) {
    Serial.println(F("pending"));
  } else if (result == kAudioResultSkipped) {
    Serial.println(F("skipped"));
  } else if (result < 0) {
    Serial.print(F("FAILED "));
    Serial.println(result);
  } else {
    Serial.print(F("ok ("));
    Serial.print(result);
    Serial.println(F(")"));
  }
}

} // namespace

USB_Driver* UsbAudioOut::offer(const usb_interface_descriptor* interface,
                               size_t length, const USB_Device* dev) {
  (void)length;
  (void)dev;
  if (getDevice() != nullptr) {
    return nullptr;
  }
  if (interface->bInterfaceClass != kClassAudio ||
      interface->bInterfaceSubClass != kSubclassAudioStreaming) {
    return nullptr;
  }
  return this;
}

bool UsbAudioOut::attach(const usb_interface_descriptor* interface,
                         size_t length) {
  (void)length;
  boundInterface = interface->bInterfaceNumber;
  attached.store(true, std::memory_order_release);

  /* Alternate setting 0 of a streaming interface carries no endpoints, so
     the descriptors handed to attach say nothing useful. Fetch the whole
     configuration and work from that. */
  requestConfiguration();
  return true;
}

void UsbAudioOut::detach(void) {
  streaming.store(false, std::memory_order_release);
  planValid.store(false, std::memory_order_release);
  attached.store(false, std::memory_order_release);
}

void UsbAudioOut::requestConfiguration(void) {
  const int r = ControlMessage(
      USB_REQTYPE_DEVICE_GET, USB_REQ_GET_DESCRIPTOR,
      static_cast<uint16_t>(USB_DT_CONFIGURATION << 8), 0u,
      kAudioConfigBufferSize, g_configBuffer, [this](int result) {
        resultConfig.store(result, std::memory_order_relaxed);
        if (result < 9) {
          return;
        }
        if (planFromConfiguration(static_cast<uint16_t>(result))) {
          planValid.store(true, std::memory_order_release);
          selectAlternate();
        }
      });
  if (r < 0) {
    resultConfig.store(r, std::memory_order_relaxed);
  }
}

bool UsbAudioOut::planFromConfiguration(uint16_t length) {
  /* The device returns at most what was asked for, so trust the smaller of
     the transfer length and the descriptor's own total. */
  const uint16_t declared = static_cast<uint16_t>(
      static_cast<uint16_t>(g_configBuffer[2]) |
      (static_cast<uint16_t>(g_configBuffer[3]) << 8));
  const uint16_t total = (declared < length) ? declared : length;

  usbDescSurvey(g_configBuffer, total, g_survey);

  if (!g_survey.speaker.found || !g_survey.speaker.asInterfaceFound) {
    return false;
  }

  asInterface = g_survey.speaker.asInterface;
  asAlternate = g_survey.speaker.asAlternate;
  acInterface = g_survey.audioControlInterface;
  featureUnit = g_survey.speaker.featureUnitId;

  const UsbAudioStreamAlt* alt = nullptr;
  for (uint8_t i = 0u; i < g_survey.altCount; ++i) {
    if (g_survey.alts[i].interfaceNumber == asInterface &&
        g_survey.alts[i].alternateSetting == asAlternate) {
      alt = &g_survey.alts[i];
      break;
    }
  }
  if (alt == nullptr || alt->dataEndpoint == 0u) {
    return false;
  }

  dataEndpoint = alt->dataEndpoint;
  feedbackEndpoint = alt->feedbackEndpoint;
  channels = alt->channelCount;
  subframeBytes = alt->subframeBytes;
  declaredPacketSize = alt->maxPacketSize;
  sampleRateHz = (alt->rateCount > 0u) ? alt->rates[0] : 0u;

  /* Only 24-bit packing is implemented, and the buffers are fixed. */
  if (channels == 0u || channels > 2u || subframeBytes != 3u ||
      sampleRateHz == 0u || alt->maxPacketSize > kAudioMaxFrameBytes) {
    return false;
  }

  bytesPerSampleFrame = static_cast<uint16_t>(channels * subframeBytes);
  maxSamplesPerFrame =
      static_cast<uint16_t>(alt->maxPacketSize / bytesPerSampleFrame);
  if (maxSamplesPerFrame > kAudioMaxSamplesPerFrame) {
    maxSamplesPerFrame = kAudioMaxSamplesPerFrame;
  }
  nominalQ16 = usbAudioRateToQ16(sampleRateHz);
  pacer.reset(nominalQ16);
  return true;
}

void UsbAudioOut::selectAlternate(void) {
  const int r = ControlMessage(USB_REQTYPE_INTERFACE_SET, USB_REQ_SET_INTERFACE,
                               asAlternate, asInterface, [this](int result) {
                                 resultSetInterface.store(
                                     result, std::memory_order_relaxed);
                                 if (result >= 0) {
                                   setSampleRate();
                                 }
                               });
  if (r < 0) {
    resultSetInterface.store(r, std::memory_order_relaxed);
  }
}

void UsbAudioOut::setSampleRate(void) {
  g_controlBuffer[0] = static_cast<uint8_t>(sampleRateHz & 0xFFu);
  g_controlBuffer[1] = static_cast<uint8_t>((sampleRateHz >> 8) & 0xFFu);
  g_controlBuffer[2] = static_cast<uint8_t>((sampleRateHz >> 16) & 0xFFu);

  const int r = ControlMessage(
      kReqTypeEndpointClassSet, kAudioSetCur, kSamplingFreqControl,
      dataEndpoint, 3u, g_controlBuffer, [this](int result) {
        resultSetRate.store(result, std::memory_order_relaxed);
        clearMute();
      });
  if (r < 0) {
    resultSetRate.store(r, std::memory_order_relaxed);
    clearMute();
  }
}

void UsbAudioOut::clearMute(void) {
  if (featureUnit == 0u || acInterface == 0xFFu) {
    resultClearMute.store(kAudioResultSkipped, std::memory_order_relaxed);
    startStreaming();
    return;
  }

  g_controlBuffer[4] = 0u;
  const uint16_t index = static_cast<uint16_t>(
      (static_cast<uint16_t>(featureUnit) << 8) | acInterface);
  const int r = ControlMessage(kReqTypeInterfaceClassSet, kAudioSetCur,
                               kMuteControl, index, 1u, &g_controlBuffer[4],
                               [this](int result) {
                                 resultClearMute.store(
                                     result, std::memory_order_relaxed);
                                 startStreaming();
                               });
  if (r < 0) {
    resultClearMute.store(r, std::memory_order_relaxed);
    startStreaming();
  }
}

void UsbAudioOut::startStreaming(void) {
  /* DMAMEM is not cleared at startup, so silence has to be written once. */
  memset(g_slotBuffer, 0, sizeof(g_slotBuffer));
  memset(g_feedbackBuffer, 0, sizeof(g_feedbackBuffer));

  pacer.reset(nominalQ16);

  /* Bind every callback BEFORE publishing 'streaming'.  A completion on one
     slot can re-enter while another slot's callback is still unbound, and the
     library calls through the pointer it was given: an empty std::function
     there is a jump to nothing.  Same reasoning as UsbHostMidi::attach. */
  for (uint8_t slot = 0u; slot < kAudioSlotCount; ++slot) {
    slotCallback[slot] = [this, slot](int result) {
      completions.fetch_add(1u, std::memory_order_relaxed);
      if (result < 0) {
        errors.fetch_add(1u, std::memory_order_relaxed);
        lastError.store(result, std::memory_order_relaxed);
      } else {
        bytesSent.fetch_add(static_cast<uint32_t>(result),
                            std::memory_order_relaxed);
      }
      if (streaming.load(std::memory_order_acquire)) {
        submitSlot(slot);
      }
    };
  }

  feedbackCallback = [this](int result) {
    if (result > 0) {
      const UsbAudioFeedback fb = usbAudioDecodeFeedback(
          g_feedbackBuffer, static_cast<uint16_t>(result), nominalQ16);
      if (fb.valid) {
        feedbackQ16.store(fb.samplesPerFrameQ16, std::memory_order_relaxed);
        pacer.setRate(fb.samplesPerFrameQ16);
      }
    }
    if (streaming.load(std::memory_order_acquire)) {
      submitFeedback();
    }
  };

  streaming.store(true, std::memory_order_release);

  for (uint8_t slot = 0u; slot < kAudioSlotCount; ++slot) {
    submitSlot(slot);
  }
  if (feedbackEndpoint != 0u) {
    submitFeedback();
  }
}

void UsbAudioOut::fillSlot(uint8_t slot) {
  uint8_t* write = g_slotBuffer[slot];

  for (uint8_t frame = 0u; frame < kAudioFramesPerSlot; ++frame) {
    const uint16_t samples = pacer.nextFrameSamples(maxSamplesPerFrame);
    const uint16_t bytes = static_cast<uint16_t>(samples * bytesPerSampleFrame);
    slotLengths[slot][frame] = static_cast<int16_t>(bytes);

    if (fillFunction == nullptr) {
      /* No source: the buffer was zeroed at stream start and nothing has
         written to it since, so only the pointer needs to move. */
      write += bytes;
      continue;
    }

    fillFunction(fillContext, g_scratch, samples, channels);

    const uint16_t values = static_cast<uint16_t>(samples * channels);
    for (uint16_t i = 0u; i < values; ++i) {
      usbAudioPack24(write, g_scratch[i]);
      write += subframeBytes;
    }
  }
}

void UsbAudioOut::submitSlot(uint8_t slot) {
  fillSlot(slot);

  const int r = IsochronousMessage(dataEndpoint, slotLengths[slot],
                                   g_slotBuffer[slot], &slotCallback[slot]);
  if (r < 0) {
    errors.fetch_add(1u, std::memory_order_relaxed);
    lastError.store(r, std::memory_order_relaxed);
    /* A submission that is never queued produces no callback, so this slot
       would go idle forever. Stop rather than stall silently. */
    streaming.store(false, std::memory_order_release);
    return;
  }
  framesSubmitted.fetch_add(kAudioFramesPerSlot, std::memory_order_relaxed);
}

void UsbAudioOut::submitFeedback(void) {
  feedbackLengths[0] = 4;
  feedbackLengths[1] = 0;
  IsochronousMessage(feedbackEndpoint, feedbackLengths, g_feedbackBuffer,
                     &feedbackCallback);
}

void UsbAudioOut::printPlan(void) {
  Serial.println();
  Serial.print(F("audio: bound to interface "));
  Serial.println(boundInterface);

  if (!planValid.load(std::memory_order_acquire)) {
    Serial.print(F("audio: no playback plan (config fetch result "));
    Serial.print(resultConfig.load(std::memory_order_relaxed));
    Serial.println(F(")"));
    return;
  }

  Serial.print(F("audio: streaming interface "));
  Serial.print(asInterface);
  Serial.print(F(" alt "));
  Serial.print(asAlternate);
  Serial.print(F(", EP 0x"));
  Serial.print(dataEndpoint, HEX);
  Serial.print(F(", feedback 0x"));
  Serial.println(feedbackEndpoint, HEX);

  Serial.print(F("       "));
  Serial.print(sampleRateHz);
  Serial.print(F(" Hz, "));
  Serial.print(channels);
  Serial.print(F(" ch, "));
  Serial.print(static_cast<unsigned>(subframeBytes) * 8u);
  Serial.print(F("-bit, packet "));
  Serial.print(declaredPacketSize);
  Serial.println(F(" B"));
}

void UsbAudioOut::printSetup(void) {
  printResult("  GET config     : ", resultConfig.load(std::memory_order_relaxed));
  printResult("  SET_INTERFACE  : ", resultSetInterface.load(std::memory_order_relaxed));
  printResult("  SET_CUR rate   : ", resultSetRate.load(std::memory_order_relaxed));
  printResult("  clear mute     : ", resultClearMute.load(std::memory_order_relaxed));
}

void UsbAudioOut::report(uint32_t intervalMs) {
  const uint32_t now = millis();
  if ((now - reportedAtMs) < intervalMs) {
    return;
  }
  reportedAtMs = now;

  if (!attached.load(std::memory_order_acquire)) {
    planPrinted = false;
    setupPrinted = false;
    return;
  }

  if (!planPrinted) {
    printPlan();
    planPrinted = true;
  }
  if (!setupPrinted &&
      (resultClearMute.load(std::memory_order_relaxed) != kAudioResultPending)) {
    printSetup();
    setupPrinted = true;
  }
  if (!planValid.load(std::memory_order_acquire)) {
    return;
  }

  const uint32_t frames = framesSubmitted.load(std::memory_order_relaxed);
  Serial.print(F("[audio frames "));
  Serial.print(frames - lastFrames);
  Serial.print(F("  transfers "));
  Serial.print(completions.load(std::memory_order_relaxed));
  Serial.print(F("  errors "));
  Serial.print(errors.load(std::memory_order_relaxed));
  const uint32_t q16 = feedbackQ16.load(std::memory_order_relaxed);
  if (q16 != 0u) {
    Serial.print(F("  feedback "));
    Serial.print(usbAudioQ16ToRate(q16));
    Serial.print(F(" Hz"));
  }
  Serial.println(F("]"));

  lastFrames = frames;
}
