// =============================================================================
// SyxProtocol.h — JT-8000 editor SysEx wire-format definitions
//
// Single source of truth for the JT-8000 editor protocol. Header-only so the
// firmware, the JUCE plugin, and (eventually) the HTML editor can all include
// the same file and never drift on byte layout.
//
// Wire format (Phase 1):
//
//   F0 7D 4A 54  00  <msg> <payload...>  F7
//   │  │  │  │   │   │
//   │  │  │  │   │   └── message type (kMsgSetParam / kMsgGetParam / kMsgParamValue)
//   │  │  │  │   └────── device ID (0x00 = single-device default; 0x7F = broadcast)
//   │  │  └──┴────────── 'JT' ASCII sub-ID
//   │  └──────────────── 0x7D = MMA non-commercial / educational manufacturer ID
//   └─────────────────── SysEx start
//
// Every byte in the payload MUST be < 0x80 (7-bit safe). The 5-byte float
// encoding below distributes a 32-bit float across 5 bytes of 7 bits each
// (35-bit field, 3 bits unused) — lossless 32-bit round-trip.
//
// Layer IDs (0x02..0x0F reserved for future engine count expansion).
//
// © 2025 Kris Bishop — MIT licensed (matches project licence).
// =============================================================================

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace SyxProto {

// -----------------------------------------------------------------------------
// Manufacturer envelope — first three bytes after F0
// -----------------------------------------------------------------------------
static constexpr uint8_t kMfrId    = 0x7D; // MMA: non-commercial / educational
static constexpr uint8_t kSubIdJ   = 0x4A; // 'J'
static constexpr uint8_t kSubIdT   = 0x54; // 'T'

// Device ID — reserved for future multi-device hosts. Use kDeviceDefault for
// normal traffic and kDeviceBroadcast to address every JT-8000 on the bus.
static constexpr uint8_t kDeviceDefault   = 0x00;
static constexpr uint8_t kDeviceBroadcast = 0x7F;

// -----------------------------------------------------------------------------
// Layer IDs — explicit routing target for SET_PARAM / GET_PARAM
// -----------------------------------------------------------------------------
// Engine-scoped: 0x00..0x02
static constexpr uint8_t kLayerA        = 0x00; // Engine A only
static constexpr uint8_t kLayerB        = 0x01; // Engine B only
static constexpr uint8_t kLayerBoth     = 0x02; // Both engines (engine-scope only)

// Manager-scoped: 0x10+
static constexpr uint8_t kLayerPerf     = 0x10; // LayerManager (Performance scope)
static constexpr uint8_t kLayerGlobalFx = 0x11; // GlobalFX scope (shared reverb)

// -----------------------------------------------------------------------------
// Message types
// -----------------------------------------------------------------------------
// Phase 1
static constexpr uint8_t kMsgSetParam       = 0x01;
static constexpr uint8_t kMsgGetParam       = 0x02;
static constexpr uint8_t kMsgParamValue     = 0x03; // reply to GET_PARAM (also unsolicited push)

// Phase 2
static constexpr uint8_t kMsgBankDumpRequest = 0x10; // editor -> firmware (request live state)
static constexpr uint8_t kMsgBankDump        = 0x11; // bidirectional: live-state body

// Hardware controller — carries CCs above the MIDI 0..127 range (e.g.
// envelope curves 147..155, performance params 140..146) inside a SysEx
// envelope so they survive the 7-bit MIDI CC limitation.
//
// Wire format (11 bytes):
//   F0 7D 4A 54 00 20 <channel> <cc_hi> <cc_lo> <value> F7
//
// All body bytes are 7-bit safe (< 0x80). The CC number is split the same
// way ParamIDs are: cc_hi = (cc >> 7) & 0x7F, cc_lo = cc & 0x7F. The
// Teensy reconstructs cc = (cc_hi << 7) | cc_lo and dispatches to
// LayerManager::handleControlChange(channel, cc, value).
static constexpr uint8_t kMsgExtCC           = 0x20;

// -----------------------------------------------------------------------------
// Payload sizes (bytes between <msg> and F7, INCLUSIVE of <msg> count = 0)
// -----------------------------------------------------------------------------
// Layout for SET_PARAM and PARAM_VALUE:
//   <msg> <layer> <pid_hi> <pid_lo> <val[0..4]>     = 1 + 1 + 2 + 5 = 9 bytes
// Layout for GET_PARAM:
//   <msg> <layer> <pid_hi> <pid_lo>                  = 1 + 1 + 2     = 4 bytes
//
// Total message length including F0 + envelope (5) + payload + F7:
//   SET_PARAM / PARAM_VALUE: 5 + 9 + 1 = 15 bytes
//   GET_PARAM:               5 + 4 + 1 = 10 bytes
//
// BANK_DUMP_REQUEST has no payload — just header + msg + F7:
//   <msg>                                            = 1 byte
//   total = 5 + 1 + 1 = 7 bytes
//
// BANK_DUMP body is variable-length; format:
//   <msg> <count_hi> <count_lo> {<entry>}*N
//
// Each entry is 8 bytes: <layer> <pid_hi> <pid_lo> <val[0..4]>.
// count_hi / count_lo together form a 14-bit count (0..16383). For Phase 2
// "live state" the count is fixed at the number of cached params currently
// available — typically ~200.
// -----------------------------------------------------------------------------
static constexpr size_t kMsgSetParamLen     = 15;
static constexpr size_t kMsgGetParamLen     = 10;
static constexpr size_t kMsgParamValueLen   = 15;
static constexpr size_t kMsgBankDumpReqLen  = 7;
// EXT_CC: F0(1) + envelope(3) + devId(1) + msg(1) + ch(1) + cc_hi(1) + cc_lo(1) + val(1) + F7(1) = 11
static constexpr size_t kMsgExtCCLen        = 11;
static constexpr size_t kBankDumpEntrySize  = 8; // layer + pid(2) + float(5)
static constexpr size_t kBankDumpHeaderSize = 6  // F0 + envelope(5) + msg
                                            + 2; // 2-byte count
// Reserve space for the largest possible bank dump. With ~138 ParamIDs ×
// at most 2 layers (engine-scope) we cap at 256 entries safely.
static constexpr size_t kBankDumpMaxEntries = 256;
static constexpr size_t kBankDumpMaxLen     = kBankDumpHeaderSize
                                            + kBankDumpEntrySize * kBankDumpMaxEntries
                                            + 1; // F7

// Header offset constants for parsing.
//   data[0]      = 0xF0
//   data[1]      = kMfrId      (0x7D)
//   data[2]      = kSubIdJ     (0x4A)
//   data[3]      = kSubIdT     (0x54)
//   data[4]      = device ID
//   data[5]      = msg type
//   data[6]      = layer            (SET_PARAM, GET_PARAM, PARAM_VALUE)
//   data[7..8]   = paramID hi, lo   (SET_PARAM, GET_PARAM, PARAM_VALUE)
//   data[9..13]  = float[5]         (SET_PARAM, PARAM_VALUE)
//   data[14]     = 0xF7             (SET_PARAM, PARAM_VALUE)
static constexpr size_t kOffsetMfrId    = 1;
static constexpr size_t kOffsetSubIdJ   = 2;
static constexpr size_t kOffsetSubIdT   = 3;
static constexpr size_t kOffsetDeviceId = 4;
static constexpr size_t kOffsetMsgType  = 5;
static constexpr size_t kOffsetLayer    = 6;
static constexpr size_t kOffsetPidHi    = 7;
static constexpr size_t kOffsetPidLo    = 8;
static constexpr size_t kOffsetFloat    = 9;  // first of 5 float bytes
static constexpr size_t kFloatBytes     = 5;

// -----------------------------------------------------------------------------
// 7-bit-safe float codec — 5 bytes of 7 bits each (lossless 32-bit round-trip)
// -----------------------------------------------------------------------------
// Layout per byte:
//   out[0] = bits  0..6   of the float's IEEE-754 bit pattern
//   out[1] = bits  7..13
//   out[2] = bits 14..20
//   out[3] = bits 21..27
//   out[4] = bits 28..31  (top 4 bits, upper 3 bits of the byte are zero)
//
// Every output byte is guaranteed < 0x80 (top bit clear) so the result is safe
// to ship inside a SysEx body.
// -----------------------------------------------------------------------------
inline void encodeFloat(float value, uint8_t out[5]) {
    union { float f; uint32_t u; } cv;   // type-pun via union (defined behaviour
    cv.f = value;                        // for trivially-copyable types in C++)
    const uint32_t u = cv.u;
    out[0] = (uint8_t)( u        & 0x7F);
    out[1] = (uint8_t)((u >>  7) & 0x7F);
    out[2] = (uint8_t)((u >> 14) & 0x7F);
    out[3] = (uint8_t)((u >> 21) & 0x7F);
    out[4] = (uint8_t)((u >> 28) & 0x0F); // top 4 bits only
}

inline float decodeFloat(const uint8_t in[5]) {
    const uint32_t u =  ((uint32_t)(in[0] & 0x7F))
                     | (((uint32_t)(in[1] & 0x7F)) <<  7)
                     | (((uint32_t)(in[2] & 0x7F)) << 14)
                     | (((uint32_t)(in[3] & 0x7F)) << 21)
                     | (((uint32_t)(in[4] & 0x0F)) << 28);
    union { float f; uint32_t u; } cv;
    cv.u = u;
    return cv.f;
}

// -----------------------------------------------------------------------------
// 14-bit ParamID split helpers — ParamID is uint16_t (range 0x0000..0x3FFF).
// We split MSB/LSB the same way NRPN does (high 7 then low 7) for familiarity.
// -----------------------------------------------------------------------------
inline uint8_t  paramIdHi(uint16_t pid) { return (uint8_t)((pid >> 7) & 0x7F); }
inline uint8_t  paramIdLo(uint16_t pid) { return (uint8_t)( pid       & 0x7F); }
inline uint16_t paramIdFromBytes(uint8_t hi, uint8_t lo) {
    return (uint16_t)(((hi & 0x7F) << 7) | (lo & 0x7F));
}

} // namespace SyxProto
