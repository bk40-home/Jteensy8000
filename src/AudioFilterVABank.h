#pragma once
// =============================================================================
// AudioFilterVABank.h  –  Switchable VA Filter Bank (Teensy AudioStream)
// =============================================================================
//
// Wraps every filter topology from VAFilterCore.h into a single Teensy-style
// AudioStream object. The active topology is selected at run-time via
// setFilterType() or a MIDI CC, allowing A/B comparison without rewiring the
// audio graph.
//
// Reference: Vadim Zavalishin, "The Art of VA Filter Design" rev 2.1.2 (2018)
//            Chapter 3 (TPT/ZDF), Chapter 4 (SVF), Chapter 5 (ladder/topology)
//
// ─── Two parameter APIs ─────────────────────────────────────────────────────
//   RAW (Hz / 0..1 post-shape):
//     frequency(hz)    – cutoff in Hz, clamped to [5, 0.45*fs]
//     resonance(r)     – 0..1, fed directly to mapResonance() (no γ, no slew)
//     setDrive(d)      – drive multiplier, written hard (no slew)
//   NORMALISED (0..1 user knob, FilterShape table applies the curves + slew):
//     setCutoffNorm(c01)    – 0..1, expanded via per-filter [fcMin,fcMax]
//     setResonanceNorm(r01) – 0..1, γ-shaped per filter then mapResonance()
//     setDriveNorm(d01)     – 0..1, scaled to ×1..4 and block-rate slewed
//
// The norm setters drive internal SlewedValue smoothers (block-rate). update()
// advances the slews each block; key tracking and env modulation then multiply
// onto the smoothed _fcTarget so they stay snappy. The raw setters bypass the
// slews and snap their targets — entry points for env / key-track / audio-rate
// callers that already produce continuous signals.
//
// ─── Supported filter types ─────────────────────────────────────────────────
//   FILTER_SVF_LP    – SVF 2-pole low-pass     (Zavalishin §4.1, p.95)
//   FILTER_SVF_HP    – SVF 2-pole high-pass
//   FILTER_SVF_BP    – SVF 2-pole band-pass
//   FILTER_SVF_NOTCH – SVF notch  (LP + HP)
//   FILTER_SVF_AP    – SVF all-pass
//   FILTER_MOOG_LP4  – Moog ladder 4-pole LP   (Zavalishin §5.1, p.133)
//   FILTER_MOOG_LP2  – Moog ladder 2-pole LP   (y2 tap)
//   FILTER_MOOG_BP2  – Moog ladder 2-pole BP   (y2 - y4)
//   FILTER_DIODE_LP  – Diode ladder 4-pole LP  (Pirkle AN-6)
//   FILTER_KORG35_LP – Korg35/TSK LP            (Zavalishin §5.8, p.151)
//   FILTER_KORG35_HP – Korg35/TSK HP            (Zavalishin §5.8, p.154)
//   FILTER_TPT1_LP   – Simple 1-pole TPT LP    (Zavalishin §3.1, p.45)
//   FILTER_TPT1_HP   – Simple 1-pole TPT HP
//
// ─── Signal routing (3 audio inputs, 1 output) ──────────────────────────────
//   Input 0  : audio signal
//   Input 1  : cutoff modulation bus  (-1..+1), scaled by setCutoffModOctaves()
//   Input 2  : resonance modulation bus (-1..+1), scaled by setResModDepth()
//
// ─── CPU optimisation notes ─────────────────────────────────────────────────
//   • g and R are computed ONCE per block (control rate), not per sample.
//   • The topology switch is hoisted out of the sample loop (taken once/block).
//   • Norm API adds ONE powf per CC update (control rate); zero per-sample cost.
//   • SlewedValue block tick: 1 mul/block while moving, free when settled.
//   • Drive/saturation is optional; bypass saves work when neutral.
//   • Diode ladder uses setCoeffs()/tick() split — block-rate fc avoids 4
//     divides/sample.
// =============================================================================

#include <Arduino.h>
#include "AudioStream.h"
#include "VAFilterCore.h"
#include "MoogDVCore.h"   // D'Angelo–Välimäki nonlinear Moog ladder (ICASSP'13)
#include "SlewedValue.h"

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
    // D'Angelo–Välimäki nonlinear Moog ladder (ICASSP'13). Physical self-osc at
    // k=4, non-iterative (fixed 5 tanh/sample). See MoogDVCore.h. Appended at
    // the end so all existing type indices (and saved patches) are unchanged.
    FILTER_MOOGDV_LP4 = 13,  // 24 dB/oct LP — authentic Moog lowpass
    FILTER_MOOGDV_LP2 = 14,  // 12 dB/oct LP (y2 tap)
    FILTER_MOOGDV_HP4 = 15,  // 24 dB/oct HP (binomial residual)
    FILTER_MOOGDV_BP  = 16,  // band-pass (pole difference)
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
    "TPT1 HP",
    "MoogDV LP4",
    "MoogDV LP2",
    "MoogDV HP4",
    "MoogDV BP"
};

// ---------------------------------------------------------------------------
// Output saturation modes (Zavalishin §6.1 p.173)
// ---------------------------------------------------------------------------
enum VASaturationType : uint8_t
{
    SAT_NONE  = 0,   // linear – no saturation
    SAT_FAST  = 1,   // Padé tanh approximation (< 0.5% error for |x|<2.5)
    SAT_TANH  = 2    // tanhf() – accurate, ~30 cycles on M7
};

// =============================================================================
// Slewing defaults (compile-time tunable). Time constants chosen by ear:
//   cutoff : 15 ms — musical sweep, no clicks, matches Roland/Korg feel
//   reso   :  8 ms — shorter so dramatic flicks still feel responsive
//   drive  :  5 ms — gain stages don't want long glides (feels laggy)
// =============================================================================
static constexpr float kCutoffSlewMs = 15.0f;
static constexpr float kResSlewMs    =  8.0f;
static constexpr float kDriveSlewMs  =  5.0f;

// ---------------------------------------------------------------------------
// AudioFilterVABank
// ---------------------------------------------------------------------------
class AudioFilterVABank : public AudioStream
{
public:
    // 3 inputs: audio, cutoff mod, resonance mod
    AudioFilterVABank();

    // ── Filter selection ────────────────────────────────────────────────────
    // Switch topology; resets DSP state to avoid clicks. The norm slews keep
    // their targets and current values; the new filter's shape row is expanded
    // immediately (applyShape) so getters return live numbers between blocks.
    void setFilterType(VAFilterType type);
    VAFilterType getFilterType() const { return _type; }
    const char* getFilterName() const  { return kVAFilterNames[_type]; }

    // ── Raw controls (Hz / 0..1 post-shape) — bypass slewing ────────────────
    // Entry points for audio-rate / env / key-track callers that already
    // produce continuous signals (no benefit from per-step smoothing).
    void frequency(float hz);
    void resonance(float r);
    void setResonanceRaw(float k) { _kTarget = va_clamp(k, 0.0f, 20.0f); }

    // ── Normalised "knob" controls (0..1, FilterShape + slew applied) ───────
    // The entry points for CC handlers and UI. Each setter updates the slew
    // target; update() advances the slew once per block and expands via the
    // FilterShape table. One slew-target write per CC; no per-sample cost.
    void  setCutoffNorm(float c01);
    void  setResonanceNorm(float r01);
    float getCutoffNorm()    const { return _cutoffSlew.target(); }
    float getResonanceNorm() const { return _resSlew.target(); }

    // Snap all slews to their current targets — call at boot or on patch change
    // so the configured starting values do not glide in audibly.
    void snap();

    // Tune the slew time constants at runtime if needed (defaults above).
    void setCutoffSlewMs(float ms)    { _cutoffSlew.setTimeMs(ms); }
    void setResonanceSlewMs(float ms) { _resSlew.setTimeMs(ms); }
    void setDriveSlewMs(float ms)     { _driveSlew.setTimeMs(ms); }

    // Read-back of the post-shape values (for diagnostics / probes that mirror
    // the actual DSP state without re-computing Hz/k).
    float getCutoffHz()  const { return _fcTarget; }
    float getResonance() const { return _res01; }   // post-γ for norm callers

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
    // RAW drive: written hard (no slew) — kept for compatibility / boot.
    void setDrive(float d)                 { _drive = va_clamp(d, 0.0f, 10.0f);
                                             _driveSlew.reset(_drive); }
    // NORM drive: 0..1 knob → ×1..4, block-rate slewed (click-free CC sweeps).
    void setDriveNorm(float d01);
    void setSaturation(VASaturationType s) { _satType  = s; }

    // MoogDV-only: passband compensation ("stays loud" at high resonance).
    // No-op for every other topology. Defaults on.
    void setMoogDVQComp(bool on) { _moogdvQComp = on; }

    // ── State ────────────────────────────────────────────────────────────────
    void reset();   // clear all filter states (call on topology switch or note-off)

    // Engine-skip gate (see JT8000_OptFlags.h OPT 6). When set inactive,
    // update() drains its inputs and returns before any DSP — provided
    // JT_OPT_FILTER_ENGINE_SKIP is enabled. Defaults active so a stand-alone
    // instance behaves normally.
    void setActive(bool a) { _active = a; }
    bool isActive() const  { return _active; }

    // AudioStream mandatory override
    virtual void update(void) override;

private:
    audio_block_t *_inQ[3];   // Teensy audio input queue

    // ── Active topology ──────────────────────────────────────────────────────
    VAFilterType _type = FILTER_SVF_LP;

    // Engine-skip gate (OPT 6). True = process normally; false = drain & skip.
    bool _active = true;

    // ── Control parameters (post-shape — what the DSP consumes each block) ───
    float _fcTarget     = 1000.0f;   // cutoff Hz
    float _kTarget      = 0.0f;      // raw resonance (topology-dependent scale)
    float _res01        = 0.0f;      // 0..1 fed to mapResonance() (post-γ for
                                     // norm callers; == user input for raw)

    // Pre-shape user-knob smoothers. target() is the latest CC value; current()
    // is what the DSP uses this block. Defaults configured in the constructor.
    SlewedValue _cutoffSlew;
    SlewedValue _resSlew;
    SlewedValue _driveSlew;          // smooths the ×1..4 drive multiplier

    // Norm API touched at least once → update() re-expands _fcTarget/_kTarget
    // from the slews each block. Stays false for raw-only callers so they keep
    // snap-behaviour.
    bool _normPrimed = false;

    float _cutoffModOct = 0.0f;
    float _resModDepth  = 0.0f;
    float _keyTrack     = 0.0f;
    float _midiNote     = 60.0f;
    float _envModOct    = 0.0f;
    float _envValue     = 0.0f;
    float _drive        = 1.0f;      // current drive multiplier (mirrors slew)

    VASaturationType _satType = SAT_TANH;

    // ── Filter state structs ─────────────────────────────────────────────────
    // All topologies pre-allocated; only the active one runs per block.
    TPT1         _tpt1;
    SVF2         _svf;
    MoogLinear4  _moog;
    DiodeLadder4 _diode;
    Korg35LP     _k35lp;
    Korg35HP     _k35hp;
    MoogDV4      _moogdv;  // D'Angelo–Välimäki authentic Moog ladder (ICASSP'13)

    // MoogDV "stays loud" passband compensation (input *(1+k)). On by default,
    // matching the host-validated test-rig behaviour. Self-contained to MoogDV;
    // no other topology reads it.
    bool _moogdvQComp = true;

    // ── Internal helpers ─────────────────────────────────────────────────────

    // Convert normalised resonance [0..1] to topology-appropriate k/R value.
    float mapResonance(float res01, VAFilterType type) const;

    // Expand normalised (cn, rn) under the active filter's FilterShape row into
    // _fcTarget, _res01, _kTarget. Called from update() each block and from
    // setFilterType()/snap() so getters return sane numbers between blocks.
    void applyShape(float cn, float rn);

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
