// Auto‑generated AKWF bank. Do not edit manually.

#pragma once
#include <Arduino.h>

#include "AKWF_BwSawrounded_Tables_Part0.h"
#include "AKWF_BwSawrounded_Tables_Part1.h"
#include "AKWF_BwSawrounded_Tables_Part2.h"

namespace AKWF_BwSawrounded {
    constexpr uint16_t count = 52;
}

// Return a pointer to the n‑th waveform table in this bank.
// If index is out of range, returns nullptr and length is set to 0.
inline const int16_t* AKWF_BwSawrounded_get(uint16_t index, uint16_t &length) {
    switch(index) {
        case 0: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_asym_saw_01;
        case 1: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_asym_saw_02;
        case 2: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_asym_saw_03;
        case 3: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_asym_saw_04;
        case 4: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_asym_saw_05;
        case 5: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_asym_saw_06;
        case 6: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_asym_saw_07;
        case 7: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_asym_saw_08;
        case 8: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_asym_saw_09;
        case 9: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_asym_saw_10;
        case 10: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_asym_saw_11;
        case 11: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_asym_saw_12;
        case 12: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_asym_saw_13;
        case 13: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_asym_saw_14;
        case 14: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_asym_saw_15;
        case 15: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_asym_saw_16;
        case 16: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_asym_saw_17;
        case 17: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_asym_saw_18;
        case 18: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_asym_saw_19;
        case 19: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_asym_saw_20;
        case 20: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_asym_saw_21;
        case 21: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_asym_saw_22;
        case 22: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_asym_saw_23;
        case 23: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_asym_saw_24;
        case 24: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_asym_saw_25;
        case 25: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_asym_saw_26;
        case 26: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_sym_saw_01;
        case 27: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_sym_saw_02;
        case 28: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_sym_saw_03;
        case 29: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_sym_saw_04;
        case 30: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_sym_saw_05;
        case 31: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_sym_saw_06;
        case 32: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_sym_saw_07;
        case 33: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_sym_saw_08;
        case 34: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_sym_saw_09;
        case 35: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_sym_saw_10;
        case 36: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_sym_saw_11;
        case 37: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_sym_saw_12;
        case 38: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_sym_saw_13;
        case 39: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_sym_saw_14;
        case 40: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_sym_saw_15;
        case 41: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_sym_saw_16;
        case 42: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_sym_saw_17;
        case 43: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_sym_saw_18;
        case 44: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_sym_saw_19;
        case 45: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_sym_saw_20;
        case 46: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_sym_saw_21;
        case 47: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_sym_saw_22;
        case 48: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_sym_saw_23;
        case 49: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_sym_saw_24;
        case 50: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_sym_saw_25;
        case 51: length = 600; return AKWF_BwSawrounded::akwf_akwf_bwsawrounded_akwf_r_sym_saw_26;
        default: length = 0; return nullptr;
    }
}
