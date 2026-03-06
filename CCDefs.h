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
    // OBXa filter extended controls
    // -------------------------------------------------------------------------
    static constexpr uint8_t FILTER_OBXA_MULTIMODE = 111;
    static constexpr uint8_t FILTER_OBXA_TWO_POLE = 112;
    static constexpr uint8_t FILTER_OBXA_XPANDER_4_POLE = 113;
    static constexpr uint8_t FILTER_OBXA_XPANDER_MODE = 114;
    static constexpr uint8_t FILTER_OBXA_BP_BLEND_2_POLE = 115;
    static constexpr uint8_t FILTER_OBXA_PUSH_2_POLE = 116;
    static constexpr uint8_t FILTER_OBXA_RES_MOD_DEPTH = 117;

    // -------------------------------------------------------------------------
    // BPM Clock and Timing (NEW - 118-122)
    // -------------------------------------------------------------------------
    static constexpr uint8_t BPM_CLOCK_SOURCE    = 118;  // 0=Internal, 1=External MIDI
    static constexpr uint8_t BPM_INTERNAL_TEMPO  = 119;  // 40-127 mapped to 40-300 BPM
    static constexpr uint8_t LFO1_TIMING_MODE    = 120;  // 0-11 (TimingMode enum)
    static constexpr uint8_t LFO2_TIMING_MODE    = 121;  // 0-11 (TimingMode enum)
    static constexpr uint8_t DELAY_TIMING_MODE   = 122;  // 0-11 (TimingMode enum)

 

    // ─────────────────────────────────────────────────────────────────────────
    // NEW: LFO1 per-destination depth amounts (JP-8000 style)
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
    static constexpr uint8_t POLY_MODE     = 128;

    // Unison detune spread — active only when POLY_MODE = UNISON.
    // 0 = all in tune;  127 = ±0.5 semitone spread across 8 voices.
    static constexpr uint8_t UNISON_DETUNE = 129;

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

            // Filter
            case FILTER_CUTOFF:       return "Cutoff";
            case FILTER_RESONANCE:    return "Resonance";
            case FILTER_ENV_AMOUNT:   return "Flt Env Amt";
            case FILTER_KEY_TRACK:    return "Key Track";
            case FILTER_OCTAVE_CONTROL: return "Oct Ctrl";
            case FILTER_OBXA_MULTIMODE: return "Multimode";
            case FILTER_OBXA_TWO_POLE: return "2 Pole";
            case FILTER_OBXA_XPANDER_4_POLE: return "Xpander";
            case FILTER_OBXA_XPANDER_MODE: return "Xpand Mode";
            case FILTER_OBXA_BP_BLEND_2_POLE: return "Blend 2p";
            case FILTER_OBXA_PUSH_2_POLE: return "Push 2p";
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

            default:                  return nullptr;
        }
    }

} // namespace CC