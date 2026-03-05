// Auto‑generated AKWF bank. Do not edit manually.

#pragma once
#include <Arduino.h>

#include "AKWF_BwSin_Tables_Part0.h"

namespace AKWF_BwSin {
    constexpr uint16_t count = 12;
}

// Return a pointer to the n‑th waveform table in this bank.
// If index is out of range, returns nullptr and length is set to 0.
inline const int16_t* AKWF_BwSin_get(uint16_t index, uint16_t &length) {
    switch(index) {
        case 0: length = 600; return AKWF_BwSin::akwf_akwf_bwsin_akwf_sin_0001;
        case 1: length = 600; return AKWF_BwSin::akwf_akwf_bwsin_akwf_sin_0002;
        case 2: length = 600; return AKWF_BwSin::akwf_akwf_bwsin_akwf_sin_0003;
        case 3: length = 600; return AKWF_BwSin::akwf_akwf_bwsin_akwf_sin_0004;
        case 4: length = 600; return AKWF_BwSin::akwf_akwf_bwsin_akwf_sin_0005;
        case 5: length = 600; return AKWF_BwSin::akwf_akwf_bwsin_akwf_sin_0006;
        case 6: length = 600; return AKWF_BwSin::akwf_akwf_bwsin_akwf_sin_0007;
        case 7: length = 600; return AKWF_BwSin::akwf_akwf_bwsin_akwf_sin_0008;
        case 8: length = 600; return AKWF_BwSin::akwf_akwf_bwsin_akwf_sin_0009;
        case 9: length = 600; return AKWF_BwSin::akwf_akwf_bwsin_akwf_sin_0010;
        case 10: length = 600; return AKWF_BwSin::akwf_akwf_bwsin_akwf_sin_0011;
        case 11: length = 600; return AKWF_BwSin::akwf_akwf_bwsin_akwf_sin_0012;
        default: length = 0; return nullptr;
    }
}
