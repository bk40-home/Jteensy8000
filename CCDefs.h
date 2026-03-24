/*
 * CCDefs.h - FULLY CORRECTED VERSION WITH BPM TIMING
 *
 * Key fixes:
 * 1. Added FX_DRY_MIX, FX_REVERB_MIX, FX_JPFX_MIX (were commented out!)
 * 2. Fixed CC number conflicts (77 was used for both SUPERSAW1_DETUNE and FX_JPFX_DIRECT_MIX)
 * 3. Added FX_REVERB_BYPASS for smart bypass control
 * 4. Reorganized FX routing controls to avoid conflicts
 * 5. Added BPM timing CCs (118-122)
 * 6. Complete CC name() function with all mappings
 */

#pragma once

#include <Arduino.h>

namespace CC {

    // -------------------------------------------------------------------------
    // Oscillator waveforms
    // -------------------------------------------------------------------------
    static constexpr uint8_t OSC1_WAVE        = 21;
    static constexpr uint8_t OSC2_WAVE        = 22;

    // -------------------------------------------------------------------------
    // Filter main controls
    // -------------------------------------------------------------------------
    static constexpr uint8_t FILTER_CUTOFF    = 23;
    static constexpr uint8_t FILTER_RESONANCE = 24;

    // -------------------------------------------------------------------------
    // Amplifier envelope (VCA)
    // -------------------------------------------------------------------------
    static constexpr uint8_t AMP_ATTACK  = 25;
    static constexpr uint8_t AMP_DECAY   = 26;
    static constexpr uint8_t AMP_SUSTAIN = 27;
    static constexpr uint8_t AMP_RELEASE = 28;

    // -------------------------------------------------------------------------
    // Filter envelope (VCF)
    // -------------------------------------------------------------------------
    static constexpr uint8_t FILTER_ENV_ATTACK  = 29;
    static constexpr uint8_t FILTER_ENV_DECAY   = 30;
    static constexpr uint8_t FILTER_ENV_SUSTAIN = 31;
    static constexpr uint8_t FILTER_ENV_RELEASE = 32;

    // -------------------------------------------------------------------------
    // Oscillator pitch/coarse/fine/detune
    // -------------------------------------------------------------------------
    static constexpr uint8_t OSC1_PITCH_OFFSET = 41;
    static constexpr uint8_t OSC2_PITCH_OFFSET = 42;
    static constexpr uint8_t OSC1_DETUNE       = 43;
    static constexpr uint8_t OSC2_DETUNE       = 44;
    static constexpr uint8_t OSC1_FINE_TUNE    = 45;
    static constexpr uint8_t OSC2_FINE_TUNE    = 46;
    static constexpr uint8_t OSC1_FEEDBACK_AMOUNT = 123;    
    static constexpr uint8_t OSC2_FEEDBACK_AMOUNT = 124;

    static constexpr uint8_t OSC1_FEEDBACK_MIX = 125;    
    static constexpr uint8_t OSC2_FEEDBACK_MIX = 126;
    
    static constexpr uint8_t OSC_CROSS_MOD_DEPTH = 19;  // 0–127 → cross-mod depth
    static constexpr uint8_t OSC_SYNC_ENABLE     = 20;  // 0 = off, ≥1 = on


    // -------------------------------------------------------------------------
    // Oscillator mix/balance and supersaw parameters
    // -------------------------------------------------------------------------
    static constexpr uint8_t OSC_MIX_BALANCE   = 47;
    static constexpr uint8_t OSC1_MIX          = 60;
    static constexpr uint8_t OSC2_MIX          = 61;
    static constexpr uint8_t SUPERSAW1_DETUNE  = 77;  // Keep existing
    static constexpr uint8_t SUPERSAW1_MIX     = 78;  // Keep existing
    static constexpr uint8_t SUPERSAW2_DETUNE  = 79;  // Keep existing
    static constexpr uint8_t SUPERSAW2_MIX     = 80;  // Keep existing
    static constexpr uint8_t SUB_MIX           = 58;
    static constexpr uint8_t NOISE_MIX         = 59;

    // -------------------------------------------------------------------------
    // Filter modulation and key tracking
    // -------------------------------------------------------------------------
    static constexpr uint8_t FILTER_ENV_AMOUNT  = 48;
    static constexpr uint8_t FILTER_KEY_TRACK   = 50;
    static constexpr uint8_t FILTER_OCTAVE_CONTROL = 84;

    // -------------------------------------------------------------------------
    // Low frequency oscillators (LFO)
    // -------------------------------------------------------------------------
    static constexpr uint8_t LFO2_FREQ        = 51;
    static constexpr uint8_t LFO2_DEPTH       = 52;
    static constexpr uint8_t LFO2_DESTINATION = 53;
    static constexpr uint8_t LFO1_FREQ        = 54;
    static constexpr uint8_t LFO1_DEPTH       = 55;
    static constexpr uint8_t LFO1_DESTINATION = 56;
    static constexpr uint8_t LFO1_WAVEFORM    = 62;
    static constexpr uint8_t LFO2_WAVEFORM    = 63;

    // -------------------------------------------------------------------------
    // Effects - Basic Reverb/Delay Controls
    // -------------------------------------------------------------------------
    static constexpr uint8_t FX_REVERB_SIZE     = 70;  // Reverb room size
    static constexpr uint8_t FX_REVERB_DAMP     = 71;  // Reverb high damping
    static constexpr uint8_t FX_DELAY_TIME      = 72;  // Legacy delay time (unused in JPFX)
    static constexpr uint8_t FX_DELAY_FEEDBACK  = 73;  // Legacy delay feedback (unused in JPFX)

    // -------------------------------------------------------------------------
    // FX MIX LEVELS - CRITICAL FOR PAGE 19
    // -------------------------------------------------------------------------
    static constexpr uint8_t FX_DRY_MIX         = 74;  // Dry signal level (pre-FX)
    static constexpr uint8_t FX_REVERB_MIX      = 75;  // Reverb wet level
    static constexpr uint8_t FX_JPFX_MIX        = 76;  // JPFX output level (bypasses reverb)
    
    // Note: CC 77-80 used by SUPERSAW (see above)

    // -------------------------------------------------------------------------
    // Glide and global modulation
    // -------------------------------------------------------------------------
    static constexpr uint8_t GLIDE_ENABLE      = 81;
    static constexpr uint8_t GLIDE_TIME        = 82;
    static constexpr uint8_t AMP_MOD_FIXED_LEVEL = 90;

    // -------------------------------------------------------------------------
    // Miscellaneous synth controls
    // -------------------------------------------------------------------------
    static constexpr uint8_t OSC1_FREQ_DC      = 86;
    static constexpr uint8_t OSC1_SHAPE_DC     = 87;
    static constexpr uint8_t OSC2_FREQ_DC      = 88;
    static constexpr uint8_t OSC2_SHAPE_DC     = 89;
    static constexpr uint8_t RING1_MIX         = 91;
    static constexpr uint8_t RING2_MIX         = 92;

    // -------------------------------------------------------------------------
    // Extended Reverb/Delay Controls
    // -------------------------------------------------------------------------
    static constexpr uint8_t FX_REVERB_LODAMP    = 93;  // Reverb low damping
    static constexpr uint8_t FX_REVERB_BYPASS    = 94;  // Reverb bypass toggle (saves CPU)
    static constexpr uint8_t FX_DELAY_MOD_RATE   = 95;  // Legacy (unused in JPFX)
    static constexpr uint8_t FX_DELAY_MOD_DEPTH  = 96;  // Legacy (unused in JPFX)
    static constexpr uint8_t FX_DELAY_INERTIA    = 97;  // Legacy (unused in JPFX)
    static constexpr uint8_t FX_DELAY_TREBLE     = 98;  // Legacy (unused in JPFX)
    // Note: CC 99-110 used by JPFX (see below)

    // -------------------------------------------------------------------------
    // JPFX Effects (JP-8000 style) - 99-110
    // -------------------------------------------------------------------------
    static constexpr uint8_t FX_BASS_GAIN        = 99;   // Bass shelf filter (-12dB to +12dB)
    static constexpr uint8_t FX_TREBLE_GAIN      = 100;  // Treble shelf filter (-12dB to +12dB)
    static constexpr uint8_t FX_MOD_EFFECT       = 103;  // Modulation variation (0..10)
    static constexpr uint8_t FX_MOD_MIX          = 104;  // Mod wet/dry mix (0..1)
    static constexpr uint8_t FX_MOD_RATE         = 105;  // Mod LFO rate (0..20 Hz)
    static constexpr uint8_t FX_MOD_FEEDBACK     = 106;  // Mod feedback (0..0.99)
    static constexpr uint8_t FX_JPFX_DELAY_EFFECT   = 107;  // JPFX Delay variation (0..4)
    static constexpr uint8_t FX_JPFX_DELAY_MIX      = 108;  // JPFX Delay wet/dry mix
    static constexpr uint8_t FX_JPFX_DELAY_FEEDBACK = 109;  // JPFX Delay feedback
    static constexpr uint8_t FX_JPFX_DELAY_TIME     = 110;  // JPFX Delay time (5..1500ms)

    // -------------------------------------------------------------------------
    // Arbitrary waveform selection
    // -------------------------------------------------------------------------
    static constexpr uint8_t OSC1_ARB_BANK  = 101;  // OSC1 waveform bank
    static constexpr uint8_t OSC2_ARB_BANK  = 102;  // OSC2 waveform bank
    static constexpr uint8_t OSC1_ARB_INDEX = 83;   // OSC1 waveform index
    static constexpr uint8_t OSC2_ARB_INDEX = 85;   // OSC2 waveform index

    // -------------------------------------------------------------------------
    // Drive / Saturation (above standard MIDI range — internal CC only)
    // CC 16: 0=bypass, 1-63=soft clip (tanh warm), 64-127=hard clip (aggressive)
    // -------------------------------------------------------------------------
    static constexpr uint8_t FX_DRIVE = 16;

    // -------------------------------------------------------------------------
    // OBXa filter extended controls
    // -------------------------------------------------------------------------
    // Multimode blend: CC 0-127 → 0.0-1.0  (LP4 → HP via pole mixing)
    // Only active in 4-pole mode (non-Xpander).
    static constexpr uint8_t FILTER_OBXA_MULTIMODE     = 111;

    // Resonance modulation depth: CC 0-127 → 0.0-1.0
    static constexpr uint8_t FILTER_OBXA_RES_MOD_DEPTH = 117;

    // ── FILTER_ENGINE (CC 113) — selects the active filter engine ───────────
    //   0  →  OBXa  — original OB-Xa / Xpander engine (default)
    //   1  →  VA    — ZDF VA bank (SVF, Moog, Diode, Korg35, TPT1)
    //
    // Switching engine resets the outgoing filter state to avoid clicks.
    // FILTER_MODE (CC 112) and VA_FILTER_TYPE (CC 115) are independent —
    // each engine remembers its last topology setting.
    static constexpr uint8_t FILTER_ENGINE          = 113;
    static constexpr uint8_t FILTER_ENGINE_OBXA     = 0;   // OBXa engine index
    static constexpr uint8_t FILTER_ENGINE_VA       = 1;   // VA bank engine index
    static constexpr uint8_t FILTER_ENGINE_COUNT    = 2;

    // ── VA_FILTER_TYPE (CC 115) — selects topology within the VA bank ────────
    //   CC 0-127 mapped into 13 equal buckets (FILTER_COUNT from AudioFilterVABank.h)
    //   0  →  SVF LP2       5  →  Moog LP4     9  →  Korg35 LP
    //   1  →  SVF HP2       6  →  Moog LP2    10  →  Korg35 HP
    //   2  →  SVF BP2       7  →  Moog BP2    11  →  TPT1 LP
    //   3  →  SVF NOTCH     8  →  Diode LP4   12  →  TPT1 HP
    //   4  →  SVF AP
    //
    // Only active when FILTER_ENGINE == FILTER_ENGINE_VA.
    // CC number re-used from old FILTER_OBXA_BP_BLEND_2_POLE (115).
    static constexpr uint8_t VA_FILTER_TYPE         = 115;
    //
    // Encoding (7 modes, CC range divided into equal buckets):
    //   0  →  OBXA_4POLE       — 4-pole LP (default, all bool flags off)
    //   1  →  OBXA_2POLE       — 2-pole LP  (TwoPole = true)
    //   2  →  OBXA_2POLE_BP    — 2-pole BP  (TwoPole + BPBlend2Pole)
    //   3  →  OBXA_2POLE_PUSH  — 2-pole push (TwoPole + Push2Pole)
    //   4  →  OBXA_XPANDER     — Xpander 4-pole (Xpander4Pole = true)
    //   5  →  OBXA_XPANDER_M   — Xpander + XpanderMode (uses FILTER_OBXA_XPANDER_MODE)
    //
    // Rule: setting any non-2-pole mode clears all 2-pole sub-flags automatically.
    // Setting mode 4/5 also clears TwoPole.  FILTER_OBXA_XPANDER_MODE (CC 114)
    // is kept as a secondary CC — only used when mode == OBXA_XPANDER_M.
    //
    // CC number re-used from old FILTER_OBXA_TWO_POLE (112) — safe, as TWO_POLE
    // is now encoded inside FILTER_MODE value; old patches sending CC 112 will
    // now select mode 1 (2-pole LP) when value > 0, which is correct.
    static constexpr uint8_t FILTER_MODE               = 112;

    // Xpander mode sub-selector: CC 0-127 → 15 discrete modes (0-14).
    // Only active when FILTER_MODE == OBXA_XPANDER_M (mode 5).
    // CC number re-used from old FILTER_OBXA_XPANDER_MODE (114).
    static constexpr uint8_t FILTER_OBXA_XPANDER_MODE  = 114;

    // ── Filter mode index constants (used by UI and SynthEngine) ─────────────
    static constexpr uint8_t FILTER_MODE_4POLE      = 0;   // OBXa standard 4-pole LP
    static constexpr uint8_t FILTER_MODE_2POLE      = 1;   // OBXa 2-pole LP
    static constexpr uint8_t FILTER_MODE_2POLE_BP   = 2;   // OBXa 2-pole BP blend
    static constexpr uint8_t FILTER_MODE_2POLE_PUSH = 3;   // OBXa 2-pole push
    static constexpr uint8_t FILTER_MODE_XPANDER    = 4;   // OBXa Xpander 4-pole
    static constexpr uint8_t FILTER_MODE_XPANDER_M  = 5;   // OBXa Xpander + mode select
    static constexpr uint8_t FILTER_MODE_COUNT      = 6;   // keep last — for bounds check

    // -------------------------------------------------------------------------
    // BPM Clock and Timing (NEW - 118-122)
    // -------------------------------------------------------------------------
    static constexpr uint8_t BPM_CLOCK_SOURCE    = 118;  // 0=Internal, 1=External MIDI
    static constexpr uint8_t BPM_INTERNAL_TEMPO  = 119;  // 40-127 mapped to 40-300 BPM
    static constexpr uint8_t LFO1_TIMING_MODE    = 120;  // 0-11 (TimingMode enum)
    static constexpr uint8_t LFO2_TIMING_MODE    = 121;  // 0-11 (TimingMode enum)
    static constexpr uint8_t DELAY_TIMING_MODE   = 122;  // 0-11 (TimingMode enum)

    // ─────────────────────────────────────────────────────────────────────────
    // STEP SEQUENCER (analogue-style CV sequencer)
    //   Outputs a bipolar control value (−1…+1) routed to Pitch/Filter/PWM/Amp.
    //   Step values: CC 64 = zero, below = negative, above = positive.
    //   "Select + Value" editing: set SEQ_STEP_SELECT then SEQ_STEP_VALUE.
    // ─────────────────────────────────────────────────────────────────────────
    static constexpr uint8_t SEQ_ENABLE       = 2;    // 0-63 off, 64-127 on
    static constexpr uint8_t SEQ_STEPS        = 3;    // 0-127 → 1-16 active steps
    static constexpr uint8_t SEQ_GATE_LENGTH  = 4;    // 0-127 → 0-100% gate
    static constexpr uint8_t SEQ_SLIDE        = 5;    // 0-127 → 0-100% glide
    static constexpr uint8_t SEQ_DIRECTION    = 6;    // 0-3: Fwd/Rev/Bounce/Random
    static constexpr uint8_t SEQ_RATE         = 7;    // 0-127 → 0.1-20 Hz (free mode)
    static constexpr uint8_t SEQ_DEPTH        = 8;    // 0-127 → 0.0-1.0 master scale
    static constexpr uint8_t SEQ_DESTINATION  = 9;    // 0-4: None/Pitch/Filter/PWM/Amp
    static constexpr uint8_t SEQ_RETRIGGER    = 13;   // 0-63 off, 64-127 on
    static constexpr uint8_t SEQ_STEP_SELECT  = 17;   // 0-15: select step to edit
    static constexpr uint8_t SEQ_STEP_VALUE   = 18;   // 0-127: value for selected step
    static constexpr uint8_t SEQ_TIMING_MODE  = 116;  // 0-11 (TimingMode enum)

   

    // ─────────────────────────────────────────────────────────────────────────
    // LFO1 per-destination depth amounts (JP-8000 style)
    // Each destination can be non-zero simultaneously for multi-target mod.
    // Final mixer gain = masterDepth * perDestDepth.
    // ─────────────────────────────────────────────────────────────────────────
    static constexpr uint8_t LFO1_PITCH_DEPTH  = 33;  // LFO1 → pitch depth 0-127
    static constexpr uint8_t LFO1_FILTER_DEPTH = 34;  // LFO1 → filter cutoff depth
    static constexpr uint8_t LFO1_PWM_DEPTH    = 35;  // LFO1 → shape/PWM depth
    static constexpr uint8_t LFO1_AMP_DEPTH    = 36;  // LFO1 → amplitude depth
    static constexpr uint8_t LFO1_DELAY        = 37;  // LFO1 fade-in after noteOn (ms)

    // ─────────────────────────────────────────────────────────────────────────
    // NEW: LFO2 per-destination depth amounts
    // ─────────────────────────────────────────────────────────────────────────
    static constexpr uint8_t LFO2_PITCH_DEPTH  = 38;
    static constexpr uint8_t LFO2_FILTER_DEPTH = 39;
    static constexpr uint8_t LFO2_PWM_DEPTH    = 40;
    static constexpr uint8_t LFO2_AMP_DEPTH    = 57;  // 40 taken by LFO2_PWM
    static constexpr uint8_t LFO2_DELAY        = 64;  // 65-69 used for pitch env below

    // ─────────────────────────────────────────────────────────────────────────
    // NEW: Pitch envelope
    // DEPTH is bipolar: CC 64 = 0 semitones, 0 = -24, 127 = +24 semitones.
    // ─────────────────────────────────────────────────────────────────────────
    static constexpr uint8_t PITCH_ENV_ATTACK   = 65;
    static constexpr uint8_t PITCH_ENV_DECAY    = 66;
    static constexpr uint8_t PITCH_ENV_SUSTAIN  = 67;
    static constexpr uint8_t PITCH_ENV_RELEASE  = 68;
    static constexpr uint8_t PITCH_ENV_DEPTH    = 69;  // bipolar, 64 = zero

    // ─────────────────────────────────────────────────────────────────────────
    // NEW: Velocity sensitivity — three targets matching JP-8000
    // 0 = velocity has no effect on target; 127 = full velocity control.
    // Applied on every noteOn — does not affect stored base parameter values.
    // ─────────────────────────────────────────────────────────────────────────
    static constexpr uint8_t VELOCITY_AMP_SENS    = 10;   // velocity → VCA level
    static constexpr uint8_t VELOCITY_FILTER_SENS = 11;   // velocity → filter cutoff offset
    static constexpr uint8_t VELOCITY_ENV_SENS    = 12;   // velocity → filter env depth

    // ─────────────────────────────────────────────────────────────────────────
    // PITCH BEND RANGE
    // ─────────────────────────────────────────────────────────────────────────
    // Sets the pitch wheel throw in semitones (both up and down).
    // CC value 0..127 maps to 0..24 semitones range.
    // Default (CC=12): ±2 semitones — standard MIDI keyboard range.
    // CC=127:  ±24 semitones (2 octaves each way).
    //
    // This is a global (synth-level) setting, stored in SynthEngine::_pitchBendRange.
    // Using CC rather than RPN 0 for simplicity — easier to set from a MIDI controller.
    // ─────────────────────────────────────────────────────────────────────────
    static constexpr uint8_t PITCH_BEND_RANGE = 127; // CC 127: 0-127 → 0-24 semitones range

    // ─────────────────────────────────────────────────────────────────────────
    // POLY MODE — voice allocation / stacking mode
    //   CC 0..42   = POLY   — 8-voice polyphony (default)
    //   CC 43..84  = MONO   — single voice, last-note priority, full glide
    //   CC 85..127 = UNISON — all 8 voices stacked on one note, detuned
    // ─────────────────────────────────────────────────────────────────────────
    static constexpr uint8_t POLY_MODE     = 14;

    // Unison detune spread — active only when POLY_MODE = UNISON.
    // 0 = all in tune;  127 = ±0.5 semitone spread across 8 voices.
    static constexpr uint8_t UNISON_DETUNE = 15;

   // -------------------------------------------------------------------------
    // Utility: return human-readable name for a CC
    // -------------------------------------------------------------------------
    inline const char* name(uint8_t cc) {
        switch (cc) {
            // Oscillators
            case OSC1_WAVE:           return "OSC1 Wave";
            case OSC2_WAVE:           return "OSC2 Wave";
            case OSC1_PITCH_OFFSET:   return "OSC1 Coarse";
            case OSC2_PITCH_OFFSET:   return "OSC2 Coarse";
            case OSC1_DETUNE:         return "OSC1 Detune";
            case OSC2_DETUNE:         return "OSC2 Detune";
            case OSC1_FINE_TUNE:      return "OSC1 Fine";
            case OSC2_FINE_TUNE:      return "OSC2 Fine";
            case OSC_MIX_BALANCE:     return "Osc Bal";
            case OSC1_MIX:            return "Osc1 Mix";
            case OSC2_MIX:            return "Osc2 Mix";
            case OSC1_FEEDBACK_AMOUNT:       return "Osc1 FBA";
            case OSC2_FEEDBACK_AMOUNT:       return "Osc2 FBA";  
            case OSC1_FEEDBACK_MIX:       return "Osc1 FBM";
            case OSC2_FEEDBACK_MIX:       return "Osc2 FBM";     
            case SUPERSAW1_DETUNE:    return "Saw1 Det";
            case SUPERSAW1_MIX:       return "Saw1 Mix";
            case SUPERSAW2_DETUNE:    return "Saw2 Det";
            case SUPERSAW2_MIX:       return "Saw2 Mix";
            case SUB_MIX:             return "Sub Mix";
            case NOISE_MIX:           return "Noise Mix";
            case OSC1_FREQ_DC:        return "Freq DC1";
            case OSC1_SHAPE_DC:       return "Shape DC1";
            case OSC2_FREQ_DC:        return "Freq DC2";
            case OSC2_SHAPE_DC:       return "Shape DC2";
            case RING1_MIX:           return "Ring1 Mix";
            case RING2_MIX:           return "Ring2 Mix";
            case OSC1_ARB_BANK:       return "OSC1 Bank";
            case OSC2_ARB_BANK:       return "OSC2 Bank";
            case OSC1_ARB_INDEX:      return "OSC1 Wave#";
            case OSC2_ARB_INDEX:      return "OSC2 Wave#";
            case OSC_CROSS_MOD_DEPTH: return "XMod Depth";
            case OSC_SYNC_ENABLE:     return "Osc Sync";

            // Filter
            case FILTER_CUTOFF:       return "Cutoff";
            case FILTER_RESONANCE:    return "Resonance";
            case FILTER_ENV_AMOUNT:   return "Flt Env Amt";
            case FILTER_KEY_TRACK:    return "Key Track";
            case FILTER_OCTAVE_CONTROL: return "Oct Ctrl";
            case FILTER_OBXA_MULTIMODE:    return "Multimode";
            case FILTER_ENGINE:            return "Flt Engine";
            case FILTER_MODE:              return "Flt Mode";
            case VA_FILTER_TYPE:           return "VA Type";
            case FILTER_OBXA_XPANDER_MODE: return "Xpand Mode";
            case FILTER_OBXA_RES_MOD_DEPTH: return "Q Depth";

            // Envelopes
            case AMP_ATTACK:          return "Amp Att";
            case AMP_DECAY:           return "Amp Dec";
            case AMP_SUSTAIN:         return "Amp Sus";
            case AMP_RELEASE:         return "Amp Rel";
            case FILTER_ENV_ATTACK:   return "Flt Att";
            case FILTER_ENV_DECAY:    return "Flt Dec";
            case FILTER_ENV_SUSTAIN:  return "Flt Sus";
            case FILTER_ENV_RELEASE:  return "Flt Rel";

            // LFOs
            case LFO1_FREQ:           return "LFO1 Freq";
            case LFO1_DEPTH:          return "LFO1 Depth";
            case LFO1_DESTINATION:    return "LFO1 Dest";
            case LFO1_WAVEFORM:       return "LFO1 Wave";
            case LFO2_FREQ:           return "LFO2 Freq";
            case LFO2_DEPTH:          return "LFO2 Depth";
            case LFO2_DESTINATION:    return "LFO2 Dest";
            case LFO2_WAVEFORM:       return "LFO2 Wave";

            // FX - Reverb
            case FX_REVERB_SIZE:      return "Rev Size";
            case FX_REVERB_DAMP:      return "Rev Damp";
            case FX_REVERB_LODAMP:    return "Rev LoDamp";
            case FX_REVERB_MIX:       return "Rev Mix";
            case FX_REVERB_BYPASS:    return "Rev Bypass";

            // FX - Mix Levels
            case FX_DRY_MIX:          return "Dry Mix";
            case FX_JPFX_MIX:         return "JPFX Mix";

            // FX - JPFX Tone
            case FX_BASS_GAIN:        return "Bass";
            case FX_TREBLE_GAIN:      return "Treble";
            case FX_DRIVE:            return "Drive";

            // FX - JPFX Modulation
            case FX_MOD_EFFECT:       return "Mod FX";
            case FX_MOD_MIX:          return "Mod Mix";
            case FX_MOD_RATE:         return "Mod Rate";
            case FX_MOD_FEEDBACK:     return "Mod FB";

            // FX - JPFX Delay
            case FX_JPFX_DELAY_EFFECT:   return "Dly FX";
            case FX_JPFX_DELAY_MIX:      return "Dly Mix";
            case FX_JPFX_DELAY_FEEDBACK: return "Dly FB";
            case FX_JPFX_DELAY_TIME:     return "Dly Time";

            // FX - Legacy (unused)
            case FX_DELAY_TIME:       return "Delay Time";
            case FX_DELAY_FEEDBACK:   return "Delay FB";
            case FX_DELAY_MOD_RATE:   return "Dly ModRate"; 
            case FX_DELAY_MOD_DEPTH:  return "Dly ModDepth";
            case FX_DELAY_INERTIA:    return "Dly Inertia";
            case FX_DELAY_TREBLE:     return "Dly Treble";

            // Global
            case GLIDE_ENABLE:        return "Glide On";
            case GLIDE_TIME:          return "Glide Time";
            case AMP_MOD_FIXED_LEVEL: return "Amp Mod";

            // BPM Timing (NEW)
            case BPM_CLOCK_SOURCE:    return "Clock Src";
            case BPM_INTERNAL_TEMPO:  return "Int BPM";
            case LFO1_TIMING_MODE:    return "LFO1 Sync";
            case LFO2_TIMING_MODE:    return "LFO2 Sync";
            case DELAY_TIMING_MODE:   return "Dly Sync";
            case POLY_MODE:           return "Poly Mode";
            case UNISON_DETUNE:       return "Uni Detune";


            // Step Sequencer
            case SEQ_ENABLE:          return "Seq On";
            case SEQ_STEPS:           return "Seq Steps";
            case SEQ_GATE_LENGTH:     return "Seq Gate";
            case SEQ_SLIDE:           return "Seq Slide";
            case SEQ_DIRECTION:       return "Seq Dir";
            case SEQ_RATE:            return "Seq Rate";
            case SEQ_DEPTH:           return "Seq Depth";
            case SEQ_DESTINATION:     return "Seq Dest";
            case SEQ_RETRIGGER:       return "Seq Retrig";
            case SEQ_STEP_SELECT:     return "Seq Step#";
            case SEQ_STEP_VALUE:      return "Seq StpVal";
            case SEQ_TIMING_MODE:     return "Seq Sync";

            default:                  return nullptr;
        }
    }

} // namespace CC