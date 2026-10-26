// =============================================================================
// JpXmodLaw.cpp — implementation (measurements and table in the .h)
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "core/dsp/JpXmodLaw.h"

#include <cmath>   // fabsf

namespace JT {
namespace JpXmodLaw {

namespace {
// Grid fit, 2026-10-25 (medians; ~4 % fit steps).
constexpr float kSawScale     = 0.50f;    // 34.5 / 69 Hz per step
constexpr float kPulseScale50 = 0.296f;   // 20.4 / 69 at a 50 % square
// 0.277 at 79 % (19.1 / 69): linear in the pulse's asymmetry |2w - 1|
// (0 for a square, 0.58 at 79 %).
constexpr float kPulseSlope   = (0.296f - 0.277f) / 0.58f;
// SYNC factor for the non-pulse waves (2026-10-24 C1 capture, 0.622-0.627;
// the grid's triangle cells land on 43.0 Hz/step = 0.623 again).  Pulse
// modulators keep their full depth under sync (grid: 20.4 both ways).
constexpr float kSyncScale    = 0.623f;
} // namespace

float depthScale(Wave w2, float pulseWidth, bool sync)
{
    switch (w2) {
        // Pulse family: width-dependent, sync-independent.
        case Wave::Square:
        case Wave::BlSquare:
            return kPulseScale50;
        case Wave::Pulse:
        case Wave::BlPulse:
            return kPulseScale50 - kPulseSlope * fabsf(2.0f * pulseWidth - 1.0f);

        // Saw family.
        case Wave::Saw:
        case Wave::SawRev:
        case Wave::BlSaw:
        case Wave::BlSawRev:
        case Wave::VarSaw:
        case Wave::BlVarSaw:
        case Wave::JpVarSaw:
            return sync ? kSawScale * kSyncScale : kSawScale;

        // Triangle family and everything unmeasured: the calibrated law.
        default:
            return sync ? kSyncScale : 1.0f;
    }
}

} // namespace JpXmodLaw
} // namespace JT
