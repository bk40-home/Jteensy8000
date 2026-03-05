// Auto‑generated AKWF bank. Do not edit manually.

#pragma once
#include <Arduino.h>

#include "AKWF_BwSawgap_Tables_Part0.h"
#include "AKWF_BwSawgap_Tables_Part1.h"

namespace AKWF_BwSawgap {
    constexpr uint16_t count = 42;
}

// Return a pointer to the n‑th waveform table in this bank.
// If index is out of range, returns nullptr and length is set to 0.
inline const int16_t* AKWF_BwSawgap_get(uint16_t index, uint16_t &length) {
    switch(index) {
        case 0: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0001;
        case 1: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0002;
        case 2: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0003;
        case 3: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0004;
        case 4: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0005;
        case 5: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0006;
        case 6: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0007;
        case 7: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0008;
        case 8: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0009;
        case 9: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0010;
        case 10: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0011;
        case 11: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0012;
        case 12: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0013;
        case 13: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0014;
        case 14: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0015;
        case 15: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0016;
        case 16: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0017;
        case 17: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0018;
        case 18: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0019;
        case 19: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0020;
        case 20: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0021;
        case 21: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0022;
        case 22: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0023;
        case 23: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0024;
        case 24: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0025;
        case 25: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0026;
        case 26: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0027;
        case 27: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0028;
        case 28: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0029;
        case 29: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0030;
        case 30: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0031;
        case 31: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0032;
        case 32: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0033;
        case 33: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0034;
        case 34: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0035;
        case 35: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0036;
        case 36: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0037;
        case 37: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0038;
        case 38: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0039;
        case 39: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0040;
        case 40: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0041;
        case 41: length = 600; return AKWF_BwSawgap::akwf_akwf_bwsawgap_akwf_gapsaw_0042;
        default: length = 0; return nullptr;
    }
}
