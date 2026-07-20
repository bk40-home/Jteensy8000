// =============================================================================
// OBXaCore.cpp — flash-resident constant tables for the OBXa filter core
// =============================================================================
// The core's CODE stays inline in the header (hot per-sample path); its
// DATA lives here with plain linkage so the .progmem section carries one
// consistent symbol flavour (see the linkage-rule comment in the header).
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "core/dsp/OBXaCore.h"

namespace JT {

// Verbatim v1/OB-Xf.  Each row mixes the five taps {y0, y1, y2, y3, y4}
// into one classic Xpander response.  Read at most once per block.
const float kObxaPoleMix[15][5] JT_FLASH_DATA = {
    { 0,  0,  0,  0,  1 },   //  0: LP4
    { 0,  0,  0,  1,  0 },   //  1: LP3
    { 0,  0,  1,  0,  0 },   //  2: LP2
    { 0,  1,  0,  0,  0 },   //  3: LP1
    { 1, -3,  3, -1,  0 },   //  4: HP3
    { 1, -2,  1,  0,  0 },   //  5: HP2
    { 1, -1,  0,  0,  0 },   //  6: HP1
    { 0,  0,  2, -4,  2 },   //  7: BP4
    { 0, -2,  2,  0,  0 },   //  8: BP2
    { 1, -2,  2,  0,  0 },   //  9: N2
    { 1, -3,  6, -4,  0 },   // 10: PH3
    { 0, -1,  2, -1,  0 },   // 11: HP2+LP1
    { 0, -1,  3, -3,  1 },   // 12: HP3+LP1
    { 0, -1,  2, -2,  0 },   // 13: N2+LP1
    { 0, -1,  3, -6,  4 },   // 14: PH3+LP1
};

} // namespace JT
