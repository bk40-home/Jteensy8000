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
/*
 * GlobalFX.h
 * ==========
 *
 * Shared global effects bus, sitting between the per-layer FX chains and the
 * performance mixer. Today it holds just the PlateReverb — historically this
 * was per-layer inside FXChainBlock, which cost ~10% CPU per instance and
 * gave us ~20% total when both layers were active. Real JP-8000 reverb is
 * global, so moving it up both saves CPU and matches the source material.
 *
 * Naming: GlobalFX (not GlobalReverb) so future global-scope effects can
 * drop in alongside reverb without reshaping clients. Today the class is
 * a thin wrapper around one PlateReverb + stereo send-summing mixers +
 * a stereo wet-out amplifier.
 *
 * SIGNAL FLOW:
 *
 *     Layer A tap L ─┐
 *                    ├─► _sendMixL ─► PlateReverb L ─► _wetAmpL ─► wet out L
 *     Layer B tap L ─┘                              (master wet level)
 *
 *     Layer A tap R ─┐
 *                    ├─► _sendMixR ─► PlateReverb R ─► _wetAmpR ─► wet out R
 *     Layer B tap R ─┘                              (master wet level)
 *
 *   "Layer A/B tap L/R" is the FXChainBlock output mixer (dry + JPFX summed)
 *   — the same stream that also feeds the perf mixer directly. The Teensy
 *   Audio Library lets one AudioStream feed multiple inputs; LayerManager
 *   creates two AudioConnections from the same tap — one into the perf
 *   mixer (dry path), one into GlobalFX (reverb path).
 *
 * PARAMETER MODEL:
 *   All reverb CCs — size, damping, shimmer, freeze, post-tank EQ, bypass,
 *   AND wet mix (FX_REVERB_MIX) — are now GLOBAL. LayerManager intercepts
 *   them in handleControlChange and calls the relevant GlobalFX setter.
 *   Layers have NO knowledge of reverb at all — no per-layer send level,
 *   no per-layer wet mix.
 *
 *   This matches the chosen design: the reverb is a single global resource
 *   and both layers contribute to it at unity amplitude. If you want one
 *   layer drier than the other, adjust that layer's overall volume.
 *
 * OWNERSHIP:
 *   LayerManager owns a GlobalFX instance as a direct member. GlobalFX
 *   exposes input-mixer references for LayerManager to wire each layer's
 *   tap into, plus AudioAmplifier references for the wet return path into
 *   the perf mixer.
 */

#pragma once
#include <Arduino.h>
#include "Audio.h"
#include "AudioEffectPlateReverbJT.h"

// Minimum mixer gain treated as "active" for auto-bypass. Below this the
// reverb tank is bypassed to save CPU. Same threshold FXChainBlock used
// historically — pulled here since GlobalFX now owns that decision.
static constexpr float GLOBAL_REVERB_MIX_THRESHOLD = 0.001f;

class GlobalFX
{
public:
    GlobalFX();
    ~GlobalFX();

    // Call after AudioMemory() has run — creates all AudioConnections.
    // Must happen before the first audio tick, i.e. from setup(), not from
    // a global constructor.
    void begin();

    // =========================================================================
    // SEND INPUTS — LayerManager wires each layer's FX-chain output into these.
    //   Channel 0 = Layer A tap (dry + JPFX summed)
    //   Channel 1 = Layer B tap
    //   Channels 2/3 are spare.
    // Slot gains here stay at unity — summing only, no per-layer scaling.
    // =========================================================================
    AudioMixer4& getReverbSendInputL() { return _sendMixL; }
    AudioMixer4& getReverbSendInputR() { return _sendMixR; }

    // =========================================================================
    // WET OUTPUTS — LayerManager wires these into the perf mixer.
    //
    // The master wet amplifier sits between the reverb tank output and the
    // perf mixer. Its gain is driven by FX_REVERB_MIX (now GLOBAL).
    // LayerManager calls setReverbMix(L, R) to update it — independent L/R
    // so stereo-imbalance tricks stay possible later if wanted. Both values
    // are usually identical.
    // =========================================================================
    AudioAmplifier& getWetOutL() { return _wetAmpL; }
    AudioAmplifier& getWetOutR() { return _wetAmpR; }

    // Direct access to the reverb tank — only useful for debug / metering.
    AudioEffectPlateReverbJT& getReverb() { return _plateReverb; }

    // =========================================================================
    // REVERB PARAMETERS — moved verbatim from FXChainBlock.
    // Every setter clamps at the boundary and mirrors into a cached field
    // so getters are cheap and the auto-bypass logic has local state to
    // consult.
    // =========================================================================

    // Tank size / dampening (affects tail character).
    void  setReverbRoomSize(float size);     // 0..1
    void  setReverbHiDamping(float damp);    // 0..1 — in-tank hi-freq absorption
    void  setReverbLoDamping(float damp);    // 0..1 — in-tank lo-freq absorption
    float getReverbRoomSize()  const { return _roomSize; }
    float getReverbHiDamping() const { return _hiDamp;   }
    float getReverbLoDamping() const { return _loDamp;   }

    // Master wet level (stereo). Per-layer sends feed the tank at unity;
    // this scales the tank's output on its way into the perf mixer.
    // Also drives the auto-bypass check: if both channels drop below
    // GLOBAL_REVERB_MIX_THRESHOLD the tank bypasses to save CPU.
    void  setReverbMix(float left, float right);
    float getReverbMixL() const { return _mixL; }
    float getReverbMixR() const { return _mixR; }

    // Manual bypass override (in addition to the auto-bypass on mix=0).
    void setReverbBypass(bool bypass);
    bool getReverbBypass() const { return _manualBypass; }

    // Extended controls — pitch-shifted feedback, infinite-hold, post-tank EQ.
    void  setReverbShimmer(float amount);    // 0..1
    void  setReverbFreeze(bool frozen);
    void  setReverbLowpass(float amount);    // 0..1 post-tank LP on wet
    void  setReverbHipass(float amount);     // 0..1 post-tank HP on wet
    float getReverbShimmer() const { return _shimmer;  }
    bool  getReverbFreeze()  const { return _frozen;   }
    float getReverbLowpass() const { return _lowpass;  }
    float getReverbHipass()  const { return _hipass;   }

private:
    // =========================================================================
    // AUDIO OBJECTS
    // =========================================================================
    AudioEffectPlateReverbJT _plateReverb;
    AudioMixer4              _sendMixL;  // sums Layer A + Layer B taps, stereo L
    AudioMixer4              _sendMixR;  // sums Layer A + Layer B taps, stereo R
    AudioAmplifier           _wetAmpL;   // master wet level, L (FX_REVERB_MIX)
    AudioAmplifier           _wetAmpR;   // master wet level, R

    // =========================================================================
    // AUDIO CONNECTIONS — heap-allocated so we wire in begin() not ctor.
    // =========================================================================
    AudioConnection* _patchSendLToReverb = nullptr;
    AudioConnection* _patchSendRToReverb = nullptr;
    AudioConnection* _patchReverbToWetL  = nullptr;
    AudioConnection* _patchReverbToWetR  = nullptr;

    // =========================================================================
    // CACHED PARAMETER STATE (same pattern FXChainBlock used)
    // =========================================================================
    float _roomSize     = 0.5f;
    float _hiDamp       = 0.5f;
    float _loDamp       = 0.5f;
    float _mixL         = 0.0f;   // starts silent; preset/CC raises it
    float _mixR         = 0.0f;
    bool  _manualBypass = false;
    float _shimmer      = 0.0f;
    bool  _frozen       = false;
    float _lowpass      = 0.0f;
    float _hipass       = 0.0f;

    // Re-evaluate the tank's bypass state after any influencing change.
    void updateReverbBypass();
};
