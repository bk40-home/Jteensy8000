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
