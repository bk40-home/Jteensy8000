// Auto‑generated AKWF bank. Do not edit manually.

#pragma once
#include <Arduino.h>

#include "AKWF_BwBlended_Tables_Part0.h"
#include "AKWF_BwBlended_Tables_Part1.h"
#include "AKWF_BwBlended_Tables_Part2.h"
#include "AKWF_BwBlended_Tables_Part3.h"

namespace AKWF_BwBlended {
    constexpr uint16_t count = 73;
}

// Return a pointer to the n‑th waveform table in this bank.
// If index is out of range, returns nullptr and length is set to 0.
inline const int16_t* AKWF_BwBlended_get(uint16_t index, uint16_t &length) {
    switch(index) {
        case 0: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0001;
        case 1: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0002;
        case 2: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0003;
        case 3: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0004;
        case 4: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0005;
        case 5: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0006;
        case 6: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0007;
        case 7: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0008;
        case 8: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0009;
        case 9: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0011;
        case 10: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0012;
        case 11: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0013;
        case 12: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0014;
        case 13: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0015;
        case 14: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0016;
        case 15: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0017;
        case 16: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0018;
        case 17: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0019;
        case 18: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0021;
        case 19: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0022;
        case 20: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0024;
        case 21: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0025;
        case 22: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0026;
        case 23: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0028;
        case 24: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0029;
        case 25: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0030;
        case 26: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0031;
        case 27: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0032;
        case 28: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0033;
        case 29: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0035;
        case 30: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0036;
        case 31: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0037;
        case 32: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0038;
        case 33: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0039;
        case 34: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0041;
        case 35: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0042;
        case 36: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0044;
        case 37: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0045;
        case 38: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0046;
        case 39: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0047;
        case 40: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0048;
        case 41: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0049;
        case 42: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0050;
        case 43: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0051;
        case 44: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0052;
        case 45: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0053;
        case 46: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0054;
        case 47: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0055;
        case 48: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0056;
        case 49: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0057;
        case 50: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0058;
        case 51: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0059;
        case 52: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0060;
        case 53: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0061;
        case 54: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0062;
        case 55: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0063;
        case 56: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0064;
        case 57: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0065;
        case 58: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0066;
        case 59: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0067;
        case 60: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0068;
        case 61: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0069;
        case 62: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0070;
        case 63: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0071;
        case 64: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0072;
        case 65: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0073;
        case 66: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0074;
        case 67: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0075;
        case 68: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0076;
        case 69: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0077;
        case 70: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0078;
        case 71: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0079;
        case 72: length = 600; return AKWF_BwBlended::akwf_akwf_bwblended_akwf_blended_0080;
        default: length = 0; return nullptr;
    }
}
