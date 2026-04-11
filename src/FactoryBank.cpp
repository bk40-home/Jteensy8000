/*
 * FactoryBank.cpp — Factory preset loader for JT-8000
 *
 * Single entry point: loadFactoryPatch()
 * Sends EVERY CC in PatchSchema::kPatchableCCs order, guaranteeing
 * no stale state inherited from the previous patch.
 *
 * This replaces: loadInitTemplateByWave(), loadTUSPreset(),
 *                loadMicrospherePreset(), loadRawPatchViaCC()
 */

#include "FactoryBank.h"
#include "PatchSchema.h"
#include "CCDefs.h"
#include "SynthEngine.h"

namespace FactoryBank {

void loadFactoryPatch(SynthEngine& synth, int index, uint8_t midiCh)
{
    // Bounds check with wrap
    if (kFactoryCount <= 0) return;
    while (index < 0)              index += kFactoryCount;
    while (index >= kFactoryCount)  index -= kFactoryCount;

    // Read patch from PROGMEM
    FactoryPatch patch;
    memcpy_P(&patch, &kPatches[index], sizeof(FactoryPatch));

    // Send all CCs atomically — prevents audio glitches from partial state
    AudioNoInterrupts();

    for (int i = 0; i < PatchSchema::kPatchableCount; ++i) {
        const uint8_t cc  = PatchSchema::kPatchableCCs[i];
        const uint8_t val = patch.cc[i];
        synth.handleControlChange(midiCh, cc, val);
    }

    AudioInterrupts();
}

} // namespace FactoryBank
