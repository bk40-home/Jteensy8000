// Auto‑generated AKWF bank catalog. Do not edit manually.

#pragma once
#include <Arduino.h>

#include "AKWF_BwBlended/AKWF_BwBlended.h"
#include "AKWF_BwPerfectwaves/AKWF_BwPerfectwaves.h"
#include "AKWF_BwSaw/AKWF_BwSaw.h"
#include "AKWF_BwSawbright/AKWF_BwSawbright.h"
#include "AKWF_BwSawgap/AKWF_BwSawgap.h"
#include "AKWF_BwSawrounded/AKWF_BwSawrounded.h"
#include "AKWF_BwSin/AKWF_BwSin.h"
#include "AKWF_BwSqu/AKWF_BwSqu.h"
#include "AKWF_BwSqurounded/AKWF_BwSqurounded.h"
#include "AKWF_BwTri/AKWF_BwTri.h"

enum class ArbBank : uint8_t {
    BwBlended,
    BwPerfectwaves,
    BwSaw,
    BwSawbright,
    BwSawgap,
    BwSawrounded,
    BwSin,
    BwSqu,
    BwSqurounded,
    BwTri
};

inline const char* akwf_bankName(ArbBank b) {
    switch(b) {
        case ArbBank::BwBlended: return "BwBlended";
        case ArbBank::BwPerfectwaves: return "BwPerfectwaves";
        case ArbBank::BwSaw: return "BwSaw";
        case ArbBank::BwSawbright: return "BwSawbright";
        case ArbBank::BwSawgap: return "BwSawgap";
        case ArbBank::BwSawrounded: return "BwSawrounded";
        case ArbBank::BwSin: return "BwSin";
        case ArbBank::BwSqu: return "BwSqu";
        case ArbBank::BwSqurounded: return "BwSqurounded";
        case ArbBank::BwTri: return "BwTri";
    }
    return "?";
}

inline uint16_t akwf_bankCount(ArbBank b) {
    switch(b) {
        case ArbBank::BwBlended: return AKWF_BwBlended::count;
        case ArbBank::BwPerfectwaves: return AKWF_BwPerfectwaves::count;
        case ArbBank::BwSaw: return AKWF_BwSaw::count;
        case ArbBank::BwSawbright: return AKWF_BwSawbright::count;
        case ArbBank::BwSawgap: return AKWF_BwSawgap::count;
        case ArbBank::BwSawrounded: return AKWF_BwSawrounded::count;
        case ArbBank::BwSin: return AKWF_BwSin::count;
        case ArbBank::BwSqu: return AKWF_BwSqu::count;
        case ArbBank::BwSqurounded: return AKWF_BwSqurounded::count;
        case ArbBank::BwTri: return AKWF_BwTri::count;
    }
    return 0;
}

inline const int16_t* akwf_get(ArbBank b, uint16_t idx, uint16_t &len) {
    switch(b) {
        case ArbBank::BwBlended: return AKWF_BwBlended_get(idx, len);
        case ArbBank::BwPerfectwaves: return AKWF_BwPerfectwaves_get(idx, len);
        case ArbBank::BwSaw: return AKWF_BwSaw_get(idx, len);
        case ArbBank::BwSawbright: return AKWF_BwSawbright_get(idx, len);
        case ArbBank::BwSawgap: return AKWF_BwSawgap_get(idx, len);
        case ArbBank::BwSawrounded: return AKWF_BwSawrounded_get(idx, len);
        case ArbBank::BwSin: return AKWF_BwSin_get(idx, len);
        case ArbBank::BwSqu: return AKWF_BwSqu_get(idx, len);
        case ArbBank::BwSqurounded: return AKWF_BwSqurounded_get(idx, len);
        case ArbBank::BwTri: return AKWF_BwTri_get(idx, len);
    }
    len = 0;
    return nullptr;
}
