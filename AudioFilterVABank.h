#pragma once
// =============================================================================
// AudioFilterVABank.h  –  Switchable VA Filter Bank (Teensy AudioStream)
// =============================================================================
//
// Wraps every filter topology from VAFilterCore.h into a single Teensy-style
// AudioStream object.  The active topology is selected at run-time via
// setFilterType() or a MIDI CC, allowing A/B comparison without rewiring
// the audio graph.
//
// Reference: Vadim Zavalishin, "The Art of VA Filter Design" rev 2.1.2 (2018)
//            Chapter 3 (TPT/ZDF), Chapter 4 (ladder/topology variants)
//
// ─── Supported filter types ────────────────────────────────────────────────
//   FILTER_SVF_LP    – SVF 2-pole low-pass     (Zavalishin §3.9, p.77)
//   FILTER_SVF_HP    – SVF 2-pole high-pass
//   FILTER_SVF_BP    – SVF 2-pole band-pass
//   FILTER_SVF_NOTCH – SVF notch  (LP + HP)
//   FILTER_SVF_AP    – SVF all-pass
//   FILTER_MOOG_LP4  – Moog ladder 4-pole LP   (Zavalishin §4.1, p.99)
//   FILTER_MOOG_LP2  – Moog ladder 2-pole LP   (y2 tap)
//   FILTER_MOOG_BP2  – Moog ladder 2-pole BP   (y2 - y4)
//   FILTER_DIODE_LP  – Roland/Diode 4-pole LP  (Zavalishin §4.3, p.107)
//   FILTER_KORG35_LP – Korg MS-20 LP            (Zavalishin §4.5.1, p.114)
//   FILTER_KORG35_HP – Korg MS-20 HP
//   FILTER_TPT1_LP   – Simple 1-pole TPT LP    (Zavalishin §3.1, p.45)
//   FILTER_TPT1_HP   – Simple 1-pole TPT HP
//
// ─── Signal routing (3 audio inputs, 1 output) ─────────────────────────────
//   Input 0  : audio signal
//   Input 1  : cutoff modulation bus  (-1..+1), scaled by setCutoffModOctaves()
//   Input 2  : resonance modulation bus (-1..+1), scaled by setResModDepth()
//
// ─── CPU optimisation notes ────────────────────────────────────────────────
//   • g and R are computed ONCE per block (control rate), not per sample.
//   • All inner-loop ops are inline-expanded from VAFilterCore.h.
//   • tanf() is the most expensive operation (~20 cycles on M7).
//   • Drive/saturation is optional (setDrive()); bypass saves 2 tanhf/sample.
//   • The Moog LP4 closed-form solve avoids iteration entirely.
// =============================================================================

#include <Arduino.h>
#include "AudioStream.h"
#include "VAFilterCore.h"

// ---------------------------------------------------------------------------
// Filter type enumeration – add new types here and implement in .cpp
// ---------------------------------------------------------------------------
enum VAFilterType : uint8_t
{
    FILTER_SVF_LP    = 0,
    FILTER_SVF_HP    = 1,
    FILTER_SVF_BP    = 2,
    FILTER_SVF_NOTCH = 3,
    FILTER_SVF_AP    = 4,
    FILTER_MOOG_LP4  = 5,
    FILTER_MOOG_LP2  = 6,
    FILTER_MOOG_BP2  = 7,
    FILTER_DIODE_LP  = 8,
    FILTER_KORG35_LP = 9,
    FILTER_KORG35_HP = 10,
    FILTER_TPT1_LP   = 11,
    FILTER_TPT1_HP   = 12,
    FILTER_COUNT           // keep last – used for bounds checking
};

// Human-readable names (useful for display / UI)
static const char* const kVAFilterNames[FILTER_COUNT] = {
    "SVF LP2",
    "SVF HP2",
    "SVF BP2",
    "SVF NOTCH",
    "SVF AP",
    "Moog LP4",
    "Moog LP2",
    "Moog BP2",
    "Diode LP4",
    "Korg35 LP",
    "Korg35 HP",
    "TPT1 LP",
    "TPT1 HP"
};

// ---------------------------------------------------------------------------
// Output saturation modes (Zavalishin §4.5 p.113)
// ---------------------------------------------------------------------------
enum VASaturationType : uint8_t
{
    SAT_NONE  = 0,   // linear – no saturation
    SAT_FAST  = 1,   // Padé tanh approximation (< 0.5% error for |x|<2.5)
    SAT_TANH  = 2    // tanhf() – accurate, ~30 cycles on M7
};

// ---------------------------------------------------------------------------
// AudioFilterVABank
// ---------------------------------------------------------------------------
class AudioFilterVABank : public AudioStream
{
public:
    // 3 inputs: audio, cutoff mod, resonance mod
    AudioFilterVABank();

    // ── Filter selection ────────────────────────────────────────────────────
    // Switch topology; resets state to avoid clicks.
    void setFilterType(VAFilterType type);
    VAFilterType getFilterType() const { return _type; }
    const char* getFilterName() const  { return kVAFilterNames[_type]; }

    // ── Core controls (Teensy-style naming matches OBXa interface) ──────────
    // Cutoff frequency in Hz.  Clamped to [5, 0.45*fs].
    void frequency(float hz);

    // Resonance 0..1 normalised for all types (mapped internally to k/R/Q).
    void resonance(float r);
    void setResonanceRaw(float k) { _kTarget = va_clamp(k, 0.0f, 20.0f); }

    // ── Modulation (matches OBXa method names for drop-in compatibility) ────
    void setCutoffModOctaves(float oct)    { _cutoffModOct  = va_clamp(oct, 0.0f, 8.0f); }
    void setResModDepth(float d)           { _resModDepth   = va_clamp(d,   0.0f, 20.0f); }
    // Alias to match OBXa's setResonanceModDepth() name
    void setResonanceModDepth(float d)     { setResModDepth(d); }

    void setKeyTrack(float amt)            { _keyTrack  = va_clamp(amt,  0.0f, 1.0f); }
    void setMidiNote(float note)           { _midiNote  = va_clamp(note, 0.0f, 127.0f); }
    void setEnvModOctaves(float oct)       { _envModOct = va_clamp(oct,  0.0f, 8.0f); }
    void setEnvValue(float env01)          { _envValue  = va_clamp(env01, 0.0f, 1.0f); }

    // ── Drive / saturation ──────────────────────────────────────────────────
    void setDrive(float d)                 { _drive    = va_clamp(d, 0.0f, 10.0f); }
    void setSaturation(VASaturationType s) { _satType  = s; }

    // ── State ────────────────────────────────────────────────────────────────
    void reset();   // clear all filter states (call on topology switch or note-off)

    // AudioStream mandatory override
    virtual void update(void) override;

private:
    audio_block_t *_inQ[3];   // Teensy audio input queue

    // ── Active topology ──────────────────────────────────────────────────────
    VAFilterType _type = FILTER_SVF_LP;

    // ── Control parameters ───────────────────────────────────────────────────
    float _fcTarget     = 1000.0f;   // cutoff Hz
    float _kTarget      = 0.0f;      // raw resonance (topology-dependent scale)
    float _res01        = 0.0f;      // normalised resonance input [0..1]

    float _cutoffModOct = 0.0f;
    float _resModDepth  = 0.0f;
    float _keyTrack     = 0.0f;
    float _midiNote     = 60.0f;
    float _envModOct    = 0.0f;
    float _envValue     = 0.0f;
    float _drive        = 1.0f;

    VASaturationType _satType = SAT_TANH;

    // ── Filter state structs ─────────────────────────────────────────────────
    // All topologies pre-allocated; only the active one runs per block.
    TPT1         _tpt1;
    SVF2         _svf;
    MoogLinear4  _moog;
    DiodeLadder4 _diode;
    Korg35LP     _k35lp;
    Korg35HP     _k35hp;

    // ── Internal helpers ─────────────────────────────────────────────────────

    // Convert normalised resonance [0..1] to topology-appropriate k/R value.
    float mapResonance(float res01, VAFilterType type) const;

    // Saturate output sample according to _satType
    inline float saturate(float x) const
    {
        switch (_satType)
        {
            case SAT_FAST: return va_tanh_fast(x);
            case SAT_TANH: return va_tanh(x);
            default:       return x;
        }
    }
};
