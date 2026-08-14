// =============================================================================
// Performance.h — dual-layer performance container for JT-8000 v2
// =============================================================================
//
// ROLE
//   A PATCH is one layer's sound (Patch.h, magic "JTP1").  A PERFORMANCE is the
//   pair of them plus the setup that decides how they are played: mode, split
//   point, the two receive channels, balance, and the voice split.
//
//   Deliberately a WRAPPER, not a new value format.  The two layers are stored
//   as complete, unmodified JTP1 images, so:
//     * "load patch into layer B" is a plain Patch::load with layer = 1 — no
//       second code path, no format to keep in sync
//     * a layer can be extracted from a performance and saved as a patch by
//       copying bytes, with no re-encode and no rounding
//     * every guarantee JTP1 already provides (CRC, schema version, retired-id
//       migration) applies to each layer for free
//
//   This mirrors the editor, which already models it this way — see
//   JP8000SyxImporter's LoadTarget::LayerA / LayerB / Current and its
//   loadPatch / loadPerformance pair.
//
// WIRE FORMAT (little-endian, byte-wise access — any alignment)
//
//   offset size  field
//   0      4     magic "JTF1"
//   4      2     schemaVersion  (same generated constant as JTP1)
//   6      16    name           (printable ASCII, NUL-padded)
//   22     1     category
//   23     1     perfMode       option index: 0 Single, 1 Layer, 2 Split
//   24     1     splitNote      0..127
//   25     1     midiChannelA   1..16
//   26     1     midiChannelB   1..16
//   27     1     balance        0..127, 64 = centre
//   28     1     voiceSplit     option index (0 == "1+7")
//   29     3     reserved       (zero)
//   32     4     lenA           bytes of layer A's JTP1 image
//   36     4     lenB           bytes of layer B's JTP1 image
//   40     lenA  layer A image
//   ...    lenB  layer B image
//   end    4     CRC-32 over every byte before this field
//
//   The outer CRC covers the inner images too.  That is redundant with their
//   own CRCs and intentionally so: it catches a performance file whose halves
//   were each individually valid but spliced from different saves.
//
// LOAD ORDER
//   Layer A is applied FIRST, then layer B.  It matters: fx.* and seq.* are
//   Scope::PatchShared — one instance, shared by both layers — so whichever
//   layer is applied last owns the FX chain and the sequencer.  Defining the
//   order here means a performance always reloads to the same sound instead of
//   depending on which half happened to be written first.
//
// CONCURRENCY
//   Control plane only.  The whole load — both layers and the setup — is
//   wrapped in ONE beginBulk()/endBulk(), so the audio plane hears the old
//   performance until the entire new one is published.  A performance is
//   therefore as atomic as a patch, not twice as torn.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "core/ParameterStore.h"
#include "core/Patch.h"

namespace JT {
namespace Performance {

inline constexpr uint8_t kMagic[4]   = { 'J', 'T', 'F', '1' };
inline constexpr size_t  kNameLen    = Patch::kNameLen;
inline constexpr size_t  kHeaderSize = 40;
inline constexpr size_t  kCrcSize    = 4;

// Worst case: header + two maximal patch images + CRC.
inline constexpr size_t kMaxEncodedSize =
    kHeaderSize + 2 * Patch::kMaxEncodedSize + kCrcSize;

// The setup half of a performance — everything that is not a layer's sound.
// Held as a plain struct so the codec and the caller agree on it explicitly,
// rather than the codec reaching into the store for seven scattered ids.
struct Setup {
    uint8_t perfMode     = 0;    // 0 Single, 1 Layer, 2 Split
    uint8_t splitNote    = 60;
    uint8_t midiChannelA = 1;    // 1..16 as the wire and the UI both speak it
    uint8_t midiChannelB = 2;
    uint8_t balance      = 64;   // 0..127, 64 = centre (both layers at unity)
    uint8_t voiceSplit   = 3;    // option index; 3 == "4+4"
};

enum class Status : uint8_t {
    Ok,
    TooShort,
    BadMagic,
    BadCrc,
    FutureVersion,
    SizeMismatch,
    BadLayer,        // an inner JTP1 image failed its own validation
};

struct Info {
    char     name[kNameLen + 1];
    uint8_t  category;
    uint16_t schemaVersion;
    Setup    setup;
};

struct LoadResult {
    Status           status;
    Info             info;        // valid when status == Ok
    Patch::LoadResult layerA;     // per-layer diagnostics, valid when Ok
    Patch::LoadResult layerB;
};

// -----------------------------------------------------------------------------
// API — control-plane, heap-free, bounds-checked.
// -----------------------------------------------------------------------------

// Capture BOTH layers and the supplied setup.  The setup is passed in rather
// than read from the store because perf.* is Scope::Performance: the caller
// (which owns the router and the engine) is the authority on it, and a codec
// silently sampling live state is how a "save" ends up recording something the
// user never saw.
size_t save(const ParameterStore& store,
            const char* name, uint8_t category, const Setup& setup,
            uint8_t* out, size_t cap);

// Validate and apply a whole performance.  On any status other than Ok the
// store is COMPLETELY untouched: both inner images are validated before the
// bulk write begins, so a corrupt layer B cannot leave layer A half-loaded.
LoadResult load(const uint8_t* data, size_t len,
                ParameterStore& store,
                Origin origin = Origin::PatchLoad,
                const Patch::Migration* migrations = nullptr,
                size_t migrationCount = 0);

// Header-only read for the browser: name, category and setup without touching
// a store and without CRC-checking the (potentially large) whole image.
bool peekInfo(const uint8_t* data, size_t len, Info& out);

// Borrow one layer's JTP1 image out of a performance, so "extract layer B as a
// patch" is a pointer and a length rather than a decode/re-encode round trip.
// Returns false if the image is malformed.  The pointer is into 'data' and is
// valid exactly as long as 'data' is.
bool layerImage(const uint8_t* data, size_t len, uint8_t layer,
                const uint8_t*& imageOut, size_t& imageLenOut);

} // namespace Performance
} // namespace JT
