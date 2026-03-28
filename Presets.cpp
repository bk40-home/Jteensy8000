/*
 * Presets.cpp — JT-8000 Preset API implementation
 *
 * All loading is delegated to FactoryBank::loadFactoryPatch().
 * This file exists only to maintain the Presets:: namespace API
 * that PresetBrowser and .ino already use.
 */

#include "Presets.h"
#include "FactoryBank.h"

namespace Presets {

int presets_totalCount()
{
    return FactoryBank::kFactoryCount;
}

int presets_initCount()
{
    return FactoryBank::kInitCount;
}

void presets_loadByGlobalIndex(SynthEngine& synth, int globalIdx, uint8_t midiCh)
{
    FactoryBank::loadFactoryPatch(synth, globalIdx, midiCh);
}

const char* presets_nameByGlobalIndex(int globalIdx)
{
    return FactoryBank::patchName(globalIdx);
}

} // namespace Presets
