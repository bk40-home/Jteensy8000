/*
  UsbDescriptorDecode.cpp - see UsbDescriptorDecode.h.
*/

#include "UsbDescriptorDecode.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

/* Standard descriptor types. */
constexpr uint8_t kDtDevice = 0x01u;
constexpr uint8_t kDtConfiguration = 0x02u;
constexpr uint8_t kDtInterface = 0x04u;
constexpr uint8_t kDtEndpoint = 0x05u;
constexpr uint8_t kDtInterfaceAssociation = 0x0Bu;
constexpr uint8_t kDtClassInterface = 0x24u; // CS_INTERFACE
constexpr uint8_t kDtClassEndpoint = 0x25u;  // CS_ENDPOINT

/* Interface class/subclass codes we care about. */
constexpr uint8_t kClassAudio = 0x01u;
constexpr uint8_t kSubclassAudioControl = 0x01u;
constexpr uint8_t kSubclassAudioStreaming = 0x02u;
constexpr uint8_t kSubclassMidiStreaming = 0x03u;

/* AudioControl class-specific subtypes. */
constexpr uint8_t kAcHeader = 0x01u;
constexpr uint8_t kAcInputTerminal = 0x02u;
constexpr uint8_t kAcOutputTerminal = 0x03u;
constexpr uint8_t kAcMixerUnit = 0x04u;
constexpr uint8_t kAcSelectorUnit = 0x05u;
constexpr uint8_t kAcFeatureUnit = 0x06u;

/* AudioStreaming class-specific subtypes. */
constexpr uint8_t kAsGeneral = 0x01u;
constexpr uint8_t kAsFormatType = 0x02u;

/* MIDIStreaming class-specific subtypes. */
constexpr uint8_t kMsHeader = 0x01u;
constexpr uint8_t kMsMidiInJack = 0x02u;
constexpr uint8_t kMsMidiOutJack = 0x03u;
constexpr uint8_t kMsElement = 0x04u;
constexpr uint8_t kMsGeneral = 0x01u; // CS_ENDPOINT subtype

/* Endpoint bmAttributes fields. */
constexpr uint8_t kEpTypeMask = 0x03u;
constexpr uint8_t kEpTypeIsochronous = 0x01u;
constexpr uint8_t kEpTypeBulk = 0x02u;
constexpr uint8_t kEpSyncShift = 2u;
constexpr uint8_t kEpSyncMask = 0x03u;
constexpr uint8_t kEpUsageShift = 4u;
constexpr uint8_t kEpUsageMask = 0x03u;

/* Terminal types used when tracing the playback path. */
constexpr uint16_t kTerminalUsbStreaming = 0x0101u;
constexpr uint16_t kTerminalOutputGroup = 0x0300u; // 0x03xx: speaker family

/* A feedback endpoint carries a 3 or 4 byte rate value, never audio. Some
   devices (the Studiologic NC2x among them) declare usage type Data on their
   feedback endpoint, so packet size is used as a corroborating test. */
constexpr uint16_t kMaxFeedbackPacketSize = 4u;

/* Descriptors are byte-packed and may sit at any alignment, so multi-byte
   fields are assembled a byte at a time rather than cast through a pointer. */
inline uint16_t read16(const uint8_t* p) {
  return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                               (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t read24(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16);
}

/* Format a single line and hand it to the sink. The buffer is deliberately
   modest: the USB host thread runs on a 1 kB stack. */
__attribute__((format(printf, 3, 4)))
void emit(UsbDescOutFn out, void* context, const char* format, ...) {
  if (out == nullptr) {
    return;
  }
  char line[128];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  out(context, line);
}

const char* endpointTypeName(uint8_t attributes) {
  switch (attributes & kEpTypeMask) {
    case 0u: return "control";
    case 1u: return "isochronous";
    case 2u: return "bulk";
    default: return "interrupt";
  }
}

const char* interfaceClassName(uint8_t cls, uint8_t subclass) {
  if (cls != kClassAudio) {
    return (cls == 0xFFu) ? "vendor-specific" : "other";
  }
  switch (subclass) {
    case kSubclassAudioControl: return "audio control";
    case kSubclassAudioStreaming: return "audio streaming";
    case kSubclassMidiStreaming: return "MIDI streaming";
    default: return "audio (unknown subclass)";
  }
}

/* Feature unit control bitmap, UAC1 Table 4-7. Only the controls that are
   plausibly useful to a host driver are named. */
void dumpFeatureControls(uint16_t controls, UsbDescOutFn out, void* context) {
  char text[96];
  text[0] = '\0';
  if ((controls & 0x0001u) != 0u) { strncat(text, "mute ", sizeof(text) - strlen(text) - 1u); }
  if ((controls & 0x0002u) != 0u) { strncat(text, "volume ", sizeof(text) - strlen(text) - 1u); }
  if ((controls & 0x0004u) != 0u) { strncat(text, "bass ", sizeof(text) - strlen(text) - 1u); }
  if ((controls & 0x0008u) != 0u) { strncat(text, "mid ", sizeof(text) - strlen(text) - 1u); }
  if ((controls & 0x0010u) != 0u) { strncat(text, "treble ", sizeof(text) - strlen(text) - 1u); }
  if ((controls & 0x0020u) != 0u) { strncat(text, "graphic-eq ", sizeof(text) - strlen(text) - 1u); }
  if ((controls & 0x0040u) != 0u) { strncat(text, "agc ", sizeof(text) - strlen(text) - 1u); }
  if ((controls & 0x0080u) != 0u) { strncat(text, "delay ", sizeof(text) - strlen(text) - 1u); }
  if ((controls & 0x0100u) != 0u) { strncat(text, "bass-boost ", sizeof(text) - strlen(text) - 1u); }
  if ((controls & 0x0200u) != 0u) { strncat(text, "loudness ", sizeof(text) - strlen(text) - 1u); }
  if (text[0] == '\0') { strncpy(text, "none", sizeof(text) - 1u); text[sizeof(text) - 1u] = '\0'; }
  emit(out, context, "      controls: %s", text);
}

/* Read the master-channel control bitmap of a feature unit. bControlSize is
   the width in bytes of each channel's bitmap; only 1 and 2 occur in practice. */
uint16_t featureMasterControls(const uint8_t* d, uint8_t length) {
  if (length < 7u) {
    return 0u;
  }
  const uint8_t controlSize = d[5];
  if (controlSize == 0u || (6u + controlSize) > length) {
    return 0u;
  }
  if (controlSize == 1u) {
    return d[6];
  }
  return read16(&d[6]);
}

void dumpAudioControlDescriptor(const uint8_t* d, uint8_t length,
                                UsbDescOutFn out, void* context) {
  const uint8_t subtype = d[2];
  switch (subtype) {
    case kAcHeader:
      if (length >= 8u) {
        emit(out, context, "    AC header: ADC %u.%02u, %u streaming interface(s)",
             d[4], d[3], d[7]);
      }
      break;
    case kAcInputTerminal:
      if (length >= 9u) {
        const uint16_t type = read16(&d[4]);
        emit(out, context, "    input terminal id=%u type=0x%04X (%s) channels=%u",
             d[3], type, usbDescTerminalTypeName(type), d[7]);
      }
      break;
    case kAcOutputTerminal:
      if (length >= 9u) {
        const uint16_t type = read16(&d[4]);
        emit(out, context, "    output terminal id=%u type=0x%04X (%s) source=%u",
             d[3], type, usbDescTerminalTypeName(type), d[7]);
      }
      break;
    case kAcMixerUnit:
      if (length >= 5u) {
        emit(out, context, "    mixer unit id=%u inputs=%u", d[3], d[4]);
      }
      break;
    case kAcSelectorUnit:
      if (length >= 5u) {
        emit(out, context, "    selector unit id=%u inputs=%u", d[3], d[4]);
      }
      break;
    case kAcFeatureUnit:
      if (length >= 7u) {
        emit(out, context, "    feature unit id=%u source=%u", d[3], d[4]);
        dumpFeatureControls(featureMasterControls(d, length), out, context);
      }
      break;
    default:
      emit(out, context, "    AC subtype 0x%02X (%u bytes)", subtype, length);
      break;
  }
}

void dumpAudioStreamingDescriptor(const uint8_t* d, uint8_t length,
                                  UsbDescOutFn out, void* context) {
  const uint8_t subtype = d[2];
  if (subtype == kAsGeneral && length >= 7u) {
    emit(out, context, "    AS general: terminal=%u delay=%u format=0x%04X",
         d[3], d[4], read16(&d[5]));
    return;
  }
  if (subtype == kAsFormatType && length >= 8u) {
    emit(out, context,
         "    format type %u: channels=%u subframe=%u bytes resolution=%u bits",
         d[3], d[4], d[5], d[6]);
    const uint8_t rateType = d[7];
    if (rateType == 0u) {
      if (length >= 14u) {
        emit(out, context, "      continuous rates %lu..%lu Hz",
             static_cast<unsigned long>(read24(&d[8])),
             static_cast<unsigned long>(read24(&d[11])));
      }
    } else {
      for (uint8_t i = 0u; i < rateType; ++i) {
        const uint16_t offset = static_cast<uint16_t>(8u + (i * 3u));
        if ((offset + 3u) > length) {
          break;
        }
        emit(out, context, "      discrete rate %lu Hz",
             static_cast<unsigned long>(read24(&d[offset])));
      }
    }
    return;
  }
  emit(out, context, "    AS subtype 0x%02X (%u bytes)", subtype, length);
}

/* MIDI jack topology. The source of each embedded OUT jack is what
   identifies which cable carries the keybed and which carries a DIN thru. */
void dumpMidiStreamingDescriptor(const uint8_t* d, uint8_t length,
                                 UsbDescOutFn out, void* context) {
  const uint8_t subtype = d[2];
  switch (subtype) {
    case kMsHeader:
      emit(out, context, "    MS header");
      break;
    case kMsMidiInJack:
      if (length >= 6u) {
        emit(out, context, "    MIDI IN jack id=%u type=%s", d[4],
             (d[3] == 0x01u) ? "embedded" : "external");
      }
      break;
    case kMsMidiOutJack:
      if (length >= 9u) {
        const uint8_t pins = d[5];
        emit(out, context, "    MIDI OUT jack id=%u type=%s inputs=%u", d[4],
             (d[3] == 0x01u) ? "embedded" : "external", pins);
        for (uint8_t i = 0u; i < pins; ++i) {
          const uint16_t offset = static_cast<uint16_t>(6u + (i * 2u));
          if ((offset + 2u) > length) {
            break;
          }
          emit(out, context, "      input %u from jack id=%u pin=%u", i,
               d[offset], d[offset + 1u]);
        }
      }
      break;
    case kMsElement:
      emit(out, context, "    MS element id=%u", d[3]);
      break;
    default:
      emit(out, context, "    MS subtype 0x%02X (%u bytes)", subtype, length);
      break;
  }
}

/* CS_ENDPOINT MS_GENERAL: the jack list order defines cable numbers. */
void dumpMidiEndpointDescriptor(const uint8_t* d, uint8_t length,
                                UsbDescOutFn out, void* context) {
  if (length < 4u || d[2] != kMsGeneral) {
    emit(out, context, "      class endpoint subtype 0x%02X", d[2]);
    return;
  }
  const uint8_t jacks = d[3];
  emit(out, context, "      MS endpoint: %u cable(s)", jacks);
  for (uint8_t i = 0u; i < jacks; ++i) {
    const uint16_t offset = static_cast<uint16_t>(4u + i);
    if (offset >= length) {
      break;
    }
    emit(out, context, "        cable %u -> jack id=%u", i, d[offset]);
  }
}

/* Record an AudioControl entity in the survey for later path tracing. */
void addUnit(UsbDescriptorSurvey& survey, uint8_t id, uint8_t sourceId,
             uint16_t terminalType, UsbAudioEntity kind, uint8_t channels,
             uint16_t controls) {
  if (survey.unitCount >= kUsbMaxAudioUnits) {
    survey.truncated = true;
    return;
  }
  UsbAudioUnit& unit = survey.units[survey.unitCount++];
  unit.id = id;
  unit.sourceId = sourceId;
  unit.terminalType = terminalType;
  unit.kind = kind;
  unit.channelCount = channels;
  unit.masterControls = controls;
}

const UsbAudioUnit* findUnit(const UsbDescriptorSurvey& survey, uint8_t id) {
  for (uint8_t i = 0u; i < survey.unitCount; ++i) {
    if (survey.units[i].id == id) {
      return &survey.units[i];
    }
  }
  return nullptr;
}

/* Walk from a speaker output terminal back to the USB streaming input
   terminal that feeds it, noting any feature unit on the way. The hop limit
   makes a malformed or circular topology terminate rather than hang. */
void traceSpeakerPath(UsbDescriptorSurvey& survey) {
  UsbSpeakerPath& path = survey.speaker;

  for (uint8_t i = 0u; i < survey.unitCount; ++i) {
    const UsbAudioUnit& terminal = survey.units[i];
    if (terminal.kind != UsbAudioEntity::OutputTerminal) {
      continue;
    }
    if ((terminal.terminalType & 0xFF00u) != kTerminalOutputGroup) {
      continue;
    }

    path.speakerTerminalId = terminal.id;
    uint8_t nextId = terminal.sourceId;

    for (uint8_t hop = 0u; hop < kUsbMaxAudioUnits; ++hop) {
      const UsbAudioUnit* unit = findUnit(survey, nextId);
      if (unit == nullptr) {
        break;
      }
      if (unit->kind == UsbAudioEntity::FeatureUnit) {
        path.featureUnitId = unit->id;
        path.hasMuteControl = ((unit->masterControls & 0x0001u) != 0u);
        path.hasVolumeControl = ((unit->masterControls & 0x0002u) != 0u);
        nextId = unit->sourceId;
        continue;
      }
      if (unit->kind == UsbAudioEntity::InputTerminal) {
        if (unit->terminalType == kTerminalUsbStreaming) {
          path.streamTerminalId = unit->id;
          path.found = true;
        }
        break;
      }
      /* Mixer and selector units expose several sources; the first is
         followed, which is correct for the single-source case and a
         reasonable guess otherwise. */
      if (unit->sourceId == 0u) {
        break;
      }
      nextId = unit->sourceId;
    }

    if (path.found) {
      break;
    }
  }

  if (!path.found) {
    return;
  }

  /* Match the streaming terminal to the alternate setting that carries it. */
  for (uint8_t i = 0u; i < survey.altCount; ++i) {
    const UsbAudioStreamAlt& alt = survey.alts[i];
    if (alt.dataEndpoint != 0u && alt.terminalLink == path.streamTerminalId) {
      path.asInterface = alt.interfaceNumber;
      path.asAlternate = alt.alternateSetting;
      path.asInterfaceFound = true;
      break;
    }
  }
}

} // namespace

const char* usbDescSyncTypeName(UsbIsoSync sync) {
  switch (sync) {
    case UsbIsoSync::NoSync: return "none";
    case UsbIsoSync::Asynchronous: return "asynchronous";
    case UsbIsoSync::Adaptive: return "adaptive";
    default: return "synchronous";
  }
}

const char* usbDescUsageTypeName(UsbIsoUsage usage) {
  switch (usage) {
    case UsbIsoUsage::Data: return "data";
    case UsbIsoUsage::Feedback: return "feedback";
    case UsbIsoUsage::ImplicitFeedbackData: return "implicit-feedback-data";
    default: return "reserved";
  }
}

const char* usbDescTerminalTypeName(uint16_t terminalType) {
  switch (terminalType) {
    case 0x0101u: return "USB streaming";
    case 0x0201u: return "microphone";
    case 0x0301u: return "speaker";
    case 0x0302u: return "headphones";
    case 0x0304u: return "desktop speaker";
    case 0x0305u: return "room speaker";
    case 0x0307u: return "LFE";
    case 0x0401u: return "handset";
    case 0x0402u: return "headset";
    case 0x0601u: return "analog connector";
    case 0x0602u: return "digital audio interface";
    case 0x0603u: return "line connector";
    case 0x0605u: return "S/PDIF";
    default: return "other";
  }
}

void usbDescDumpDevice(const uint8_t* descriptor, uint16_t length,
                       UsbDescOutFn out, void* context) {
  if (descriptor == nullptr || length < 18u) {
    emit(out, context, "device descriptor: short (%u bytes)", length);
    return;
  }
  emit(out, context, "device descriptor:");
  emit(out, context, "  USB %u.%02u  class=%u/%u/%u  maxPacket0=%u",
       descriptor[3], descriptor[2], descriptor[4], descriptor[5],
       descriptor[6], descriptor[7]);
  emit(out, context, "  VID=0x%04X PID=0x%04X rev=0x%04X configs=%u",
       read16(&descriptor[8]), read16(&descriptor[10]),
       read16(&descriptor[12]), descriptor[17]);
}

void usbDescDumpConfiguration(const uint8_t* descriptor, uint16_t length,
                              UsbDescOutFn out, void* context) {
  if (descriptor == nullptr || length < 9u) {
    emit(out, context, "configuration descriptor: short (%u bytes)", length);
    return;
  }

  /* Class/subclass of the interface being walked, so CS_INTERFACE and
     CS_ENDPOINT descriptors are decoded in the right dialect. */
  uint8_t currentClass = 0u;
  uint8_t currentSubclass = 0u;

  uint16_t offset = 0u;
  while ((offset + 2u) <= length) {
    const uint8_t* d = &descriptor[offset];
    const uint8_t bLength = d[0];
    const uint8_t bType = d[1];

    /* A zero or over-long bLength would loop forever or read past the end. */
    if (bLength < 2u || (offset + bLength) > length) {
      emit(out, context, "malformed descriptor at offset %u (bLength=%u)",
           offset, bLength);
      return;
    }

    switch (bType) {
      case kDtConfiguration:
        emit(out, context,
             "configuration %u: total=%u interfaces=%u power=%u mA%s",
             d[5], read16(&d[2]), d[4], static_cast<unsigned>(d[8]) * 2u,
             ((d[7] & 0x40u) != 0u) ? " (self-powered)" : "");
        break;

      case kDtInterfaceAssociation:
        emit(out, context,
             "interface association: first=%u count=%u function=%u/%u",
             d[2], d[3], d[4], d[5]);
        break;

      case kDtInterface:
        currentClass = d[5];
        currentSubclass = d[6];
        emit(out, context,
             "  interface %u alt %u: %u endpoint(s) class=%u/%u/%u (%s)",
             d[2], d[3], d[4], d[5], d[6], d[7],
             interfaceClassName(currentClass, currentSubclass));
        break;

      case kDtEndpoint: {
        const uint8_t address = d[2];
        const uint8_t attributes = d[3];
        const uint8_t type = attributes & kEpTypeMask;
        if (type == kEpTypeIsochronous) {
          const UsbIsoSync sync =
              static_cast<UsbIsoSync>((attributes >> kEpSyncShift) & kEpSyncMask);
          const UsbIsoUsage usage =
              static_cast<UsbIsoUsage>((attributes >> kEpUsageShift) & kEpUsageMask);
          emit(out, context,
               "    endpoint 0x%02X %s iso maxPacket=%u interval=%u sync=%s usage=%s",
               address, ((address & 0x80u) != 0u) ? "IN " : "OUT",
               read16(&d[4]), d[6], usbDescSyncTypeName(sync),
               usbDescUsageTypeName(usage));
          /* Audio endpoint descriptors are 9 bytes and carry bSynchAddress. */
          if (bLength >= 9u && d[8] != 0u) {
            emit(out, context, "      feedback endpoint 0x%02X", d[8]);
          }
        } else {
          emit(out, context, "    endpoint 0x%02X %s %s maxPacket=%u interval=%u",
               address, ((address & 0x80u) != 0u) ? "IN " : "OUT",
               endpointTypeName(attributes), read16(&d[4]), d[6]);
        }
        break;
      }

      case kDtClassInterface:
        if (bLength >= 3u) {
          if (currentClass == kClassAudio) {
            switch (currentSubclass) {
              case kSubclassAudioControl:
                dumpAudioControlDescriptor(d, bLength, out, context);
                break;
              case kSubclassAudioStreaming:
                dumpAudioStreamingDescriptor(d, bLength, out, context);
                break;
              case kSubclassMidiStreaming:
                dumpMidiStreamingDescriptor(d, bLength, out, context);
                break;
              default:
                break;
            }
          } else {
            emit(out, context, "    class descriptor subtype 0x%02X (%u bytes)",
                 d[2], bLength);
          }
        }
        break;

      case kDtClassEndpoint:
        if (bLength >= 4u && currentSubclass == kSubclassAudioStreaming) {
          /* bmAttributes bit 0: the endpoint accepts SET_CUR of the sampling
             frequency, which is how the rate is selected at run time. */
          emit(out, context,
               "      endpoint controls: sampling-freq=%s pitch=%s",
               ((d[3] & 0x01u) != 0u) ? "yes" : "no",
               ((d[3] & 0x02u) != 0u) ? "yes" : "no");
        } else if (currentSubclass == kSubclassMidiStreaming) {
          dumpMidiEndpointDescriptor(d, bLength, out, context);
        } else if (bLength >= 3u) {
          emit(out, context, "      class endpoint subtype 0x%02X", d[2]);
        }
        break;

      case kDtDevice:
      default:
        emit(out, context, "    descriptor type 0x%02X (%u bytes)", bType, bLength);
        break;
    }

    offset = static_cast<uint16_t>(offset + bLength);
  }
}

void usbDescSurvey(const uint8_t* descriptor, uint16_t length,
                   UsbDescriptorSurvey& survey) {
  memset(&survey, 0, sizeof(survey));
  survey.audioControlInterface = 0xFFu;
  if (descriptor == nullptr || length < 9u) {
    survey.malformed = true;
    return;
  }

  uint8_t currentClass = 0u;
  uint8_t currentSubclass = 0u;
  /* Index into survey.alts for the AudioStreaming alt setting being parsed,
     or kUsbMaxAudioAlts when none is active. */
  uint8_t currentAlt = kUsbMaxAudioAlts;
  /* Address of the most recent endpoint, so a following CS_ENDPOINT can be
     attributed to it. */
  uint8_t lastEndpoint = 0u;

  uint16_t offset = 0u;
  while ((offset + 2u) <= length) {
    const uint8_t* d = &descriptor[offset];
    const uint8_t bLength = d[0];
    const uint8_t bType = d[1];

    if (bLength < 2u || (offset + bLength) > length) {
      survey.malformed = true;
      return;
    }

    if (bType == kDtInterface && bLength >= 9u) {
      currentClass = d[5];
      currentSubclass = d[6];
      currentAlt = kUsbMaxAudioAlts;
      lastEndpoint = 0u;

      if (currentClass == kClassAudio) {
        if (currentSubclass == kSubclassAudioControl) {
          survey.audioControlFound = true;
          survey.audioControlInterface = d[2];
        } else if (currentSubclass == kSubclassMidiStreaming) {
          survey.midi.found = true;
          survey.midi.interfaceNumber = d[2];
          survey.midi.alternateSetting = d[3];
        } else if (currentSubclass == kSubclassAudioStreaming) {
          if (survey.altCount < kUsbMaxAudioAlts) {
            currentAlt = survey.altCount++;
            UsbAudioStreamAlt& alt = survey.alts[currentAlt];
            memset(&alt, 0, sizeof(alt));
            alt.interfaceNumber = d[2];
            alt.alternateSetting = d[3];
          } else {
            survey.truncated = true;
          }
        }
      }
    } else if (bType == kDtEndpoint && bLength >= 7u) {
      const uint8_t address = d[2];
      const uint8_t attributes = d[3];
      const uint8_t type = attributes & kEpTypeMask;
      const uint16_t packetSize = read16(&d[4]);
      lastEndpoint = address;

      if (currentSubclass == kSubclassMidiStreaming && type == kEpTypeBulk) {
        if ((address & 0x80u) != 0u) {
          survey.midi.inEndpoint = address;
          survey.midi.inMaxPacketSize = packetSize;
        } else {
          survey.midi.outEndpoint = address;
          survey.midi.outMaxPacketSize = packetSize;
        }
      } else if (currentAlt < kUsbMaxAudioAlts && type == kEpTypeIsochronous) {
        UsbAudioStreamAlt& alt = survey.alts[currentAlt];
        const UsbIsoUsage usage =
            static_cast<UsbIsoUsage>((attributes >> kEpUsageShift) & kEpUsageMask);

        /* An alternate setting holds at most one data endpoint and one
           feedback endpoint. Classify by declared usage type, by the
           bSynchAddress already learned from the data endpoint, and by
           packet size, because a feedback endpoint is never large enough to
           carry audio. Without all three tests a device that mis-declares
           its feedback endpoint as Data overwrites the data endpoint. */
        const bool namedAsFeedback =
            (alt.feedbackEndpoint != 0u) && (address == alt.feedbackEndpoint);
        const bool isFeedback = (usage == UsbIsoUsage::Feedback) ||
                                namedAsFeedback ||
                                (packetSize <= kMaxFeedbackPacketSize) ||
                                (alt.dataEndpoint != 0u);

        if (isFeedback) {
          alt.feedbackEndpoint = address;
          alt.feedbackPacketSize = packetSize;
        } else {
          alt.dataEndpoint = address;
          alt.maxPacketSize = packetSize;
          alt.interval = d[6];
          alt.syncType = static_cast<UsbIsoSync>((attributes >> kEpSyncShift) &
                                                 kEpSyncMask);
          alt.usageType = usage;
          /* bSynchAddress names the feedback endpoint before it is reached. */
          if (bLength >= 9u && d[8] != 0u) {
            alt.feedbackEndpoint = d[8];
          }
        }
      }
    } else if (bType == kDtClassInterface && bLength >= 3u) {
      if (currentAlt < kUsbMaxAudioAlts) {
        UsbAudioStreamAlt& alt = survey.alts[currentAlt];
        if (d[2] == kAsGeneral && bLength >= 7u) {
          alt.terminalLink = d[3];
          alt.formatTag = read16(&d[5]);
        } else if (d[2] == kAsFormatType && bLength >= 8u) {
          alt.channelCount = d[4];
          alt.subframeBytes = d[5];
          alt.bitResolution = d[6];
          const uint8_t rateType = d[7];
          if (rateType == 0u) {
            /* Continuous: lower and upper bounds only. */
            alt.continuousRates = true;
            if (bLength >= 14u) {
              alt.rates[0] = read24(&d[8]);
              alt.rates[1] = read24(&d[11]);
              alt.rateCount = 2u;
            }
          } else {
            for (uint8_t i = 0u; i < rateType; ++i) {
              const uint16_t rateOffset = static_cast<uint16_t>(8u + (i * 3u));
              if ((rateOffset + 3u) > bLength) {
                break;
              }
              if (alt.rateCount >= kUsbMaxRatesPerAlt) {
                survey.truncated = true;
                break;
              }
              alt.rates[alt.rateCount++] = read24(&d[rateOffset]);
            }
          }
        }
      } else if (currentSubclass == kSubclassAudioControl) {
        switch (d[2]) {
          case kAcInputTerminal:
            if (bLength >= 9u) {
              addUnit(survey, d[3], 0u, read16(&d[4]),
                      UsbAudioEntity::InputTerminal, d[7], 0u);
            }
            break;
          case kAcOutputTerminal:
            if (bLength >= 9u) {
              addUnit(survey, d[3], d[7], read16(&d[4]),
                      UsbAudioEntity::OutputTerminal, 0u, 0u);
            }
            break;
          case kAcFeatureUnit:
            if (bLength >= 7u) {
              addUnit(survey, d[3], d[4], 0u, UsbAudioEntity::FeatureUnit, 0u,
                      featureMasterControls(d, bLength));
            }
            break;
          case kAcSelectorUnit:
          case kAcMixerUnit:
            /* Only the first input pin is retained; enough to follow a
               single-source path. */
            if (bLength >= 6u) {
              addUnit(survey, d[3], d[5], 0u,
                      (d[2] == kAcSelectorUnit) ? UsbAudioEntity::SelectorUnit
                                                : UsbAudioEntity::MixerUnit,
                      0u, 0u);
            }
            break;
          default:
            break;
        }
      } else if (currentSubclass == kSubclassMidiStreaming) {
        if ((d[2] == kMsMidiInJack && bLength >= 6u) ||
            (d[2] == kMsMidiOutJack && bLength >= 9u)) {
          if (survey.midi.jackCount >= kUsbMaxMidiJacks) {
            survey.truncated = true;
          } else {
            UsbMidiJack& jack = survey.midi.jacks[survey.midi.jackCount++];
            jack.isOutput = (d[2] == kMsMidiOutJack);
            jack.embedded = (d[3] == 0x01u);
            jack.id = d[4];
            jack.sourceId = jack.isOutput ? d[6] : 0u;
            jack.sourcePin = jack.isOutput ? d[7] : 0u;
          }
        }
      }
    } else if (bType == kDtClassEndpoint && bLength >= 4u) {
      if (currentAlt < kUsbMaxAudioAlts && d[2] == kAsGeneral) {
        survey.alts[currentAlt].hasSamplingFreqControl = ((d[3] & 0x01u) != 0u);
      } else if (currentSubclass == kSubclassMidiStreaming && d[2] == kMsGeneral) {
        /* Cable numbers are the index into this jack list. */
        const bool isInput = ((lastEndpoint & 0x80u) != 0u);
        uint8_t* cableJacks =
            isInput ? survey.midi.inCableJackId : survey.midi.outCableJackId;
        uint8_t count = 0u;
        for (uint8_t i = 0u; i < d[3]; ++i) {
          const uint16_t jackOffset = static_cast<uint16_t>(4u + i);
          if (jackOffset >= bLength) {
            break;
          }
          if (count >= kUsbMaxMidiCables) {
            survey.truncated = true;
            break;
          }
          cableJacks[count++] = d[jackOffset];
        }
        if (isInput) {
          survey.midi.inCableCount = count;
        } else {
          survey.midi.outCableCount = count;
        }
      }
    }

    offset = static_cast<uint16_t>(offset + bLength);
  }

  traceSpeakerPath(survey);
}

void usbDescDumpSurvey(const UsbDescriptorSurvey& survey, UsbDescOutFn out,
                       void* context) {
  emit(out, context, "---- survey ----");

  if (survey.malformed) {
    emit(out, context, "WARNING: descriptor blob is malformed, results partial");
  }
  if (survey.truncated) {
    emit(out, context, "WARNING: capacity exceeded, some entries dropped");
  }

  if (survey.midi.found) {
    emit(out, context, "MIDI: interface %u alt %u  IN=0x%02X (%u B)  OUT=0x%02X (%u B)",
         survey.midi.interfaceNumber, survey.midi.alternateSetting,
         survey.midi.inEndpoint, survey.midi.inMaxPacketSize,
         survey.midi.outEndpoint, survey.midi.outMaxPacketSize);
    for (uint8_t i = 0u; i < survey.midi.inCableCount; ++i) {
      const uint8_t jackId = survey.midi.inCableJackId[i];
      const UsbMidiJack* jack = nullptr;
      for (uint8_t j = 0u; j < survey.midi.jackCount; ++j) {
        if (survey.midi.jacks[j].id == jackId && survey.midi.jacks[j].isOutput) {
          jack = &survey.midi.jacks[j];
          break;
        }
      }
      if (jack != nullptr) {
        emit(out, context, "  IN  cable %u: jack %u fed by jack %u pin %u", i,
             jackId, jack->sourceId, jack->sourcePin);
      } else {
        emit(out, context, "  IN  cable %u: jack %u", i, jackId);
      }
    }
    for (uint8_t i = 0u; i < survey.midi.outCableCount; ++i) {
      emit(out, context, "  OUT cable %u: jack %u", i,
           survey.midi.outCableJackId[i]);
    }
    for (uint8_t j = 0u; j < survey.midi.jackCount; ++j) {
      const UsbMidiJack& jack = survey.midi.jacks[j];
      emit(out, context, "  jack %u: %s %s source=%u", jack.id,
           jack.embedded ? "embedded" : "external",
           jack.isOutput ? "OUT" : "IN", jack.sourceId);
    }
  } else {
    emit(out, context, "MIDI: no MIDIStreaming interface found");
  }

  if (survey.audioControlFound) {
    emit(out, context, "audio control interface: %u", survey.audioControlInterface);
  } else {
    emit(out, context, "audio: no AudioControl interface found");
  }

  for (uint8_t i = 0u; i < survey.altCount; ++i) {
    const UsbAudioStreamAlt& alt = survey.alts[i];
    if (alt.dataEndpoint == 0u) {
      emit(out, context, "audio if %u alt %u: zero-bandwidth (no endpoint)",
           alt.interfaceNumber, alt.alternateSetting);
      continue;
    }
    emit(out, context,
         "audio if %u alt %u: EP 0x%02X %s sync=%s maxPacket=%u interval=%u",
         alt.interfaceNumber, alt.alternateSetting, alt.dataEndpoint,
         ((alt.dataEndpoint & 0x80u) != 0u) ? "IN" : "OUT",
         usbDescSyncTypeName(alt.syncType), alt.maxPacketSize, alt.interval);
    emit(out, context,
         "    terminal=%u format=0x%04X channels=%u subframe=%u B res=%u bits setRate=%s",
         alt.terminalLink, alt.formatTag, alt.channelCount, alt.subframeBytes,
         alt.bitResolution, alt.hasSamplingFreqControl ? "yes" : "no");
    if (alt.feedbackEndpoint != 0u) {
      emit(out, context, "    feedback endpoint 0x%02X (%u B)",
           alt.feedbackEndpoint, alt.feedbackPacketSize);
    }
    for (uint8_t r = 0u; r < alt.rateCount; ++r) {
      emit(out, context, "    rate%s %lu Hz", alt.continuousRates ? " bound" : "",
           static_cast<unsigned long>(alt.rates[r]));
    }
  }

  if (survey.speaker.found) {
    emit(out, context, "speaker path: terminal %u <- stream terminal %u",
         survey.speaker.speakerTerminalId, survey.speaker.streamTerminalId);
    emit(out, context, "  feature unit %u volume=%s mute=%s",
         survey.speaker.featureUnitId,
         survey.speaker.hasVolumeControl ? "yes" : "no",
         survey.speaker.hasMuteControl ? "yes" : "no");
    if (survey.speaker.asInterfaceFound) {
      emit(out, context, "  playback: set interface %u to alt %u",
           survey.speaker.asInterface, survey.speaker.asAlternate);
    } else {
      emit(out, context, "  WARNING: no streaming alt matches that terminal");
    }
  } else {
    emit(out, context, "speaker path: none found");
  }

  emit(out, context, "----------------");
}
