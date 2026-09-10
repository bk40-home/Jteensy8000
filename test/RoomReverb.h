// =============================================================================
// RoomReverb.h — short, cheap early-reflection room
// =============================================================================
//
// WHY THIS EXISTS
//   The plate is the expensive algorithm and every other candidate (Hall, and
//   a bigger plate) costs MORE.  Room is the one that goes the other way: it
//   is the option to reach for when the voice count is high and the reverb is
//   what has to give.  It is also the right sound for a small ambience, which
//   the plate has never been good at — a plate at short decay still sounds
//   like a plate, not like a room.
//
// TOPOLOGY, AND WHY IT IS SHAPED LIKE THIS
//   Real rooms carry their DIRECTION in the early reflections and go diffuse
//   (and effectively mono) in the late tail.  The structure follows that:
//
//     in (mono) -> ER delay line -+-> 3 taps -> L
//                                 +-> 3 taps -> R
//                                 +-> tail feed
//
//     tail: one MONO feedback loop (allpass, allpass, damped delay), then one
//           decorrelating allpass per output channel to open it back to stereo.
//
//   A mono tail is not a shortcut for its own sake — it is what a diffuse field
//   actually is — but it does halve the tail's memory traffic against a
//   stereo tank, which is where the CPU saving comes from.
//
// COST, MEASURED IN PSRAM ACCESSES PER SAMPLE
//     Room   1 write + 6 reads (ER)  +  1 write + 1 read (tail)   =  9
//     Plate  2 writes + 2 reads (tank) + 6 taps + 4 (2 mod APFs)  = 14
//   So about 0.64x the plate on memory traffic, plus a larger saving in
//   arithmetic: no pitch shifters, and no MODULATED allpass, which on the
//   plate costs a fractional read and a lerp on every sample.
//
//   NOTE this corrects an earlier estimate of ~0.4x, which assumed no tail
//   feedback at all.  A pure tapped-delay room with no tail sounds like a
//   slapback, not a room, so the tail earns its cost.
//
// ALL DIFFUSION IS IN DTCM
//   The five allpasses total 1053 floats as a member, in the same spirit as
//   the plate keeping its input diffusers off PSRAM: short lines read every
//   sample are exactly the wrong thing to put behind a 100 ns bus.
//
// PARAMETER MAPPING (option P1 — the shared reverb.* controls)
//     size     -> tail feedback AND early-reflection spread
//     damp     -> HF damping inside the tail loop
//     lodamp   -> LF damping inside the tail loop
//     lowpass  -> post-tail master LP (wet only), same as the plate
//     hipass   -> post-tail master HP (wet only), same as the plate
//     shimmer  -> NO-OP.  Deliberately an empty body rather than a silent
//                 partial effect, so "does nothing here" is visible in source.
//     freeze   -> tail feedback to unity, input muted except a small bleed
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>

#include "core/AudioConfig.h"
#include "core/dsp/IReverb.h"
#include "core/dsp/ReverbPrimitives.h"

namespace JT {

class RoomReverb : public IReverb {
public:
    // ---- buffer sizing (samples @ 44.1 kHz) --------------------------------
    // Mutually prime lengths so reflections never line up into a pitched comb.

    // Early-reflection line, ~62 ms.  The six taps below all read from it.
    static constexpr uint32_t kErLen = 2757;

    // Late-tail loop, ~41 ms.  Short on purpose: this is a ROOM.  A longer
    // loop just turns it into a worse plate.
    static constexpr uint32_t kTailLen = 1801;

    // Diffusion allpasses — DTCM members, not pool.
    static constexpr uint32_t kApLen0 = 149;   // tail loop
    static constexpr uint32_t kApLen1 = 211;   // tail loop
    static constexpr uint32_t kApLen2 = 173;   // output decorrelation, L
    static constexpr uint32_t kApLen3 = 239;   // output decorrelation, R
    static constexpr uint32_t kApTotal = kApLen0 + kApLen1 + kApLen2 + kApLen3;  // 772

    // 4558 floats = 17.8 KB, against the plate's 23418 / 91.5 KB.
    static constexpr uint32_t kPoolFloats = kErLen + kTailLen;

    // Six early reflections, three per channel, in samples.  Values are prime
    // and interleaved L/R so the two ears never receive a reflection at the
    // same instant — that simultaneity is what makes a cheap ER cluster
    // collapse to the centre and sound like a delay rather than a space.
    static constexpr uint32_t kTapL[3] = {  277,  1123,  2111 };
    static constexpr uint32_t kTapR[3] = {  409,  1471,  2591 };

    RoomReverb() = default;

    // ---- IReverb ------------------------------------------------------------
    uint32_t    poolFloats() const override { return kPoolFloats; }
    const char* name()       const override { return "room"; }

    void begin(float* pool) override;
    void clearTail() override;

    void setSize(float n)    override;
    void setHiDamp(float n)  override;
    void setLoDamp(float n)  override;
    void setLowpass(float n) override;
    void setHipass(float n)  override;
    void setShimmer(float)   override {}   // no shimmer stage — see the banner
    void setFreeze(bool on)  override;

    void processBlock(float* left, float* right, size_t n,
                      float mixStart, float mixEnd) override;

private:
    // Tap gains fall off with distance and ALTERNATE SIGN.  Alternating
    // polarity is what stops six taps of the same signal summing into a
    // comb-filtered honk on transients.
    static constexpr float kTapGain[3] = { 0.72f, -0.50f, 0.34f };

    // Wet trim.  Six taps plus a tail loop sum well above unity on a
    // transient; this brings the algorithm to roughly the plate's loudness so
    // switching does not also change level.
    static constexpr float kWetScale = 0.34f;

    Rv::DelayLine  _er;
    Rv::DelayLine  _tail;
    Rv::Allpass    _ap[4];
    float          _apBuf[kApTotal] = { 0.0f };   // DTCM member

    Rv::OnePole_LP _tailLP;
    Rv::OnePole_HP _tailHP;
    Rv::OnePole_LP _masterLP[2];
    Rv::OnePole_HP _masterHP[2];

    float _feedback     = 0.45f;   // tail loop gain (size)
    float _erSpread     = 1.0f;    // 0..1 scale on tap distance (size)
    float _tailFb       = 0.0f;    // carried between samples
    float _hiDampCoeff  = 0.3f;
    float _loDampCoeff  = 0.0f;
    float _masterLpCoeff = 0.0f;
    float _masterHpCoeff = 0.0f;
    bool  _frozen        = false;
    float _savedFeedback = 0.45f;

    float* _pool = nullptr;
};

} // namespace JT
