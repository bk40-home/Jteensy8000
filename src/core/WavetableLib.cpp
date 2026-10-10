// =============================================================================
// WavetableLib.cpp — implementation (the ONLY includer of the AKWF headers)
// =============================================================================
// See WavetableLib.h for the firewall rationale and the v1 semantics.
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "core/WavetableLib.h"

#include "core/AudioConfig.h"   // JT_COLD

// The 2.1 MB catalogue enters the build HERE and nowhere else.
#include "data/akwf/AKWF_All.h"
// Measured JP-8000 SHAPE morphs (~306 KB flash) — same firewall, same folder.
#include "data/akwf/JpMorph/JpMorph_VSaw.h"
#include "data/akwf/JpMorph/JpMorph_VTri.h"

namespace JT {
namespace WavetableLib {

JT_COLD uint16_t bankCount(int bank)
{
    if (bank < 0 || bank >= kNumBanks) return 0;
    return akwf_bankCount((ArbBank)bank);
}

JT_COLD const int16_t* akwfTable(int bank, int index, uint16_t& lenOut)
{
    lenOut = 0;
    if (bank < 0 || bank >= kNumBanks) return nullptr;

    // Clamp the index to the bank's real size — a bank switch while the
    // index knob sits high must land on the new bank's last wave, not on
    // the per-bank accessor's nullptr path (v1 behaviour).
    const uint16_t count = akwf_bankCount((ArbBank)bank);
    if (count == 0) return nullptr;
    if (index < 0)             index = 0;
    if (index >= (int)count)   index = (int)count - 1;

    return akwf_get((ArbBank)bank, (uint16_t)index, lenOut);
}

JT_COLD int bankFromNorm(float norm01)
{
    // v1: bank = value*10/128 over CC — i.e. an even bucket per bank.
    if (norm01 < 0.0f) norm01 = 0.0f;
    int bank = (int)(norm01 * (float)kNumBanks);
    if (bank >= kNumBanks) bank = kNumBanks - 1;    // norm==1.0 edge
    return bank;
}

namespace {
// One descriptor per generated set, built from the generated header's own
// constants — a regenerated table with different frame/level geometry needs
// no edit here.  constexpr: built at compile time (no static-init order
// hazard), 7 fields each; the sample data they point at stays in flash.
constexpr WaveTableSet makeSet(const int16_t* data, uint32_t spf, uint16_t frames,
                     uint16_t levels, const uint16_t* off,
                     const uint16_t* len, const uint16_t* harm)
{
    WaveTableSet s;
    s.data = data;  s.samplesPerFrame = spf;  s.frames = frames;  s.levels = levels;
    s.levelOffset = off;  s.levelLen = len;  s.levelHarm = harm;
    return s;
}

constexpr WaveTableSet kJpVSaw = makeSet(
    JpMorphVSaw::kData, JpMorphVSaw::kSamplesPerFrame, JpMorphVSaw::kFrames,
    JpMorphVSaw::kLevels, JpMorphVSaw::kLevelOffset, JpMorphVSaw::kLevelLen,
    JpMorphVSaw::kLevelHarm);

constexpr WaveTableSet kJpVTri = makeSet(
    JpMorphVTri::kData, JpMorphVTri::kSamplesPerFrame, JpMorphVTri::kFrames,
    JpMorphVTri::kLevels, JpMorphVTri::kLevelOffset, JpMorphVTri::kLevelLen,
    JpMorphVTri::kLevelHarm);
} // namespace

JT_COLD const WaveTableSet* jpMorphSet(Wave w)
{
    switch (w) {
    case Wave::JpVarSaw: return &kJpVSaw;
    case Wave::JpVarTri: return &kJpVTri;
    default:             return nullptr;
    }
}

JT_COLD int indexFromNorm(float norm01, int bank)
{
    if (norm01 < 0.0f) norm01 = 0.0f;
    const uint16_t count = bankCount(bank);
    if (count == 0) return 0;
    int idx = (int)(norm01 * (float)count);
    if (idx >= (int)count) idx = (int)count - 1;
    return idx;
}

} // namespace WavetableLib
} // namespace JT
