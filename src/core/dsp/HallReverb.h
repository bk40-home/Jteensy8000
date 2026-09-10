// =============================================================================
// HallReverb.h — 4x4 feedback delay network with a Householder mixing matrix
// =============================================================================
//
// WHY AN FDN RATHER THAN A BIGGER PLATE
//   A plate is two coupled delays; lengthening them makes the ringing more
//   obvious, not the space bigger.  A hall needs many mutually prime paths
//   recombining, which is exactly what a feedback delay network is.  Four
//   lines is the smallest N that sounds like a room rather than a comb, and
//   it is also the size at which the Householder matrix collapses to almost
//   no arithmetic (see below).
//
// THE HOUSEHOLDER MATRIX, AND WHY IT IS FREE
//   The general mixing matrix for an FDN is N x N — 16 multiply-accumulates
//   for N=4.  The Householder form H = I - (2/N)*J, with J the all-ones
//   matrix, reduces to:
//
//       s      = 0.5 * (d0 + d1 + d2 + d3)
//       out_i  = d_i - s
//
//   Three adds, one multiply, four subtracts — total, for the whole matrix.
//   It is also ORTHOGONAL, so the matrix itself is lossless and the decay is
//   set purely by the feedback gain.  That separation is what makes `size`
//   behave predictably here: nothing else in the loop is throwing away energy.
//
// COST, IN PSRAM ACCESSES PER SAMPLE
//     Hall   4 writes + 4 reads                                   =  8
//     Room   1 write + 6 reads (ER) + 1 write + 1 read (tail)     =  9
//     Plate  2 writes + 2 reads + 6 taps + 4 (2 modulated APFs)   = 14
//
//   So the hall is the cheapest of the three on memory TRAFFIC — the plate's
//   six output taps and its two modulated allpasses (each a fractional read
//   plus a lerp) cost more than four plain delay lines do.
//
//   BUT: measured on the host it comes out at 0.98x the plate, essentially a
//   wash.  That is not a contradiction — the host has no PSRAM, so the
//   benchmark measures ARITHMETIC only, and the hall does more of it (four
//   input allpasses, eight damping filters, the matrix).  On Teensy, where a
//   scattered PSRAM read is 80-150 ns against ~10-15 ns sequential, the 8-vs-14
//   access difference should push the hall clearly below the plate.
//
//   TREAT 0.98x AS AN UPPER BOUND, NOT THE ANSWER.  The only figure that
//   settles it is synthCPUmax on real hardware with each algorithm selected.
//
// WHY THERE IS NO LINE MODULATION
//   Many FDN halls modulate their line lengths slightly to break up metallic
//   ringing.  That turns every read into a fractional read plus an
//   interpolation — the exact cost the plate pays and this algorithm avoids.
//   Mutually prime line lengths plus four input allpasses give enough density
//   without it.  If ringing ever becomes audible on a long decay, modulating
//   ONE line is the cheapest fix and is a deliberate later option, not an
//   oversight.
//
// PARAMETER MAPPING (option P1 — the shared reverb.* controls)
//     size     -> feedback gain, i.e. decay time
//     damp     -> HF damping, one-pole LP per line INSIDE the loop
//     lodamp   -> LF damping, one-pole HP per line INSIDE the loop
//     lowpass  -> post master LP (wet only), same as plate and room
//     hipass   -> post master HP (wet only)
//     shimmer  -> NO-OP, empty body (see RoomReverb.h for the reasoning)
//     freeze   -> feedback to unity; the matrix is lossless, so this really
//                 does hold indefinitely rather than decaying slowly
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>

#include "core/AudioConfig.h"
#include "core/dsp/IReverb.h"
#include "core/dsp/ReverbPrimitives.h"

namespace JT {

class HallReverb : public IReverb {
public:
    static constexpr uint8_t kLines = 4;

    // Mutually prime line lengths, 54 .. 114 ms.  Primality matters more here
    // than in the plate: any common factor between two lines puts their echoes
    // on a shared grid and the network rings at that period.
    static constexpr uint32_t kLineLen[kLines] = { 2381, 3137, 4013, 5011 };

    // Input diffusion allpasses — DTCM members, not pool.  Four lines alone
    // have thin echo density in the first few milliseconds; these fill it in.
    static constexpr uint32_t kApLen[4] = { 131, 199, 293, 367 };
    static constexpr uint32_t kApTotal  = 131 + 199 + 293 + 367;   // 990

    // 14542 floats = 56.8 KB, against the plate's 23418 / 91.5 KB.
    static constexpr uint32_t kPoolFloats = 2381 + 3137 + 4013 + 5011;

    HallReverb() = default;

    // ---- IReverb ------------------------------------------------------------
    uint32_t    poolFloats() const override { return kPoolFloats; }
    const char* name()       const override { return "hall"; }

    void begin(float* pool) override;
    void clearTail() override;

    void setSize(float n)    override;
    void setHiDamp(float n)  override;
    void setLoDamp(float n)  override;
    void setLowpass(float n) override;
    void setHipass(float n)  override;
    void setShimmer(float)   override {}   // no shimmer stage
    void setFreeze(bool on)  override;

    void processBlock(float* left, float* right, size_t n,
                      float mixStart, float mixEnd) override;

private:
    // Householder scale for N=4: 2/N == 0.5.  Named rather than inlined so the
    // relationship to kLines is visible if the network is ever widened to 8.
    static constexpr float kHouseholder = 2.0f / (float)kLines;

    // Wet trim, chosen so a switch between algorithms does not also change
    // level.  Four summed lines run hotter than the plate's tapped pair.
    static constexpr float kWetScale = 0.30f;

    static constexpr float kApGain = 0.58f;

    Rv::DelayLine  _line[kLines];
    Rv::OnePole_LP _lineLP[kLines];
    Rv::OnePole_HP _lineHP[kLines];

    Rv::Allpass    _ap[4];
    float          _apBuf[kApTotal] = { 0.0f };   // DTCM member

    Rv::OnePole_LP _masterLP[2];
    Rv::OnePole_HP _masterHP[2];

    // Feedback carried between samples — the network's state.
    float _fb[kLines] = { 0.0f, 0.0f, 0.0f, 0.0f };

    float _decay         = 0.75f;
    float _hiDampCoeff   = 0.3f;
    float _loDampCoeff   = 0.0f;
    float _masterLpCoeff = 0.0f;
    float _masterHpCoeff = 0.0f;
    bool  _frozen        = false;
    float _savedDecay    = 0.75f;

    float* _pool = nullptr;
};

} // namespace JT
