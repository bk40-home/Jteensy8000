#pragma once
// =============================================================================
// JP8000SyxMap.h — JP-8000/8080 SysEx → JT-8000 CC mapping table
//
// SINGLE SOURCE OF TRUTH for converting JP-8000 SysEx patch data into
// JT-8000 CC values. Used by:
//   - JP8000SyxLoader.cpp  (firmware, SD card import)
//   - HTML editor           (JavaScript mirror of this table)
//   - JUCE plugin           (C++ direct include)
//
// PATCH FORMAT (from Roland JP-8080 MIDI Implementation):
//   248 bytes total (JP-8080) or 239 bytes (JP-8000).
//   Bytes 0x00-0x0F : 16-char ASCII patch name
//   Bytes 0x10-0x49 : synthesis parameters (one byte each)
//   Bytes 0x4A-0x172: control matrix (14-bit velocity/controller depths)
//   Bytes 0x173-0x177: JP-8080 tail (unison, gain, ext trigger)
//
// This file only maps the synthesis parameters (0x10-0x49) since those
// are the ones with direct JT-8000 CC equivalents. The control matrix
// and JP-8080 tail are handled separately by the loader if needed.
//
// USAGE:
//   for (const auto& slot : JP8000SyxMap::kSlots) {
//       uint8_t raw = patchData[slot.offset];
//       uint8_t cc  = slot.cc;
//       uint8_t val = JP8000SyxMap::applyTransform(slot, raw, patchData);
//       synth.handleControlChange(ch, cc, val);
//   }
//
// ENUM REMAPPING:
//   JP-8000 and JT-8000 use different waveform/filter/FX enums.
//   Lookup tables in this header handle all conversions.
//   Values are precomputed CC midpoints from ccFromWaveform() in WaveForms.h.
// =============================================================================

#include "CCDefs.h"

namespace JP8000SyxMap {

// =============================================================================
// PATCH NAME
// =============================================================================
static constexpr uint8_t NAME_OFFSET = 0x00;
static constexpr uint8_t NAME_LENGTH = 16;

// Extract patch name from raw SysEx data block.
// Writes a null-terminated string of up to maxLen-1 printable characters.
inline void extractName(const uint8_t* data, uint16_t dataLen,
                        char* out, uint8_t maxLen)
{
    uint8_t i = 0;
    for (; i < NAME_LENGTH && i < (maxLen - 1) && i < dataLen; ++i) {
        uint8_t c = data[NAME_OFFSET + i];
        out[i] = (c >= 0x20 && c <= 0x7E) ? (char)c : ' ';
    }
    // Trim trailing spaces
    while (i > 0 && out[i - 1] == ' ') --i;
    out[i] = '\0';
}

// =============================================================================
// TRANSFORM TYPES
// =============================================================================
enum class SyxXform : uint8_t {
    Direct,             // Pass raw value through (0-127)
    BoolTo127,          // 0 → 0, nonzero → 127
    Signed64Direct,     // Already ±64 bipolar, pass through unchanged
    Signed64ToUni,      // ±64 bipolar → 0-127 unipolar: abs(raw-64)*2, clamped
    Osc1WaveRemap,      // JP OSC1 enum (0-6) → JT waveform CC
    Osc2WaveRemap,      // JP OSC2 enum (0-3) → JT waveform CC
    Lfo1WaveRemap,      // JP LFO1 waveform (0-3) → JT LFO waveform CC
    FilterTypeRemap,    // JP filter (0=HPF,1=BPF,2=LPF) → JT FILTER_MODE CC
    MfxTypeRemap,       // JP multi-FX (0-12) → JT FX_MOD_EFFECT CC
    DelayTypeRemap,     // JP delay (0-4) → JT FX_JPFX_DELAY_EFFECT CC
    Osc2Range,          // JP 0-50 (±25 semi) → JT 0-127 (±64)
    Osc2Fine,           // JP 0-100 (±50 cents) → JT 0-127
    MonoToPolyMode,     // JP mono (0/1) → JT POLY_MODE CC (0=POLY, 64=MONO)
    OscCtrl1,           // Context-dependent on waveform (detune or PW)
    OscCtrl2,           // Context-dependent on waveform (mix or feedback)
    Ignore,             // Read but do not send (informational only)
};

// =============================================================================
// SLOT TABLE ENTRY
// =============================================================================
struct SyxSlot {
    uint8_t  offset;    // Byte position in the SysEx data block (0x00-based)
    uint8_t  cc;        // JT-8000 CC number (from CCDefs.h)
    SyxXform xform;     // Transform to apply before sending
};

// =============================================================================
// WAVEFORM LOOKUP TABLES
//
// CC midpoint values computed from ccFromWaveform() in WaveForms.h:
//   14 waveforms mapped across 0-127, midpoint = (bin_start + bin_end) / 2
//
//   SINE=4, SAW=13, SQR=22, TRI=31, ARB=40, PULSE=49, SAW_REV=59,
//   S&H=68, TRI_VAR=77, BL_SAW=86, BL_SAW_REV=95, BL_SQR=104,
//   BL_PULSE=113, SUPERSAW=123
// =============================================================================

// --- JP-8000 OSC1 waveform enum → JT CC value ---
// JP: 0=SuperSaw, 1=TWM, 2=Feedback, 3=Noise, 4=Pulse, 5=Saw, 6=Triangle
static constexpr uint8_t kOsc1WaveMap[] = {
    123,    // 0: SuperSaw  → SUPERSAW (CC 123)
     77,    // 1: TWM       → TRIANGLE_VAR (CC 77) — variable-width triangle
     13,    // 2: Feedback  → SAW (CC 13) — closest tonal match
     68,    // 3: Noise     → SAMPLE_HOLD (CC 68) — noise-like; also set NOISE_MIX
    113,    // 4: Pulse     → BL_PULSE (CC 113) — band-limited pulse
     86,    // 5: Saw       → BL_SAW (CC 86) — band-limited sawtooth
     31,    // 6: Triangle  → TRIANGLE (CC 31)
};
static constexpr uint8_t kOsc1WaveMapSize = sizeof(kOsc1WaveMap);

// --- JP-8000 OSC2 waveform enum → JT CC value ---
// JP: 0=Pulse, 1=Triangle, 2=Saw, 3=Noise
static constexpr uint8_t kOsc2WaveMap[] = {
    113,    // 0: Pulse    → BL_PULSE (CC 113)
     31,    // 1: Triangle → TRIANGLE (CC 31)
     86,    // 2: Saw      → BL_SAW (CC 86)
     68,    // 3: Noise    → SAMPLE_HOLD (CC 68)
};
static constexpr uint8_t kOsc2WaveMapSize = sizeof(kOsc2WaveMap);

// --- JP-8000 LFO1 waveform enum → JT LFO waveform enum ---
// JP: 0=Triangle, 1=Saw, 2=Square, 3=S&H
// JT LFO waveform enum values (passed directly as CC):
//   Tri=0, Saw=1, Sqr=2, S&H=3 (same ordering — direct passthrough)
static constexpr uint8_t kLfo1WaveMap[] = {
    0,      // 0: Triangle → 0
    1,      // 1: Saw      → 1
    2,      // 2: Square   → 2
    3,      // 3: S&H      → 3
};

// --- JP-8000 filter type → JT FILTER_MODE CC ---
// JP: 0=HPF, 1=BPF, 2=LPF
// JT FILTER_MODE (CC 112): depends on engine, but standard mapping:
//   LPF=0, BPF=1, HPF=2 (common convention)
// Invert the JP ordering to match JT.
static constexpr uint8_t kFilterTypeMap[] = {
    2,      // 0: HPF → 2 (JT HPF)
    1,      // 1: BPF → 1 (JT BPF)
    0,      // 2: LPF → 0 (JT LPF)
};

// --- JP-8000 Multi-FX type → JT FX_MOD_EFFECT CC ---
// JP: 0-12 (see plan for full list)
// JT FX_MOD_EFFECT (CC 103): 0-10 variation index
// FX types 5-7 (short delays) and 8-12 (drive/distortion) need special handling.
static constexpr uint8_t kMfxTypeMap[] = {
    10,     //  0: Super Chorus Slow → 10 (Super Chorus)
    10,     //  1: Super Chorus Fast → 10 (Super Chorus) + set rate high
     0,     //  2: Chorus 1          →  0
     1,     //  3: Chorus 2          →  1
     3,     //  4: Flanger           →  3 (Flanger 1)
     0,     //  5: Short Delay       →  0 (ignore mod, use delay section)
     0,     //  6: Short Delay FB    →  0 (ignore mod, use delay section)
     0,     //  7: Short Delay Rev   →  0 (ignore mod, use delay section)
     0,     //  8: Overdrive         →  0 (map to FX_DRIVE instead)
     0,     //  9: Distortion 1      →  0 (map to FX_DRIVE instead)
     0,     // 10: Distortion 2      →  0 (map to FX_DRIVE instead)
     0,     // 11: Distortion 3      →  0 (map to FX_DRIVE instead)
     0,     // 12: Distortion 4      →  0 (map to FX_DRIVE instead)
};
static constexpr uint8_t kMfxTypeMapSize = sizeof(kMfxTypeMap);

// Is this JP MFX type a drive/distortion effect?
// Used by the loader to set FX_DRIVE CC instead of FX_MOD_EFFECT.
inline constexpr bool isMfxDrive(uint8_t jpType) { return jpType >= 8 && jpType <= 12; }

// Is this JP MFX type a short delay variant?
// Used by the loader to route to the delay section instead.
inline constexpr bool isMfxShortDelay(uint8_t jpType) { return jpType >= 5 && jpType <= 7; }

// Is this JP MFX Super Chorus Fast? (needs rate boost)
inline constexpr bool isMfxSuperChoFast(uint8_t jpType) { return jpType == 1; }

// --- JP-8000 delay type → JT FX_JPFX_DELAY_EFFECT CC ---
// JP: 0=PanL→R, 1=Delay, 2=PanL←R, 3=Delay2, 4=MonoLong
// JT: 0=MONO_SHORT, 1=MONO_LONG, 2=PAN_LR, 3=PAN_RL, 4=PAN_STEREO
static constexpr uint8_t kDelayTypeMap[] = {
    2,      // 0: Pan L->R → PAN_LR (2)
    0,      // 1: Delay    → MONO_SHORT (0)
    3,      // 2: Pan L<-R → PAN_RL (3)
    1,      // 3: Delay 2  → MONO_LONG (1)
    1,      // 4: Mono Long→ MONO_LONG (1)
};
static constexpr uint8_t kDelayTypeMapSize = sizeof(kDelayTypeMap);

// =============================================================================
// MAIN SLOT TABLE — JP-8000 SysEx offset → JT-8000 CC
//
// Only synthesis parameters (0x10-0x49) are listed.
// The control matrix (0x4A+) and JP-8080 tail (0x173+) are omitted.
//
// Parameters marked Ignore are read for context (e.g. waveform selection)
// but do not directly produce a CC message. The loader handles them
// separately for context-dependent dispatch.
// =============================================================================
static constexpr SyxSlot kSlots[] = {

    // ── LFO / Modulation ─────────────────────────────────────────────────
    { 0x10, CC::LFO1_WAVEFORM,      SyxXform::Lfo1WaveRemap    },
    { 0x11, CC::LFO1_FREQ,          SyxXform::Direct           },
    { 0x12, CC::LFO1_DELAY,         SyxXform::Direct           }, // JP "Fade" = JT "Delay"
    { 0x13, CC::LFO2_FREQ,          SyxXform::Direct           },
    // 0x14 LFO2 Depth Select — no direct CC; JT uses per-dest depths
    { 0x15, CC::RING1_MIX,          SyxXform::BoolTo127        },
    // 0x16 Cross Mod Depth — no JT CC (future work)
    { 0x17, CC::OSC_MIX_BALANCE,    SyxXform::Signed64Direct   },

    // LFO / Env destination and per-dest depths
    { 0x18, CC::LFO1_DESTINATION,   SyxXform::Direct           }, // JP 0-2 same as JT
    { 0x19, CC::LFO1_PITCH_DEPTH,   SyxXform::Signed64ToUni    },
    { 0x1A, CC::LFO2_PITCH_DEPTH,   SyxXform::Signed64ToUni    },

    // Pitch envelope
    { 0x1B, CC::PITCH_ENV_DEPTH,    SyxXform::Signed64Direct   },
    { 0x1C, CC::PITCH_ENV_ATTACK,   SyxXform::Direct           },
    { 0x1D, CC::PITCH_ENV_DECAY,    SyxXform::Direct           },

    // ── Oscillators ──────────────────────────────────────────────────────
    { 0x1E, CC::OSC1_WAVE,          SyxXform::Osc1WaveRemap    },
    { 0x1F, CC::OSC1_MIX,           SyxXform::OscCtrl1         }, // context: SuperSaw→detune
    { 0x20, CC::OSC1_MIX,           SyxXform::OscCtrl2         }, // context: SuperSaw→mix
    { 0x21, CC::OSC2_WAVE,          SyxXform::Osc2WaveRemap    },
    // 0x22 OSC2 Sync — no JT CC yet
    { 0x23, CC::OSC2_PITCH_OFFSET,  SyxXform::Osc2Range        },
    { 0x24, CC::OSC2_FINE_TUNE,     SyxXform::Osc2Fine         },
    { 0x25, CC::OSC2_MIX,           SyxXform::OscCtrl1         }, // context: same as OSC1
    { 0x26, CC::OSC2_MIX,           SyxXform::OscCtrl2         }, // context: same as OSC1

    // ── Filter ───────────────────────────────────────────────────────────
    { 0x27, CC::FILTER_MODE,        SyxXform::FilterTypeRemap  },
    // 0x28 Cutoff Slope — maps to filter engine topology, handled by loader
    { 0x29, CC::FILTER_CUTOFF,      SyxXform::Direct           },
    { 0x2A, CC::FILTER_RESONANCE,   SyxXform::Direct           },
    { 0x2B, CC::FILTER_KEY_TRACK,   SyxXform::Signed64Direct   },
    { 0x2C, CC::LFO1_FILTER_DEPTH,  SyxXform::Signed64ToUni    },
    { 0x2D, CC::LFO2_FILTER_DEPTH,  SyxXform::Signed64ToUni    },
    { 0x2E, CC::FILTER_ENV_AMOUNT,  SyxXform::Signed64Direct   },

    // ── Filter Envelope ──────────────────────────────────────────────────
    { 0x2F, CC::FILTER_ENV_ATTACK,  SyxXform::Direct           },
    { 0x30, CC::FILTER_ENV_DECAY,   SyxXform::Direct           },
    { 0x31, CC::FILTER_ENV_SUSTAIN, SyxXform::Direct           },
    { 0x32, CC::FILTER_ENV_RELEASE, SyxXform::Direct           },

    // ── Amp ──────────────────────────────────────────────────────────────
    { 0x33, CC::AMP_MOD_FIXED_LEVEL,SyxXform::Direct           },
    { 0x34, CC::LFO1_AMP_DEPTH,     SyxXform::Signed64ToUni    },
    { 0x35, CC::LFO2_AMP_DEPTH,     SyxXform::Signed64ToUni    },

    // ── Amp Envelope ─────────────────────────────────────────────────────
    { 0x36, CC::AMP_ATTACK,         SyxXform::Direct           },
    { 0x37, CC::AMP_DECAY,          SyxXform::Direct           },
    { 0x38, CC::AMP_SUSTAIN,        SyxXform::Direct           },
    { 0x39, CC::AMP_RELEASE,        SyxXform::Direct           },

    // ── Tone / Effects ───────────────────────────────────────────────────
    // 0x3A Pan Mode — no JT CC
    { 0x3B, CC::FX_BASS_GAIN,       SyxXform::Signed64Direct   },
    { 0x3C, CC::FX_TREBLE_GAIN,     SyxXform::Signed64Direct   },
    { 0x3D, CC::FX_MOD_EFFECT,      SyxXform::MfxTypeRemap     },
    { 0x3E, CC::FX_MOD_MIX,         SyxXform::Direct           },
    { 0x3F, CC::FX_JPFX_DELAY_EFFECT, SyxXform::DelayTypeRemap },
    { 0x40, CC::FX_JPFX_DELAY_TIME, SyxXform::Direct           },
    { 0x41, CC::FX_JPFX_DELAY_FEEDBACK, SyxXform::Direct       },
    { 0x42, CC::FX_JPFX_DELAY_MIX,  SyxXform::Direct           },

    // ── Performance ──────────────────────────────────────────────────────
    { 0x43, CC::PITCH_BEND_RANGE,   SyxXform::Direct           }, // up range only
    // 0x44 Bend Down — JT uses symmetric; skip
    { 0x45, CC::GLIDE_ENABLE,       SyxXform::BoolTo127        },
    { 0x46, CC::GLIDE_TIME,         SyxXform::Direct           },
    { 0x47, CC::POLY_MODE,          SyxXform::MonoToPolyMode   },
    // 0x48 Legato — no JT CC
    // 0x49 OSC Shift — no JT CC
};

static constexpr uint8_t kSlotCount = sizeof(kSlots) / sizeof(kSlots[0]);

// =============================================================================
// TRANSFORM FUNCTIONS
// =============================================================================

// Clamp a value to 0-127 (CC safe range).
inline constexpr uint8_t clamp7(int v) {
    return (uint8_t)(v < 0 ? 0 : (v > 127 ? 127 : v));
}

// Apply the basic transform for a slot.
// For context-dependent transforms (OscCtrl1/2), the caller must handle
// those separately using the waveform byte — this function returns the
// raw value unchanged for those types.
inline uint8_t applyTransform(SyxXform xf, uint8_t raw)
{
    switch (xf) {
        case SyxXform::Direct:
            return raw;

        case SyxXform::BoolTo127:
            return raw ? 127 : 0;

        case SyxXform::Signed64Direct:
            // Already in ±64 format (0=min, 64=centre, 127=max)
            return raw;

        case SyxXform::Signed64ToUni:
            // Convert ±64 bipolar to 0-127 unipolar.
            // JP: 64=zero, 0=-64, 127=+63
            // JT per-dest depth is unipolar 0-127 (0=none, 127=max)
            // Take absolute distance from centre, scale to 0-127.
            {
                int dist = (int)raw - 64;
                if (dist < 0) dist = -dist;
                return clamp7(dist * 2);
            }

        case SyxXform::Osc1WaveRemap:
            return (raw < kOsc1WaveMapSize) ? kOsc1WaveMap[raw] : 86; // default: BL_SAW

        case SyxXform::Osc2WaveRemap:
            return (raw < kOsc2WaveMapSize) ? kOsc2WaveMap[raw] : 86;

        case SyxXform::Lfo1WaveRemap:
            return (raw < 4) ? kLfo1WaveMap[raw] : 0;

        case SyxXform::FilterTypeRemap:
            return (raw < 3) ? kFilterTypeMap[raw] : 0; // default: LPF

        case SyxXform::MfxTypeRemap:
            return (raw < kMfxTypeMapSize) ? kMfxTypeMap[raw] : 0;

        case SyxXform::DelayTypeRemap:
            return (raw < kDelayTypeMapSize) ? kDelayTypeMap[raw] : 0;

        case SyxXform::Osc2Range:
            // JP: 0-50 where 25=centre (±25 semitones)
            // JT: 0-127 where 64=centre (±64)
            // Scale: (raw * 127) / 50, but preserve centre alignment
            return clamp7((int)(raw * 127 + 25) / 50);

        case SyxXform::Osc2Fine:
            // JP: 0-100 where 50=centre (±50 cents)
            // JT: 0-127 where 64=centre
            return clamp7((int)(raw * 127 + 50) / 100);

        case SyxXform::MonoToPolyMode:
            // JP: 0=poly, 1=mono
            // JT POLY_MODE: 0-42=POLY, 43-84=MONO, 85-127=UNISON
            return (raw == 0) ? 0 : 64;

        case SyxXform::OscCtrl1:
        case SyxXform::OscCtrl2:
        case SyxXform::Ignore:
            // Context-dependent or informational — return raw.
            // The loader handles routing to the correct CC.
            return raw;
    }
    return raw; // fallback
}

// =============================================================================
// CONTEXT-DEPENDENT OSC CONTROL DISPATCH
//
// JP-8000 OSC Control 1 and Control 2 change meaning based on waveform.
// These helpers tell the loader which CC to actually target.
//
// OSC1:
//   SuperSaw (0): Ctrl1 → SUPERSAW1_DETUNE, Ctrl2 → SUPERSAW1_MIX
//   TWM (1):      Ctrl1 → OSC1_MIX,         Ctrl2 → OSC1_FEEDBACK_AMOUNT
//   Feedback (2): Ctrl1 → OSC1_MIX,         Ctrl2 → OSC1_FEEDBACK_AMOUNT
//   Noise (3):    Ctrl1 → (ignored),         Ctrl2 → (ignored)
//   Pulse (4):    Ctrl1 → OSC1_MIX (PW),    Ctrl2 → (ignored)
//   Saw (5):      Ctrl1 → (ignored),         Ctrl2 → (ignored)
//   Triangle (6): Ctrl1 → OSC1_MIX (TW),    Ctrl2 → (ignored)
//
// Returns the CC number to use, or 0 if the control should be skipped.
// =============================================================================

// OSC1 Ctrl1 target CC based on OSC1 waveform
inline uint8_t osc1Ctrl1CC(uint8_t jpWave) {
    switch (jpWave) {
        case 0: return CC::SUPERSAW1_DETUNE;     // SuperSaw → detune
        case 1: return CC::OSC1_MIX;             // TWM → pulse width
        case 2: return CC::OSC1_MIX;             // Feedback → mix
        case 4: return CC::OSC1_MIX;             // Pulse → pulse width
        case 6: return CC::OSC1_MIX;             // Triangle → triangle mod
        default: return 0;                       // Noise, Saw → no Ctrl1 mapping
    }
}

// OSC1 Ctrl2 target CC based on OSC1 waveform
inline uint8_t osc1Ctrl2CC(uint8_t jpWave) {
    switch (jpWave) {
        case 0: return CC::SUPERSAW1_MIX;         // SuperSaw → mix
        case 1: return CC::OSC1_FEEDBACK_AMOUNT;   // TWM → feedback
        case 2: return CC::OSC1_FEEDBACK_AMOUNT;   // Feedback → feedback
        default: return 0;                         // Others → no Ctrl2 mapping
    }
}

// OSC2 Ctrl1 target CC based on OSC2 waveform
// JP OSC2: 0=Pulse, 1=Triangle, 2=Saw, 3=Noise
// OSC2 has no SuperSaw option, so Ctrl1 is always PW/mix or unused.
inline uint8_t osc2Ctrl1CC(uint8_t jpWave) {
    switch (jpWave) {
        case 0: return CC::OSC2_MIX;             // Pulse → pulse width
        case 1: return CC::OSC2_MIX;             // Triangle → triangle mod
        default: return 0;                       // Saw, Noise → no Ctrl1
    }
}

// OSC2 Ctrl2 target CC based on OSC2 waveform
inline uint8_t osc2Ctrl2CC(uint8_t jpWave) {
    // JP OSC2 has no feedback/supersaw, so Ctrl2 is unused for all waveforms.
    (void)jpWave;
    return 0;
}

// =============================================================================
// SAFE DEFAULTS — sent before any patch load to reset JT-exclusive params
//
// Parameters that exist in JT-8000 but not in JP-8000 patches need sensible
// defaults so they don't carry over from a previous patch.
// =============================================================================
struct DefaultCC {
    uint8_t cc;
    uint8_t val;
};

static constexpr DefaultCC kDefaults[] = {
    { CC::FILTER_ENGINE,             0   },  // OBXa engine (default for JP-8000 compat)
    { CC::VA_FILTER_TYPE,            0   },  // (irrelevant when engine=OBXa)
    { CC::FILTER_OBXA_MULTIMODE,     0   },  // No multimode blending
    { CC::FILTER_OBXA_XPANDER_MODE,  0   },  // Standard OBXa mode
    { CC::FILTER_OBXA_RES_MOD_DEPTH, 0   },  // No resonance modulation
    { CC::FILTER_OCTAVE_CONTROL,    64   },  // Default mod depth scaling
    { CC::FX_DRIVE,                  0   },  // No drive (unless JP patch has overdrive FX)
    { CC::FX_REVERB_BYPASS,        127   },  // Reverb off (JP-8000 has no reverb)
    { CC::FX_DRY_MIX,             127   },  // Full dry signal
    { CC::FX_JPFX_MIX,            127   },  // Full JPFX chain
    { CC::NOISE_MIX,                0   },  // No noise unless explicitly set
    { CC::SUB_MIX,                  0   },  // No sub unless explicitly set
    { CC::UNISON_DETUNE,           64   },  // Mid-range unison spread
    { CC::PITCH_ENV_SUSTAIN,        0   },  // JP has no pitch env sustain
    { CC::PITCH_ENV_RELEASE,        0   },  // JP has no pitch env release
    { CC::LFO1_PWM_DEPTH,           0   },  // Reset per-dest depths
    { CC::LFO2_PWM_DEPTH,           0   },
    { CC::LFO1_DEPTH,              64   },  // Neutral master depth
    { CC::LFO2_DEPTH,              64   },
    { CC::LFO2_DESTINATION,         0   },  // JP has no LFO2 dest in patch
    { CC::SEQ_ENABLE,                0   },  // Step sequencer off
};

static constexpr uint8_t kDefaultCount = sizeof(kDefaults) / sizeof(kDefaults[0]);

} // namespace JP8000SyxMap
