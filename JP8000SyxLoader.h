#pragma once
// =============================================================================
// JP8000SyxLoader.h — JP-8000/8080 SysEx patch parser and loader
//
// Parses .syx and .mid files containing Roland JP-8000/8080 DT1 patch dumps.
// Extracts individual patches and loads them into SynthEngine via the
// standard handleControlChange() path, using JP8000SyxMap.h for the
// offset→CC translation.
//
// TWO USAGE MODES:
//
//   1. BUFFER MODE — parse a complete file already in RAM:
//        JP8000SyxLoader loader;
//        int n = loader.scanBuffer(fileData, fileLen, isMidi);
//        loader.loadPatch(synth, patchIndex);
//
//   2. STREAM MODE (for SD card) — parse from a File handle:
//        JP8000SyxLoader loader;
//        int n = loader.scanFile(sdFile, isMidi);
//        loader.loadPatchFromFile(synth, sdFile, patchIndex);
//
// MEMORY:
//   The loader stores only an index (file offsets + names) — never the full
//   file. Maximum 128 patches per file (one JP-8000 bank). Index cost:
//   128 × (4 bytes offset + 17 bytes name) = ~2.7 KB.
//
// THREAD SAFETY:
//   loadPatch() and loadPatchFromFile() bracket CC dispatch with
//   AudioNoInterrupts/AudioInterrupts, same as all other preset loaders.
// =============================================================================

#include <Arduino.h>

// Forward declare — avoids pulling in full SynthEngine header here
class SynthEngine;

namespace JP8000SyxLoader {

// ── Constants ────────────────────────────────────────────────────────────────
static constexpr int     MAX_PATCHES_PER_FILE = 128;
static constexpr uint8_t PATCH_NAME_LEN       = 17;   // 16 chars + null

// Minimum data length for a valid JP-8000 patch (after DT1 header).
// JP-8000 sends 239 bytes, JP-8080 sends 248. Accept anything >= 74
// (enough for all synthesis params through 0x49).
static constexpr uint16_t MIN_PATCH_DATA_LEN  = 74;

// Maximum data length we'll read for a single patch.
static constexpr uint16_t MAX_PATCH_DATA_LEN  = 256;

// Roland DT1 SysEx signature bytes (after F0):
//   41 <dev> 00 06 12
static constexpr uint8_t ROLAND_ID     = 0x41;
static constexpr uint8_t MODEL_MSB     = 0x00;
static constexpr uint8_t MODEL_LSB     = 0x06;
static constexpr uint8_t CMD_DT1       = 0x12;

// ── Patch index entry ────────────────────────────────────────────────────────
struct PatchEntry {
    uint32_t fileOffset;                 // Byte offset to the DATA start in file
    uint16_t dataLen;                    // Length of patch data (excl. header/checksum)
    char     name[PATCH_NAME_LEN];       // Null-terminated display name
};

// ── Loader class ─────────────────────────────────────────────────────────────
class Loader {
public:
    Loader() = default;

    // ── Scanning ─────────────────────────────────────────────────────────

    // Scan a complete buffer (file already in RAM).
    // isMidi = true for .mid files, false for raw .syx.
    // Returns number of patches found (0 on error).
    int scanBuffer(const uint8_t* buf, uint32_t len, bool isMidi);

    // ── Patch info ───────────────────────────────────────────────────────

    int  patchCount() const { return _count; }

    // Get patch name by index. Returns empty string if out of range.
    const char* patchName(int idx) const;

    // ── Loading (buffer mode) ────────────────────────────────────────────

    // Load a patch from the previously scanned buffer into the synth.
    // The buffer pointer must still be valid (same data passed to scanBuffer).
    // Returns true on success.
    bool loadPatch(SynthEngine& synth, const uint8_t* buf, uint32_t bufLen,
                   int patchIdx, uint8_t midiCh = 1);

    // ── Loading (single-shot, no prior scan needed) ──────────────────────

    // Load a single patch directly from a data block (the 240-byte region
    // after the DT1 address bytes, before checksum).
    // This is the core routine — scanBuffer/loadPatch ultimately call this.
    static void loadPatchData(SynthEngine& synth, const uint8_t* data,
                              uint16_t dataLen, uint8_t midiCh = 1);

    // Extract patch name from a data block (no synth interaction).
    static void getPatchName(const uint8_t* data, uint16_t dataLen,
                             char* nameOut, uint8_t maxLen);

private:
    // ── Internal scanning ────────────────────────────────────────────────

    // Scan raw .syx data (sequential F0..F7 messages).
    int _scanSyx(const uint8_t* buf, uint32_t len);

    // Scan MIDI file data (parse MTrk, extract F0 events).
    int _scanMidi(const uint8_t* buf, uint32_t len);

    // Try to register a DT1 message as a patch.
    // msgStart = offset of the byte AFTER F0 (i.e. manufacturer ID byte).
    // msgLen   = number of bytes from manufacturer ID to just before F7.
    // Returns true if it was a valid JP-8000 patch.
    bool _tryRegisterPatch(const uint8_t* buf, uint32_t msgStart, uint32_t msgLen);

    // Read a MIDI variable-length quantity.
    static uint32_t _readVLQ(const uint8_t* buf, uint32_t len, uint32_t& pos);

    // ── Index storage ────────────────────────────────────────────────────
    PatchEntry _patches[MAX_PATCHES_PER_FILE];
    int        _count = 0;
};

} // namespace JP8000SyxLoader
