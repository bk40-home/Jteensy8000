/*
 * FactoryBank.h — JT-8000 Factory Preset Bank
 *
 * Copyright (c) 2025 Kris Bishop
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * ---------------------------------------------------------------------------
 * 14 init patches + 34 factory sounds = 48 total
 *
 * Each patch is a flat array of CC values in PatchSchema::kPatchableCCs order.
 * The loader sends EVERY CC, guaranteeing no stale state from previous patches.
 *
 * IMPORTANT: cc[] has 101 elements — the 5 reverb CCs (FX_REVERB_SIZE/DAMP/
 * LODAMP/MIX/BYPASS) were removed from PatchSchema when reverb moved to the
 * shared GlobalFX bus (Phase 3). Reverb state is now serialised per-Performance,
 * not per-patch. The struct and column comment below reflect the 90-column schema.
 *
 * Init patch convention (indices 0..kInitCount-1):
 *   - OSC1/OSC2 WAVE: both set to the target waveform.
 *   - OSC1_MIX=127, OSC2_MIX=0 — single oscillator output.
 *   - FILTER_CUTOFF=127 (wide open), FILTER_ENGINE=32 (OBXa bucket centre).
 *   - All bipolar CCs at 64 (exact zero): PITCH_OFFSET, FINE_TUNE, DETUNE,
 *     FILTER_ENV_AMOUNT, FILTER_KEY_TRACK, PITCH_ENV_DEPTH, BASS/TREBLE GAIN.
 *   - Amp/filter envelopes: A=0, D=0, S=127, R=0 (instant full sustain).
 *   - Pitch envelope fully off: all ADSR=0, DEPTH=64 (bipolar zero).
 *   - FX_DRY_MIX=0, FX_JPFX_MIX=127 — signal routed fully through JPFX chain.
 *   - POLY_MODE=21 (bucket-centre for POLY zone 0..42).
 *   - UNISON_DETUNE=64 (bipolar zero).
 *   - AMP_MOD_FIXED_LEVEL=127 (full fixed amplitude).
 *   - All modulation (LFO, velocity, ring, feedback) = 0.
 */

#pragma once
#include <Arduino.h>
#include "PatchSchema.h"

class SynthEngine;  // forward declaration for loadFactoryPatch()

namespace FactoryBank {

static constexpr int kFactoryCount = 48;
static constexpr int kInitCount    = 14;

// ---------------------------------------------------------------------------
// Column index reference (PatchSchema::kPatchableCCs order, 90 columns):
// ---------------------------------------------------------------------------
// Bipolar CCs (64 = exact zero): [2][3] PITCH_OFFSET, [4][5] FINE_TUNE,
//   [6][7] DETUNE, [23] FILTER_ENV_AMOUNT, [24] FILTER_KEY_TRACK,
//   [62] PITCH_ENV_DEPTH, [66] FX_BASS_GAIN, [67] FX_TREBLE_GAIN,
//   [83] UNISON_DETUNE.
// Toggle CCs (0 or 127 only): [79] GLIDE_ENABLE.
// Enum CCs (bucket-centre): [26] FILTER_ENGINE (OBXa=32, VA=96),
//   [82] POLY_MODE (POLY=21, MONO=63, UNISON=106).
// ---------------------------------------------------------------------------
//   0  OSC1_WAVE
//   1  OSC2_WAVE
//   2  OSC1_PITCH_OFFSET
//   3  OSC2_PITCH_OFFSET
//   4  OSC1_FINE_TUNE
//   5  OSC2_FINE_TUNE
//   6  OSC1_DETUNE
//   7  OSC2_DETUNE
//   8  OSC_MIX_BALANCE
//   9  OSC1_MIX
//  10  OSC2_MIX
//  11  SUB_MIX
//  12  NOISE_MIX
//  13  SUPERSAW1_DETUNE
//  14  SUPERSAW1_MIX
//  15  SUPERSAW2_DETUNE
//  16  SUPERSAW2_MIX
//  17  OSC1_FEEDBACK_AMOUNT
//  18  OSC2_FEEDBACK_AMOUNT
//  19  OSC1_FEEDBACK_MIX
//  20  OSC2_FEEDBACK_MIX
//  21  FILTER_CUTOFF
//  22  FILTER_RESONANCE
//  23  FILTER_ENV_AMOUNT
//  24  FILTER_KEY_TRACK
//  25  FILTER_OCTAVE_CONTROL
//  26  FILTER_ENGINE
//  27  FILTER_MODE
//  28  VA_FILTER_TYPE
//  29  FILTER_OBXA_XPANDER_MODE
//  30  FILTER_OBXA_MULTIMODE
//  31  FILTER_OBXA_RES_MOD_DEPTH
//  32  AMP_ATTACK
//  33  AMP_DECAY
//  34  AMP_SUSTAIN
//  35  AMP_RELEASE
//  36  FILTER_ENV_ATTACK
//  37  FILTER_ENV_DECAY
//  38  FILTER_ENV_SUSTAIN
//  39  FILTER_ENV_RELEASE
//  40  LFO1_FREQ
//  41  LFO1_DEPTH
//  42  LFO1_DESTINATION
//  43  LFO1_WAVEFORM
//  44  LFO1_PITCH_DEPTH
//  45  LFO1_FILTER_DEPTH
//  46  LFO1_PWM_DEPTH
//  47  LFO1_AMP_DEPTH
//  48  LFO1_DELAY
//  49  LFO2_FREQ
//  50  LFO2_DEPTH
//  51  LFO2_DESTINATION
//  52  LFO2_WAVEFORM
//  53  LFO2_PITCH_DEPTH
//  54  LFO2_FILTER_DEPTH
//  55  LFO2_PWM_DEPTH
//  56  LFO2_AMP_DEPTH
//  57  LFO2_DELAY
//  58  PITCH_ENV_ATTACK
//  59  PITCH_ENV_DECAY
//  60  PITCH_ENV_SUSTAIN
//  61  PITCH_ENV_RELEASE
//  62  PITCH_ENV_DEPTH
//  63  VELOCITY_AMP_SENS
//  64  VELOCITY_FILTER_SENS
//  65  VELOCITY_ENV_SENS
//  66  FX_BASS_GAIN
//  67  FX_TREBLE_GAIN
//  68  FX_DRIVE
//  69  FX_MOD_EFFECT
//  70  FX_MOD_MIX
//  71  FX_MOD_RATE
//  72  FX_MOD_FEEDBACK
//  73  FX_JPFX_DELAY_EFFECT
//  74  FX_JPFX_DELAY_MIX
//  75  FX_JPFX_DELAY_FEEDBACK
//  76  FX_JPFX_DELAY_TIME
//  77  FX_DRY_MIX
//  78  FX_JPFX_MIX
//  79  GLIDE_ENABLE
//  80  GLIDE_TIME
//  81  AMP_MOD_FIXED_LEVEL
//  82  POLY_MODE
//  83  UNISON_DETUNE
//  84  RING1_MIX
//  85  RING2_MIX
//  86  OSC1_FREQ_DC
//  87  OSC1_SHAPE_DC
//  88  OSC2_FREQ_DC
//  89  OSC2_SHAPE_DC

struct FactoryPatch {
    const char* name;
    uint8_t     cc[101];   // 90 columns — reverb removed from schema (Phase 3)
};

PROGMEM static const FactoryPatch kPatches[kFactoryCount] = {

    // ---- Init Patches [0..13] — one per oscillator waveform ----

    { "Init Sine",
          4,   4,  64,  64,  64,  64,  64,  64,   0, 127,   0,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0, 127,   0,  64,  64,   0,  32,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
          0,   0, 127,   0,   0,   0, 127,   0,   0,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0,   0, 127,   0, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0, 127,  21,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "Init Sawtooth",
         13,  13,  64,  64,  64,  64,  64,  64,   0, 127,   0,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0, 127,   0,  64,  64,   0,  32,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
          0,   0, 127,   0,   0,   0, 127,   0,   0,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0,   0, 127,   0, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0, 127,  21,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "Init Square",
         22,  22,  64,  64,  64,  64,  64,  64,   0, 127,   0,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0, 127,   0,  64,  64,   0,  32,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
          0,   0, 127,   0,   0,   0, 127,   0,   0,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0,   0, 127,   0, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0, 127,  21,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "Init Triangle",
         31,  31,  64,  64,  64,  64,  64,  64,   0, 127,   0,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0, 127,   0,  64,  64,   0,  32,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
          0,   0, 127,   0,   0,   0, 127,   0,   0,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0,   0, 127,   0, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0, 127,  21,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "Init Arbitrary",
         40,  40,  64,  64,  64,  64,  64,  64,   0, 127,   0,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0, 127,   0,  64,  64,   0,  32,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
          0,   0, 127,   0,   0,   0, 127,   0,   0,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0,   0, 127,   0, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0, 127,  21,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "Init Pulse",
         49,  49,  64,  64,  64,  64,  64,  64,   0, 127,   0,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0, 127,   0,  64,  64,   0,  32,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
          0,   0, 127,   0,   0,   0, 127,   0,   0,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0,   0, 127,   0, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0, 127,  21,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "Init Saw Reverse",
         59,  59,  64,  64,  64,  64,  64,  64,   0, 127,   0,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0, 127,   0,  64,  64,   0,  32,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
          0,   0, 127,   0,   0,   0, 127,   0,   0,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0,   0, 127,   0, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0, 127,  21,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "Init Sample & Hold",
         68,  68,  64,  64,  64,  64,  64,  64,   0, 127,   0,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0, 127,   0,  64,  64,   0,  32,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
          0,   0, 127,   0,   0,   0, 127,   0,   0,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0,   0, 127,   0, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0, 127,  21,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "Init Triangle Var",
         77,  77,  64,  64,  64,  64,  64,  64,   0, 127,   0,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0, 127,   0,  64,  64,   0,  32,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
          0,   0, 127,   0,   0,   0, 127,   0,   0,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0,   0, 127,   0, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0, 127,  21,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "Init BL Saw",
         86,  86,  64,  64,  64,  64,  64,  64,   0, 127,   0,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0, 127,   0,  64,  64,   0,  32,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
          0,   0, 127,   0,   0,   0, 127,   0,   0,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0,   0, 127,   0, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0, 127,  21,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "Init BL Saw Rev",
         95,  95,  64,  64,  64,  64,  64,  64,   0, 127,   0,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0, 127,   0,  64,  64,   0,  32,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
          0,   0, 127,   0,   0,   0, 127,   0,   0,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0,   0, 127,   0, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0, 127,  21,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "Init BL Square",
        104, 104,  64,  64,  64,  64,  64,  64,   0, 127,   0,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0, 127,   0,  64,  64,   0,  32,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
          0,   0, 127,   0,   0,   0, 127,   0,   0,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0,   0, 127,   0, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0, 127,  21,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "Init BL Pulse",
        113, 113,  64,  64,  64,  64,  64,  64,   0, 127,   0,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0, 127,   0,  64,  64,   0,  32,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
          0,   0, 127,   0,   0,   0, 127,   0,   0,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0,   0, 127,   0, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0, 127,  21,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "Init Supersaw",
        123, 123,  64,  64,  64,  64,  64,  64,   0, 127,   0,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0, 127,   0,  64,  64,   0,  32,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
          0,   0, 127,   0,   0,   0, 127,   0,   0,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0,   0, 127,   0, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0, 127,  21,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "AdagioBass",
        110,  10,  80,  78,  64,  64,  64,  64,   0, 100,  80,  15,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  63,  33,   0,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         63, 127,   0,   0,   0,   0, 127,   0,   5,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,  38,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0,  66,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "AdagioLead",
         10,  10,  80,  99,  64,  64,  64,  64,   0, 100,  80,  15,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  50,  64,  87,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
          0, 127,   0,   0,   0,   0, 127,   0,  16,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0, 127,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "AdagioPad",
        110,  10,  80,  86,  64,  64,  64,  64,   0, 100,  80,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  50,   0,   0,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         63, 127,   0,   0,   0,   0, 127,   0,  58,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,  59,  64,  64,  78,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0,  74,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "AdagioString",
         10,  10,  80,  76,  64,  64,  64,  64,   0, 100,  80,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  50,   0,   0,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         64,  42, 118,   0,   0,   0, 127,   0,  73,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,  59,  64,  66,  69,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0,  91,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "AdagioSupersaw",
        110,  10,  80, 102,  64,  64,  64,  64,   0, 100,  80,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  50,   0,   0,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         63, 127,  25,   0,   0,   0, 127,   0,   2,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,  52,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0,  73,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "AirwavePadHigh",
        110,  10,  80,  97,  64,  64,  64,  64,   0, 100,  80,   7,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  50,  67, 116,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         63, 127,   0,   0,   0,   0, 127,   0,  43,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   1,  64,  64,  71,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0, 105,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "Airwave",
        110,  10,  80,  87,  64,  64,  64,  64,   0, 100,  80,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  50,   0,   0,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         63, 127,   0,   0,   0,   0, 127,   0,  46,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   1,  64,  64,  64,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0,  83,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "AirwavePadLow",
        110,  10,  80,  88,  64,  64,  64,  64,   0, 100,  80,   6,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  50,  72, 100,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
          0, 127,   0,   0,   0,   0, 127,   0,  44,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0,  78,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "CarteBlanche",
        110,  10,  80, 102,  64,  64,  64,  64,   0, 100,  80,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  50,   0,   0,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         63, 127,   0,   0,   0,   0, 127,   0,  58,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,  59,  64,  64,  78,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0,  74,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "CarteBlancheWide",
        110,  10,  80, 112,  64,  64,  64,  64,   0, 100,  80,   9,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  60,  64, 109,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         39, 127,  45,   0,   0,   0, 127,   0,   0,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,  41,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0, 127,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "CarteBlancheThin",
        110,  10,  76,  93,  64,  64,  64,  64,   0, 100,  80,   0,   0,  15,   0,  15, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  53,  57, 127,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         37, 127,  44,  45,   0,   0, 127,   0,   0,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,  38,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   1,  64,  49, 103,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0, 127,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "CatchBass",
         10,  90,  80,  16,  64,  64,  64,  64,   0, 100,   0,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  50,   0,   0,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         14, 127,  29,  38,   0,   0, 127,   0,   0,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,  35,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,  59,  64,  95,  64,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0,  47,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "CatchLead",
         10,  10,  80, 105,  64,  64,  64,  64,   0, 100,  80,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  66,  59,  97,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         39, 127,  25,  38,   0,   0, 127,   0,   6,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,  51,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   1,  64,  87,  85,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0,  87,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "EIGBass",
         10,  10,  84,  91,  64,  64,  64,  64,   0, 100,  80,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  50,  94,   0,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         42, 127,  31,   0,   0,   0, 127,   0,   0,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,  51,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0, 103,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "EIGDist",
         10,  70,  80,  80,  64,  64,  64,  64,   0, 100,  80,   9,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  64, 100,   0,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         33, 127,  38,   0,   0,   0, 127,   0,   0,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,  42,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,  59,  64, 114,  65,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0,  63,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "EIGLead",
        110,  10,  80,  97,  64,  64,  64,  64,   0, 100,  80,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  50,   0,   0,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         37, 127,  44,   0,   0,   0, 127,   0,   6,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,  60,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,  59,  64,  69,  80,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0,  58,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "EIGNoise",
        110,  30,  80, 109,  64,  64,  64,  64,   0, 100,  80,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  50,   0,   0,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
          0, 127,   0,   0,   0,   0, 127,   0,   0,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,  69,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0, 127,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "LigayaLead",
        110,  10,  80, 108,  64,  64,  64,  64,   0, 100,  80,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  50,   0,   0,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         30, 127,  29,   0,   0,   0, 127,   0,   0,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,  35,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0,  79,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "LigayaPad",
        110,  10,  80,  98,  64,  64,  64,  64,   0, 100,  80,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  50,   0,   0,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         63, 127,   0,   0,   0,   0, 127,   0,  58,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,  59,  64,  64,  64,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0,  84,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "LigayaString",
         10,  10,  80,  82,  64,  64,  64,  64,   0, 100,  80,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  50,   0,   0,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         39,  60,  94,   0,   0,   0, 127,   0,  73,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,  59,  64,  66,  64,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0,  80,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "SaturnPad",
        110,  10,  80,  93,  64,  64,  64,  64,   0, 100,  80,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  50,   0,   0,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         63, 127,   0,   0,   0,   0, 127,   0,  58,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   1,  64,  64,  64,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0,  83,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "SaturnString",
         10,  10,  80,  87,  64,  64,  64,  64,   0, 100,  80,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  50,   0,   0,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         63,  42, 118,   0,   0,   0, 127,   0,  73,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,  59,  64,  66,  64,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0, 100,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "ZBagpipe",
         10,  10,  80,  64,  64,  64,  64,  64,   0, 100,  80,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  50,   0,   0,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         44, 100,   0,  14,   0,   0, 127,   0,  27,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0,  38,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "ZBassoon",
         10,  10,  60,  70,  64,  64,  64,  64,   0, 100,  80,  15,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  50,   0,   0,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         31, 105,  60,   0,   0,   0, 127,   0,  21,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,  26,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0,  32,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "ZClarinet",
         10,  30,  80,  57,  64,  64,  64,  64,   0, 100,  80,  10,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  50,  42,  61,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         24, 103,  49,   0,   0,   0, 127,   0,  22,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,  26,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,  59,  64,  66,  55,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0,  45,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "ZFlute",
         10,  70,  80,  57,  64,  64,  64,  64,   0, 100,  80,  10,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  50,  60,  53,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         20, 100,  40,   0,   0,   0, 127,   0,  24,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,  26,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0,  45,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "ZHorn",
         10,  10,  93,  66,  64,  64,  64,  64,   0, 100,  80,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  50,   0,   0,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         41, 100,  27,  38,   0,   0, 127,   0,  27,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0,  28,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "ZPiccolo",
         10, 110,  80,  40,  64,  64,  64,  64,   0, 100,  80,  11,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  50,   0,   0,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         44, 102,   0,   0,   0,   0, 127,   0,  24,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0,  38,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "ZTimpani",
         10,  30,  80,  16,  64,  64,  64,  64,   0, 100,  80,  16,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  37,  44,  72,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         17, 127,  76,   0,   0,   0, 127,   0,   0,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,  58,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,  59,  64, 127, 117,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0,  23,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "ZTrombone",
         10,  70,  60,  73,  64,  64,  64,  64,   0, 100,  80,  15,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  44,  42, 112,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         27,  95,  49,   0,   0,   0, 127,   0,  21,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,  26,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,   0,  64,   0,   0,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0,  39,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "ZTrumpet",
         10,  10,  61,  57,  64,  64,  64,  64,   0, 100,  80,  10,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  44,  60,  53,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         26, 103,  49,   0,   0,   0, 127,   0,  21,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,  26,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,  59,  64,  66,  55,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0,  45,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "ZTuba",
         10,  10,  39,  86,  64,  64,  64,  64,   0, 100,  80,  13,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  57,   0,   0,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         26, 103,  49,   0,   0,   0, 127,   0,  43,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,  26,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,  59,  64,  66,   0,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0,  21,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "ZViolin",
         10,  10,  80,  68,  64,  64,  64,  64,   0, 100,  80,   0,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  50,   0,   0,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         29, 105,  34,   0,   0,   0, 127,   0,  24,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,  59,  64,  95,  64,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0,  55,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    },
    { "ZViolin2",
         10,  10,  80,  89,  64,  64,  64,  64,   0, 100,  80,   6,   0,   0,   0,   0, // [0] OSC1_WAVE..SUPERSAW2_DETUNE
          0,   0,   0,   0,   0,  53,  64,  73,  65,   0,  96,   0,   0,   0,   0,   0, // [16] SUPERSAW2_MIX..FILTER_OBXA_RES_MOD_DEPTH
         29, 105,  34,   0,   0,   0, 127,   0,  24,   0,   0,   0,   0,   0,   0,   0, // [32] AMP_ATTACK..LFO1_AMP_DEPTH
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   5,  40,   0,  10,  64,   0, // [48] LFO1_DELAY..VELOCITY_AMP_SENS
          0,   0,  64,  64,   0,  59,  64,  83,  56,   0,  64,   0,   0, 127,   0, 127, // [64] VELOCITY_FILTER_SENS..GLIDE_ENABLE
          0,  53,   0,  64,   0,   0,   0,   0,   0,   0,  // [80] GLIDE_TIME..OSC2_SHAPE_DC 
        0,   0,  12,   0,   0,   0,   0,   0,   0,   0,   0, // [90] OSC_CROSS_MOD_DEPTH..OSC2_ARB_INDEX
    }
};

// Name accessor (PROGMEM-safe on Teensy ARM)
inline const char* patchName(int idx) {
    if (idx < 0 || idx >= kFactoryCount) return "---";
    return kPatches[idx].name;
}

// Load a factory patch by index — sends every CC, no stale state.
// Declared here, defined in FactoryBank.cpp.
void loadFactoryPatch(SynthEngine& synth, int index, uint8_t midiCh = 1);

} // namespace FactoryBank
