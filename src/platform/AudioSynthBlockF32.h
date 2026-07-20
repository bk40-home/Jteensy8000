// =============================================================================
// AudioSynthBlockF32.h — Teensy audio-graph wrapper around SynthCore
// =============================================================================
//
// ROLE (design brief §6.1 / F1b)
//   The ONE AudioStream_F32 node the whole synthesizer occupies.  Its
//   update() runs in the audio ISR, calls SynthCore::renderBlock() — the
//   identical function the host tests exercise — and transmits stereo F32.
//   Everything musical lives in core/; this file is deliberately boring.
//
// PERFORMANCE PROBE (v1 lesson: never Serial.print from the ISR)
//   update() measures its own duration with the ARM DWT cycle counter into
//   volatile fields; loop() reads and resets them at 1 Hz for the status
//   line.  Budget check for §10: cycles per block / (600 MHz * 2.9 ms).
//
// HARDWARE-VERIFICATION STATUS
//   Host-syntax-checked against stubs only — see OFFLINE_TESTING.md S2 for
//   the names to verify against your installed OpenAudio version.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <AudioStream_F32.h>

#include "core/SynthCore.h"
#include "core/ParameterStore.h"

namespace JT {

class AudioSynthBlockF32 : public AudioStream_F32 {
public:
    // combPool: SynthCore::kCombPoolFloats floats, declared DMAMEM by the
    // sketch so the 14 KB of delay lines live in OCRAM, not DTCM.
    // reverbPool: SynthCore::kReverbPoolFloats floats, declared EXTMEM (PSRAM)
    // by the sketch so the ~155 KB of reverb delay lines live off-chip, not in
    // DTCM.  Defaulted null so any host stub that omits it still builds.
    // fxPool: SynthCore::kFxPoolFloats floats, declared EXTMEM (PSRAM) by the
    // sketch so the ~534 KB of FX-chain delay/mod lines also live off-chip.
    AudioSynthBlockF32(ParameterStore& store, float* combPool,
                       float* reverbPool = nullptr, float* fxPool = nullptr)
        : AudioStream_F32(0, nullptr),      // pure source: no input queues
          _core(store, combPool, reverbPool, fxPool)
    {
    }

    // Control-plane access for main.cpp's MIDI routing.
    SynthCore& core() { return _core; }

    // --- perf probe: written in the ISR, read+reset from loop() ---
    volatile uint32_t perfLastCycles = 0;   // most recent block
    volatile uint32_t perfMaxCycles  = 0;   // worst block since last reset
    void perfReset() { perfMaxCycles = 0; }

    virtual void update() override;

private:
    SynthCore _core;
};

} // namespace JT
