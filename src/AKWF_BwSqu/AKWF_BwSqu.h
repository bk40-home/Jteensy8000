// Auto‑generated AKWF bank. Do not edit manually.

#pragma once
#include <Arduino.h>

#include "AKWF_BwSqu_Tables_Part0.h"
#include "AKWF_BwSqu_Tables_Part1.h"
#include "AKWF_BwSqu_Tables_Part2.h"
#include "AKWF_BwSqu_Tables_Part3.h"
#include "AKWF_BwSqu_Tables_Part4.h"

namespace AKWF_BwSqu {
    constexpr uint16_t count = 100;
}

// Return a pointer to the n‑th waveform table in this bank.
// If index is out of range, returns nullptr and length is set to 0.
inline const int16_t* AKWF_BwSqu_get(uint16_t index, uint16_t &length) {
    switch(index) {
        case 0: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0001;
        case 1: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0002;
        case 2: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0003;
        case 3: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0004;
        case 4: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0005;
        case 5: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0006;
        case 6: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0007;
        case 7: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0008;
        case 8: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0009;
        case 9: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0010;
        case 10: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0011;
        case 11: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0012;
        case 12: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0013;
        case 13: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0014;
        case 14: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0015;
        case 15: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0016;
        case 16: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0017;
        case 17: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0018;
        case 18: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0019;
        case 19: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0020;
        case 20: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0021;
        case 21: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0022;
        case 22: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0023;
        case 23: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0024;
        case 24: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0025;
        case 25: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0026;
        case 26: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0027;
        case 27: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0028;
        case 28: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0029;
        case 29: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0030;
        case 30: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0031;
        case 31: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0032;
        case 32: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0033;
        case 33: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0034;
        case 34: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0035;
        case 35: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0036;
        case 36: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0037;
        case 37: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0038;
        case 38: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0039;
        case 39: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0040;
        case 40: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0041;
        case 41: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0042;
        case 42: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0043;
        case 43: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0044;
        case 44: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0045;
        case 45: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0046;
        case 46: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0047;
        case 47: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0048;
        case 48: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0049;
        case 49: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0050;
        case 50: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0051;
        case 51: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0052;
        case 52: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0053;
        case 53: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0054;
        case 54: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0055;
        case 55: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0056;
        case 56: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0057;
        case 57: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0058;
        case 58: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0059;
        case 59: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0060;
        case 60: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0061;
        case 61: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0062;
        case 62: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0063;
        case 63: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0064;
        case 64: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0065;
        case 65: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0066;
        case 66: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0067;
        case 67: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0068;
        case 68: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0069;
        case 69: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0070;
        case 70: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0071;
        case 71: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0072;
        case 72: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0073;
        case 73: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0074;
        case 74: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0075;
        case 75: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0076;
        case 76: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0077;
        case 77: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0078;
        case 78: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0079;
        case 79: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0080;
        case 80: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0081;
        case 81: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0082;
        case 82: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0083;
        case 83: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0084;
        case 84: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0085;
        case 85: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0086;
        case 86: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0087;
        case 87: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0088;
        case 88: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0089;
        case 89: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0090;
        case 90: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0091;
        case 91: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0092;
        case 92: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0093;
        case 93: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0094;
        case 94: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0095;
        case 95: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0096;
        case 96: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0097;
        case 97: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0098;
        case 98: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0099;
        case 99: length = 600; return AKWF_BwSqu::akwf_akwf_bwsqu_akwf_squ_0100;
        default: length = 0; return nullptr;
    }
}
