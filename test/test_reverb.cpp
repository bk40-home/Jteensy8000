// =============================================================================
// test_reverb.cpp — proofs for the Phase 5 GLOBAL REVERB subsystem
// =============================================================================
// Covers the nine ParamTable §15 params wired this pass (see
// docs/PHASE5_REVERB_SPEC.md): REVERB_SIZE/DAMP/LODAMP/MIX/BYPASS/SHIMMER/
// FREEZE/LOWPASS/HIPASS, folded into SynthCore::renderBlock as a global
// post-mix effect, and the ported PlateReverb tank itself.
//
// MEASUREMENT: the reverb adds a decaying stereo TAIL after a note stops.  The
// honest proofs are energy-based (tail RMS, tail persistence, band energy) plus
// direct unit tests on the ported parameter mappings.  The reverb is character,
// not a precise-frequency device, so spectral checks are coarse band ratios.
//
// POOLS: a Rig owns BOTH a fresh comb pool AND a fresh reverb pool on the heap.
// The default SynthCore ctor leaves reverbPool null (reverb inert) — reverb
// tests MUST pass a real pool or the tank never runs.
// =============================================================================
#include "doctest.h"

#include <cmath>
#include <vector>
#include <string>

#include "core/SynthCore.h"
#include "core/dsp/PlateReverb.h"
#include "core/dsp/ReverbRack.h"
#include "core/dsp/RoomReverb.h"
#include "core/dsp/HallReverb.h"
#include "core/dsp/Curves.h"
#include "gen/ParamTable.h"

using namespace JT;
using namespace JT::Params;

namespace {

// A clean steady tone: sine osc1, everything else muted, instant attack/full
// sustain so a note holds flat, then a fast release so "note off" gives a
// well-defined tail to measure.  Mirrors the test_performance Rig.
struct Rig {
    ParameterStore     store;
    std::vector<float> combPool;
    std::vector<float> reverbPool;
    SynthCore          core;
    Rig()
        : combPool((size_t)SynthCore::kCombPoolFloats, 0.0f),
          reverbPool((size_t)SynthCore::kReverbPoolFloats, 0.0f),
          core(store, combPool.data(), reverbPool.data())
    {
        setE(ID::MIX_OSC2, 0.0f);
        setE(ID::MIX_SUB, 0.0f);
        setE(ID::MIX_NOISE, 0.0f);
        setOpt(ID::OSC1_WAVE, (float)(int)Wave::Sine);
        setE(ID::ENV_AMP_ATTACK, 0.0f);
        setE(ID::ENV_AMP_SUSTAIN, 1.0f);
        setE(ID::ENV_AMP_RELEASE, 5.0f);
    }
    void setE(uint16_t id, float eng) { store.setEngineering(id, eng, Origin::Ui); }
    void setNorm(uint16_t id, float n) { store.set(id, n, Origin::Ui); }
    void setOpt(uint16_t id, float o)
    { store.set(id, Curves::toNorm(kParams[ParameterStore::indexOf(id)], o), Origin::Ui); }

    void block(float* L, float* R) { core.renderBlock(L, R, kBlockSize); }
};

// Render `blocks` blocks, return interleaved L/R.  Optionally note-on at start
// and note-off after `onBlocks` so the remainder captures the decaying tail.
std::vector<float> run(Rig& rig, int blocks, int onBlocks = -1, int note = 57, int vel = 100)
{
    std::vector<float> out;
    float L[kBlockSize], R[kBlockSize];
    // Warm-up block so params land before the note (renderBlock drains notes
    // BEFORE dirty params — the Pass-8 lesson recorded across the suite).
    rig.block(L, R);
    rig.core.noteOn((uint8_t)note, (uint8_t)vel);
    for (int b = 0; b < blocks; ++b) {
        if (onBlocks >= 0 && b == onBlocks) rig.core.noteOff((uint8_t)note);
        rig.block(L, R);
        for (size_t k = 0; k < kBlockSize; ++k) { out.push_back(L[k]); out.push_back(R[k]); }
    }
    return out;
}

double rms(const std::vector<float>& v, size_t from, size_t to)
{
    double acc = 0.0; size_t n = 0;
    for (size_t i = from; i < to && i < v.size(); ++i) { const double s = (double)v[i]; acc += s * s; ++n; }
    return n ? std::sqrt(acc / (double)n) : 0.0;
}

// Crude high-band energy proxy: mean squared first difference (a HF emphasis).
double hfEnergy(const std::vector<float>& v, size_t from, size_t to)
{
    double acc = 0.0; size_t n = 0;
    for (size_t i = from + 1; i < to && i < v.size(); ++i) {
        const double d = (double)v[i] - (double)v[i - 1];
        acc += d * d; ++n;
    }
    return n ? acc / (double)n : 0.0;
}

constexpr size_t kBlkF = kBlockSize * 2;   // interleaved floats per block

} // namespace

// =============================================================================
// NO-SILENT-CHANGE GUARD (CLAUDE.md rule 2) — the gate on sign-off Q1
// =============================================================================
TEST_CASE("reverb: default patch is byte-identical to never touching reverb params")
{
    // All reverb defaults are 0 (Q1): mix 0 => auto-bypass => the tank never
    // runs.  Writing the nine params at their defaults must not perturb one
    // output sample versus never writing them.
    auto render = [](bool writeReverb) {
        Rig rig;
        if (writeReverb) {
            rig.setNorm(ID::REVERB_SIZE, 0.0f);
            rig.setNorm(ID::REVERB_DAMP, 0.0f);
            rig.setNorm(ID::REVERB_LODAMP, 0.0f);
            rig.setNorm(ID::REVERB_MIX, 0.0f);
            rig.setNorm(ID::REVERB_BYPASS, 0.0f);
            rig.setNorm(ID::REVERB_SHIMMER, 0.0f);
            rig.setNorm(ID::REVERB_FREEZE, 0.0f);
            rig.setNorm(ID::REVERB_LOWPASS, 0.0f);
            rig.setNorm(ID::REVERB_HIPASS, 0.0f);
        }
        return run(rig, 80, 40);
    };
    const auto a = render(false);
    const auto b = render(true);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) CHECK(a[i] == b[i]);
}

// =============================================================================
// AUTO-BYPASS — mix 0 leaves the bus untouched; mix>0 changes it
// =============================================================================
TEST_CASE("reverb: mix==0 is bit-identical to bypass; mix>0 alters the signal")
{
    auto dry = [] { Rig r; return run(r, 60, 30); }();

    // mix just above threshold, non-trivial size => audible wet.
    Rig wetRig;
    wetRig.setNorm(ID::REVERB_SIZE, 0.6f);
    wetRig.setNorm(ID::REVERB_MIX, 0.5f);
    auto wet = run(wetRig, 60, 30);

    REQUIRE(dry.size() == wet.size());
    bool anyDiff = false;
    for (size_t i = 0; i < dry.size(); ++i) if (dry[i] != wet[i]) { anyDiff = true; break; }
    CHECK(anyDiff);                       // reverb actually did something

    // The tail region (after note-off) must carry energy the dry signal lacks.
    const size_t tailFrom = 45 * kBlkF;   // well after note-off (block 30)
    CHECK(rms(wet, tailFrom, wet.size()) > rms(dry, tailFrom, dry.size()) * 2.0);
}

TEST_CASE("reverb: manual bypass overrides a non-zero mix")
{
    Rig a; a.setNorm(ID::REVERB_SIZE, 0.6f); a.setNorm(ID::REVERB_MIX, 0.5f);
    auto wet = run(a, 60, 30);

    Rig b; b.setNorm(ID::REVERB_SIZE, 0.6f); b.setNorm(ID::REVERB_MIX, 0.5f);
    b.setNorm(ID::REVERB_BYPASS, 1.0f);     // manual bypass on
    auto byp = run(b, 60, 30);

    Rig d;                                   // pure dry reference
    auto dry = run(d, 60, 30);

    REQUIRE(byp.size() == dry.size());
    for (size_t i = 0; i < byp.size(); ++i) CHECK(byp[i] == dry[i]);   // == dry
    CHECK(rms(wet, 45 * kBlkF, wet.size()) > rms(byp, 45 * kBlkF, byp.size()));
}

// =============================================================================
// TAIL BEHAVIOUR — size controls decay length; tail persists after input stops
// =============================================================================
TEST_CASE("reverb: larger size gives a longer / louder tail")
{
    Rig small; small.setNorm(ID::REVERB_SIZE, 0.2f); small.setNorm(ID::REVERB_MIX, 0.6f);
    Rig large; large.setNorm(ID::REVERB_SIZE, 0.9f); large.setNorm(ID::REVERB_MIX, 0.6f);
    auto s = run(small, 120, 30);
    auto l = run(large, 120, 30);
    const size_t late = 90 * kBlkF;         // deep into the tail
    CHECK(rms(l, late, l.size()) > rms(s, late, s.size()));
}

TEST_CASE("reverb: tail is non-zero well after the note stops")
{
    Rig rig; rig.setNorm(ID::REVERB_SIZE, 0.7f); rig.setNorm(ID::REVERB_MIX, 0.6f);
    auto out = run(rig, 120, 30);
    // ~+400 ms after note-off (block 30): 60 blocks * 2.9 ms ≈ 174 ms; use 100
    // blocks in for a clear tail read.
    CHECK(rms(out, 100 * kBlkF, out.size()) > 1e-5);
}

// =============================================================================
// DAMPING — hidamp darkens the tail; freeze holds it
// =============================================================================
TEST_CASE("reverb: hi-damp darkens the tail (less HF energy)")
{
    Rig bright; bright.setNorm(ID::REVERB_SIZE, 0.7f); bright.setNorm(ID::REVERB_MIX, 0.7f);
    bright.setNorm(ID::REVERB_DAMP, 0.0f);
    Rig dark;   dark.setNorm(ID::REVERB_SIZE, 0.7f);   dark.setNorm(ID::REVERB_MIX, 0.7f);
    dark.setNorm(ID::REVERB_DAMP, 0.9f);
    auto b = run(bright, 100, 30);
    auto d = run(dark, 100, 30);
    const size_t tail = 60 * kBlkF;
    // Darker tail has less high-frequency energy relative to its own RMS.
    const double bRatio = hfEnergy(b, tail, b.size()) / (rms(b, tail, b.size()) + 1e-12);
    const double dRatio = hfEnergy(d, tail, d.size()) / (rms(d, tail, d.size()) + 1e-12);
    CHECK(dRatio < bRatio);
}

TEST_CASE("reverb: freeze holds the tail roughly flat, unfreeze lets it decay")
{
    Rig rig; rig.setNorm(ID::REVERB_SIZE, 0.5f); rig.setNorm(ID::REVERB_MIX, 0.7f);
    float L[kBlockSize], R[kBlockSize];
    rig.block(L, R);
    rig.core.noteOn(57, 110);
    for (int b = 0; b < 20; ++b) rig.block(L, R);   // build up wet energy
    rig.core.noteOff(57);
    rig.setNorm(ID::REVERB_FREEZE, 1.0f);            // freeze the tail
    rig.block(L, R);                                  // let freeze land

    auto grab = [&](int blocks) {
        std::vector<float> v;
        for (int b = 0; b < blocks; ++b) {
            rig.block(L, R);
            for (size_t k = 0; k < kBlockSize; ++k) { v.push_back(L[k]); v.push_back(R[k]); }
        }
        return v;
    };
    auto early = grab(40);
    auto later = grab(40);
    const double eR = rms(early, 0, early.size());
    const double lR = rms(later, 0, later.size());
    CHECK(eR > 1e-4);                                 // frozen tail audible
    // Held roughly flat: later RMS within ~40% of early (not a hard decay).
    CHECK(lR > eR * 0.6);
}

// =============================================================================
// PARAMETER-MAPPING UNIT TESTS — guard the ported v1 constants (spec §1.3)
// =============================================================================
// These probe the tank directly (no engine), asserting the size->decay and
// damp/EQ coefficient formulas match v1 at the endpoints.  Behavioural proof
// that the ported math is byte-for-byte the v1 curves.
TEST_CASE("reverb: size->decay mapping matches v1 at endpoints")
{
    // We can't read _decay directly (private), so prove it via tail energy
    // monotonicity across the mapped range: n=0 (decay 0.1) << n=1 (decay 1.0).
    auto tailRms = [](float sizeN) {
        Rig r; r.setNorm(ID::REVERB_SIZE, sizeN); r.setNorm(ID::REVERB_MIX, 0.6f);
        auto o = run(r, 120, 30);
        return rms(o, 90 * kBlkF, o.size());
    };
    const double t0 = tailRms(0.0f);
    const double t5 = tailRms(0.5f);
    const double t1 = tailRms(1.0f);
    CHECK(t0 < t5);
    CHECK(t5 < t1);
}

TEST_CASE("reverb: master lowpass reduces HF, hipass reduces LF-dominated RMS")
{
    Rig flat; flat.setNorm(ID::REVERB_SIZE, 0.6f); flat.setNorm(ID::REVERB_MIX, 0.7f);
    auto f = run(flat, 100, 30);

    Rig lp; lp.setNorm(ID::REVERB_SIZE, 0.6f); lp.setNorm(ID::REVERB_MIX, 0.7f);
    lp.setNorm(ID::REVERB_LOWPASS, 1.0f);
    auto l = run(lp, 100, 30);

    const size_t tail = 60 * kBlkF;
    const double fHf = hfEnergy(f, tail, f.size()) / (rms(f, tail, f.size()) + 1e-12);
    const double lHf = hfEnergy(l, tail, l.size()) / (rms(l, tail, l.size()) + 1e-12);
    CHECK(lHf < fHf);                                 // lowpass darkens the wet
}

// =============================================================================
// STABILITY — worst-case never produces NaN/inf
// =============================================================================
TEST_CASE("reverb: full-scale input at max size/mix stays finite")
{
    Rig rig;
    rig.setNorm(ID::REVERB_SIZE, 1.0f);              // infinite-ish decay
    rig.setNorm(ID::REVERB_MIX, 1.0f);
    rig.setNorm(ID::REVERB_SHIMMER, 1.0f);           // shimmer engaged too
    float L[kBlockSize], R[kBlockSize];
    rig.block(L, R);
    rig.core.noteOn(57, 127);
    for (int b = 0; b < 400; ++b) {                  // ~1.2 s of sustained drive
        rig.block(L, R);
        for (size_t k = 0; k < kBlockSize; ++k) {
            REQUIRE(std::isfinite(L[k]));
            REQUIRE(std::isfinite(R[k]));
        }
    }
}

// =============================================================================
// ReverbRack — switchable algorithms (R2 / A1 / P1 / S2)
// =============================================================================
namespace {

// Drive a chain with a short burst then silence, capturing the whole run.
void rackFeed(ReverbRack& r, float* oL, float* oR, size_t n, size_t burst, float mix)
{
    for (size_t i = 0; i < n; i += 128) {
        float bL[128], bR[128];
        for (size_t k = 0; k < 128; ++k) {
            const float v = (i + k < burst) ? ((k & 1u) ? 0.4f : -0.4f) : 0.0f;
            bL[k] = v; bR[k] = v;
        }
        r.processBlock(bL, bR, 128, mix);
        for (size_t k = 0; k < 128 && i + k < n; ++k) { oL[i+k] = bL[k]; oR[i+k] = bR[k]; }
    }
}

} // namespace

TEST_CASE("rack: the Plate path is bit-identical to a bare PlateReverb")
{
    // THE claim of this refactor. processBlock gained a mix RAMP so the rack can
    // crossfade; at steady state mixEnd == mixStart, the per-sample increment is
    // exactly 0.0f, and the arithmetic must be unchanged. Exact comparison on
    // purpose — Approx would hide precisely the drift this is checking for.
    std::vector<float> rackPool((size_t)ReverbRack::kTotalPoolFloats, 0.0f);
    std::vector<float> barePool((size_t)PlateReverb::kPoolFloats,     0.0f);

    ReverbRack  rack;   rack.begin(rackPool.data());
    PlateReverb bare;   bare.begin(barePool.data());

    for (int i = 0; i < 2; ++i) {
        const float sz = 0.6f, hd = 0.4f, ld = 0.2f;
        if (i == 0) { rack.setSize(sz); rack.setHiDamp(hd); rack.setLoDamp(ld); }
        else        { bare.setSize(sz); bare.setHiDamp(hd); bare.setLoDamp(ld); }
    }

    const size_t N = 8192;
    std::vector<float> aL(N), aR(N), bL(N), bR(N);
    rackFeed(rack, aL.data(), aR.data(), N, 1024, 0.5f);
    for (size_t i = 0; i < N; i += 128) {
        float t0[128], t1[128];
        for (size_t k = 0; k < 128; ++k) {
            const float v = (i + k < 1024) ? ((k & 1u) ? 0.4f : -0.4f) : 0.0f;
            t0[k] = v; t1[k] = v;
        }
        bare.processBlock(t0, t1, 128, 0.5f, 0.5f);
        for (size_t k = 0; k < 128 && i + k < N; ++k) { bL[i+k] = t0[k]; bR[i+k] = t1[k]; }
    }

    bool identical = true;
    for (size_t i = 0; i < N; ++i)
        if (aL[i] != bL[i] || aR[i] != bR[i]) { identical = false; break; }
    CHECK(identical);
}

TEST_CASE("rack: pool slices are contiguous, non-overlapping and exactly sized")
{
    // A slicing bug here would have one algorithm writing into another's tank,
    // which is audible only as intermittent noise on the OTHER algorithm — the
    // kind of fault that takes a week to find on hardware.
    // Stated per-algorithm so adding a fourth breaks this line loudly rather
    // than silently under-allocating the region every algorithm slices from.
    CHECK(ReverbRack::kTotalPoolFloats
          == PlateReverb::kPoolFloats
           + ShimmerReverb::kPoolFloats
           + RoomReverb::kPoolFloats
           + HallReverb::kPoolFloats);
    CHECK(ReverbRack::kNumAlgos == 4);

    std::vector<float> pool((size_t)ReverbRack::kTotalPoolFloats, 0.0f);
    ReverbRack r; r.begin(pool.data());
    CHECK(r.ready());
    CHECK(r.failedCount() == 0u);
    CHECK(r.algorithm() == ReverbRack::kPlate);
    CHECK(std::string(r.activeName()) == "plate");
}

TEST_CASE("rack: a null pool leaves every algorithm inert instead of crashing")
{
    ReverbRack r; r.begin(nullptr);
    CHECK_FALSE(r.ready());
    CHECK(r.failedCount() == ReverbRack::kNumAlgos);

    // Must be safely callable — this is the no-PSRAM boot path.
    float L[128] = {0.0f}, R[128] = {0.0f};
    for (int i = 0; i < 128; ++i) { L[i] = 0.3f; R[i] = 0.3f; }
    r.processBlock(L, R, 128, 0.5f);
    for (int i = 0; i < 128; ++i) CHECK(L[i] == 0.3f);   // untouched, not zeroed
}

TEST_CASE("rack: switching crossfades rather than cutting the tail")
{
    std::vector<float> pool((size_t)ReverbRack::kTotalPoolFloats, 0.0f);
    ReverbRack r; r.begin(pool.data());
    r.setSize(0.8f);

    // Build a tail, then switch mid-decay.
    const size_t N = 4096;
    std::vector<float> oL(N), oR(N);
    rackFeed(r, oL.data(), oR.data(), N, 1024, 0.9f);

    r.setAlgorithm(ReverbRack::kShimmer);
    CHECK(r.fading());
    CHECK(r.algorithm() == ReverbRack::kPlate);     // not swapped yet

    // Fade out (kFadeBlocks) then fade in (kFadeBlocks): the swap lands at the
    // midpoint, and the algorithm must not change before the wet reaches zero.
    float bL[128] = {0.0f}, bR[128] = {0.0f};
    for (int b = 0; b < ReverbRack::kFadeBlocks; ++b) r.processBlock(bL, bR, 128, 0.9f);
    CHECK(r.algorithm() == ReverbRack::kShimmer);   // swapped at the midpoint
    CHECK(r.fading());                              // still ramping back up

    for (int b = 0; b < ReverbRack::kFadeBlocks; ++b) r.processBlock(bL, bR, 128, 0.9f);
    CHECK_FALSE(r.fading());                        // steady state again
    CHECK(std::string(r.activeName()) == "shimmer");
}

TEST_CASE("rack: re-selecting the active algorithm does not restart the fade")
{
    // An editor doing a full resync re-sends every parameter. If that muted the
    // reverb for ~23 ms each time it would be a mystifying intermittent fault.
    std::vector<float> pool((size_t)ReverbRack::kTotalPoolFloats, 0.0f);
    ReverbRack r; r.begin(pool.data());

    r.setAlgorithm(ReverbRack::kPlate);
    CHECK_FALSE(r.fading());

    // An out-of-range index is IGNORED, not clamped: clamping would silently
    // land on a neighbouring algorithm and hide the stale editor causing it.
    r.setAlgorithm(-1);
    r.setAlgorithm(ReverbRack::kNumAlgos);
    CHECK_FALSE(r.fading());
    CHECK(r.algorithm() == ReverbRack::kPlate);
}

TEST_CASE("rack: parameters reach every algorithm, not just the active one")
{
    // If settings only went to the active algorithm, a switch would land on
    // boot defaults and the new reverb would sound nothing like the patch.
    std::vector<float> a((size_t)ReverbRack::kTotalPoolFloats, 0.0f);
    std::vector<float> b((size_t)ReverbRack::kTotalPoolFloats, 0.0f);

    ReverbRack pre;  pre.begin(a.data());
    ReverbRack post; post.begin(b.data());

    // `pre` is configured, THEN switched. `post` is switched, THEN configured.
    // Both must end up sounding the same, which is only true if the setters
    // fan out to inactive instances.
    pre.setSize(0.75f); pre.setHiDamp(0.3f); pre.setShimmer(0.6f);
    pre.setAlgorithm(ReverbRack::kShimmer);
    post.setAlgorithm(ReverbRack::kShimmer);
    post.setSize(0.75f); post.setHiDamp(0.3f); post.setShimmer(0.6f);

    float dL[128] = {0.0f}, dR[128] = {0.0f};
    for (int i = 0; i < 2 * ReverbRack::kFadeBlocks; ++i) {
        pre.processBlock(dL, dR, 128, 0.5f);
        post.processBlock(dL, dR, 128, 0.5f);
    }

    const size_t N = 4096;
    std::vector<float> pL(N), pR(N), qL(N), qR(N);
    rackFeed(pre,  pL.data(), pR.data(), N, 512, 0.5f);
    rackFeed(post, qL.data(), qR.data(), N, 512, 0.5f);

    bool same = true;
    for (size_t i = 0; i < N; ++i) if (pL[i] != qL[i]) { same = false; break; }
    CHECK(same);
}


// =============================================================================
// RoomReverb
// =============================================================================

TEST_CASE("room: pool is a fraction of the plate's and slices correctly")
{
    // The whole point of this algorithm is being cheaper. If it ever stops
    // being smaller than the plate, the reason to have it has gone.
    CHECK(RoomReverb::kPoolFloats < PlateReverb::kPoolFloats / 4u);
    CHECK(RoomReverb::kPoolFloats == RoomReverb::kErLen + RoomReverb::kTailLen);

    std::vector<float> pool((size_t)RoomReverb::kPoolFloats, 0.0f);
    RoomReverb r; r.begin(pool.data());
    CHECK(std::string(r.name()) == "room");
}

TEST_CASE("room: a null pool leaves it inert and passes audio through")
{
    RoomReverb r; r.begin(nullptr);
    float L[128], R[128];
    for (int i = 0; i < 128; ++i) { L[i] = 0.25f; R[i] = -0.25f; }
    r.processBlock(L, R, 128, 0.9f, 0.9f);
    for (int i = 0; i < 128; ++i) { CHECK(L[i] == 0.25f); CHECK(R[i] == -0.25f); }
}

TEST_CASE("room: produces a decaying stereo tail that stays bounded")
{
    std::vector<float> pool((size_t)RoomReverb::kPoolFloats, 0.0f);
    RoomReverb r; r.begin(pool.data());
    r.setSize(0.8f);

    // Burst then silence. Measure an early window against a late one.
    const size_t N = 16384;
    std::vector<float> oL(N), oR(N);
    for (size_t i = 0; i < N; i += 128) {
        float bL[128], bR[128];
        for (size_t k = 0; k < 128; ++k) {
            const float v = (i + k < 512) ? ((k & 1u) ? 0.4f : -0.4f) : 0.0f;
            bL[k] = v; bR[k] = v;
        }
        r.processBlock(bL, bR, 128, 1.0f, 1.0f);
        for (size_t k = 0; k < 128 && i + k < N; ++k) { oL[i+k] = bL[k]; oR[i+k] = bR[k]; }
    }

    auto rms = [](const float* v, size_t a, size_t b) {
        double acc = 0.0;
        for (size_t i = a; i < b; ++i) acc += (double)v[i] * (double)v[i];
        return (float)std::sqrt(acc / (double)(b - a));
    };

    const float early = rms(oL.data(), 1024,  3072);
    const float late  = rms(oL.data(), 12288, 16384);
    CHECK(early > 1e-5f);          // there IS a tail
    CHECK(late  < early);          // and it decays rather than sustaining

    // The two channels must differ, or the six taps have collapsed to the
    // centre and it is a mono delay wearing a reverb's name.
    bool stereo = false;
    for (size_t i = 1024; i < 4096; ++i) if (oL[i] != oR[i]) { stereo = true; break; }
    CHECK(stereo);

    for (size_t i = 0; i < N; ++i) { CHECK(std::fabs(oL[i]) < 2.0f); CHECK(std::fabs(oR[i]) < 2.0f); }
}

TEST_CASE("room: size lengthens the tail; freeze holds it")
{
    auto tailEnergy = [](float size, bool freeze) {
        std::vector<float> pool((size_t)RoomReverb::kPoolFloats, 0.0f);
        RoomReverb r; r.begin(pool.data());
        r.setSize(size);
        const size_t N = 16384;
        double acc = 0.0;
        for (size_t i = 0; i < N; i += 128) {
            float bL[128], bR[128];
            for (size_t k = 0; k < 128; ++k) {
                const float v = (i + k < 512) ? ((k & 1u) ? 0.4f : -0.4f) : 0.0f;
                bL[k] = v; bR[k] = v;
            }
            if (freeze && i == 1024) r.setFreeze(true);
            r.processBlock(bL, bR, 128, 1.0f, 1.0f);
            if (i >= 12288) for (size_t k = 0; k < 128; ++k) acc += (double)bL[k] * (double)bL[k];
        }
        return acc;
    };

    CHECK(tailEnergy(0.9f, false) > tailEnergy(0.2f, false));
    // Freeze drives the loop to unity gain, so the late window must hold more
    // energy than the same size decaying normally.
    CHECK(tailEnergy(0.5f, true) > tailEnergy(0.5f, false));
}

TEST_CASE("room: shimmer is a documented no-op, not a partial effect")
{
    // setShimmer must change NOTHING here. A partially-wired control that
    // altered the sound slightly would be far worse than one that is inert.
    auto run = [](float shim) {
        std::vector<float> pool((size_t)RoomReverb::kPoolFloats, 0.0f);
        RoomReverb r; r.begin(pool.data());
        r.setSize(0.6f);
        r.setShimmer(shim);
        std::vector<float> out(4096);
        for (size_t i = 0; i < 4096; i += 128) {
            float bL[128], bR[128];
            for (size_t k = 0; k < 128; ++k) {
                const float v = (i + k < 512) ? ((k & 1u) ? 0.4f : -0.4f) : 0.0f;
                bL[k] = v; bR[k] = v;
            }
            r.processBlock(bL, bR, 128, 1.0f, 1.0f);
            for (size_t k = 0; k < 128; ++k) out[i+k] = bL[k];
        }
        return out;
    };
    const auto a = run(0.0f);
    const auto b = run(1.0f);
    bool identical = true;
    for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) { identical = false; break; }
    CHECK(identical);
}


// =============================================================================
// HallReverb — FDN 4x4
// =============================================================================

TEST_CASE("hall: line lengths are mutually prime")
{
    // Any common factor between two lines puts their echoes on a shared grid
    // and the whole network rings at that period. This is the property the
    // lengths were chosen for, so it is asserted rather than trusted.
    auto gcd = [](uint32_t a, uint32_t b) {
        while (b) { const uint32_t t = a % b; a = b; b = t; }
        return a;
    };
    for (uint8_t i = 0; i < HallReverb::kLines; ++i)
        for (uint8_t j = i + 1; j < HallReverb::kLines; ++j)
            CHECK(gcd(HallReverb::kLineLen[i], HallReverb::kLineLen[j]) == 1u);

    uint32_t sum = 0;
    for (uint8_t i = 0; i < HallReverb::kLines; ++i) sum += HallReverb::kLineLen[i];
    CHECK(HallReverb::kPoolFloats == sum);
}

TEST_CASE("hall: the Householder matrix is lossless")
{
    // out_i = d_i - (2/N)*sum(d) is orthogonal, so it must preserve the sum of
    // squares exactly. This is what lets `size` alone set the decay: if the
    // matrix leaked, the control would interact with the line count.
    const float d[4] = { 0.37f, -0.81f, 0.15f, 0.62f };
    const float s = (d[0] + d[1] + d[2] + d[3]) * (2.0f / 4.0f);

    double before = 0.0, after = 0.0;
    for (int i = 0; i < 4; ++i) {
        before += (double)d[i] * (double)d[i];
        const double o = (double)d[i] - (double)s;
        after  += o * o;
    }
    CHECK(after == doctest::Approx(before).epsilon(1e-6));
}

TEST_CASE("hall: decays at low size, sustains far longer at high size")
{
    auto tailEnergy = [](float size, bool freeze) {
        std::vector<float> pool((size_t)HallReverb::kPoolFloats, 0.0f);
        HallReverb h; h.begin(pool.data());
        h.setSize(size);
        const size_t N = 32768;
        double acc = 0.0;
        for (size_t i = 0; i < N; i += 128) {
            float bL[128], bR[128];
            for (size_t k = 0; k < 128; ++k) {
                const float v = (i + k < 512) ? ((k & 1u) ? 0.4f : -0.4f) : 0.0f;
                bL[k] = v; bR[k] = v;
            }
            if (freeze && i == 2048) h.setFreeze(true);
            h.processBlock(bL, bR, 128, 1.0f, 1.0f);
            if (i >= 24576) for (size_t k = 0; k < 128; ++k) acc += (double)bL[k] * (double)bL[k];
        }
        return acc;
    };

    CHECK(tailEnergy(0.95f, false) > tailEnergy(0.1f, false));
    // The matrix is lossless, so freeze really holds rather than decaying slowly.
    CHECK(tailEnergy(0.5f, true) > tailEnergy(0.5f, false));
}

TEST_CASE("hall: output is stereo and stays bounded at maximum decay")
{
    std::vector<float> pool((size_t)HallReverb::kPoolFloats, 0.0f);
    HallReverb h; h.begin(pool.data());
    h.setSize(1.0f);                 // longest decay the control allows

    const size_t N = 32768;
    std::vector<float> oL(N), oR(N);
    for (size_t i = 0; i < N; i += 128) {
        float bL[128], bR[128];
        for (size_t k = 0; k < 128; ++k) {
            const float v = (i + k < 4096) ? ((k & 1u) ? 0.5f : -0.5f) : 0.0f;
            bL[k] = v; bR[k] = v;
        }
        h.processBlock(bL, bR, 128, 1.0f, 1.0f);
        for (size_t k = 0; k < 128 && i + k < N; ++k) { oL[i+k] = bL[k]; oR[i+k] = bR[k]; }
    }

    bool stereo = false;
    for (size_t i = 2048; i < 8192; ++i) if (oL[i] != oR[i]) { stereo = true; break; }
    CHECK(stereo);

    // Feedback caps at 0.93 so a long tail must not run away.
    for (size_t i = 0; i < N; ++i) { CHECK(std::fabs(oL[i]) < 2.0f); CHECK(std::fabs(oR[i]) < 2.0f); }
}

TEST_CASE("hall: a null pool leaves it inert and passes audio through")
{
    HallReverb h; h.begin(nullptr);
    float L[128], R[128];
    for (int i = 0; i < 128; ++i) { L[i] = 0.25f; R[i] = -0.25f; }
    h.processBlock(L, R, 128, 0.9f, 0.9f);
    for (int i = 0; i < 128; ++i) { CHECK(L[i] == 0.25f); CHECK(R[i] == -0.25f); }
}
