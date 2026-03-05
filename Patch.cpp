#include "Patch.h"
#include "CCDefs.h"
#include "Mapping.h"
#include "PatchSchema.h"

using namespace JT8000Map;
using namespace CC;

int Patch::buildUsedCCList(uint8_t* outList, int maxCount) const {
    const int n = min(maxCount, PatchSchema::kPatchableCount);
    for (int i=0;i<n;++i) outList[i] = PatchSchema::kPatchableCCs[i];
    return n;
}

void Patch::captureFrom(SynthEngine& synth) {
    clear();
    uint8_t used[128];
    int n = buildUsedCCList(used, 128);

    for (int i=0;i<n;++i) {
        uint8_t cc = used[i];
        uint8_t cv = 0;
        switch (cc) {
            case FILTER_CUTOFF:    cv = cutoff_hz_to_cc(synth.getFilterCutoff()); break;
            case FILTER_RESONANCE: cv = resonance_to_cc(synth.getFilterResonance()); break;

            case AMP_ATTACK:  cv = time_ms_to_cc(synth.getAmpAttack()); break;
            case AMP_DECAY:   cv = time_ms_to_cc(synth.getAmpDecay()); break;
            case AMP_SUSTAIN: cv = norm_to_cc(synth.getAmpSustain());  break;
            case AMP_RELEASE: cv = time_ms_to_cc(synth.getAmpRelease()); break;

            case FILTER_ENV_ATTACK:  cv = time_ms_to_cc(synth.getFilterEnvAttack()); break;
            case FILTER_ENV_DECAY:   cv = time_ms_to_cc(synth.getFilterEnvDecay()); break;
            case FILTER_ENV_SUSTAIN: cv = norm_to_cc(synth.getFilterEnvSustain()); break;
            case FILTER_ENV_RELEASE: cv = time_ms_to_cc(synth.getFilterEnvRelease()); break;

            case LFO1_FREQ:  cv = lfo_hz_to_cc(synth.getLFO1Frequency()); break;
            case LFO1_DEPTH: cv = norm_to_cc(synth.getLFO1Amount());      break;
            case LFO2_FREQ:  cv = lfo_hz_to_cc(synth.getLFO2Frequency()); break;
            case LFO2_DEPTH: cv = norm_to_cc(synth.getLFO2Amount());      break;

            case OSC1_WAVE: cv = (uint8_t)synth.getOsc1Waveform(); break;
            case OSC2_WAVE: cv = (uint8_t)synth.getOsc2Waveform(); break;

            case OSC_MIX_BALANCE: cv = norm_to_cc(synth.getOscMix2()); break; // or synth balance getter
            case OSC1_MIX:        cv = norm_to_cc(synth.getOscMix1()); break;
            case OSC2_MIX:        cv = norm_to_cc(synth.getOscMix2()); break;
            case SUB_MIX:         cv = norm_to_cc(synth.getSubMix());  break;
            case NOISE_MIX:       cv = norm_to_cc(synth.getNoiseMix()); break;

            case GLIDE_ENABLE: cv = synth.getGlideEnabled() ? 127 : 0; break;
            case GLIDE_TIME:   cv = (uint8_t)constrain(lroundf((synth.getGlideTimeMs()/500.0f)*127.0f),0,127); break;

            default: cv = value[cc]; break; // fallback
        }
        setCC(cc, cv);
    }
}

void Patch::applyTo(SynthEngine& synth, uint8_t midiChannel, bool batch) const {
    if (batch) AudioNoInterrupts();
    for (int cc=0; cc<128; ++cc) {
        if (!has[cc]) continue;
        synth.handleControlChange(midiChannel, (uint8_t)cc, value[cc]);
    }
    if (batch) AudioInterrupts();
}
