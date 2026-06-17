// =============================================================================
// ParamMap.cpp — JT-8000 ParamID routing table (data + lookup)
//
// One row per ParamID, sorted by ParamID for binary search.
// Source of truth: JT8000_ParamMap_Phase0_v2.2.md
//
// To add a param: insert in ParamID-sorted order; the assertion in find()
// will catch any out-of-order rows in debug builds.
//
// © 2025 Kris Bishop — MIT licensed.
// =============================================================================

#include "ParamMap.h"
#include "ParamDefs.h"

namespace ParamMap {

// =============================================================================
// The table — exactly 126 entries (90 Patch + 3 Patch-NS + 17 Patch-SX + 7 Perf + 9 GlobalFx).
//
// (Previous: 117. Added: 9 SysEx-only envelope curve entries.)
//
// Three macro helpers for compactness — they expand to single-row Entry literals.
// Using macros keeps the table dense and easy to read; expanding inline would
// triple its length and obscure the data.
// =============================================================================

#define ENT_PATCH(  pid, cc, vt) { (pid), (cc),                       kPatch,     vt }
#define ENT_PERF(   pid, cc, vt) { (pid), (cc),                       kPerf,      vt }
#define ENT_GFX(    pid, cc, vt) { (pid), (cc),                       kGlobalFx,  vt }
#define ENT_SYX(    pid,     vt) { (pid), kNoCC,                      kSysExOnly, vt }

static const Entry kTable[] = {
    // -----------------------------------------------------------------------------
    // 0x01XX — Oscillator: waveforms, pitch, mix
    // -----------------------------------------------------------------------------
    ENT_PATCH(0x0100, CC::OSC1_WAVE,            kEnumN),
    ENT_PATCH(0x0101, CC::OSC2_WAVE,            kEnumN),
    ENT_PATCH(0x0110, CC::OSC1_PITCH_OFFSET,    kEnumN),    // 5 discrete steps
    ENT_PATCH(0x0111, CC::OSC2_PITCH_OFFSET,    kEnumN),
    ENT_PATCH(0x0112, CC::OSC1_DETUNE,          kBipolar),  // -12..+12 semitones
    ENT_PATCH(0x0113, CC::OSC2_DETUNE,          kBipolar),
    ENT_PATCH(0x0114, CC::OSC1_FINE_TUNE,       kBipolar),  // -100..+100 cents
    ENT_PATCH(0x0115, CC::OSC2_FINE_TUNE,       kBipolar),
    ENT_PATCH(0x0120, CC::OSC_MIX_BALANCE,      kNorm01),
    ENT_PATCH(0x0121, CC::OSC1_MIX,             kNorm01),
    ENT_PATCH(0x0122, CC::OSC2_MIX,             kNorm01),
    ENT_PATCH(0x0130, CC::SUPERSAW1_DETUNE,     kNorm01),
    ENT_PATCH(0x0131, CC::SUPERSAW1_MIX,        kNorm01),
    ENT_PATCH(0x0132, CC::SUPERSAW2_DETUNE,     kNorm01),
    ENT_PATCH(0x0133, CC::SUPERSAW2_MIX,        kNorm01),
    ENT_PATCH(0x0140, CC::OSC1_ARB_BANK,        kEnumN),
    ENT_PATCH(0x0141, CC::OSC2_ARB_BANK,        kEnumN),
    ENT_PATCH(0x0142, CC::OSC1_ARB_INDEX,       kInt7),
    ENT_PATCH(0x0143, CC::OSC2_ARB_INDEX,       kInt7),

    // -----------------------------------------------------------------------------
    // 0x02XX — Sub / Noise / Ring / DC
    // -----------------------------------------------------------------------------
    ENT_PATCH(0x0200, CC::SUB_MIX,              kNorm01),
    ENT_PATCH(0x0201, CC::NOISE_MIX,            kNorm01),
    ENT_PATCH(0x0210, CC::RING1_MIX,            kNorm01),
    ENT_PATCH(0x0211, CC::RING2_MIX,            kNorm01),
    ENT_PATCH(0x0220, CC::OSC1_FREQ_DC,         kCont),     // 0..+24 semis (unipolar)
    ENT_PATCH(0x0221, CC::OSC2_FREQ_DC,         kCont),
    ENT_PATCH(0x0222, CC::OSC1_SHAPE_DC,        kNorm01),
    ENT_PATCH(0x0223, CC::OSC2_SHAPE_DC,        kNorm01),

    // -----------------------------------------------------------------------------
    // 0x03XX — Cross-mod / Sync / Feedback
    // 0x0300, 0x0301 are Patch-NS (not in kPatchableCCs[]) but the CC dispatch
    // path is identical — scope=kPatch routes correctly via setCCExplicit.
    // -----------------------------------------------------------------------------
    ENT_PATCH(0x0300, CC::OSC_CROSS_MOD_DEPTH,  kNorm01),
    ENT_PATCH(0x0301, CC::OSC_SYNC_ENABLE,      kToggle),
    ENT_PATCH(0x0310, CC::OSC1_FEEDBACK_AMOUNT, kNorm01),
    ENT_PATCH(0x0311, CC::OSC2_FEEDBACK_AMOUNT, kNorm01),
    ENT_PATCH(0x0312, CC::OSC1_FEEDBACK_MIX,    kNorm01),
    ENT_PATCH(0x0313, CC::OSC2_FEEDBACK_MIX,    kNorm01),

    // -----------------------------------------------------------------------------
    // 0x04XX — Filter
    // -----------------------------------------------------------------------------
    ENT_PATCH(0x0400, CC::FILTER_CUTOFF,        kCont),
    ENT_PATCH(0x0401, CC::FILTER_RESONANCE,     kCont),     // engine-aware curve in firmware
    ENT_PATCH(0x0402, CC::FILTER_ENV_AMOUNT,    kBipolar),
    ENT_PATCH(0x0403, CC::FILTER_KEY_TRACK,     kBipolar),
    ENT_PATCH(0x0404, CC::FILTER_OCTAVE_CONTROL,   kCont),     // 0..10 unipolar
    ENT_PATCH(0x0410, CC::FILTER_ENGINE,        kEnumN),    // 2-state OBXa/VA
    ENT_PATCH(0x0411, CC::FILTER_MODE,          kEnumN),    // engine-context: OBXa Mode / VA Type
    // 0x0412 RETIRED: VA_FILTER_TYPE folded into 0x0411 (FILTER_MODE). The CC
    // dispatch reads the active engine and routes Type accordingly. Leaving the
    // address unmapped means a stale 0x0412 SysEx is ignored rather than routed
    // to a dead CC. Re-use 0x0412 only after the JUCE side drops it too.
    ENT_PATCH(0x0413, CC::FILTER_OBXA_XPANDER_MODE,    kEnumN),  // engine-context: Xpander / VA Drive
    ENT_PATCH(0x0414, CC::FILTER_OBXA_MULTIMODE,       kNorm01), // engine-context: Multimode / VA Sat
    ENT_PATCH(0x0415, CC::FILTER_OBXA_RES_MOD_DEPTH,   kNorm01),

    // -----------------------------------------------------------------------------
    // 0x05XX — Amp envelope
    // -----------------------------------------------------------------------------
    ENT_PATCH(0x0500, CC::AMP_ATTACK,       kCont),     // log ms via cc_to_time_ms
    ENT_PATCH(0x0501, CC::AMP_DECAY,        kCont),
    ENT_PATCH(0x0502, CC::AMP_SUSTAIN,      kNorm01),
    ENT_PATCH(0x0503, CC::AMP_RELEASE,      kCont),
    // Curve exponents — SysEx-only (no CC alias). Native float 0.2..5.0.
    ENT_SYX(SysExOnlyIds::kAmpAttackCurve,    kSysEx),
    ENT_SYX(SysExOnlyIds::kAmpDecayCurve,     kSysEx),
    ENT_SYX(SysExOnlyIds::kAmpReleaseCurve,   kSysEx),

    // -----------------------------------------------------------------------------
    // 0x06XX — Filter envelope
    // -----------------------------------------------------------------------------
    ENT_PATCH(0x0600, CC::FILTER_ENV_ATTACK,    kCont),
    ENT_PATCH(0x0601, CC::FILTER_ENV_DECAY,     kCont),
    ENT_PATCH(0x0602, CC::FILTER_ENV_SUSTAIN,   kNorm01),
    ENT_PATCH(0x0603, CC::FILTER_ENV_RELEASE,   kCont),
    // Curve exponents — SysEx-only (no CC alias). Native float 0.2..5.0.
    ENT_SYX(SysExOnlyIds::kFilterAttackCurve,  kSysEx),
    ENT_SYX(SysExOnlyIds::kFilterDecayCurve,   kSysEx),
    ENT_SYX(SysExOnlyIds::kFilterReleaseCurve, kSysEx),

    // -----------------------------------------------------------------------------
    // 0x07XX — Pitch envelope
    // -----------------------------------------------------------------------------
    ENT_PATCH(0x0700, CC::PITCH_ENV_ATTACK,     kCont),
    ENT_PATCH(0x0701, CC::PITCH_ENV_DECAY,      kCont),
    ENT_PATCH(0x0702, CC::PITCH_ENV_SUSTAIN,    kNorm01),
    ENT_PATCH(0x0703, CC::PITCH_ENV_RELEASE,    kCont),
    ENT_PATCH(0x0704, CC::PITCH_ENV_DEPTH,      kBipolar),  // -24..+24 semis post-Q1 fix
    // Curve exponents — SysEx-only (no CC alias). Native float 0.2..5.0.
    ENT_SYX(SysExOnlyIds::kPitchAttackCurve,   kSysEx),
    ENT_SYX(SysExOnlyIds::kPitchDecayCurve,    kSysEx),
    ENT_SYX(SysExOnlyIds::kPitchReleaseCurve,  kSysEx),

    // -----------------------------------------------------------------------------
    // 0x08XX — LFO 1
    // -----------------------------------------------------------------------------
    ENT_PATCH(0x0800, CC::LFO1_FREQ,            kCont),     // exp Hz
    ENT_PATCH(0x0801, CC::LFO1_DEPTH,    kNorm01),
    ENT_PATCH(0x0802, CC::LFO1_DESTINATION,            kEnumN),
    ENT_PATCH(0x0803, CC::LFO1_WAVEFORM,        kEnumN),
    ENT_PATCH(0x0810, CC::LFO1_PITCH_DEPTH,     kNorm01),
    ENT_PATCH(0x0811, CC::LFO1_FILTER_DEPTH,    kNorm01),
    ENT_PATCH(0x0812, CC::LFO1_PWM_DEPTH,       kNorm01),
    ENT_PATCH(0x0813, CC::LFO1_AMP_DEPTH,       kNorm01),
    ENT_PATCH(0x0820, CC::LFO1_DELAY,           kCont),     // 0..4000 ms linear

    // -----------------------------------------------------------------------------
    // 0x09XX — LFO 2
    // -----------------------------------------------------------------------------
    ENT_PATCH(0x0900, CC::LFO2_FREQ,            kCont),
    ENT_PATCH(0x0901, CC::LFO2_DEPTH,    kNorm01),
    ENT_PATCH(0x0902, CC::LFO2_DESTINATION,            kEnumN),
    ENT_PATCH(0x0903, CC::LFO2_WAVEFORM,        kEnumN),
    ENT_PATCH(0x0910, CC::LFO2_PITCH_DEPTH,     kNorm01),
    ENT_PATCH(0x0911, CC::LFO2_FILTER_DEPTH,    kNorm01),
    ENT_PATCH(0x0912, CC::LFO2_PWM_DEPTH,       kNorm01),
    ENT_PATCH(0x0913, CC::LFO2_AMP_DEPTH,       kNorm01),
    ENT_PATCH(0x0920, CC::LFO2_DELAY,           kCont),

    // -----------------------------------------------------------------------------
    // 0x0AXX — Velocity sensitivity
    // -----------------------------------------------------------------------------
    ENT_PATCH(0x0A00, CC::VELOCITY_AMP_SENS,         kNorm01),
    ENT_PATCH(0x0A01, CC::VELOCITY_FILTER_SENS,      kNorm01),
    ENT_PATCH(0x0A02, CC::VELOCITY_ENV_SENS,         kNorm01),

    // -----------------------------------------------------------------------------
    // 0x0BXX — Glide / Voice / Pitch bend
    // -----------------------------------------------------------------------------
    ENT_PATCH(0x0B00, CC::GLIDE_ENABLE,         kToggle),
    ENT_PATCH(0x0B01, CC::GLIDE_TIME,           kCont),     // log ms post-Q3 fix
    ENT_PATCH(0x0B10, CC::POLY_MODE,            kEnumN),
    ENT_PATCH(0x0B11, CC::UNISON_DETUNE,        kNorm01),
    ENT_PATCH(0x0B20, CC::PITCH_BEND_RANGE,     kCont),     // 0..24 semis
    ENT_PATCH(0x0B30, CC::AMP_MOD_FIXED_LEVEL,  kNorm01),

    // -----------------------------------------------------------------------------
    // 0x0CXX — FX (Patch-scope)
    // -----------------------------------------------------------------------------
    ENT_PATCH(0x0C00, CC::FX_DRIVE,             kNorm01),   // norm at this layer
    ENT_PATCH(0x0C10, CC::FX_BASS_GAIN,         kBipolar),  // -12..+12 dB (no DZ in firmware — see Appendix A)
    ENT_PATCH(0x0C11, CC::FX_TREBLE_GAIN,       kBipolar),
    ENT_PATCH(0x0C20, CC::FX_MOD_EFFECT,        kSignedOff),// -1..10 (CC 0 = OFF)
    ENT_PATCH(0x0C21, CC::FX_MOD_MIX,           kNorm01),
    ENT_PATCH(0x0C22, CC::FX_MOD_RATE,          kCont),     // 0..20 Hz linear
    ENT_PATCH(0x0C23, CC::FX_MOD_FEEDBACK,      kSignedOff),// -1..0.99 (CC 0 = "use preset")
    ENT_PATCH(0x0C30, CC::FX_JPFX_DELAY_EFFECT,      kSignedOff),
    ENT_PATCH(0x0C31, CC::FX_JPFX_DELAY_MIX,    kNorm01),
    ENT_PATCH(0x0C32, CC::FX_JPFX_DELAY_FEEDBACK, kSignedOff),
    ENT_PATCH(0x0C33, CC::FX_JPFX_DELAY_TIME,   kCont),     // 0..1500 ms linear
    ENT_PATCH(0x0C40, CC::FX_DRY_MIX,           kNorm01),
    ENT_PATCH(0x0C41, CC::FX_JPFX_MIX,          kNorm01),

    // -----------------------------------------------------------------------------
    // 0x0C8X — SysEx-only abstractions (Q5/Q6 split toggles, Option D).
    // No CC alias. Adapter handles entirely; firmware state unchanged.
    // -----------------------------------------------------------------------------
    ENT_SYX(SysExOnlyIds::kFxModEnabled,      kToggle),
    ENT_SYX(SysExOnlyIds::kFxModVariation,    kEnumN),
    ENT_SYX(SysExOnlyIds::kFxModFbOverride,   kToggle),
    ENT_SYX(SysExOnlyIds::kFxModFbValue,      kNorm01),
    ENT_SYX(SysExOnlyIds::kFxDelayEnabled,    kToggle),
    ENT_SYX(SysExOnlyIds::kFxDelayVariation,  kEnumN),
    ENT_SYX(SysExOnlyIds::kFxDelayFbOverride, kToggle),
    ENT_SYX(SysExOnlyIds::kFxDelayFbValue,    kNorm01),

    // -----------------------------------------------------------------------------
    // 0x0DXX — BPM / Clock / Timing
    // -----------------------------------------------------------------------------
    ENT_PATCH(0x0D00, CC::BPM_CLOCK_SOURCE,     kEnumN),    // 2-state
    ENT_PATCH(0x0D01, CC::BPM_INTERNAL_TEMPO,   kCont),     // 40..300 BPM linear
    ENT_PATCH(0x0D10, CC::LFO1_TIMING_MODE,     kEnumN),
    ENT_PATCH(0x0D11, CC::LFO2_TIMING_MODE,     kEnumN),
    ENT_PATCH(0x0D12, CC::DELAY_TIMING_MODE,    kEnumN),

    // -----------------------------------------------------------------------------
    // 0x0EXX — Step Sequencer
    // -----------------------------------------------------------------------------
    ENT_PATCH(0x0E00, CC::SEQ_ENABLE,           kToggle),
    ENT_PATCH(0x0E01, CC::SEQ_STEPS,            kEnumN),    // 1..16
    ENT_PATCH(0x0E02, CC::SEQ_GATE_LENGTH,      kNorm01),
    ENT_PATCH(0x0E03, CC::SEQ_SLIDE,            kNorm01),
    ENT_PATCH(0x0E04, CC::SEQ_DIRECTION,        kEnumN),
    ENT_PATCH(0x0E05, CC::SEQ_RATE,             kCont),     // exp Hz
    ENT_PATCH(0x0E06, CC::SEQ_DEPTH,            kBipolar),
    ENT_PATCH(0x0E07, CC::SEQ_DESTINATION,      kEnumN),
    ENT_PATCH(0x0E08, CC::SEQ_RETRIGGER,        kToggle),
    ENT_PATCH(0x0E10, CC::SEQ_STEP_SELECT,      kInt7),     // Patch-NS (transient)
    ENT_PATCH(0x0E11, CC::SEQ_STEP_VALUE,       kInt7),     // raw byte stored
    ENT_PATCH(0x0E20, CC::SEQ_TIMING_MODE,      kEnumN),

    // -----------------------------------------------------------------------------
    // 0x10XX — Performance (LayerManager-owned)
    // -----------------------------------------------------------------------------
    ENT_PERF(0x1000, CC::PERF_MODE,             kEnumN),
    ENT_PERF(0x1001, CC::PERF_VOICE_SPLIT,      kInt7),     // 1..7 raw
    ENT_PERF(0x1002, CC::PERF_SPLIT_NOTE,       kInt7),
    ENT_PERF(0x1003, CC::PERF_BALANCE,          kBipolar),
    ENT_PERF(0x1004, CC::PERF_EDIT_TARGET,      kEnumN),
    ENT_PERF(0x1005, CC::PERF_MIDI_CHANNEL_A,   kInt7),
    ENT_PERF(0x1006, CC::PERF_MIDI_CHANNEL_B,   kInt7),

    // -----------------------------------------------------------------------------
    // 0x11XX — Global FX (shared reverb)
    // -----------------------------------------------------------------------------
    ENT_GFX(0x1100, CC::FX_REVERB_SIZE,         kNorm01),
    ENT_GFX(0x1101, CC::FX_REVERB_DAMP,   kNorm01),
    ENT_GFX(0x1102, CC::FX_REVERB_LODAMP,   kNorm01),
    ENT_GFX(0x1103, CC::FX_REVERB_MIX,          kNorm01),
    ENT_GFX(0x1104, CC::FX_REVERB_BYPASS,       kToggle),
    ENT_GFX(0x1105, CC::FX_REVERB_SHIMMER,      kNorm01),
    ENT_GFX(0x1106, CC::FX_REVERB_FREEZE,       kToggle),
    ENT_GFX(0x1107, CC::FX_REVERB_LOWPASS,      kNorm01),
    ENT_GFX(0x1108, CC::FX_REVERB_HIPASS,       kNorm01),
};

#undef ENT_PATCH
#undef ENT_PERF
#undef ENT_GFX
#undef ENT_SYX

static constexpr size_t kTableSize = sizeof(kTable) / sizeof(kTable[0]);

// =============================================================================
// Lookup — binary search by paramId. Table is kept sorted by hand; the order
// mirrors the Phase 0 markdown so reading the two side-by-side is sane.
// =============================================================================
const Entry* find(uint16_t paramId) {
    // Binary search — table is sorted by paramId.
    size_t lo = 0;
    size_t hi = kTableSize;
    while (lo < hi) {
        const size_t mid = (lo + hi) >> 1;
        const uint16_t midId = kTable[mid].paramId;
        if (midId == paramId) return &kTable[mid];
        if (midId <  paramId) lo = mid + 1;
        else                  hi = mid;
    }
    return nullptr;
}

size_t entryCount() { return kTableSize; }

// Reverse lookup by CC alias — O(N) linear scan, fine at UI rate.
const Entry* findByCC(uint8_t cc) {
    if (cc == kNoCC) return nullptr;  // caller asked for a SysEx-only param
    for (size_t i = 0; i < kTableSize; ++i) {
        if (kTable[i].ccAlias == cc) return &kTable[i];
    }
    return nullptr;
}

// Index-based accessor — bounds-checked.
const Entry* entryAt(size_t i) {
    if (i >= kTableSize) return nullptr;
    return &kTable[i];
}

} // namespace ParamMap
