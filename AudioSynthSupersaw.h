#pragma once

#include <Arduino.h>
#include "AudioStream.h"

#define SUPERSAW_VOICES 7

class AudioSynthSupersaw : public AudioStream {
public:
    AudioSynthSupersaw();

    void setFrequency(float freq);
    void setAmplitude(float amp);
    void setDetune(float amount);
    void setMix(float mix);
    void setOutputGain(float gain);
    // Enable or disable 2× oversampling. When enabled the supersaw oscillator
    // generates two internal samples per output sample and averages them to
    // reduce aliasing at the cost of CPU. Disabled by default.
    void setOversample(bool enable);
    /**
     * @brief Enable or disable simple mix‑dependent gain compensation.
     *
     * When enabled the supersaw will automatically boost its output level
     * as the mix parameter increases.  A completely dry single saw (mix=0)
     * remains at unity gain, while a fully wet supersaw (mix=1) will be
     * scaled up towards the compensationMaxGain value.  Intermediate
     * positions smoothly interpolate between unity and the maximum.  This
     * compensation helps counteract the natural level drop which occurs
     * when several detuned oscillators are summed together【312547542588087†L932-L960】.
     *
     * @param enable Set to true to enable dynamic gain compensation.
     */
    void setMixCompensation(bool enable);

    /**
     * @brief Set the maximum gain applied when mix is fully wet.
     *
     * This value defines the upper bound of the dynamic gain compensation.
     * When mix=0 the gain factor is always 1.0f.  When mix=1 the gain
     * factor will equal this value.  Changing this allows you to
     * fine‑tune the perceived loudness of the full supersaw without
     * altering the dry level.  Typical values range from 1.0 (no
     * compensation) up to around 2.0.  The default is 1.5.
     *
     * @param maxGain Maximum compensation gain for mix=1.
     */
    void setCompensationMaxGain(float maxGain);

    /**
     * @brief Enable or disable band‑limited oscillators using the PolyBLEP method.
     *
     * When true, each voice uses a band‑limited sawtooth based on PolyBLEP
     * correction which substantially reduces aliasing without requiring
     * oversampling. When false (the default) the oscillator uses the
     * conventional naive saw (phase * 2 − 1).  Enabling PolyBLEP adds a
     * small amount of extra math per sample but may allow you to disable
     * oversampling entirely for lower CPU usage with improved sound quality.
     *
     * @param enable Set to true to enable PolyBLEP band‑limited saws.
     */
    void setBandLimited(bool enable);
    void noteOn();

    virtual void update(void) override;

private:
    float freq;
    float detuneAmt;
    float mixAmt;
    float amp;
    float outputGain;
    float phases[SUPERSAW_VOICES];
    float phaseInc[SUPERSAW_VOICES];
    float gains[SUPERSAW_VOICES];
    float hpfPrevIn;
    float hpfPrevOut;
    float hpfAlpha;

    // Flag indicating whether 2× oversampling is active
    bool oversample2x;

    // Flag indicating whether to use PolyBLEP band‑limited saw waveforms.
    // When false a simple naive saw is generated.
    bool usePolyBLEP;

    float detuneCurve(float x);
    void calculateIncrements();
    void calculateGains();
    void calculateHPF();

    // Mix compensation flag and scaling.  When mixCompensationEnabled is
    // true, the final output will be multiplied by a factor which
    // interpolates between 1.0f at mix=0 and compensationMaxGain at mix=1.
    bool mixCompensationEnabled;
    float compensationMaxGain;

};
