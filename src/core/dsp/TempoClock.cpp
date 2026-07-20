// =============================================================================
// TempoClock.cpp — implementation
// =============================================================================
// See TempoClock.h for role, scope and the flagged v1 deviation (the
// corrected divide vs v1's multiply in the sync-frequency formula).
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "core/dsp/TempoClock.h"

namespace JT {

void TempoClock::setBpm(float bpm)
{
    // v1's internal-clock range, BPMClockManager.cpp:88.
    if (bpm < 40.0f)       _bpm = 40.0f;
    else if (bpm > 300.0f) _bpm = 300.0f;
    else                   _bpm = bpm;
}

float TempoClock::freqForMode(int mode) const
{
    if (mode <= kFree || mode >= kNumModes) return -1.0f;   // "not synced"

    // CORRECTED vs v1's getFrequencyForMode (spec §3 decision #3): v1
    // multiplied by kMult where the musical-label math requires a divide.
    // (BPM/60) is quarter-notes-per-second; dividing by "quarter-notes per
    // cycle" yields cycles-per-second (Hz) of the selected division.
    return (_bpm / 60.0f) / kMult[mode];
}

} // namespace JT
