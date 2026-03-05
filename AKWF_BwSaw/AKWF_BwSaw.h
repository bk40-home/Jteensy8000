// Auto‑generated AKWF bank. Do not edit manually.

#pragma once
#include <Arduino.h>

#include "AKWF_BwSaw_Tables_Part0.h"
#include "AKWF_BwSaw_Tables_Part1.h"
#include "AKWF_BwSaw_Tables_Part2.h"

namespace AKWF_BwSaw {
    constexpr uint16_t count = 50;
}

// Return a pointer to the n‑th waveform table in this bank.
// If index is out of range, returns nullptr and length is set to 0.
inline const int16_t* AKWF_BwSaw_get(uint16_t index, uint16_t &length) {
    switch(index) {
        case 0: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0001;
        case 1: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0002;
        case 2: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0003;
        case 3: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0004;
        case 4: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0005;
        case 5: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0006;
        case 6: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0007;
        case 7: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0008;
        case 8: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0009;
        case 9: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0010;
        case 10: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0011;
        case 11: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0012;
        case 12: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0013;
        case 13: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0014;
        case 14: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0015;
        case 15: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0016;
        case 16: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0017;
        case 17: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0018;
        case 18: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0019;
        case 19: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0020;
        case 20: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0021;
        case 21: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0022;
        case 22: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0023;
        case 23: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0024;
        case 24: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0025;
        case 25: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0026;
        case 26: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0027;
        case 27: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0028;
        case 28: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0029;
        case 29: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0030;
        case 30: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0031;
        case 31: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0032;
        case 32: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0033;
        case 33: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0034;
        case 34: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0035;
        case 35: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0036;
        case 36: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0037;
        case 37: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0038;
        case 38: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0039;
        case 39: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0040;
        case 40: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0041;
        case 41: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0042;
        case 42: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0043;
        case 43: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0044;
        case 44: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0045;
        case 45: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0046;
        case 46: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0047;
        case 47: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0048;
        case 48: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0049;
        case 49: length = 600; return AKWF_BwSaw::akwf_akwf_bwsaw_akwf_saw_0050;
        default: length = 0; return nullptr;
    }
}
