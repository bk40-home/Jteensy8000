// =============================================================================
// WaveTableSet.h — descriptor for the ONE wavetable reader in OscCore
// =============================================================================
//
// ROLE
//   Describes any int16 single-cycle wavetable data the oscillator can play,
//   from the simplest case up to a measured morph set:
//
//     frames  — cycles along a morph axis driven by SHAPE (1 = no morph)
//     levels  — band-limited copies of every frame ("mips"); the reader picks
//               the first level whose highest harmonic stays under Nyquist
//               for the note being played (1 = single full-band table)
//
//   AKWF ARB tables are the degenerate case: frames = 1, levels = 1, length
//   600, harmonic limit unknown (0).  The JP-8000 SHAPE morph tables
//   (data/akwf/JpMorph/*, measured from JE-8086 captures by
//   tools/jp_wavemorph_gen.py) use 17 frames × 7 levels.
//
// MEMORY LAYOUT (what the generator emits and the reader assumes)
//   data[frame * samplesPerFrame + levelOffset[level] + i],  0 <= i < levelLen[level]
//   Levels are stored most-harmonics-first, so level 0 is the full table.
//
// LIFETIME
//   The descriptor and everything it points at must outlive the oscillator
//   using it.  Generated sets are static const flash data (forever); the ARB
//   case is built inside OscCore itself from the (flash) AKWF pointer.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>

namespace JT {

struct WaveTableSet {
    const int16_t*  data            = nullptr;  // frames × samplesPerFrame
    uint32_t        samplesPerFrame = 0;        // sum of levelLen[]
    uint16_t        frames          = 0;        // >= 1 (1 = no morph)
    uint16_t        levels          = 0;        // >= 1
    const uint16_t* levelOffset     = nullptr;  // start of each level in a frame
    const uint16_t* levelLen        = nullptr;  // samples per cycle, any length >= 2
    const uint16_t* levelHarm       = nullptr;  // highest harmonic kept (0 = unknown/full band)

    bool valid() const
    {
        return data != nullptr && frames >= 1 && levels >= 1
            && levelOffset != nullptr && levelLen != nullptr && levelHarm != nullptr;
    }
};

} // namespace JT
