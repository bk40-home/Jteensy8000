/*
  UsbDescriptorDecode.h - USB configuration descriptor decoder and survey.

  Phase 0 diagnostic for the JT-8000 USB host project. Decodes a raw USB
  configuration descriptor blob into human-readable text and extracts the
  fields that decide how a USB Audio Class 1.0 sink and a USB MIDI Streaming
  source must be driven.

  Free of Arduino and teensy4_usbhost dependencies so it can be host-compiled
  and unit-tested. Output is delivered one line at a time through a
  caller-supplied sink, so the same decoder serves Serial, a file or a test
  harness.

  Revision 2:
    - fixed: a feedback endpoint sharing an alternate setting with its data
      endpoint used to overwrite the data endpoint in the survey.
    - added: isochronous usage-type decoding, MIDI jack topology and cable
      mapping, feature unit control bitmaps, and tracing of the signal path
      that reaches a speaker terminal.
*/

#ifndef JT8000_USB_DESCRIPTOR_DECODE_H
#define JT8000_USB_DESCRIPTOR_DECODE_H

#include <cstdint>

/* Fixed capacities. Anything larger sets the survey's 'truncated' flag
   rather than overrunning: a diagnostic must never be the thing that
   crashes the board. Sized for a composite keyboard with headroom. */
static constexpr uint8_t kUsbMaxAudioAlts = 8u;   // audio streaming alt settings
static constexpr uint8_t kUsbMaxRatesPerAlt = 8u; // discrete rates per alt
static constexpr uint8_t kUsbMaxAudioUnits = 16u; // terminals + units
static constexpr uint8_t kUsbMaxMidiJacks = 12u;  // MIDI jacks
static constexpr uint8_t kUsbMaxMidiCables = 8u;  // cables per MIDI endpoint

/* Endpoint bmAttributes bits 3:2 for isochronous endpoints. This field
   decides the clocking strategy:
     Adaptive     - sink follows our data rate, we stay clock master.
     Asynchronous - sink runs on its own clock and reports its rate through a
                    feedback endpoint, so we must follow it. */
enum class UsbIsoSync : uint8_t {
  NoSync = 0u,
  Asynchronous = 1u,
  Adaptive = 2u,
  Synchronous = 3u
};

/* Endpoint bmAttributes bits 5:4. Note that some devices declare a feedback
   endpoint with usage type Data, so this must not be the only test used to
   tell a feedback endpoint from a data endpoint. */
enum class UsbIsoUsage : uint8_t {
  Data = 0u,
  Feedback = 1u,
  ImplicitFeedbackData = 2u,
  Reserved = 3u
};

/* Kind of AudioControl entity, used when tracing a signal path. */
enum class UsbAudioEntity : uint8_t {
  Unknown = 0u,
  InputTerminal,
  OutputTerminal,
  MixerUnit,
  SelectorUnit,
  FeatureUnit
};

/* One entity in the AudioControl topology. Only the fields needed to walk
   from an output terminal back to a streaming terminal are retained. */
struct UsbAudioUnit {
  uint8_t id;
  uint8_t sourceId;      // upstream entity, 0 when none or multi-source
  uint16_t terminalType; // terminals only, 0 for units
  UsbAudioEntity kind;
  uint8_t channelCount;
  uint16_t masterControls; // feature units only, bitmap of supported controls
};

/* One alternate setting of an AudioStreaming interface. Alt 0 is the
   mandatory zero-bandwidth setting and carries no endpoint. */
struct UsbAudioStreamAlt {
  uint8_t interfaceNumber;
  uint8_t alternateSetting;
  uint8_t dataEndpoint;        // 0 when the alt has no data endpoint
  uint8_t feedbackEndpoint;    // 0 when absent
  uint8_t terminalLink;        // AC terminal this stream connects to
  UsbIsoSync syncType;
  UsbIsoUsage usageType;
  uint16_t maxPacketSize;      // bytes per frame
  uint16_t feedbackPacketSize; // declared size of the feedback packet
  uint8_t interval;
  uint8_t channelCount;
  uint8_t subframeBytes;       // bytes per sample per channel
  uint8_t bitResolution;       // significant bits within the subframe
  uint16_t formatTag;          // 0x0001 = PCM
  bool hasSamplingFreqControl; // endpoint accepts SET_CUR SAMPLING_FREQ
  bool continuousRates;        // true: rates[0]=lower, rates[1]=upper
  uint8_t rateCount;
  uint32_t rates[kUsbMaxRatesPerAlt];
};

/* One MIDI jack. Embedded jacks face the USB endpoints; external jacks face
   physical DIN sockets or the instrument itself. */
struct UsbMidiJack {
  uint8_t id;
  bool isOutput;   // OUT jack (device to host direction of data flow)
  bool embedded;   // false = external
  uint8_t sourceId;  // OUT jacks: first input pin's source entity
  uint8_t sourcePin; // OUT jacks: first input pin number
};

/* The MIDIStreaming interface, its bulk endpoints and its cable mapping.
   Cable numbers are the index into the endpoint's associated jack list. */
struct UsbMidiInterfaceInfo {
  bool found;
  uint8_t interfaceNumber;
  uint8_t alternateSetting;
  uint8_t inEndpoint;  // device to host, 0 when absent
  uint8_t outEndpoint; // host to device, 0 when absent
  uint16_t inMaxPacketSize;
  uint16_t outMaxPacketSize;
  uint8_t inCableCount;
  uint8_t outCableCount;
  uint8_t inCableJackId[kUsbMaxMidiCables];
  uint8_t outCableJackId[kUsbMaxMidiCables];
  uint8_t jackCount;
  UsbMidiJack jacks[kUsbMaxMidiJacks];
};

/* Result of tracing an output terminal of type Speaker back to the USB
   streaming input terminal that feeds it. This identifies, without
   guesswork, which AudioStreaming interface reaches the loudspeakers. */
struct UsbSpeakerPath {
  bool found;
  uint8_t speakerTerminalId;
  uint8_t streamTerminalId;
  uint8_t featureUnitId;   // 0 when the path has no feature unit
  bool hasVolumeControl;
  bool hasMuteControl;
  uint8_t asInterface;     // AudioStreaming interface carrying the stream
  uint8_t asAlternate;     // alternate setting with the data endpoint
  bool asInterfaceFound;
};

/* Everything Phase 0 needs to answer, in one struct. */
struct UsbDescriptorSurvey {
  UsbMidiInterfaceInfo midi;
  uint8_t audioControlInterface; // 0xFF when absent
  bool audioControlFound;
  uint8_t altCount;
  UsbAudioStreamAlt alts[kUsbMaxAudioAlts];
  uint8_t unitCount;
  UsbAudioUnit units[kUsbMaxAudioUnits];
  UsbSpeakerPath speaker;
  bool truncated; // capacity exceeded, results incomplete
  bool malformed; // a descriptor had an impossible bLength
};

/* Line sink. 'text' is null-terminated and carries no trailing newline;
   the sink adds whatever line ending its transport needs. */
typedef void (*UsbDescOutFn)(void* context, const char* text);

/* Decode and print a device descriptor (18 bytes). */
void usbDescDumpDevice(const uint8_t* descriptor, uint16_t length,
                       UsbDescOutFn out, void* context);

/* Decode and print a complete configuration descriptor blob, including
   class-specific Audio Control, Audio Streaming and MIDI Streaming
   descriptors. 'length' should be wTotalLength. */
void usbDescDumpConfiguration(const uint8_t* descriptor, uint16_t length,
                              UsbDescOutFn out, void* context);

/* Walk the same blob and fill the survey. Never prints. The survey is large;
   prefer a static instance over a stack local on constrained threads. */
void usbDescSurvey(const uint8_t* descriptor, uint16_t length,
                   UsbDescriptorSurvey& survey);

/* Print the survey as a compact decision table. */
void usbDescDumpSurvey(const UsbDescriptorSurvey& survey, UsbDescOutFn out,
                       void* context);

/* Human-readable field names. */
const char* usbDescSyncTypeName(UsbIsoSync sync);
const char* usbDescUsageTypeName(UsbIsoUsage usage);
const char* usbDescTerminalTypeName(uint16_t terminalType);

#endif // JT8000_USB_DESCRIPTOR_DECODE_H
