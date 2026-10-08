/*
  UsbHostMidi.h - USB MIDI class driver for the Teensy 4.1 host port.

  Claims a MIDIStreaming interface, streams its IN endpoint continuously
  and delivers decoded messages through a lock-free queue. Transmission on the
  OUT endpoint is queued the same way, so parameter mirrors can be sent
  back to a controller without any caller ever blocking on USB.

  Endpoints may be BULK or INTERRUPT. The class specification says bulk, but
  plenty of controllers ship interrupt MIDI endpoints (the Novation Launchkey
  MK2 is one; the Studiologic NC2x is bulk). Each direction is driven with the
  transfer type its own descriptor declares.

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

/* Delay between attach() and the first transfer on either MIDI endpoint.

   attach() runs BEFORE the library has finished configuring the device: it
   queues SET_CONFIGURATION, calls the drivers, and only sends SET_INTERFACE
   for each interface once SET_CONFIGURATION completes. Both requests reset
   the device's data toggles to DATA0. A read submitted at attach time can
   complete before that reset (a controller's connect burst does exactly
   this), leaving the host expecting DATA1 while the device restarts at
   DATA0: the first transfer arrives and nothing after it ever does. That is
   the Launchkey MK2 failure, on the root port and behind a hub alike.

   Waiting until the control sequence has finished means neither side has
   moved its toggle when streaming starts. The sequence takes a few
   milliseconds; 100 ms is ample and imperceptible at plug-in. */
static constexpr uint32_t kUsbMidiStartDelayMs = 100u;

/* Endpoint transfer types, as bmAttributes & 0x03 encodes them. Zero means
   "no endpoint in this direction". */
static constexpr uint8_t kUsbMidiEpNone = 0u;
static constexpr uint8_t kUsbMidiEpBulk = 2u;
static constexpr uint8_t kUsbMidiEpInterrupt = 3u;

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
     the routing configuration decides what to narrow it to.

     This is the DEFAULT mask, used for any device without a vendor rule
     (see setCableMaskForVendor). */
  void setCableMask(uint16_t mask) {
    cableMask.store(mask, std::memory_order_relaxed);
  }
  uint16_t getCableMask(void) const {
    return cableMask.load(std::memory_order_relaxed);
  }

  /* Per-vendor override of the default mask. Cable use is a property of the
     instrument, not of the port: the Studiologic NC2x mirrors each key onto
     cables 0 and 1 and must be narrowed to one, while the Launchkey MK2 puts
     its keys on cable 0 and InControl on cable 1 and needs both. One rule is
     enough for the instruments in use; a second call replaces the first.

     Call from setup() before the host stack starts: the rule is read on the
     USB host thread without synchronisation. */
  void setCableMaskForVendor(uint16_t vendorId, uint16_t mask) {
    ruleVid = vendorId;
    ruleMask = mask;
  }

  /* Messages decoded correctly but rejected by the cable mask. */
  uint32_t messagesFiltered(void) const {
    return filtered.load(std::memory_order_relaxed);
  }

  /* Endpoints found during attach, for reporting. Zero when absent. */
  uint8_t inEndpoint(void) const { return endpointIn; }
  uint8_t outEndpoint(void) const { return endpointOut; }

  /* IN endpoint details as DECLARED by the device, for reporting.
     inType() is kUsbMidiEpBulk or kUsbMidiEpInterrupt. inInterval() is the
     raw bInterval: for a full-speed interrupt endpoint it must be 1..255,
     and 0 is the value that leaves an unpatched host library unable to
     schedule the endpoint at all (see the patch notes delivered with this
     driver). */
  uint8_t inType(void) const { return endpointInType; }
  uint8_t inInterval(void) const { return endpointInInterval; }
  uint16_t inPacketSize(void) const { return endpointInPacket; }

  /* Identity of the bound device, for reporting. Zero when none. */
  uint16_t deviceVid(void) const { return vid; }
  uint16_t devicePid(void) const { return pid; }

  /* The mask actually in force for the bound device: the vendor rule's mask
     when the device matches it, the default mask otherwise. */
  uint16_t effectiveCableMask(void) const;

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

  /* Walk the interface's descriptors for its bulk or interrupt endpoints and
     the cable list attached to the IN endpoint. */
  bool parseInterface(const usb_interface_descriptor* interface, size_t length);

  /* Issue one IN or OUT transfer with the type the endpoint declared. */
  int startInTransfer(uint8_t slot);
  int startOutTransfer(uint16_t bytes);

  /* Runs from the start-delay timer on the USB host thread: publishes
     'connected' and submits the reads. */
  void startStreams(uint32_t generation);

  void submitRead(uint8_t slot);
  void handleTransfer(uint8_t slot, int result);
  void push(const UsbMidiMessage& message);

  void startTransmit(void);
  uint16_t fillTransmitBuffer(void);

  uint8_t outputCable = 0u;

  uint8_t interfaceNumber = 0u;
  uint8_t endpointIn = 0u;
  uint8_t endpointOut = 0u;
  uint8_t endpointInType = kUsbMidiEpNone;
  uint8_t endpointOutType = kUsbMidiEpNone;
  uint8_t endpointInInterval = 0u;
  uint16_t endpointInPacket = 0u;
  uint16_t readSize = kUsbMidiReadSize;
  uint8_t cablesIn = 0u;
  uint8_t cableJackIn[kUsbMidiMaxCables] = {0u};
  uint16_t vid = 0u;
  uint16_t pid = 0u;

  /* Vendor rule; ruleVid 0 means no rule (0 is not an assigned vendor ID). */
  uint16_t ruleVid = 0u;
  uint16_t ruleMask = 0xFFFFu;

  USBCallback readCallback[kUsbMidiReadSlots];
  USBCallback writeCallback;

  /* True from a successful attach() until detach(). Used to refuse a second
     interface instead of getDevice(): when attach() fails, the library calls
     detach() but leaves the driver's device pointer set, so getDevice() stays
     non-null and every later offer() would be refused until reboot. */
  std::atomic_bool bound{false};

  /* Bumped on every attach; the start-delay timer captures it so a timer
     belonging to a device that has since gone cannot start streams on a
     newer one. USB host thread only. */
  uint32_t attachGeneration = 0u;

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
