/*
  UsbHostMidi.h - USB MIDI class driver for the Teensy 4.1 host port.

  Claims a MIDIStreaming interface, streams its bulk IN endpoint continuously
  and delivers decoded messages through a lock-free queue. Transmission on the
  bulk OUT endpoint is queued the same way, so parameter mirrors can be sent
  back to a controller without any caller ever blocking on USB.

  The driver binds at interface level rather than device level, so it
  coexists with an audio class driver on the same composite instrument. A
  device-level driver would claim the whole keyboard and lock the audio
  interfaces out.

  Threading: the USB host thread fills the queue, the consumer drains it.
  Single producer, single consumer, no locking. read() may be called from
  loop() or from the audio update, but from one of them only.
*/

#ifndef JT8000_USB_HOST_MIDI_H
#define JT8000_USB_HOST_MIDI_H

#include <atomic>
#include <cstdint>

#include "UsbMidiPacket.h"
#include "teensy4_usbhost.h"

/* Queued messages. A keybed glissando produces a few hundred messages per
   second at most, so this is several seconds of buffering even if the
   consumer stalls. */
static constexpr uint16_t kUsbMidiQueueLength = 256u;

/* Bulk reads in flight. Two means a second request is already queued when
   the first completes, so a fast burst of messages is never dropped in the
   gap between transfers. */
static constexpr uint8_t kUsbMidiReadSlots = 2u;

/* Bytes per read. Matches the endpoint's declared 64-byte maximum. */
static constexpr uint16_t kUsbMidiReadSize = 64u;

/* Outbound queue depth. A parameter resync can dump well over a hundred
   messages in one burst, so this is sized to swallow one without loss. */
static constexpr uint16_t kUsbMidiSendQueueLength = 192u;

/* Bytes per write, and therefore packets per transfer: 64/4 = 16. */
static constexpr uint16_t kUsbMidiWriteSize = 64u;

/* Cables an endpoint may carry, per the class specification. */
static constexpr uint8_t kUsbMidiMaxCables = 16u;

/* Consecutive failed transfers before the driver gives up, so a persistent
   error cannot become a resubmission spin. */
static constexpr uint8_t kUsbMidiMaxConsecutiveErrors = 8u;

class UsbHostMidi : public USB_Driver, public USB_Driver::Factory {
public:
  UsbHostMidi() = default;
  ~UsbHostMidi() override = default;

  /* True while a MIDIStreaming interface is bound and reading. */
  bool isConnected(void) const {
    return connected.load(std::memory_order_acquire);
  }

  /* Messages waiting to be read. */
  uint16_t available(void) const;

  /* Take the oldest message. Returns false when the queue is empty. */
  bool read(UsbMidiMessage& message);

  /* Discard everything queued, for use when routing changes and stale notes
     would be misleading. */
  void clear(void);

  /* ---- transmission -----------------------------------------------------

     Queue a message for the instrument. Returns false only when the queue is
     full or the message cannot be encoded; it never blocks and never touches
     USB directly, so it is safe to call from a MIDI handler.

     Nothing leaves until flushOutput() runs or a transfer already in flight
     completes, which keeps a burst of parameter mirrors in one transfer
     instead of one transfer per CC. */
  bool send(const UsbMidiMessage& message);

  /* Convenience wrappers. 'channel' is 1-16, matching how MIDI is spoken
     everywhere else in the firmware rather than the wire's 0-15. */
  bool sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
  bool sendNoteOff(uint8_t channel, uint8_t note, uint8_t velocity);
  bool sendControlChange(uint8_t channel, uint8_t cc, uint8_t value);
  bool sendProgramChange(uint8_t channel, uint8_t program);

  /* Pitch bend takes the RAW unsigned form, 0..16383 with 8192 at rest.
     Named explicitly because every Teensy MIDI transport delivers the
     centred form instead, and mixing the two is the recurring bug this
     project has been bitten by more than once. */
  bool sendPitchBendRaw(uint8_t channel, uint16_t raw14);

  /* System realtime: clock, start, continue, stop and friends. */
  bool sendRealtime(uint8_t status);

  /* Start a transfer if one is not already running. Call once per loop
     pass; cheap when there is nothing to send. */
  void flushOutput(void);

  /* Cable messages are transmitted on. The NC2x exposes one embedded input
     jack, so cable 0 is the only valid choice there. */
  void setOutputCable(uint8_t cable) { outputCable = static_cast<uint8_t>(cable & 0x0Fu); }

  uint16_t sendQueueDepth(void) const;
  uint32_t messagesSent(void) const {
    return sent.load(std::memory_order_relaxed);
  }
  uint32_t messagesUnsent(void) const {
    return unsent.load(std::memory_order_relaxed);
  }
  uint32_t sendErrors(void) const {
    return txErrors.load(std::memory_order_relaxed);
  }

  /* Accept only the cables whose bit is set: bit 0 for cable 0 and so on.
     Filtering here rather than downstream keeps unwanted traffic out of the
     queue entirely, which matters on an instrument that duplicates the same
     keypress onto several cables at different transpositions.

     This is a mechanism, not a policy: the default accepts everything, and
     the routing configuration decides what to narrow it to. */
  void setCableMask(uint16_t mask) {
    cableMask.store(mask, std::memory_order_relaxed);
  }
  uint16_t getCableMask(void) const {
    return cableMask.load(std::memory_order_relaxed);
  }

  /* Messages decoded correctly but rejected by the cable mask. */
  uint32_t messagesFiltered(void) const {
    return filtered.load(std::memory_order_relaxed);
  }

  /* Endpoints found during attach, for reporting. Zero when absent. */
  uint8_t inEndpoint(void) const { return endpointIn; }
  uint8_t outEndpoint(void) const { return endpointOut; }

  /* Cable to jack mapping declared by the endpoint's class descriptor. */
  uint8_t cableCount(void) const { return cablesIn; }
  uint8_t cableJackId(uint8_t cable) const {
    return (cable < cablesIn) ? cableJackIn[cable] : 0u;
  }

  /* Counters, all monotonic since attach. */
  uint32_t transfersCompleted(void) const {
    return transfers.load(std::memory_order_relaxed);
  }
  uint32_t bytesReceived(void) const {
    return bytes.load(std::memory_order_relaxed);
  }
  uint32_t messagesDecoded(void) const {
    return messages.load(std::memory_order_relaxed);
  }
  uint32_t messagesDropped(void) const {
    return dropped.load(std::memory_order_relaxed);
  }
  uint32_t transferErrors(void) const {
    return errors.load(std::memory_order_relaxed);
  }
  int32_t lastTransferError(void) const {
    return lastError.load(std::memory_order_relaxed);
  }
  uint32_t messagesOnCable(uint8_t cable) const {
    return (cable < kUsbMidiMaxCables)
               ? cableMessages[cable].load(std::memory_order_relaxed)
               : 0u;
  }

private:
  USB_Driver* offer(const usb_interface_descriptor* interface, size_t length,
                    const USB_Device* dev) override;

  bool attach(const usb_interface_descriptor* interface, size_t length) override;

  void detach(void) override;

  /* Walk the interface's descriptors for its bulk endpoints and the cable
     list attached to the IN endpoint. */
  bool parseInterface(const usb_interface_descriptor* interface, size_t length);

  void submitRead(uint8_t slot);
  void handleTransfer(uint8_t slot, int result);
  void push(const UsbMidiMessage& message);

  void startTransmit(void);
  uint16_t fillTransmitBuffer(void);

  uint8_t outputCable = 0u;

  uint8_t interfaceNumber = 0u;
  uint8_t endpointIn = 0u;
  uint8_t endpointOut = 0u;
  uint16_t readSize = kUsbMidiReadSize;
  uint8_t cablesIn = 0u;
  uint8_t cableJackIn[kUsbMidiMaxCables] = {0u};

  USBCallback readCallback[kUsbMidiReadSlots];
  USBCallback writeCallback;

  std::atomic_bool connected{false};
  std::atomic<uint16_t> cableMask{0xFFFFu};
  std::atomic<uint32_t> filtered{0u};
  std::atomic<uint32_t> transfers{0u};
  std::atomic<uint32_t> bytes{0u};
  std::atomic<uint32_t> messages{0u};
  std::atomic<uint32_t> dropped{0u};
  std::atomic<uint32_t> errors{0u};
  std::atomic<int32_t> lastError{0};
  std::atomic<uint32_t> cableMessages[kUsbMidiMaxCables];
  std::atomic<uint32_t> sent{0u};
  std::atomic<uint32_t> unsent{0u};
  std::atomic<uint32_t> txErrors{0u};
  /* Owns the transmit buffer and the send queue's tail while true, so only
     one context ever drains the queue even though either thread may start a
     transfer. */
  std::atomic_bool txBusy{false};

  /* Queue indices: head is written by the USB host thread, tail by the
     consumer. */
  std::atomic<uint16_t> head{0u};
  std::atomic<uint16_t> tail{0u};
  UsbMidiMessage queue[kUsbMidiQueueLength];

  /* Outbound queue: head written by callers, tail by whoever holds txBusy. */
  std::atomic<uint16_t> sendHead{0u};
  std::atomic<uint16_t> sendTail{0u};
  UsbMidiMessage sendQueue[kUsbMidiSendQueueLength];

  /* USB host thread only. */
  uint8_t consecutiveErrors = 0u;
};

#endif // JT8000_USB_HOST_MIDI_H
