// =============================================================================
// Performance.cpp — implementation
// =============================================================================
// Format, load order and concurrency rationale live in Performance.h.
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "core/Performance.h"

#include <string.h>

#include "core/AudioConfig.h"      // JT_COLD
#include "core/dsp/Crc32.h"
#include "core/dsp/Curves.h"
#include "gen/ParamTable.h"

namespace JT {
namespace Performance {

namespace {

// Byte-wise little-endian access, so the image may sit at any alignment.
// Duplicated from Patch.cpp rather than shared: four two-line helpers are a
// smaller cost than a header that exists only to couple two codecs together.
void     put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
uint16_t get16(const uint8_t* p)       { return (uint16_t)(p[0] | (p[1] << 8)); }

void put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
uint32_t get32(const uint8_t* p)
{
    return (uint32_t)p[0]        | ((uint32_t)p[1] << 8) |
          ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

char sanitise(char c) { return (c >= 32 && c <= 126) ? c : '_'; }

uint8_t clamp7(int v) { return (uint8_t)((v < 0) ? 0 : (v > 127) ? 127 : v); }
uint8_t clampCh(int v) { return (uint8_t)((v < 1) ? 1 : (v > 16) ? 16 : v); }

// Parse and bounds-check the header without touching the CRC.  Shared by
// peekInfo, layerImage and load so the three cannot disagree about where the
// inner images start — the classic way a container format grows a bug.
bool parseHeader(const uint8_t* data, size_t len,
                 Info& out, size_t& lenA, size_t& lenB, Status& why)
{
    why = Status::TooShort;
    if (data == nullptr || len < kHeaderSize + kCrcSize) return false;

    if (memcmp(data, kMagic, sizeof kMagic) != 0) { why = Status::BadMagic; return false; }

    out.schemaVersion = get16(data + 4);
    if (out.schemaVersion > Params::kSchemaVersion) {
        why = Status::FutureVersion;    // refuse rather than guess
        return false;
    }

    for (size_t i = 0; i < kNameLen; ++i) out.name[i] = (char)data[6 + i];
    out.name[kNameLen] = '\0';
    out.category = data[22];

    out.setup.perfMode     = data[23];
    out.setup.splitNote    = data[24];
    out.setup.midiChannelA = data[25];
    out.setup.midiChannelB = data[26];
    out.setup.balance      = data[27];
    out.setup.voiceSplit   = data[28];

    lenA = get32(data + 32);
    lenB = get32(data + 36);

    // Overflow-safe: compare against the remaining budget instead of summing
    // two attacker-controlled 32-bit lengths and hoping the total fits.
    const size_t budget = len - kHeaderSize - kCrcSize;
    if (lenA > budget || lenB > budget - lenA) { why = Status::SizeMismatch; return false; }
    if (kHeaderSize + lenA + lenB + kCrcSize != len) { why = Status::SizeMismatch; return false; }

    why = Status::Ok;
    return true;
}

} // namespace

// -----------------------------------------------------------------------------
// save
// -----------------------------------------------------------------------------
JT_COLD size_t save(const ParameterStore& store,
                    const char* name, uint8_t category, const Setup& setup,
                    uint8_t* out, size_t cap)
{
    if (out == nullptr || cap < kHeaderSize + kCrcSize) return 0;

    // Encode the two layers straight into their final position, so there is no
    // scratch buffer and no copy — the images are built where they will live.
    const size_t roomForLayers = cap - kHeaderSize - kCrcSize;

    const size_t lenA = Patch::save(store, name, category,
                                    out + kHeaderSize, roomForLayers, 0);
    if (lenA == 0) return 0;

    const size_t lenB = Patch::save(store, name, category,
                                    out + kHeaderSize + lenA,
                                    roomForLayers - lenA, 1);
    if (lenB == 0) return 0;

    memcpy(out, kMagic, sizeof kMagic);
    put16(out + 4, Params::kSchemaVersion);

    for (size_t i = 0; i < kNameLen; ++i) {
        const char c = (name != nullptr && name[i] != '\0') ? name[i] : ' ';
        out[6 + i] = (uint8_t)sanitise(c);
        if (name == nullptr || name[i] == '\0') {
            for (size_t j = i; j < kNameLen; ++j) out[6 + j] = (uint8_t)' ';
            break;
        }
    }

    out[22] = category;
    out[23] = setup.perfMode;
    out[24] = clamp7(setup.splitNote);
    out[25] = clampCh(setup.midiChannelA);
    out[26] = clampCh(setup.midiChannelB);
    out[27] = clamp7(setup.balance);
    out[28] = setup.voiceSplit;
    out[29] = out[30] = out[31] = 0;      // reserved: zero, not left as garbage

    put32(out + 32, (uint32_t)lenA);
    put32(out + 36, (uint32_t)lenB);

    const size_t total = kHeaderSize + lenA + lenB;
    put32(out + total, Crc32::compute(out, total));
    return total + kCrcSize;
}

// -----------------------------------------------------------------------------
// peekInfo / layerImage
// -----------------------------------------------------------------------------
JT_COLD bool peekInfo(const uint8_t* data, size_t len, Info& out)
{
    size_t lenA = 0, lenB = 0;
    Status why  = Status::Ok;
    return parseHeader(data, len, out, lenA, lenB, why);
}

JT_COLD bool layerImage(const uint8_t* data, size_t len, uint8_t layer,
                        const uint8_t*& imageOut, size_t& imageLenOut)
{
    Info   info{};
    size_t lenA = 0, lenB = 0;
    Status why  = Status::Ok;
    if (!parseHeader(data, len, info, lenA, lenB, why)) return false;

    imageOut    = (layer == 0u) ? data + kHeaderSize : data + kHeaderSize + lenA;
    imageLenOut = (layer == 0u) ? lenA : lenB;
    return true;
}

// -----------------------------------------------------------------------------
// load
// -----------------------------------------------------------------------------
JT_COLD LoadResult load(const uint8_t* data, size_t len,
                        ParameterStore& store,
                        Origin origin,
                        const Patch::Migration* migrations,
                        size_t migrationCount)
{
    LoadResult r{};
    size_t lenA = 0, lenB = 0;
    Status why  = Status::Ok;

    if (!parseHeader(data, len, r.info, lenA, lenB, why)) { r.status = why; return r; }

    // Outer CRC before anything else touches the inner images: it also catches
    // a file whose two halves were each valid but spliced from different saves.
    const size_t body = len - kCrcSize;
    if (Crc32::compute(data, body) != get32(data + body)) {
        r.status = Status::BadCrc;
        return r;
    }

    const uint8_t* imgA = data + kHeaderSize;
    const uint8_t* imgB = imgA + lenA;

    // VALIDATE BOTH BEFORE WRITING EITHER.  Patch::load against a throwaway
    // store is the honest way to ask "would this apply cleanly?" — it exercises
    // the identical code path the real load will take, rather than a
    // reimplementation of it that could drift.  The scratch store is a stack
    // object; it costs ~2.5 KB briefly on the control plane, never in the ISR.
    {
        ParameterStore probe;
        const Patch::LoadResult a =
            Patch::load(imgA, lenA, probe, origin, migrations, migrationCount, 0);
        const Patch::LoadResult b =
            Patch::load(imgB, lenB, probe, origin, migrations, migrationCount, 1);
        if (a.status != Patch::Status::Ok || b.status != Patch::Status::Ok) {
            r.status = Status::BadLayer;
            return r;
        }
    }

    // One bulk for the WHOLE performance: the audio plane hears the old one
    // until every last value of the new one is published.  Patch::load opens
    // its own bulk internally; ParameterStore nests, so the inner pairs are
    // no-ops and only this outermost endBulk() publishes.
    store.beginBulk();

    r.layerA = Patch::load(imgA, lenA, store, origin, migrations, migrationCount, 0);
    r.layerB = Patch::load(imgB, lenB, store, origin, migrations, migrationCount, 1);

    // Setup last: perf.* is Scope::Performance and single-instance, so it does
    // not care which layer wrote it, but applying it after the layers means the
    // engine repartitions the voice pool against the patches that are already
    // in place rather than against the outgoing ones.
    const Setup& s = r.info.setup;
    store.setEngineering(Params::ID::PERF_MODE,           (float)s.perfMode,       origin);
    store.setEngineering(Params::ID::PERF_SPLIT_NOTE,     (float)s.splitNote,      origin);
    store.setEngineering(Params::ID::PERF_MIDI_CHANNEL_A, (float)(s.midiChannelA - 1), origin);
    store.setEngineering(Params::ID::PERF_MIDI_CHANNEL_B, (float)(s.midiChannelB - 1), origin);
    store.setEngineering(Params::ID::PERF_BALANCE,        (float)s.balance,        origin);
    store.setEngineering(Params::ID::PERF_VOICE_SPLIT,    (float)s.voiceSplit,     origin);

    store.endBulk();

    r.status = Status::Ok;
    return r;
}

} // namespace Performance
} // namespace JT
