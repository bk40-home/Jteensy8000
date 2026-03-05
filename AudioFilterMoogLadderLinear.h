#pragma once
#include <Arduino.h>
#include "AudioStream.h"

// Linear Moog ladder: 4x identical ZDF one-poles in cascade with feedback.
// Now exposes cutoff/resonance modulators as extra inputs, Audio.h-style.

class AudioFilterMoogLadderLinear : public AudioStream {
public:
  // NOTE: now 3 inputs to support mod buses
  AudioFilterMoogLadderLinear() : AudioStream(3, _inQ) {}

  // Set the target cutoff frequency in Hz.  The value is constrained
  // between 5 Hz and the maximum allowed by _maxCutoffFraction.  Raising
  // _maxCutoffFraction via setMaxCutoffFraction() increases the top end
  // of the filter when fully open.
  void frequency(float hz) {
    float maxHz = AUDIO_SAMPLE_RATE_EXACT * _maxCutoffFraction;
    _fcTarget = constrain(hz, 5.0f, maxHz);
  }
  void resonance(float k)  { _k = max(0.0f, k); }          // self-osc ~ k≈4
  void portamento(float ms){ _portaMs = max(0.0f, ms); }

  // NEW: modulation scaling (same as diode version)
  void setCutoffModOctaves(float oct) { _modOct = max(0.0f, oct); }
  void setResonanceModDepth(float d)  { _resModDepth = max(0.0f, d); }

  virtual void update(void) override;

private:
  audio_block_t* _inQ[3];

  // TPT states
  float _s1=0,_s2=0,_s3=0,_s4=0;

  // last outputs
  float _y1=0,_y2=0,_y3=0,_y4=0;

  // control
  float _fs       = AUDIO_SAMPLE_RATE_EXACT;
  float _fc       = 1000.0f;
  float _fcTarget = 1000.0f;
  float _k        = 0.0f;
  float _portaMs  = 0.0f;

  // feedback guards
  float _dc  = 0.0f;     // DC tracker
  float _env = 0.0f;     // envelope for thresholded safe-k

  // Mod scaling
  float _modOct      = 0.0f; // octaves per +1 on In1
  float _resModDepth = 0.0f; // k units per +1 on In2

  // Maximum cutoff frequency as a fraction of the sampling rate.  The default
  // 0.45 corresponds to allowing the cutoff to sweep up to 45% of fs,
  // similar to the maximum permitted by frequency().  Raising this
  // fraction increases high‑frequency content when the filter is wide open.
  float _maxCutoffFraction = 0.45f;

  // Enable high‑frequency compensation.  When true, the filter cross‑fades
  // between a 4‑pole and a 2‑pole response as the cutoff approaches
  // the top of its range.  This behaviour mimics the JP‑8000’s filter,
  // which does not fully attenuate high frequencies when the cutoff is
  // wide open.  Disable for a pure 4‑pole Moog ladder.
  bool _hfCompensation = false;

public:
  // Set the maximum cutoff frequency as a fraction of the sampling rate.
  // The argument should be in the range (0.0, 0.5].  Values closer to 0.5
  // allow more high‑frequency content when the filter is wide open.  The
  // default is 0.45f.
  void setMaxCutoffFraction(float fraction) {
    // clamp to a sensible range to avoid instability
    if (fraction < 0.01f) fraction = 0.01f;
    if (fraction > 0.5f)  fraction = 0.5f;
    _maxCutoffFraction = fraction;
    // Adjust the target cutoff if it exceeds the new maximum
    if (_fcTarget > AUDIO_SAMPLE_RATE_EXACT * _maxCutoffFraction) {
      _fcTarget = AUDIO_SAMPLE_RATE_EXACT * _maxCutoffFraction;
    }
  }

  // Enable or disable high‑frequency compensation.  When enabled the
  // filter will gradually reduce the order of the ladder from 4 poles to
  // 2 poles as the cutoff approaches the maximum, resulting in a more
  // open sound above 10 kHz.  When disabled (default) the filter remains
  // a 4‑pole ladder at all frequencies.
  void setHighFreqCompensation(bool enable) {
    _hfCompensation = enable;
  }

  inline float cutoffAlpha() const {
    if (_portaMs <= 0.0f) return 1.0f;
    const float tau = _portaMs * 0.001f;
    return 1.0f - expf(-1.0f / (tau * _fs));
  }
};

