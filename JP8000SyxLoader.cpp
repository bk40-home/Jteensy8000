// =============================================================================
// JP8000SyxLoader.cpp — JP-8000/8080 SysEx patch parser and loader
//
// See JP8000SyxLoader.h for usage and architecture notes.
//
// SIGNAL FLOW:
//   File bytes → scanBuffer() builds index → loadPatch() reads data →
//   loadPatchData() sends defaults, then iterates kSlots[] dispatching
//   CCs through synth.handleControlChange().
//
// CONTEXT-DEPENDENT DISPATCH:
//   OSC1/2 Control 1 and Control 2 (offsets 0x1F/0x20 and 0x25/0x26)
//   are NOT in kSlots[] because their target CC depends on the active
//   waveform. loadPatchData() reads the waveform byte first, then uses
//   osc1Ctrl1CC() / osc1Ctrl2CC() etc. to determine the correct CC.
//
// MFX DRIVE MAPPING:
//   JP-8000 MFX types 8-12 (overdrive/distortion) have no direct mod
//   effect equivalent. Instead, the loader maps them to FX_DRIVE CC
//   with a scaled intensity value.
//
// MFX SHORT DELAY:
//   JP-8000 MFX types 5-7 use the multi-FX slot for a short delay.
//   The loader routes the MFX level to the delay section instead.
// =============================================================================

#include "JP8000SyxLoader.h"
#include "JP8000SyxMap.h"
#include "SynthEngine.h"
#include "DebugTrace.h"
#include <Audio.h>      // AudioNoInterrupts / AudioInterrupts

namespace JP8000SyxLoader {

// ── Helper: safe byte read from data block ───────────────────────────────────
static inline uint8_t safeRead(const uint8_t* data, uint16_t dataLen, uint8_t offset) {
    return (offset < dataLen) ? data[offset] : 0;
}

// =============================================================================
// getPatchName — extract 16-char ASCII name from patch data
// =============================================================================
void Loader::getPatchName(const uint8_t* data, uint16_t dataLen,
                          char* nameOut, uint8_t maxLen)
{
    JP8000SyxMap::extractName(data, dataLen, nameOut, maxLen);
}

// =============================================================================
// loadPatchData — core loader: converts SysEx data → CC messages
//
// This is the workhorse. Every other load function ultimately calls this.
// The data pointer must point to the patch data block (starting at the
// patch name, offset 0x00), NOT the full SysEx message.
// =============================================================================
void Loader::loadPatchData(SynthEngine& synth, const uint8_t* data,
                           uint16_t dataLen, uint8_t midiCh)
{
    if (!data || dataLen < MIN_PATCH_DATA_LEN) return;

    AudioNoInterrupts();

    // ── Step 1: Send safe defaults for JT-exclusive parameters ───────────
    // These reset params that have no JP-8000 source so they don't bleed
    // from a previous patch.
    for (uint8_t i = 0; i < JP8000SyxMap::kDefaultCount; ++i) {
        synth.handleControlChange(midiCh,
            JP8000SyxMap::kDefaults[i].cc,
            JP8000SyxMap::kDefaults[i].val);
    }

    // ── Step 2: Read waveform bytes (needed for context-dependent dispatch) ─
    const uint8_t jpOsc1Wave = safeRead(data, dataLen, 0x1E);
    const uint8_t jpOsc2Wave = safeRead(data, dataLen, 0x21);
    const uint8_t jpMfxType  = safeRead(data, dataLen, 0x3D);

    // ── Step 3: Iterate the main slot table ──────────────────────────────
    for (uint8_t s = 0; s < JP8000SyxMap::kSlotCount; ++s) {
        const auto& slot = JP8000SyxMap::kSlots[s];

        // Skip if offset is beyond available data
        if (slot.offset >= dataLen) continue;

        const uint8_t raw = data[slot.offset];

        // Skip context-dependent transforms — handled in step 4
        if (slot.xform == JP8000SyxMap::SyxXform::OscCtrl1 ||
            slot.xform == JP8000SyxMap::SyxXform::OscCtrl2 ||
            slot.xform == JP8000SyxMap::SyxXform::Ignore) {
            continue;
        }

        const uint8_t val = JP8000SyxMap::applyTransform(slot.xform, raw);
        synth.handleControlChange(midiCh, slot.cc, val);
    }

    // ── Step 4: Context-dependent OSC Control 1 & 2 dispatch ─────────────
    //
    // OSC1 Ctrl1 (0x1F) and Ctrl2 (0x20)
    {
        const uint8_t ctrl1Raw = safeRead(data, dataLen, 0x1F);
        const uint8_t ctrl2Raw = safeRead(data, dataLen, 0x20);

        const uint8_t cc1 = JP8000SyxMap::osc1Ctrl1CC(jpOsc1Wave);
        const uint8_t cc2 = JP8000SyxMap::osc1Ctrl2CC(jpOsc1Wave);

        if (cc1 != 0) synth.handleControlChange(midiCh, cc1, ctrl1Raw);
        if (cc2 != 0) synth.handleControlChange(midiCh, cc2, ctrl2Raw);
    }

    // OSC2 Ctrl1 (0x25) and Ctrl2 (0x26)
    {
        const uint8_t ctrl1Raw = safeRead(data, dataLen, 0x25);
        const uint8_t ctrl2Raw = safeRead(data, dataLen, 0x26);

        const uint8_t cc1 = JP8000SyxMap::osc2Ctrl1CC(jpOsc2Wave);
        const uint8_t cc2 = JP8000SyxMap::osc2Ctrl2CC(jpOsc2Wave);

        if (cc1 != 0) synth.handleControlChange(midiCh, cc1, ctrl1Raw);
        if (cc2 != 0) synth.handleControlChange(midiCh, cc2, ctrl2Raw);
    }

    // ── Step 5: MFX drive/distortion special handling ────────────────────
    // JP MFX types 8-12 are overdrive/distortion. Map to FX_DRIVE CC.
    // Scale: type 8 (mild overdrive) → low drive, type 12 (heavy dist) → high.
    if (JP8000SyxMap::isMfxDrive(jpMfxType)) {
        // Map MFX types 8-12 → FX_DRIVE 32-127 (soft clip → hard clip range)
        // FX_DRIVE: 0=bypass, 1-63=soft clip, 64-127=hard clip
        const uint8_t driveVal = 32 + ((jpMfxType - 8) * 24); // 32,56,80,104,127
        synth.handleControlChange(midiCh, CC::FX_DRIVE, driveVal > 127 ? 127 : driveVal);

        // Disable mod effect since the JP is using that slot for drive
        synth.handleControlChange(midiCh, CC::FX_MOD_EFFECT, 0);
    }

    // ── Step 6: MFX short-delay special handling ─────────────────────────
    // JP MFX types 5-7 use the multi-FX slot for a short delay effect.
    // Route the MFX level to the delay mix instead.
    if (JP8000SyxMap::isMfxShortDelay(jpMfxType)) {
        const uint8_t mfxLevel = safeRead(data, dataLen, 0x3E);

        // Set the main delay section to mono short
        synth.handleControlChange(midiCh, CC::FX_JPFX_DELAY_EFFECT, 0); // MONO_SHORT
        synth.handleControlChange(midiCh, CC::FX_JPFX_DELAY_MIX, mfxLevel);

        // Disable mod effect
        synth.handleControlChange(midiCh, CC::FX_MOD_EFFECT, 0);
    }

    // ── Step 7: Super Chorus Fast rate boost ─────────────────────────────
    // JP MFX type 1 (Super Chorus Fast) differs from type 0 (Slow) only in
    // the internal LFO rate. Set FX_MOD_RATE to a high value.
    if (JP8000SyxMap::isMfxSuperChoFast(jpMfxType)) {
        synth.handleControlChange(midiCh, CC::FX_MOD_RATE, 100);
    }

    // ── Step 8: JP-8080 unison params (if data is long enough) ───────────
    if (dataLen > 0x174) {
        const uint8_t unisonSw    = safeRead(data, dataLen, 0x173);
        const uint8_t unisonDet   = safeRead(data, dataLen, 0x174);

        if (unisonSw) {
            // Switch to unison mode (CC 85-127 range)
            synth.handleControlChange(midiCh, CC::POLY_MODE, 100);
            // Scale JP 0-50 detune to JT 0-127
            synth.handleControlChange(midiCh, CC::UNISON_DETUNE,
                JP8000SyxMap::clamp7((int)(unisonDet * 127 + 25) / 50));
        }
    }

    AudioInterrupts();

    // Log the patch name for debug
    char name[JP8000SyxMap::NAME_LENGTH + 1];
    JP8000SyxMap::extractName(data, dataLen, name, sizeof(name));
    JT_LOGF("[SYX] Loaded: \"%s\" (%u bytes)\n", name, dataLen);
}

// =============================================================================
// scanBuffer — build patch index from a complete file in RAM
// =============================================================================
int Loader::scanBuffer(const uint8_t* buf, uint32_t len, bool isMidi)
{
    _count = 0;
    if (!buf || len < 12) return 0;

    return isMidi ? _scanMidi(buf, len) : _scanSyx(buf, len);
}

// =============================================================================
// patchName — return name for a given patch index
// =============================================================================
const char* Loader::patchName(int idx) const
{
    if (idx < 0 || idx >= _count) return "";
    return _patches[idx].name;
}

// =============================================================================
// loadPatch — load a specific patch from the previously scanned buffer
// =============================================================================
bool Loader::loadPatch(SynthEngine& synth, const uint8_t* buf, uint32_t bufLen,
                       int patchIdx, uint8_t midiCh)
{
    if (patchIdx < 0 || patchIdx >= _count) return false;

    const PatchEntry& pe = _patches[patchIdx];

    // Bounds check: the data region must fit within the buffer
    if (pe.fileOffset + pe.dataLen > bufLen) return false;

    loadPatchData(synth, buf + pe.fileOffset, pe.dataLen, midiCh);
    return true;
}

// =============================================================================
// _tryRegisterPatch — validate and register a DT1 message as a patch
//
// buf + msgStart points to the byte AFTER F0 (manufacturer ID).
// msgLen is the count of bytes from manufacturer ID to just before F7.
// =============================================================================
bool Loader::_tryRegisterPatch(const uint8_t* buf, uint32_t msgStart, uint32_t msgLen)
{
    if (_count >= MAX_PATCHES_PER_FILE) return false;
    if (msgLen < 10) return false; // Need at least header + 1 data byte

    const uint8_t* msg = buf + msgStart;

    // Validate Roland DT1 header: 41 <dev> 00 06 12
    if (msg[0] != ROLAND_ID)  return false;
    // msg[1] = device ID (don't filter — accept any)
    if (msg[2] != MODEL_MSB)  return false;
    if (msg[3] != MODEL_LSB)  return false;
    if (msg[4] != CMD_DT1)    return false;

    // Address bytes: msg[5..8]
    // Data starts at msg[9], checksum is the last byte before F7
    const uint32_t headerLen = 9;  // bytes before data (mfr+dev+model+cmd+addr)
    if (msgLen <= headerLen + 1) return false; // need data + checksum

    const uint32_t dataLen = msgLen - headerLen - 1; // exclude checksum
    if (dataLen < MIN_PATCH_DATA_LEN) return false;

    // Register this patch
    PatchEntry& pe = _patches[_count];
    pe.fileOffset = msgStart + headerLen;  // points to first data byte in buffer
    pe.dataLen    = (uint16_t)(dataLen > MAX_PATCH_DATA_LEN ? MAX_PATCH_DATA_LEN : dataLen);

    // Extract name from the data
    const uint8_t* patchData = buf + pe.fileOffset;
    JP8000SyxMap::extractName(patchData, pe.dataLen, pe.name, PATCH_NAME_LEN);

    _count++;
    return true;
}

// =============================================================================
// _scanSyx — scan raw .syx file (sequential F0..F7 messages)
// =============================================================================
int Loader::_scanSyx(const uint8_t* buf, uint32_t len)
{
    uint32_t pos = 0;

    while (pos < len) {
        // Find next F0
        while (pos < len && buf[pos] != 0xF0) ++pos;
        if (pos >= len) break;

        const uint32_t f0Pos = pos;
        ++pos; // skip F0

        // Find matching F7
        while (pos < len && buf[pos] != 0xF7) ++pos;
        if (pos >= len) break;

        // msg body is between F0+1 and F7 (exclusive)
        const uint32_t msgStart = f0Pos + 1;
        const uint32_t msgLen   = pos - msgStart; // bytes from after F0 to before F7

        _tryRegisterPatch(buf, msgStart, msgLen);

        ++pos; // skip F7
    }

    JT_LOGF("[SYX] Scanned .syx: %d patches\n", _count);
    return _count;
}

// =============================================================================
// _readVLQ — read a MIDI variable-length quantity
// =============================================================================
uint32_t Loader::_readVLQ(const uint8_t* buf, uint32_t len, uint32_t& pos)
{
    uint32_t val = 0;
    while (pos < len) {
        uint8_t b = buf[pos++];
        val = (val << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return val;
}

// =============================================================================
// _scanMidi — scan a standard MIDI file for SysEx events
//
// Minimal MIDI parser: handles MThd + MTrk, reads delta times and status
// bytes just enough to extract F0 (SysEx) events. Ignores all other events.
// =============================================================================
int Loader::_scanMidi(const uint8_t* buf, uint32_t len)
{
    // Verify MIDI header
    if (len < 14) return 0;
    if (buf[0] != 'M' || buf[1] != 'T' || buf[2] != 'h' || buf[3] != 'd') return 0;

    const uint32_t headerLen = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                               ((uint32_t)buf[6] << 8)  | (uint32_t)buf[7];
    const uint16_t nTracks = ((uint16_t)buf[10] << 8) | buf[11];

    uint32_t pos = 8 + headerLen;

    for (uint16_t trk = 0; trk < nTracks && pos + 8 <= len; ++trk) {
        // Verify track header
        if (buf[pos] != 'M' || buf[pos+1] != 'T' ||
            buf[pos+2] != 'r' || buf[pos+3] != 'k') break;

        const uint32_t trkLen = ((uint32_t)buf[pos+4] << 24) | ((uint32_t)buf[pos+5] << 16) |
                                ((uint32_t)buf[pos+6] << 8)  | (uint32_t)buf[pos+7];
        const uint32_t trkStart = pos + 8;
        const uint32_t trkEnd   = trkStart + trkLen;

        uint32_t p = trkStart;

        while (p < trkEnd && p < len) {
            // Read delta time (VLQ)
            _readVLQ(buf, trkEnd, p);
            if (p >= trkEnd) break;

            const uint8_t status = buf[p];

            if (status == 0xF0) {
                // SysEx event: F0 <vlq_length> <data...including F7>
                ++p; // skip F0
                uint32_t msgLen = _readVLQ(buf, trkEnd, p);

                // The body (after F0) is at position p, length msgLen.
                // It includes the trailing F7 byte, so actual message content
                // is msgLen-1 bytes (from manufacturer ID to checksum).
                if (msgLen > 0 && p + msgLen <= len) {
                    // msgLen includes F7 at the end
                    const uint32_t contentLen = msgLen - 1; // exclude F7
                    _tryRegisterPatch(buf, p, contentLen);
                }
                p += msgLen;

            } else if (status == 0xFF) {
                // Meta event: FF <type> <vlq_length> <data>
                p += 2; // skip FF + type
                uint32_t metaLen = _readVLQ(buf, trkEnd, p);
                p += metaLen;

            } else if (status >= 0x80) {
                // Regular MIDI message — skip based on status
                p++; // skip status byte
                if (status >= 0xC0 && status < 0xE0) {
                    p += 1;  // program change / channel pressure: 1 data byte
                } else if (status >= 0x80 && status < 0xC0) {
                    p += 2;  // note on/off, CC, pitch bend: 2 data bytes
                } else if (status >= 0xE0 && status < 0xF0) {
                    p += 2;  // pitch bend: 2 data bytes
                } else if (status == 0xF2) {
                    p += 2;  // song position
                } else if (status == 0xF3) {
                    p += 1;  // song select
                }
                // F1, F4-F6, F8-FE: no data bytes

            } else {
                // Running status or unexpected byte — advance to avoid infinite loop
                p++;
            }
        }

        pos = trkEnd;
    }

    JT_LOGF("[SYX] Scanned .mid: %d patches\n", _count);
    return _count;
}

} // namespace JP8000SyxLoader
