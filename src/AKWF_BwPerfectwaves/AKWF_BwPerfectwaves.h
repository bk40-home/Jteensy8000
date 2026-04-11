// Auto‑generated AKWF bank. Do not edit manually.

#pragma once
#include <Arduino.h>

#include "AKWF_BwPerfectwaves_Tables_Part0.h"

namespace AKWF_BwPerfectwaves {
    constexpr uint16_t count = 4;
}

// Return a pointer to the n‑th waveform table in this bank.
// If index is out of range, returns nullptr and length is set to 0.
inline const int16_t* AKWF_BwPerfectwaves_get(uint16_t index, uint16_t &length) {
    switch(index) {
        case 0: length = 600; return AKWF_BwPerfectwaves::akwf_akwf_bwperfectwaves_akwf_saw;
        case 1: length = 600; return AKWF_BwPerfectwaves::akwf_akwf_bwperfectwaves_akwf_sin;
        case 2: length = 600; return AKWF_BwPerfectwaves::akwf_akwf_bwperfectwaves_akwf_squ;
        case 3: length = 600; return AKWF_BwPerfectwaves::akwf_akwf_bwperfectwaves_akwf_tri;
        default: length = 0; return nullptr;
    }
}
