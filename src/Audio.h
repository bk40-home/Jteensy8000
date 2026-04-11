// =============================================================================
// Audio.h — JT-8000 local override of the Teensy Audio library header
// =============================================================================
//
// WHY THIS FILE EXISTS:
//   The Teensy Audio library ships synth_waveform.h / synth_waveform.cpp.
//   The JT-8000 uses a fork (Synth_Waveform.h/.cpp) that adds:
//     - AudioSynthWaveformJT: renamed AudioSynthWaveformModulated with
//       extensible simultaneous FM + PWM modulation (future delivery).
//
// HOW THE GUARD TRICK WORKS:
//   1. This file is found first (sketch folder shadows the library).
//   2. We include "Synth_Waveform.h" first — this sets the synth_waveform_h_
//      include guard.
//   3. #include_next <Audio.h> forwards to the real library Audio.h.
//      The library Audio.h tries to #include "synth_waveform.h" but the
//      guard is already set, so the library HEADER is skipped.
//   4. Our class declarations win; library class declarations are suppressed.
//
// WHY THERE ARE NO DUPLICATE-SYMBOL LINKER ERRORS:
//   The library's synth_waveform.cpp still compiles (Arduino always compiles
//   all library .cpp files). It defines AudioSynthWaveform::update(),
//   BandLimitedWaveform::*, and AudioSynthWaveformModulated::update().
//   Our Synth_Waveform.cpp defines ONLY AudioSynthWaveformJT::update() —
//   a symbol the library never defines. Zero conflict.
//   AudioSynthWaveformModulated from the library is dead code; the linker
//   discards it since no JT-8000 code references it.
//
// MAINTENANCE:
//   Nothing to update when the Audio library upgrades. #include_next always
//   picks up the current installed version automatically.
// =============================================================================

#ifndef _AUDIO_H_JT8000_OVERRIDE_
#define _AUDIO_H_JT8000_OVERRIDE_

// Step 1: Include our JT fork first.
// Sets synth_waveform_h_ guard so library header is skipped in step 2.
#include "Synth_Waveform.h"

// Step 2: Forward to the real installed Audio.h in the library folder.
// Picks up all other audio classes (mixers, filters, effects, etc.)
// without any manual maintenance.
#include_next <Audio.h>

#endif // _AUDIO_H_JT8000_OVERRIDE_
