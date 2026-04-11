#!/usr/bin/env python3
"""
generate_factory_bank.py
Generates FactoryBank.h with flat CC arrays aligned to PatchSchema::kPatchableCCs.
"""

# ---- PatchSchema CC order (95 entries) ----
# Index : CC_NAME
CC_NAMES = [
    "OSC1_WAVE", "OSC2_WAVE",                                          # 0-1
    "OSC1_PITCH_OFFSET", "OSC2_PITCH_OFFSET",                          # 2-3
    "OSC1_FINE_TUNE", "OSC2_FINE_TUNE",                                # 4-5
    "OSC1_DETUNE", "OSC2_DETUNE",                                      # 6-7
    "OSC_MIX_BALANCE",                                                  # 8
    "OSC1_MIX", "OSC2_MIX",                                            # 9-10
    "SUB_MIX", "NOISE_MIX",                                            # 11-12
    "SUPERSAW1_DETUNE", "SUPERSAW1_MIX",                               # 13-14
    "SUPERSAW2_DETUNE", "SUPERSAW2_MIX",                               # 15-16
    "OSC1_FEEDBACK_AMOUNT", "OSC2_FEEDBACK_AMOUNT",                    # 17-18
    "OSC1_FEEDBACK_MIX", "OSC2_FEEDBACK_MIX",                         # 19-20
    "FILTER_CUTOFF", "FILTER_RESONANCE",                               # 21-22
    "FILTER_ENV_AMOUNT", "FILTER_KEY_TRACK",                           # 23-24
    "FILTER_OCTAVE_CONTROL",                                            # 25
    "FILTER_ENGINE", "FILTER_MODE", "VA_FILTER_TYPE",                  # 26-28
    "FILTER_OBXA_XPANDER_MODE",                                        # 29
    "FILTER_OBXA_MULTIMODE", "FILTER_OBXA_RES_MOD_DEPTH",             # 30-31
    "AMP_ATTACK", "AMP_DECAY", "AMP_SUSTAIN", "AMP_RELEASE",          # 32-35
    "FILTER_ENV_ATTACK", "FILTER_ENV_DECAY",                           # 36-37
    "FILTER_ENV_SUSTAIN", "FILTER_ENV_RELEASE",                        # 38-39
    "LFO1_FREQ", "LFO1_DEPTH", "LFO1_DESTINATION", "LFO1_WAVEFORM",  # 40-43
    "LFO1_PITCH_DEPTH", "LFO1_FILTER_DEPTH",                          # 44-45
    "LFO1_PWM_DEPTH", "LFO1_AMP_DEPTH", "LFO1_DELAY",                # 46-48
    "LFO2_FREQ", "LFO2_DEPTH", "LFO2_DESTINATION", "LFO2_WAVEFORM",  # 49-52
    "LFO2_PITCH_DEPTH", "LFO2_FILTER_DEPTH",                          # 53-54
    "LFO2_PWM_DEPTH", "LFO2_AMP_DEPTH", "LFO2_DELAY",                # 55-57
    "PITCH_ENV_ATTACK", "PITCH_ENV_DECAY",                             # 58-59
    "PITCH_ENV_SUSTAIN", "PITCH_ENV_RELEASE", "PITCH_ENV_DEPTH",      # 60-62
    "VELOCITY_AMP_SENS", "VELOCITY_FILTER_SENS", "VELOCITY_ENV_SENS", # 63-65
    "FX_BASS_GAIN", "FX_TREBLE_GAIN", "FX_DRIVE",                     # 66-68
    "FX_MOD_EFFECT", "FX_MOD_MIX", "FX_MOD_RATE", "FX_MOD_FEEDBACK", # 69-72
    "FX_JPFX_DELAY_EFFECT", "FX_JPFX_DELAY_MIX",                     # 73-74
    "FX_JPFX_DELAY_FEEDBACK", "FX_JPFX_DELAY_TIME",                   # 75-76
    "FX_REVERB_SIZE", "FX_REVERB_DAMP", "FX_REVERB_LODAMP",           # 77-79
    "FX_REVERB_MIX", "FX_REVERB_BYPASS",                              # 80-81
    "FX_DRY_MIX", "FX_JPFX_MIX",                                      # 82-83
    "GLIDE_ENABLE", "GLIDE_TIME",                                      # 84-85
    "AMP_MOD_FIXED_LEVEL",                                              # 86
    "POLY_MODE", "UNISON_DETUNE",                                      # 87-88
    "RING1_MIX", "RING2_MIX",                                          # 89-90
    "OSC1_FREQ_DC", "OSC1_SHAPE_DC",                                   # 91-92
    "OSC2_FREQ_DC", "OSC2_SHAPE_DC",                                   # 93-94
]

NUM_CCS = len(CC_NAMES)
assert NUM_CCS == 95, f"Expected 95 CCs, got {NUM_CCS}"

# Index lookup
IDX = {name: i for i, name in enumerate(CC_NAMES)}

# ---- Safe defaults for every CC ----
SAFE = [0] * NUM_CCS
SAFE[IDX["OSC1_PITCH_OFFSET"]]     = 64   # 0 semitones
SAFE[IDX["OSC2_PITCH_OFFSET"]]     = 64
SAFE[IDX["OSC1_FINE_TUNE"]]        = 64   # 0 cents
SAFE[IDX["OSC2_FINE_TUNE"]]        = 64
SAFE[IDX["OSC1_DETUNE"]]           = 64   # 0 Hz detune
SAFE[IDX["OSC2_DETUNE"]]           = 64
SAFE[IDX["OSC1_MIX"]]              = 127  # full
SAFE[IDX["OSC2_MIX"]]              = 0
SAFE[IDX["FILTER_CUTOFF"]]         = 127  # wide open
SAFE[IDX["FILTER_ENV_AMOUNT"]]     = 65   # slight positive
SAFE[IDX["FILTER_KEY_TRACK"]]      = 65
SAFE[IDX["FILTER_ENGINE"]]         = 96   # VA engine (high half = VA)
SAFE[IDX["FILTER_MODE"]]           = 0    # LP
SAFE[IDX["VA_FILTER_TYPE"]]        = 0    # SVF (first VA type)
SAFE[IDX["AMP_SUSTAIN"]]           = 127  # full sustain
SAFE[IDX["FILTER_ENV_SUSTAIN"]]    = 127
SAFE[IDX["PITCH_ENV_ATTACK"]]      = 5    # short
SAFE[IDX["PITCH_ENV_DECAY"]]       = 40   # medium
SAFE[IDX["PITCH_ENV_RELEASE"]]     = 10   # short
SAFE[IDX["PITCH_ENV_DEPTH"]]       = 64   # 0 semitones (neutral)
SAFE[IDX["FX_BASS_GAIN"]]          = 64   # 0 dB
SAFE[IDX["FX_TREBLE_GAIN"]]        = 64   # 0 dB
SAFE[IDX["FX_MOD_MIX"]]            = 64   # 50%
SAFE[IDX["FX_JPFX_DELAY_MIX"]]     = 64   # 50%
SAFE[IDX["FX_DRY_MIX"]]            = 127  # full dry
SAFE[IDX["AMP_MOD_FIXED_LEVEL"]]   = 127  # full level
SAFE[IDX["UNISON_DETUNE"]]         = 64   # mid spread

# ---- Waveform CC values (from ccFromWaveform with 14 waveforms) ----
# bucket midpoint = ((i * 128) / 14 + ((i+1) * 128) / 14) // 2
WAVE_CCS = []
for i in range(14):
    start = (i * 128) // 14
    end = ((i + 1) * 128) // 14
    WAVE_CCS.append((start + end) // 2)

WAVE_NAMES = [
    "Sine", "Sawtooth", "Square", "Triangle", "Arbitrary", "Pulse",
    "Saw Reverse", "Sample & Hold", "Triangle Var",
    "BL Saw", "BL Saw Rev", "BL Square", "BL Pulse", "Supersaw"
]

# ---- Build init patches (one per waveform) ----
init_patches = []
for wi in range(14):
    p = list(SAFE)
    p[IDX["OSC1_WAVE"]] = WAVE_CCS[wi]
    p[IDX["OSC2_WAVE"]] = WAVE_CCS[wi]
    init_patches.append((f"Init {WAVE_NAMES[wi]}", p))

# ---- TUS patch data (from TUS_Presets.h) ----
# Fields: name, osc1Wave, osc2Wave, osc1Coarse, osc2Coarse, ssDetune,
#         osc1Mix, osc2Mix, noiseMix, subMix,
#         filterCutoff, filterRes, filterEnvAmt,
#         ampAttack, ampDecay, ampSustain, ampRelease, ampLevel,
#         lfo1Rate, lfo2Rate,
#         fxType, fxParam1, fxParam2,
#         glideOn, glideTime

TUS_RAW = [
    ("AdagioBass",       110, 10,  80, 78,   0, 100,  80,   0,  15,  63, 33,  0,  63,127,  0,  0,  66,   5, 38,  0, 63,109, 127,  0),
    ("AdagioLead",        10, 10,  80, 99,   0, 100,  80,   0,  15,  50, 64, 87,   0,127,  0,  0, 127,  16,  0,  0, 64, 64, 127,  0),
    ("AdagioPad",        110, 10,  80, 86,   0, 100,  80,   0,   0,  50,  0,  0,  63,127,  0,  0,  74,  58,  0,  3, 64, 78, 127,  0),
    ("AdagioString",      10, 10,  80, 76,   0, 100,  80,   0,   0,  50,  0,  0,  64, 42,118,  0,  91,  73,  0,  3, 66, 69, 127,  0),
    ("AdagioSupersaw",   110, 10,  80,102,   0, 100,  80,   0,   0,  50,  0,  0,  63,127, 25,  0,  73,   2, 52,  0, 64, 64, 127,  0),
    ("AirwavePadHigh",   110, 10,  80, 97,   0, 100,  80,   0,   7,  50, 67,116,  63,127,  0,  0, 105,  43,  0,  2, 64, 71, 127,  0),
    ("Airwave",          110, 10,  80, 87,   0, 100,  80,   0,   0,  50,  0,  0,  63,127,  0,  0,  83,  46,  0,  2, 64, 64, 127,  0),
    ("AirwavePadLow",    110, 10,  80, 88,   0, 100,  80,   0,   6,  50, 72,100,   0,127,  0,  0,  78,  44,  0,  0, 64, 64, 127,  0),
    ("CarteBlanche",     110, 10,  80,102,   0, 100,  80,   0,   0,  50,  0,  0,  63,127,  0,  0,  74,  58,  0,  3, 64, 78, 127,  0),
    ("CarteBlancheWide", 110, 10,  80,112,   0, 100,  80,   0,   9,  60, 64,109,  39,127, 45,  0, 127,   0, 41,  0, 81, 73, 127,  0),
    ("CarteBlancheThin", 110, 10,  76, 93,  15, 100,  80,   0,   0,  53, 57,127,  37,127, 44, 45, 127,   0, 38,  2, 49,103, 127,  0),
    ("CatchBass",         10, 90,  80, 16,   0, 100,   0,   0,   0,  50,  0,  0,  14,127, 29, 38,  47,   0, 35,  3, 95, 64, 127,  0),
    ("CatchLead",         10, 10,  80,105,   0, 100,  80,   0,   0,  66, 59, 97,  39,127, 25, 38,  87,   6, 51,  2, 87, 85, 127,  0),
    ("EIGBass",           10, 10,  84, 91,   0, 100,  80,   0,   0,  50, 94,  0,  42,127, 31,  0, 103,   0, 51,  0,113, 71, 127,  0),
    ("EIGDist",           10, 70,  80, 80,   0, 100,  80,   0,   9,  64,100,  0,  33,127, 38,  0,  63,   0, 42,  3,114, 65, 127,  0),
    ("EIGLead",          110, 10,  80, 97,   0, 100,  80,   0,   0,  50,  0,  0,  37,127, 44,  0,  58,   6, 60,  3, 69, 80, 127,  0),
    ("EIGNoise",         110, 30,  80,109,   0, 100,  80,   0,   0,  50,  0,  0,   0,127,  0,  0, 127,   0, 69,  0, 64, 64, 127,  0),
    ("LigayaLead",       110, 10,  80,108,   0, 100,  80,   0,   0,  50,  0,  0,  30,127, 29,  0,  79,   0, 35,  0, 64, 64, 127,  0),
    ("LigayaPad",        110, 10,  80, 98,   0, 100,  80,   0,   0,  50,  0,  0,  63,127,  0,  0,  84,  58,  0,  3, 64, 64, 127,  0),
    ("LigayaString",      10, 10,  80, 82,   0, 100,  80,   0,   0,  50,  0,  0,  39, 60, 94,  0,  80,  73,  0,  3, 66, 64, 127,  0),
    ("SaturnPad",        110, 10,  80, 93,   0, 100,  80,   0,   0,  50,  0,  0,  63,127,  0,  0,  83,  58,  0,  2, 64, 64, 127,  0),
    ("SaturnString",      10, 10,  80, 87,   0, 100,  80,   0,   0,  50,  0,  0,  63, 42,118,  0, 100,  73,  0,  3, 66, 64, 127,  0),
    ("ZBagpipe",          10, 10,  80, 64,   0, 100,  80,   0,   0,  50,  0,  0,  44,100,  0, 14,  38,  27,  0,  0, 64, 64, 127,  0),
    ("ZBassoon",          10, 10,  60, 70,   0, 100,  80,   0,  15,  50,  0,  0,  31,105, 60,  0,  32,  21, 26,  0, 75, 64, 127,  0),
    ("ZClarinet",         10, 30,  80, 57,   0, 100,  80,   0,  10,  50, 42, 61,  24,103, 49,  0,  45,  22, 26,  3, 66, 55, 127,  0),
    ("ZFlute",            10, 70,  80, 57,   0, 100,  80,   0,  10,  50, 60, 53,  20,100, 40,  0,  45,  24, 26,  0, 66, 55, 127,  0),
    ("ZHorn",             10, 10,  93, 66,   0, 100,  80,   0,   0,  50,  0,  0,  41,100, 27, 38,  28,  27,  0,  0, 75, 64, 127,  0),
    ("ZPiccolo",          10,110,  80, 40,   0, 100,  80,   0,  11,  50,  0,  0,  44,102,  0,  0,  38,  24,  0,  0, 64, 64, 127,  0),
    ("ZTimpani",          10, 30,  80, 16,   0, 100,  80,   0,  16,  37, 44, 72,  17,127, 76,  0,  23,   0, 58,  3,127,117, 127,  0),
    ("ZTrombone",         10, 70,  60, 73,   0, 100,  80,   0,  15,  44, 42,112,  27, 95, 49,  0,  39,  21, 26,  0, 66, 52, 127,  0),
    ("ZTrumpet",          10, 10,  61, 57,   0, 100,  80,   0,  10,  44, 60, 53,  26,103, 49,  0,  45,  21, 26,  3, 66, 55, 127,  0),
    ("ZTuba",             10, 10,  39, 86,   0, 100,  80,   0,  13,  57,  0,  0,  26,103, 49,  0,  21,  43, 26,  3, 66,  0, 127,  0),
    ("ZViolin",           10, 10,  80, 68,   0, 100,  80,   0,   0,  50,  0,  0,  29,105, 34,  0,  55,  24,  0,  3, 95, 64, 127,  0),
    ("ZViolin2",          10, 10,  80, 89,   0, 100,  80,   0,   6,  53, 64, 73,  29,105, 34,  0,  53,  24,  0,  3, 83, 56, 127,  0),
]

def convert_tus(t):
    """Convert a TUS tuple to a 95-element CC array."""
    name, o1w, o2w, o1c, o2c, ssd, o1m, o2m, nm, sm, fcut, fres, fenv, \
        aa, ad, as_, ar, al, l1r, l2r, fxt, fxp1, fxp2, gon, gt = t

    p = list(SAFE)  # start from safe defaults

    p[IDX["OSC1_WAVE"]]        = o1w
    p[IDX["OSC2_WAVE"]]        = o2w
    p[IDX["OSC1_PITCH_OFFSET"]]= o1c
    p[IDX["OSC2_PITCH_OFFSET"]]= o2c
    p[IDX["SUPERSAW1_DETUNE"]] = ssd
    p[IDX["SUPERSAW2_DETUNE"]] = ssd
    p[IDX["OSC1_MIX"]]         = o1m
    p[IDX["OSC2_MIX"]]         = o2m
    p[IDX["NOISE_MIX"]]        = nm
    p[IDX["SUB_MIX"]]          = sm
    p[IDX["FILTER_CUTOFF"]]    = fcut
    p[IDX["FILTER_RESONANCE"]] = fres
    p[IDX["FILTER_ENV_AMOUNT"]]= fenv
    p[IDX["AMP_ATTACK"]]       = aa
    p[IDX["AMP_DECAY"]]        = ad
    p[IDX["AMP_SUSTAIN"]]      = as_
    p[IDX["AMP_RELEASE"]]      = ar
    p[IDX["AMP_MOD_FIXED_LEVEL"]] = al
    p[IDX["LFO1_FREQ"]]        = l1r
    p[IDX["LFO2_FREQ"]]        = l2r
    p[IDX["GLIDE_ENABLE"]]     = gon
    p[IDX["GLIDE_TIME"]]       = gt

    # Map JP-8000 FX type to JPFX mod/delay
    # 0=none, 1=chorus, 2=chorus2, 3=flanger, 4=delay, 5=long delay
    if fxt in (1, 2):
        # Chorus → mod effect CC value ~1 maps to variation 0 (Chorus 1)
        # CC value: ((variation * 127) / 11) + 1 ≈ variation=0 → cc=1
        p[IDX["FX_MOD_EFFECT"]] = 1   # CC 1 → variation 0 (Chorus 1)
        p[IDX["FX_MOD_RATE"]]  = fxp1
        p[IDX["FX_MOD_FEEDBACK"]] = fxp2
    elif fxt == 3:
        # Flanger → variation 5 (Deep Flanger)
        # CC for variation 5: ((5 * 127) / 11) + 1 ≈ 59
        p[IDX["FX_MOD_EFFECT"]] = 59
        p[IDX["FX_MOD_RATE"]]  = fxp1
        p[IDX["FX_MOD_FEEDBACK"]] = fxp2
    else:
        p[IDX["FX_MOD_EFFECT"]] = 0  # off

    if fxt == 4:
        # Delay → variation 0 (Mono Short): CC = 1
        p[IDX["FX_JPFX_DELAY_EFFECT"]] = 1
        p[IDX["FX_JPFX_DELAY_TIME"]]   = fxp1
        p[IDX["FX_JPFX_DELAY_FEEDBACK"]] = fxp2
    elif fxt == 5:
        # Long delay → variation 1 (Mono Long): CC ≈ 26
        p[IDX["FX_JPFX_DELAY_EFFECT"]] = 26
        p[IDX["FX_JPFX_DELAY_TIME"]]   = fxp1
        p[IDX["FX_JPFX_DELAY_FEEDBACK"]] = fxp2
    else:
        p[IDX["FX_JPFX_DELAY_EFFECT"]] = 0  # off

    return p

# ---- Build TUS patches ----
tus_patches = []
for t in TUS_RAW:
    tus_patches.append((t[0], convert_tus(t)))

# ---- Combine all patches ----
all_patches = init_patches + tus_patches

# ---- Generate C++ header ----
lines = []
lines.append("/*")
lines.append(" * FactoryBank.h — JT-8000 Factory Preset Bank")
lines.append(" *")
lines.append(" * Auto-generated by generate_factory_bank.py")
lines.append(" * DO NOT EDIT MANUALLY — regenerate from the script.")
lines.append(" *")
lines.append(f" * {len(init_patches)} init patches + {len(tus_patches)} factory sounds = {len(all_patches)} total")
lines.append(" *")
lines.append(" * Each patch is a flat array of CC values in PatchSchema::kPatchableCCs order.")
lines.append(" * The loader sends EVERY CC, guaranteeing no stale state from previous patches.")
lines.append(" *")
lines.append(" * To add a patch:")
lines.append(" *   1. Copy an init row")
lines.append(" *   2. Change the values you need")
lines.append(" *   3. Increment kFactoryCount")
lines.append(" *   4. Use the column comments below to find the right index")
lines.append(" */")
lines.append("")
lines.append("#pragma once")
lines.append("#include <Arduino.h>")
lines.append("#include \"PatchSchema.h\"")
lines.append("")
lines.append("namespace FactoryBank {")
lines.append("")
lines.append(f"static constexpr int kFactoryCount = {len(all_patches)};")
lines.append(f"static constexpr int kInitCount    = {len(init_patches)};")
lines.append("")

# Column header comment
lines.append("// Column index reference (PatchSchema::kPatchableCCs order):")
lines.append("// Idx CC_Name")
for i, name in enumerate(CC_NAMES):
    lines.append(f"// {i:2d}  {name}")
lines.append("")

lines.append("struct FactoryPatch {")
lines.append("    const char* name;")
lines.append(f"    uint8_t     cc[{NUM_CCS}];")
lines.append("};")
lines.append("")
lines.append(f"PROGMEM static const FactoryPatch kPatches[kFactoryCount] = {{")

for pi, (name, cc_vals) in enumerate(all_patches):
    # Format array compactly with groups
    vals_str = ", ".join(f"{v:3d}" for v in cc_vals)

    # Add section comments
    if pi == 0:
        lines.append("")
        lines.append("    // ---- Init Patches (one per waveform) ----")
    elif pi == len(init_patches):
        lines.append("")
        lines.append("    // ---- Factory Sounds (converted from TUS JP-8000 bank) ----")

    lines.append(f'    {{ "{name}",')
    # Split into groups of ~16 for readability
    for start in range(0, NUM_CCS, 16):
        chunk = cc_vals[start:start+16]
        prefix = "      { " if start == 0 else "        "
        suffix = " }" if start + 16 >= NUM_CCS else ""
        chunk_str = ", ".join(f"{v:3d}" for v in chunk)
        # Add group label
        group_names = CC_NAMES[start:start+16]
        label = f"// [{start}] {group_names[0]}..{group_names[-1]}"
        lines.append(f"      {chunk_str}, {label}")
    sep = "," if pi < len(all_patches) - 1 else ""
    lines.append(f"    }}{sep}")

lines.append("};")
lines.append("")
lines.append("// Name accessor (PROGMEM-safe on Teensy ARM)")
lines.append("inline const char* patchName(int idx) {")
lines.append("    if (idx < 0 || idx >= kFactoryCount) return \"---\";")
lines.append("    return kPatches[idx].name;")
lines.append("}")
lines.append("")
lines.append("} // namespace FactoryBank")

# Write
with open("/home/claude/FactoryBank.h", "w") as f:
    f.write("\n".join(lines) + "\n")

print(f"Generated FactoryBank.h: {len(all_patches)} patches, {NUM_CCS} CCs each")
