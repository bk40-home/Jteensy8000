/*
 * TUS_Presets.h  –  The Usual Suspects (JE-8086) JP-8000 performance bank
 * Automatically generated from The_Usual_Suspects_-_JE-8086_Demo_Performances.syx
 *
 * 32 performances decoded into 34 unique patches.
 * All values are pre-converted to JT-8000 CC units.
 * Stored in PROGMEM (Teensy flash) – ~1088 bytes.
 *
 * Parameter encoding:
 *   osc1Wave / osc2Wave : CC waveform value (from ccFromWaveform())
 *   oscXCoarse          : CC pitch offset (64 = 0 semitones)
 *   ampATK/DCY/REL      : 0=fastest, 127=slowest (Roland inverted)
 *   ampSUS              : 0-127 level (not inverted)
 *   filterCutoff        : 0-127 (direct from JP-8000)
 */

#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// TUSPatch – one pre-decoded JP-8000 patch in JT-8000 CC units
// ---------------------------------------------------------------------------
struct TUSPatch {
    const char* name;
    uint8_t     osc1Wave;      // CC waveform for OSC1
    uint8_t     osc2Wave;      // CC waveform for OSC2
    uint8_t     osc1Coarse;    // CC::OSC1_PITCH_OFFSET (64=0)
    uint8_t     osc2Coarse;    // CC::OSC2_PITCH_OFFSET (64=0)
    uint8_t     ssDetune;      // SuperSaw detune (0=min, 127=max)
    uint8_t     osc1Mix;       // CC::OSC1_MIX
    uint8_t     osc2Mix;       // CC::OSC2_MIX
    uint8_t     noiseMix;      // CC::NOISE_MIX
    uint8_t     subMix;        // CC::SUB_MIX (from sub osc level)
    uint8_t     filterCutoff;  // CC::FILTER_CUTOFF
    uint8_t     filterRes;     // CC::FILTER_RESONANCE
    uint8_t     filterEnvAmt;  // CC::FILTER_ENV_AMOUNT
    uint8_t     ampAttack;     // CC::AMP_ATTACK
    uint8_t     ampDecay;      // CC::AMP_DECAY
    uint8_t     ampSustain;    // CC::AMP_SUSTAIN
    uint8_t     ampRelease;    // CC::AMP_RELEASE
    uint8_t     ampLevel;      // CC::AMP_MOD_FIXED_LEVEL
    uint8_t     lfo1Rate;      // CC::LFO1_FREQ (raw 0-127)
    uint8_t     lfo2Rate;      // CC::LFO2_FREQ (raw 0-127)
    uint8_t     fxType;        // JP-8000 FX type (0=none,1=cho,2=cho2,3=fla,4=dly,5=ldly)
    uint8_t     fxParam1;      // FX rate/depth
    uint8_t     fxParam2;      // FX feedback/mix
    uint8_t     glideOn;       // 0 or 127
    uint8_t     glideTime;     // CC::GLIDE_TIME
};

// JP-8000 → JT-8000 waveform CC values (approximate)
// These must match ccFromWaveform() in WaveForms.cpp.
// We use raw numeric values here to allow PROGMEM storage without
// calling functions at initialisation time.
//
// SAW=0, SQR=1, PW=2(→SQR), TRI=3, SINE=4, NOISE=5, SSAW=6
// JT-8000 CC values (from ccFromWaveform): SAW≈10, SQR≈30, TRI≈50, SINE≈70, NOISE≈90, SSAW≈110
//
// For accurate waveform CC values, load via JP8000SyxLoader::load()
// which calls ccFromWaveform() at runtime.

// 34 unique patches from "The Usual Suspects" JP-8000 performance bank
static constexpr int kTUS_COUNT = 34;

// All string data and patch data in PROGMEM (Teensy 4.1 flash)
PROGMEM static const TUSPatch kTUS_Patches[kTUS_COUNT] = {
    { "AdagioBass", 110, 10,  80, 78,   0,
      100,  80,   0,  15,
       63, 33,  0,
       63,127,  0,  0,  66,
        5, 38,
        0, 63,109,
      127,  0 },  // OSC1=SSAW OSC2=SAW Cut=63 Glide=1
    { "AdagioLead",  10, 10,  80, 99,   0,
      100,  80,   0,  15,
       50, 64, 87,
        0,127,  0,  0, 127,
       16,  0,
        0, 64, 64,
      127,  0 },  // OSC1=SAW OSC2=SAW Cut=50 Glide=1
    { "AdagioPad", 110, 10,  80, 86,   0,
      100,  80,   0,   0,
       50,  0,  0,
       63,127,  0,  0,  74,
       58,  0,
        3, 64, 78,
      127,  0 },  // OSC1=SSAW OSC2=SAW Cut=50 Glide=1
    { "AdagioString",  10, 10,  80, 76,   0,
      100,  80,   0,   0,
       50,  0,  0,
       64, 42,118,  0,  91,
       73,  0,
        3, 66, 69,
      127,  0 },  // OSC1=SAW OSC2=SAW Cut=50 Glide=1
    { "AdagioSupersaw", 110, 10,  80,102,   0,
      100,  80,   0,   0,
       50,  0,  0,
       63,127, 25,  0,  73,
        2, 52,
        0, 64, 64,
      127,  0 },  // OSC1=SSAW OSC2=SAW Cut=50 Glide=1
    { "AirwavePadHigh", 110, 10,  80, 97,   0,
      100,  80,   0,   7,
       50, 67,116,
       63,127,  0,  0, 105,
       43,  0,
        2, 64, 71,
      127,  0 },  // OSC1=SSAW OSC2=SAW Cut=50 Glide=1
    { "Airwave", 110, 10,  80, 87,   0,
      100,  80,   0,   0,
       50,  0,  0,
       63,127,  0,  0,  83,
       46,  0,
        2, 64, 64,
      127,  0 },  // OSC1=SSAW OSC2=SAW Cut=50 Glide=1
    { "AirwavePadLow", 110, 10,  80, 88,   0,
      100,  80,   0,   6,
       50, 72,100,
        0,127,  0,  0,  78,
       44,  0,
        0, 64, 64,
      127,  0 },  // OSC1=SSAW OSC2=SAW Cut=50 Glide=1
    { "CarteBlancheWide", 110, 10,  80,112,   0,
      100,  80,   0,   9,
       60, 64,109,
       39,127, 45,  0, 127,
        0, 41,
        0, 81, 73,
      127,  0 },  // OSC1=SSAW OSC2=SAW Cut=60 Glide=1
    { "CarteBlancheThin", 110, 10,  76, 93,  15,
      100,  80,   0,   0,
       53, 57,127,
       37,127, 44, 45, 127,
        0, 38,
        2, 49,103,
      127,  0 },  // OSC1=SSAW OSC2=SAW Cut=53 Glide=1
    { "CatchBass",  10, 90,  80, 16,   0,
      100,   0,   0,   0,
       50,  0,  0,
       14,127, 29, 38,  47,
        0, 35,
        3, 95, 64,
      127,  0 },  // OSC1=SAW OSC2=NOISE Cut=50 Glide=1
    { "CatchLead",  10, 10,  80,105,   0,
      100,  80,   0,   0,
       66, 59, 97,
       39,127, 25, 38,  87,
        6, 51,
        2, 87, 85,
      127,  0 },  // OSC1=SAW OSC2=SAW Cut=66 Glide=1
    { "EIGBass",  10, 10,  84, 91,   0,
      100,  80,   0,   0,
       50, 94,  0,
       42,127, 31,  0, 103,
        0, 51,
        0,113, 71,
      127,  0 },  // OSC1=SAW OSC2=SAW Cut=50 Glide=1
    { "EIGDist",  10, 70,  80, 80,   0,
      100,  80,   0,   9,
       64,100,  0,
       33,127, 38,  0,  63,
        0, 42,
        3,114, 65,
      127,  0 },  // OSC1=SAW OSC2=SINE Cut=64 Glide=1
    { "EIGLead", 110, 10,  80, 97,   0,
      100,  80,   0,   0,
       50,  0,  0,
       37,127, 44,  0,  58,
        6, 60,
        3, 69, 80,
      127,  0 },  // OSC1=SSAW OSC2=SAW Cut=50 Glide=1
    { "EIGNoise", 110, 30,  80,109,   0,
      100,  80,   0,   0,
       50,  0,  0,
        0,127,  0,  0, 127,
        0, 69,
        0, 64, 64,
      127,  0 },  // OSC1=SSAW OSC2=PW Cut=50 Glide=1
    { "LigayaLead", 110, 10,  80,108,   0,
      100,  80,   0,   0,
       50,  0,  0,
       30,127, 29,  0,  79,
        0, 35,
        0, 64, 64,
      127,  0 },  // OSC1=SSAW OSC2=SAW Cut=50 Glide=1
    { "LigayaPad",  10, 10,  80, 82,   0,
      100,  80,   0,   0,
       50,  0,  0,
       64, 42,118,  0,  83,
       35,  0,
        3, 95, 93,
      127,  0 },  // OSC1=SAW OSC2=SAW Cut=50 Glide=1
    { "OutOfTheBlue",  10, 10,  80,107,   0,
      100,  80,   0,   0,
       50,  0,  0,
       19,127, 51,  0,  46,
        5, 35,
        2, 97,106,
      127,  0 },  // OSC1=SAW OSC2=SAW Cut=50 Glide=1
    { "SolsticeBass", 110, 90,  80, 16,   0,
      100,   0,   0,   0,
       56, 69,115,
       24,127, 34,  0,  43,
        0, 40,
        0, 64, 64,
      127,  0 },  // OSC1=SSAW OSC2=NOISE Cut=56 Glide=1
    { "SolsticeFlanger", 110, 50,  80, 56,   0,
      100,  80,   0,   0,
       56,  0,  0,
       63,127, 43,  0, 100,
        2, 30,
        3, 47, 73,
      127,  0 },  // OSC1=SSAW OSC2=TRI Cut=56 Glide=1
    { "SolsticeLeadH", 110, 10,  80,111,   0,
      100,  80,   0,   0,
       50,  0,  0,
       51,125, 35,  0, 106,
        2, 36,
        3, 64, 89,
      127,  0 },  // OSC1=SSAW OSC2=SAW Cut=50 Glide=1
    { "SolsticeLeadLow", 110, 10,  80,111,   0,
      100,  80,   0,   0,
       50,  0,  0,
       59,125, 35,  0, 101,
        2, 36,
        3, 63,100,
      127,  0 },  // OSC1=SSAW OSC2=SAW Cut=50 Glide=1
    { "ZCello",  10, 10,  80, 81,   0,
      100,  80,   0,   0,
       50,  0,  0,
       37,105, 34,  0,  68,
       24,  0,
        3, 95, 64,
      127,  0 },  // OSC1=SAW OSC2=SAW Cut=50 Glide=1
    { "ZDoubleBass",  10, 10,  80, 83,   0,
      100,  80,   0,   0,
       50,  0,  0,
       37,105, 34,  0,  62,
       24,  0,
        3, 95, 64,
      127,  0 },  // OSC1=SAW OSC2=SAW Cut=50 Glide=1
    { "ZFrenchHorn",  10, 10,  53, 68,   0,
      100,  80,   0,   1,
       50, 45, 86,
       64, 42,118,  0,  69,
       30,  0,
        0, 95, 64,
      127,  0 },  // OSC1=SAW OSC2=SAW Cut=50 Glide=1
    { "ZHorn",  10, 10,  93, 66,   0,
      100,  80,   0,   0,
       50,  0,  0,
       41,100, 27, 38,  28,
       27,  0,
        0, 75, 64,
      127,  0 },  // OSC1=SAW OSC2=SAW Cut=50 Glide=1
    { "ZPiccolo",  10,110,  80, 40,   0,
      100,  80,   0,  11,
       50,  0,  0,
       44,102,  0,  0,  38,
       24,  0,
        0, 64, 64,
      127,  0 },  // OSC1=SAW OSC2=SSAW Cut=50 Glide=1
    { "ZTimpani",  10, 30,  80, 16,   0,
      100,  80,   0,  16,
       37, 44, 72,
       17,127, 76,  0,  23,
        0, 58,
        3,127,117,
      127,  0 },  // OSC1=SAW OSC2=SQR Cut=37 Glide=1
    { "ZTrombone",  10, 70,  60, 73,   0,
      100,  80,   0,  15,
       44, 42,112,
       27, 95, 49,  0,  39,
       21, 26,
        0, 66, 52,
      127,  0 },  // OSC1=SAW OSC2=SINE Cut=44 Glide=1
    { "ZTrumpet",  10, 10,  61, 57,   0,
      100,  80,   0,  10,
       44, 60, 53,
       26,103, 49,  0,  45,
       21, 26,
        3, 66, 55,
      127,  0 },  // OSC1=SAW OSC2=SAW Cut=44 Glide=1
    { "ZTuba",  10, 10,  39, 86,   0,
      100,  80,   0,  13,
       57,  0,  0,
       26,103, 49,  0,  21,
       43, 26,
        3, 66,  0,
      127,  0 },  // OSC1=SAW OSC2=SAW Cut=57 Glide=1
    { "ZViolin",  10, 10,  80, 68,   0,
      100,  80,   0,   0,
       50,  0,  0,
       29,105, 34,  0,  55,
       24,  0,
        3, 95, 64,
      127,  0 },  // OSC1=SAW OSC2=SAW Cut=50 Glide=1
    { "ZViolin2",  10, 10,  80, 89,   0,
      100,  80,   0,   6,
       53, 64, 73,
       29,105, 34,  0,  53,
       24,  0,
        3, 83, 56,
      127,  0 },  // OSC1=SAW OSC2=SAW Cut=53 Glide=1
};

// Inline accessor for patch name (PROGMEM-safe on AVR; direct on Teensy ARM)
inline const char* tus_patchName(int idx) {
    if (idx < 0 || idx >= kTUS_COUNT) return "---";
    return kTUS_Patches[idx].name;
}
