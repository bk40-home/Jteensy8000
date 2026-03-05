// Auto‑generated AKWF bank. Do not edit manually.

#pragma once
#include <Arduino.h>

#include "AKWF_BwSqurounded_Tables_Part0.h"
#include "AKWF_BwSqurounded_Tables_Part1.h"
#include "AKWF_BwSqurounded_Tables_Part2.h"

namespace AKWF_BwSqurounded {
    constexpr uint16_t count = 52;
}

// Return a pointer to the n‑th waveform table in this bank.
// If index is out of range, returns nullptr and length is set to 0.
inline const int16_t* AKWF_BwSqurounded_get(uint16_t index, uint16_t &length) {
    switch(index) {
        case 0: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rasymsqu_01;
        case 1: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rasymsqu_02;
        case 2: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rasymsqu_03;
        case 3: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rasymsqu_04;
        case 4: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rasymsqu_05;
        case 5: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rasymsqu_06;
        case 6: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rasymsqu_07;
        case 7: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rasymsqu_08;
        case 8: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rasymsqu_09;
        case 9: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rasymsqu_10;
        case 10: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rasymsqu_11;
        case 11: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rasymsqu_12;
        case 12: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rasymsqu_13;
        case 13: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rasymsqu_14;
        case 14: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rasymsqu_15;
        case 15: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rasymsqu_16;
        case 16: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rasymsqu_17;
        case 17: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rasymsqu_18;
        case 18: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rasymsqu_19;
        case 19: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rasymsqu_20;
        case 20: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rasymsqu_21;
        case 21: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rasymsqu_22;
        case 22: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rasymsqu_23;
        case 23: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rasymsqu_24;
        case 24: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rasymsqu_25;
        case 25: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rasymsqu_26;
        case 26: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rsymsqu_01;
        case 27: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rsymsqu_02;
        case 28: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rsymsqu_03;
        case 29: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rsymsqu_04;
        case 30: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rsymsqu_05;
        case 31: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rsymsqu_06;
        case 32: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rsymsqu_07;
        case 33: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rsymsqu_08;
        case 34: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rsymsqu_09;
        case 35: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rsymsqu_10;
        case 36: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rsymsqu_11;
        case 37: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rsymsqu_12;
        case 38: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rsymsqu_13;
        case 39: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rsymsqu_14;
        case 40: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rsymsqu_15;
        case 41: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rsymsqu_16;
        case 42: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rsymsqu_17;
        case 43: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rsymsqu_18;
        case 44: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rsymsqu_19;
        case 45: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rsymsqu_20;
        case 46: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rsymsqu_21;
        case 47: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rsymsqu_22;
        case 48: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rsymsqu_23;
        case 49: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rsymsqu_24;
        case 50: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rsymsqu_25;
        case 51: length = 600; return AKWF_BwSqurounded::akwf_akwf_bwsqurounded_akwf_rsymsqu_26;
        default: length = 0; return nullptr;
    }
}
