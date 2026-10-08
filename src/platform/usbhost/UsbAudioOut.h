/*
  UsbAudioOut.h - USB Audio Class 1.0 playback driver for the Teensy 4.1
  host port.

  Derived from the Phase 0 smoke test, with two changes that matter.

  It binds at interface level rather than device level, so it coexists with
  the MIDI driver on the same composite instrument. A device-level driver
  claims the whole keyboard and locks every other interface out.

  Because the library offers only alternate setting 0 of each interface, and
  a UAC1 streaming interface's alternate 0 is the mandatory zero-bandwidth
  setting with no endpoints at all, the driver fetches the full configuration
  descriptor itself during attach. That also recovers the audio control
  interface and feature unit, which an interface-level attach cannot see.

  Audio is pulled, not pushed: the driver asks a caller-supplied function for
  samples as it fills each isochronous buffer. That callback runs on the USB
  host thread.
*/

#ifndef JT8000_USB_AUDIO_OUT_H
#define JT8000_USB_AUDIO_OUT_H

#include <atomic>
#include <cstdint>

#include "UsbAudioRate.h"
#include "UsbDescriptorDecode.h"
#include "teensy4_usbhost.h"

/* Transfers in flight, and frames per transfer. Eight frames is the
   library's maximum per isochronous call. */
static constexpr uint8_t kAudioSlotCount = 3u;
static constexpr uint8_t kAudioFramesPerSlot = 8u;

/* Upper bounds used to size buffers. */
static constexpr uint16_t kAudioMaxFrameBytes = 384u;
static constexpr uint16_t kAudioMaxSamplesPerFrame = 64u;
static constexpr uint16_t kAudioConfigBufferSize = 512u;

/* Sentinels for control transfer results. */
static constexpr int32_t kAudioResultPending = 0x7FFFFFFF;
static constexpr int32_t kAudioResultSkipped = 0x7FFFFFFE;

/* Consecutive failed isochronous completions before streaming is abandoned.
   Isochronous traffic is never retried by the controller, so an occasional
   error on a live device is normal and must not stop the stream; a long
   unbroken run of them means the device is not there any more. 64 slots is
   ~0.5 s of audio at 8 frames per slot. */
static constexpr uint8_t kAudioMaxConsecutiveErrors = 64u;

/* Fills 'frames' sample frames of interleaved 24-bit signed samples. Called
   from the USB host thread, once per eight frames, so it must not block. */
typedef void (*UsbAudioFillFn)(void* context, int32_t* interleaved,
                               uint16_t frames, uint8_t channels);

class UsbAudioOut : public USB_Driver, public USB_Driver::Factory {
public:
  UsbAudioOut() = default;
  ~UsbAudioOut() override = default;

  /* Register the audio source. Set this before a device attaches; with no
     source the driver streams silence. */
  void setSource(UsbAudioFillFn fill, void* context) {
    fillContext = context;
    fillFunction = fill;
  }

  bool isStreaming(void) const {
    return streaming.load(std::memory_order_acquire);
  }
  bool isPlanValid(void) const {
    return planValid.load(std::memory_order_acquire);
  }

  /* Print the plan and setup results once, then a statistics line. Call from
     loop(); it rate-limits itself. */
  void report(uint32_t intervalMs);

  /* Format the attached device asked for, valid once streaming has begun.
     Zero until then: the descriptors are fetched asynchronously, so a caller
     must latch this rather than read it once at startup. */
  uint32_t deviceSampleRate(void) const { return sampleRateHz; }
  uint8_t deviceChannels(void) const { return channels; }

  uint32_t framesSent(void) const {
    return framesSubmitted.load(std::memory_order_relaxed);
  }
  uint32_t transferErrors(void) const {
    return errors.load(std::memory_order_relaxed);
  }

private:
  USB_Driver* offer(const usb_interface_descriptor* interface, size_t length,
                    const USB_Device* dev) override;
  bool attach(const usb_interface_descriptor* interface, size_t length) override;
  void detach(void) override;

  void requestConfiguration(void);
  bool planFromConfiguration(uint16_t length);
  void selectAlternate(void);
  void setSampleRate(void);
  void clearMute(void);
  void startStreaming(void);

  void fillSlot(uint8_t slot);
  void submitSlot(uint8_t slot);
  void submitFeedback(void);

  /* Common completion bookkeeping for data and feedback transfers. Returns
     true when the transfer may be resubmitted, false when streaming has been
     stopped. USB host thread only. */
  bool completionAllowsResubmit(int result);

  void printPlan(void);
  void printSetup(void);

  /* Interface we were offered, which is not necessarily the one that reaches
     the speakers. Control requests are addressed to the device, so the
     driver may operate whichever streaming interface the descriptors
     identify regardless of which one it was bound to. */
  uint8_t boundInterface = 0u;

  uint8_t asInterface = 0u;
  uint8_t asAlternate = 0u;
  uint8_t dataEndpoint = 0u;
  uint8_t feedbackEndpoint = 0u;
  uint8_t acInterface = 0xFFu;
  uint8_t featureUnit = 0u;
  uint8_t channels = 0u;
  uint8_t subframeBytes = 0u;
  uint16_t bytesPerSampleFrame = 0u;
  uint16_t maxSamplesPerFrame = 0u;
  uint16_t declaredPacketSize = 0u;
  uint32_t sampleRateHz = 0u;
  uint32_t nominalQ16 = 0u;

  UsbAudioPacketPacer pacer;

  isolength slotLengths[kAudioSlotCount];
  isolength feedbackLengths;
  USBCallback slotCallback[kAudioSlotCount];
  USBCallback feedbackCallback;

  UsbAudioFillFn fillFunction = nullptr;
  void* fillContext = nullptr;

  std::atomic_bool attached{false};
  std::atomic_bool planValid{false};
  std::atomic_bool streaming{false};
  std::atomic<int32_t> resultConfig{kAudioResultPending};
  std::atomic<int32_t> resultSetInterface{kAudioResultPending};
  std::atomic<int32_t> resultSetRate{kAudioResultPending};
  std::atomic<int32_t> resultClearMute{kAudioResultPending};
  std::atomic<uint32_t> framesSubmitted{0u};
  std::atomic<uint32_t> bytesSent{0u};
  std::atomic<uint32_t> completions{0u};
  std::atomic<uint32_t> errors{0u};
  std::atomic<int32_t> lastError{0};
  std::atomic<uint32_t> feedbackQ16{0u};

  /* Bumped on every attach so report() can tell a fresh device from the one
     it already described, even when unplug and replug both happen inside one
     report interval and it never saw the detached state in between. */
  std::atomic<uint32_t> attachGeneration{0u};

  /* USB host thread only. */
  uint8_t consecutiveErrors = 0u;

  /* Main thread only. */
  uint32_t reportedAtMs = 0u;
  uint32_t lastFrames = 0u;
  uint32_t reportedGeneration = 0u;
  bool planPrinted = false;
  bool setupPrinted = false;
};

#endif // JT8000_USB_AUDIO_OUT_H
