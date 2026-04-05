/* Audio Library for Teensy
 * Copyright (c) 2025, Kris Bishop, bishopkris40@hotmail.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
 #pragma once
#include <Arduino.h>
#include "Audio.h"
#include "Waveforms.h"  // ✅ use the same waveform IDs & names as main osc
#include "BPMClockManager.h"  // For tempo sync

enum LFODestination {
    LFO_DEST_NONE = 0,
    LFO_DEST_PITCH,
    LFO_DEST_FILTER,
    LFO_DEST_PWM,
    LFO_DEST_AMP,
    NUM_LFO_DESTS
};

// LFO destination names — indices must match LFODestination
static const char* LFODestNames[NUM_LFO_DESTS]
    __attribute__((unused)) = {
    "None",           // LFO_DEST_NONE
    "Pitch",          // LFO_DEST_PITCH
    "Filter",         // LFO_DEST_FILTER
    "Pulse Width",    // LFO_DEST_PWM   (called “PWM” in enum; UI shows “Pulse Width”)
    "Amp"             // LFO_DEST_AMP
};




class LFOBlock {
public:
    // --- Lifecycle
    LFOBlock();
    void update();

    // --- Parameter Setters
    /**
     * @brief Enable or disable the LFO.  When disabled, the underlying
     * AudioSynthWaveform is muted and no CPU is consumed generating a
     * waveform.  You can re‑enable the LFO later and it will pick up where
     * it left off (free‑running).
     *
     * @param enabled true to enable the LFO, false to disable it
     */
    void setEnabled(bool enabled);
    /**
     * @brief Query whether the LFO is currently enabled.
     */
    bool isEnabled() const;
    void setWaveformType(int type);
    void setFrequency(float freq);
    void setAmplitude(float amp);
    void setDestination(LFODestination destination);

    // ADD to public methods (after line 52):
    /**
     * @brief Set timing mode for this LFO
     * @param mode TIMING_FREE (Hz) or musical division
     */
    void setTimingMode(TimingMode mode);
    
    /**
     * @brief Get current timing mode
     */
    TimingMode getTimingMode() const { return _timingMode; }
    
    /**
     * @brief Update frequency from BPM clock (called by SynthEngine)
     * @param bpmClock Reference to global BPM clock manager
     */
    void updateFromBPMClock(const BPMClockManager& bpmClock);




    // --- Parameter Getters
    float getFrequency() const;
    float getAmplitude() const;
    int getWaveform() const;
    LFODestination getDestination() const;
    // --- Outputs
    AudioStream& output();

private:
    int _type = 0;
    float _freq = 1.0f;
    float _amp = 0.0f;
    // Track whether the LFO is currently enabled.  When disabled, the
    // waveform generator is muted (amplitude set to 0) and no phase is
    // advanced.  This allows the LFO to be free‑running across notes
    // without wasting CPU cycles when it’s not audible.
    bool _enabled = false;
    TimingMode _timingMode = TIMING_FREE;  // Default: free-running Hz
    float _freeRunningFreq = 1.0f;         // Stored Hz when in free mode
    AudioSynthWaveform _lfo;
    LFODestination _destination = LFO_DEST_NONE;
    // Preserve the current phase when muting/unmuting.  AudioSynthWaveform
    // stores its phase in a private accumulator, so we approximate
    // free‑running behaviour by caching our notion of phase.  This is
    // advanced in update() when enabled and restored when re‑enabling.
    float _phase = 0.0f;
};
