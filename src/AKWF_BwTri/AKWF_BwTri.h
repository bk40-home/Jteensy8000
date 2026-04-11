// Auto‑generated AKWF bank. Do not edit manually.

#pragma once
#include <Arduino.h>

#include "AKWF_BwTri_Tables_Part0.h"
#include "AKWF_BwTri_Tables_Part1.h"

namespace AKWF_BwTri {
    constexpr uint16_t count = 25;
}

// Return a pointer to the n‑th waveform table in this bank.
// If index is out of range, returns nullptr and length is set to 0.
inline const int16_t* AKWF_BwTri_get(uint16_t index, uint16_t &length) {
    switch(index) {
        case 0: length = 600; return AKWF_BwTri::akwf_akwf_bwtri_akwf_tri_0001;
        case 1: length = 600; return AKWF_BwTri::akwf_akwf_bwtri_akwf_tri_0002;
        case 2: length = 600; return AKWF_BwTri::akwf_akwf_bwtri_akwf_tri_0003;
        case 3: length = 600; return AKWF_BwTri::akwf_akwf_bwtri_akwf_tri_0004;
        case 4: length = 600; return AKWF_BwTri::akwf_akwf_bwtri_akwf_tri_0005;
        case 5: length = 600; return AKWF_BwTri::akwf_akwf_bwtri_akwf_tri_0006;
        case 6: length = 600; return AKWF_BwTri::akwf_akwf_bwtri_akwf_tri_0007;
        case 7: length = 600; return AKWF_BwTri::akwf_akwf_bwtri_akwf_tri_0008;
        case 8: length = 600; return AKWF_BwTri::akwf_akwf_bwtri_akwf_tri_0009;
        case 9: length = 600; return AKWF_BwTri::akwf_akwf_bwtri_akwf_tri_0010;
        case 10: length = 600; return AKWF_BwTri::akwf_akwf_bwtri_akwf_tri_0011;
        case 11: length = 600; return AKWF_BwTri::akwf_akwf_bwtri_akwf_tri_0012;
        case 12: length = 600; return AKWF_BwTri::akwf_akwf_bwtri_akwf_tri_0013;
        case 13: length = 600; return AKWF_BwTri::akwf_akwf_bwtri_akwf_tri_0014;
        case 14: length = 600; return AKWF_BwTri::akwf_akwf_bwtri_akwf_tri_0015;
        case 15: length = 600; return AKWF_BwTri::akwf_akwf_bwtri_akwf_tri_0016;
        case 16: length = 600; return AKWF_BwTri::akwf_akwf_bwtri_akwf_tri_0017;
        case 17: length = 600; return AKWF_BwTri::akwf_akwf_bwtri_akwf_tri_0018;
        case 18: length = 600; return AKWF_BwTri::akwf_akwf_bwtri_akwf_tri_0019;
        case 19: length = 600; return AKWF_BwTri::akwf_akwf_bwtri_akwf_tri_0020;
        case 20: length = 600; return AKWF_BwTri::akwf_akwf_bwtri_akwf_tri_0021;
        case 21: length = 600; return AKWF_BwTri::akwf_akwf_bwtri_akwf_tri_0022;
        case 22: length = 600; return AKWF_BwTri::akwf_akwf_bwtri_akwf_tri_0023;
        case 23: length = 600; return AKWF_BwTri::akwf_akwf_bwtri_akwf_tri_0024;
        case 24: length = 600; return AKWF_BwTri::akwf_akwf_bwtri_akwf_tri_0025;
        default: length = 0; return nullptr;
    }
}
