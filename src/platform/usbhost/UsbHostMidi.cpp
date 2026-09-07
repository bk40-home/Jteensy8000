/*
  UsbHostMidi.cpp - see UsbHostMidi.h.
*/

#include "UsbHostMidi.h"

#include <Arduino.h>
#include <cstring>

namespace {

/* Descriptor types and class codes. */
constexpr uint8_t kDtEndpoint = 0x05u;
constexpr uint8_t kDtClassEndpoint = 0x25u;
constexpr uint8_t kClassAudio = 0x01u;
constexpr uint8_t kSubclassMidiStreaming = 0x03u;
constexpr uint8_t kMsGeneral = 0x01u;

constexpr uint8_t kEpTypeMask = 0x03u;
constexpr uint8_t kEpTypeBulk = 0x02u;
constexpr uint8_t kEpDirectionIn = 0x80u;

/* Read buffers live in OCRAM: the USB controller's DMA cannot reach the
   tightly coupled memory that ordinary globals occupy on a Teensy 4.x. The
   32-byte alignment and stride keep each slot on its own cache lines, so the
   library's invalidate on one buffer never discards data in another. */
DMAMEM __attribute__((aligned(32))) uint8_t
    g_readBuffer[kUsbMidiReadSlots][kUsbMidiReadSize];
DMAMEM __attribute__((aligned(32))) uint8_t g_writeBuffer[kUsbMidiWriteSize];

uint16_t read16(const uint8_t* p) {
  return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                               (static_cast<uint16_t>(p[1]) << 8));
}

} // namespace

USB_Driver* UsbHostMidi::offer(const usb_interface_descriptor* interface,
                               size_t length, const USB_Device* dev) {
  (void)length;
  (void)dev;
  /* One interface at a time: the buffers and the queue are shared. */
  if (getDevice() != nullptr) {
    return nullptr;
  }
  if (interface->bInterfaceClass != kClassAudio ||
      interface->bInterfaceSubClass != kSubclassMidiStreaming) {
    return nullptr;
  }
  return this;
}

bool UsbHostMidi::attach(const usb_interface_descriptor* interface,
                         size_t length) {
  if (!parseInterface(interface, length)) {
    return false;
  }
  if (endpointIn == 0u) {
    /* An output-only MIDI interface is legal but useless here. */
    return false;
  }

  transfers.store(0u, std::memory_order_relaxed);
  bytes.store(0u, std::memory_order_relaxed);
  messages.store(0u, std::memory_order_relaxed);
  dropped.store(0u, std::memory_order_relaxed);
  errors.store(0u, std::memory_order_relaxed);
  filtered.store(0u, std::memory_order_relaxed);
  sent.store(0u, std::memory_order_relaxed);
  unsent.store(0u, std::memory_order_relaxed);
  txErrors.store(0u, std::memory_order_relaxed);
  sendHead.store(0u, std::memory_order_relaxed);
  sendTail.store(0u, std::memory_order_relaxed);
  txBusy.store(false, std::memory_order_relaxed);
  lastError.store(0, std::memory_order_relaxed);
  for (uint8_t i = 0u; i < kUsbMidiMaxCables; ++i) {
    cableMessages[i].store(0u, std::memory_order_relaxed);
  }
  head.store(0u, std::memory_order_relaxed);
  tail.store(0u, std::memory_order_relaxed);
  consecutiveErrors = 0u;

  /* Bind every callback BEFORE publishing 'connected'.
     attach() runs on the USB host thread while loop() may already be calling
     flushOutput(), which gates on 'connected' and submits a transfer passing
     &writeCallback.  Publishing the flag first opens a window in which that
     pointer refers to a default-constructed std::function; the library then
     calls through it on completion, which with exceptions disabled is a jump
     to nothing.  It presents as an instruction access violation at an address
     in no valid memory region. */
  for (uint8_t slot = 0u; slot < kUsbMidiReadSlots; ++slot) {
    readCallback[slot] = [this, slot](int result) {
      handleTransfer(slot, result);
    };
  }

  writeCallback = [this](int result) {
    if (result < 0) {
      txErrors.fetch_add(1u, std::memory_order_relaxed);
      lastError.store(result, std::memory_order_relaxed);
    }
    /* Release ownership before looking for more work, so the next transfer
       can be started by whichever context gets there first. */
    txBusy.store(false, std::memory_order_release);
    startTransmit();
  };

  /* Callbacks are bound; publishing with release ordering makes them visible
     to the other thread before it can observe the flag. */
  connected.store(true, std::memory_order_release);

  for (uint8_t slot = 0u; slot < kUsbMidiReadSlots; ++slot) {
    submitRead(slot);
  }
  return true;
}

void UsbHostMidi::detach(void) {
  connected.store(false, std::memory_order_release);
  /* No transfer can still be running once the device is gone, so the flag is
     cleared rather than left owned by a callback that will never arrive. */
  txBusy.store(false, std::memory_order_release);
}

bool UsbHostMidi::parseInterface(const usb_interface_descriptor* interface,
                                 size_t length) {
  const uint8_t* blob = reinterpret_cast<const uint8_t*>(interface);
  interfaceNumber = interface->bInterfaceNumber;
  endpointIn = 0u;
  endpointOut = 0u;
  cablesIn = 0u;

  /* Address of the endpoint most recently seen, so that a class-specific
     endpoint descriptor can be attributed to it. */
  uint8_t lastEndpoint = 0u;

  size_t offset = 0u;
  while ((offset + 2u) <= length) {
    const uint8_t* d = &blob[offset];
    const uint8_t bLength = d[0];
    const uint8_t bType = d[1];

    /* A zero or over-long bLength would loop forever or read past the end. */
    if (bLength < 2u || (offset + bLength) > length) {
      return false;
    }

    if (bType == kDtEndpoint && bLength >= 7u) {
      const uint8_t address = d[2];
      const uint8_t attributes = d[3];
      lastEndpoint = address;
      if ((attributes & kEpTypeMask) == kEpTypeBulk) {
        if ((address & kEpDirectionIn) != 0u) {
          endpointIn = address;
          const uint16_t packet = read16(&d[4]);
          /* Never request more than the buffer holds. */
          readSize = (packet < kUsbMidiReadSize) ? packet : kUsbMidiReadSize;
        } else {
          endpointOut = address;
        }
      }
    } else if (bType == kDtClassEndpoint && bLength >= 4u && d[2] == kMsGeneral) {
      /* MS_GENERAL lists the embedded jacks this endpoint serves. The list
         order defines the cable numbers that arrive in packet byte 0. */
      if ((lastEndpoint & kEpDirectionIn) != 0u) {
        const uint8_t jacks = d[3];
        for (uint8_t i = 0u; i < jacks; ++i) {
          const size_t jackOffset = 4u + i;
          if (jackOffset >= bLength || cablesIn >= kUsbMidiMaxCables) {
            break;
          }
          cableJackIn[cablesIn++] = d[jackOffset];
        }
      }
    }

    offset += bLength;
  }

  return true;
}

void UsbHostMidi::submitRead(uint8_t slot) {
  const int r = BulkMessage(endpointIn, readSize, g_readBuffer[slot],
                            &readCallback[slot]);
  if (r < 0) {
    errors.fetch_add(1u, std::memory_order_relaxed);
    lastError.store(r, std::memory_order_relaxed);
    /* A submission that is never queued produces no callback, so this slot
       would go idle forever. Stop rather than stall silently. */
    connected.store(false, std::memory_order_release);
  }
}

void UsbHostMidi::handleTransfer(uint8_t slot, int result) {
  if (!connected.load(std::memory_order_acquire)) {
    return;
  }

  if (result < 0) {
    errors.fetch_add(1u, std::memory_order_relaxed);
    lastError.store(result, std::memory_order_relaxed);
    if (++consecutiveErrors >= kUsbMidiMaxConsecutiveErrors) {
      connected.store(false, std::memory_order_release);
      return;
    }
    submitRead(slot);
    return;
  }

  consecutiveErrors = 0u;
  transfers.fetch_add(1u, std::memory_order_relaxed);
  bytes.fetch_add(static_cast<uint32_t>(result), std::memory_order_relaxed);

  /* The endpoint delivers whole 4-byte event packets; a transfer that is not
     a multiple of four is malformed, so the remainder is ignored rather than
     read past. */
  const uint8_t* data = g_readBuffer[slot];
  const uint32_t packets = static_cast<uint32_t>(result) / 4u;
  const uint16_t mask = cableMask.load(std::memory_order_relaxed);
  UsbMidiMessage message;
  for (uint32_t i = 0u; i < packets; ++i) {
    if (!usbMidiDecodePacket(&data[i * 4u], message)) {
      continue;
    }

    /* Per-cable counters record everything that arrives, so a filtered cable
       is still visible as activity when diagnosing. Decoding happens before
       the mask test because the zero padding at the tail of a short transfer
       would otherwise be miscounted as traffic on cable 0. */
    messages.fetch_add(1u, std::memory_order_relaxed);
    if (message.cable < kUsbMidiMaxCables) {
      cableMessages[message.cable].fetch_add(1u, std::memory_order_relaxed);
    }

    if (((mask >> message.cable) & 1u) == 0u) {
      filtered.fetch_add(1u, std::memory_order_relaxed);
      continue;
    }

    push(message);
  }

  submitRead(slot);
}

void UsbHostMidi::push(const UsbMidiMessage& message) {
  const uint16_t h = head.load(std::memory_order_relaxed);
  const uint16_t next = static_cast<uint16_t>((h + 1u) % kUsbMidiQueueLength);

  /* Queue full: drop the newest and count it. Dropping the oldest instead
     would risk discarding a note on whose note off then never matches. */
  if (next == tail.load(std::memory_order_acquire)) {
    dropped.fetch_add(1u, std::memory_order_relaxed);
    return;
  }

  queue[h] = message;
  head.store(next, std::memory_order_release);
}

bool UsbHostMidi::send(const UsbMidiMessage& message) {
  /* Encoding is validated on the way in rather than at transmit time: a
     caller gets an immediate false it can act on, and the transmit path
     never has to deal with a message it cannot express. */
  uint8_t probe[4];
  if (!usbMidiEncodePacket(message, probe)) {
    unsent.fetch_add(1u, std::memory_order_relaxed);
    return false;
  }

  const uint16_t h = sendHead.load(std::memory_order_relaxed);
  const uint16_t next =
      static_cast<uint16_t>((h + 1u) % kUsbMidiSendQueueLength);
  if (next == sendTail.load(std::memory_order_acquire)) {
    unsent.fetch_add(1u, std::memory_order_relaxed);
    return false;
  }

  sendQueue[h] = message;
  sendHead.store(next, std::memory_order_release);
  return true;
}

bool UsbHostMidi::sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
  UsbMidiMessage message;
  const uint8_t status = static_cast<uint8_t>(0x90u | ((channel - 1u) & 0x0Fu));
  if (!usbMidiBuildFromStatus(outputCable, status, note, velocity, message)) {
    return false;
  }
  return send(message);
}

bool UsbHostMidi::sendNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
  UsbMidiMessage message;
  const uint8_t status = static_cast<uint8_t>(0x80u | ((channel - 1u) & 0x0Fu));
  if (!usbMidiBuildFromStatus(outputCable, status, note, velocity, message)) {
    return false;
  }
  return send(message);
}

bool UsbHostMidi::sendControlChange(uint8_t channel, uint8_t cc, uint8_t value) {
  UsbMidiMessage message;
  const uint8_t status = static_cast<uint8_t>(0xB0u | ((channel - 1u) & 0x0Fu));
  if (!usbMidiBuildFromStatus(outputCable, status, cc, value, message)) {
    return false;
  }
  return send(message);
}

bool UsbHostMidi::sendProgramChange(uint8_t channel, uint8_t program) {
  UsbMidiMessage message;
  const uint8_t status = static_cast<uint8_t>(0xC0u | ((channel - 1u) & 0x0Fu));
  if (!usbMidiBuildFromStatus(outputCable, status, program, 0u, message)) {
    return false;
  }
  return send(message);
}

bool UsbHostMidi::sendPitchBendRaw(uint8_t channel, uint16_t raw14) {
  UsbMidiMessage message;
  const uint8_t status = static_cast<uint8_t>(0xE0u | ((channel - 1u) & 0x0Fu));
  const uint8_t lsb = static_cast<uint8_t>(raw14 & 0x7Fu);
  const uint8_t msb = static_cast<uint8_t>((raw14 >> 7) & 0x7Fu);
  if (!usbMidiBuildFromStatus(outputCable, status, lsb, msb, message)) {
    return false;
  }
  return send(message);
}

bool UsbHostMidi::sendRealtime(uint8_t status) {
  UsbMidiMessage message;
  if (!usbMidiBuildFromStatus(outputCable, status, 0u, 0u, message)) {
    return false;
  }
  return send(message);
}

uint16_t UsbHostMidi::sendQueueDepth(void) const {
  const uint16_t h = sendHead.load(std::memory_order_acquire);
  const uint16_t t = sendTail.load(std::memory_order_relaxed);
  return static_cast<uint16_t>((h + kUsbMidiSendQueueLength - t) %
                               kUsbMidiSendQueueLength);
}

uint16_t UsbHostMidi::fillTransmitBuffer(void) {
  /* Only ever called by the holder of txBusy, so the tail is single-consumer
     even though either thread may be the one holding it. */
  uint16_t used = 0u;
  uint16_t t = sendTail.load(std::memory_order_relaxed);

  while ((used + 4u) <= kUsbMidiWriteSize) {
    if (t == sendHead.load(std::memory_order_acquire)) {
      break;
    }
    if (!usbMidiEncodePacket(sendQueue[t], &g_writeBuffer[used])) {
      /* Validated on the way in, so this cannot normally happen; skip rather
         than transmit a malformed packet. */
      unsent.fetch_add(1u, std::memory_order_relaxed);
    } else {
      used = static_cast<uint16_t>(used + 4u);
      sent.fetch_add(1u, std::memory_order_relaxed);
    }
    t = static_cast<uint16_t>((t + 1u) % kUsbMidiSendQueueLength);
    sendTail.store(t, std::memory_order_release);
  }

  return used;
}

void UsbHostMidi::startTransmit(void) {
  if (!connected.load(std::memory_order_acquire) || endpointOut == 0u) {
    return;
  }

  /* Claim the transmit path. Losing the race simply means someone else is
     already sending, and their completion callback will pick up whatever is
     queued behind them. */
  bool expected = false;
  if (!txBusy.compare_exchange_strong(expected, true,
                                      std::memory_order_acq_rel)) {
    return;
  }

  const uint16_t bytes = fillTransmitBuffer();
  if (bytes == 0u) {
    txBusy.store(false, std::memory_order_release);
    return;
  }

  const int r = BulkMessage(endpointOut, bytes, g_writeBuffer, &writeCallback);
  if (r < 0) {
    txErrors.fetch_add(1u, std::memory_order_relaxed);
    lastError.store(r, std::memory_order_relaxed);
    txBusy.store(false, std::memory_order_release);
  }
}

void UsbHostMidi::flushOutput(void) {
  /* A message queued between fillTransmitBuffer() finding the queue empty
     and txBusy being released waits here rather than being lost, which is
     why this is called every loop pass and not only after send(). */
  startTransmit();
}

uint16_t UsbHostMidi::available(void) const {
  const uint16_t h = head.load(std::memory_order_acquire);
  const uint16_t t = tail.load(std::memory_order_relaxed);
  return static_cast<uint16_t>((h + kUsbMidiQueueLength - t) %
                               kUsbMidiQueueLength);
}

bool UsbHostMidi::read(UsbMidiMessage& message) {
  const uint16_t t = tail.load(std::memory_order_relaxed);
  if (t == head.load(std::memory_order_acquire)) {
    return false;
  }
  message = queue[t];
  tail.store(static_cast<uint16_t>((t + 1u) % kUsbMidiQueueLength),
             std::memory_order_release);
  return true;
}

void UsbHostMidi::clear(void) {
  tail.store(head.load(std::memory_order_acquire), std::memory_order_release);
}
