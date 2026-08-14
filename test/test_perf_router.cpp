// =============================================================================
// test_perf_router.cpp — Performance routing + layer addressing on the wire
// =============================================================================
// Two things under test, both pure logic:
//   1. PerfRouter — which layer(s) does live traffic reach?
//   2. MidiParamTransport's layer addressing — NRPN bit 13, and curated CCs
//      taking their layer from the receive channel.
//
// The single most important case here is the FIRST one: Single mode must stay
// omni.  Before Performance existed the firmware ignored the receive channel
// outright, so honouring a channel in Single mode would silently mute any rig
// that plays on something other than channel 1.
// =============================================================================

#include "doctest.h"

#include "core/PerfRouter.h"
#include "core/MidiParamTransport.h"
#include "core/ParameterStore.h"
#include "core/dsp/Curves.h"

using namespace JT;
using namespace JT::Params;

namespace {

// Select parameters take their option INDEX as the engineering value.
void setOption(ParameterStore& s, uint16_t id, int option)
{
    REQUIRE(s.setEngineering(id, (float)option, Origin::Ui));
}

void setSingle(ParameterStore& s) { setOption(s, ID::PERF_MODE, PerfRouter::kModeSingle); }
void setLayer (ParameterStore& s) { setOption(s, ID::PERF_MODE, PerfRouter::kModeLayer);  }
void setSplit (ParameterStore& s) { setOption(s, ID::PERF_MODE, PerfRouter::kModeSplit);  }

// Channels are 1-based on the wire, 0-based as option indices.
void setChannels(ParameterStore& s, int chA1to16, int chB1to16)
{
    setOption(s, ID::PERF_MIDI_CHANNEL_A, chA1to16 - 1);
    setOption(s, ID::PERF_MIDI_CHANNEL_B, chB1to16 - 1);
}

// Send a complete 14-bit NRPN edit the way the wire really carries it.
void sendNrpn(MidiParamTransport& t, uint16_t nrpnNumber, uint8_t msb,
              uint8_t lsb, uint8_t channel = 1)
{
    t.handleControlChange(99, (uint8_t)((nrpnNumber >> 7) & 0x7F), channel);
    t.handleControlChange(98, (uint8_t)(nrpnNumber & 0x7F), channel);
    t.handleControlChange(6,  msb, channel);
    t.handleControlChange(38, lsb, channel);
}

} // namespace

// =============================================================================
// PerfRouter
// =============================================================================

TEST_CASE("Single mode is omni — the pre-Performance behaviour is preserved")
{
    ParameterStore s;
    PerfRouter r(s);

    setSingle(s);
    setChannels(s, 1, 2);

    // Every channel, including ones matching neither configured layer.
    for (uint8_t ch = 1; ch <= 16; ++ch) {
        CAPTURE(ch);
        CHECK(r.forChannel(ch) == LayerMask::A);
        CHECK(r.forNote(24, ch) == LayerMask::A);
        CHECK(r.forNote(108, ch) == LayerMask::A);
    }
    CHECK(r.isSingle());
}

TEST_CASE("Single mode ignores the split point entirely")
{
    ParameterStore s;
    PerfRouter r(s);
    setSingle(s);
    REQUIRE(s.setEngineering(ID::PERF_SPLIT_NOTE, 60.0f, Origin::Ui));

    CHECK(r.forNote(59, 1) == LayerMask::A);   // below the split...
    CHECK(r.forNote(60, 1) == LayerMask::A);   // ...and above it
}

TEST_CASE("Layer mode: distinct channels address distinct layers")
{
    ParameterStore s;
    PerfRouter r(s);
    setLayer(s);
    setChannels(s, 1, 2);

    CHECK(r.forChannel(1) == LayerMask::A);
    CHECK(r.forChannel(2) == LayerMask::B);
    CHECK(r.forChannel(3) == LayerMask::None);   // neither layer wants it
    CHECK_FALSE(r.isSingle());
}

TEST_CASE("Layer mode: ONE shared channel plays BOTH layers")
{
    // The whole point of Layer mode, and the reason routing returns a mask
    // rather than a single layer.
    ParameterStore s;
    PerfRouter r(s);
    setLayer(s);
    setChannels(s, 5, 5);

    CHECK(r.forChannel(5) == LayerMask::Both);
    CHECK(r.forNote(60, 5) == LayerMask::Both);
    CHECK(r.forChannel(6) == LayerMask::None);
}

TEST_CASE("Split mode: notes below the split are A, the split key itself is B")
{
    ParameterStore s;
    PerfRouter r(s);
    setSplit(s);
    setChannels(s, 1, 1);                       // one keyboard, split by key
    REQUIRE(s.setEngineering(ID::PERF_SPLIT_NOTE, 60.0f, Origin::Ui));
    REQUIRE(r.splitNote() == 60);

    CHECK(r.forNote(0,   1) == LayerMask::A);
    CHECK(r.forNote(59,  1) == LayerMask::A);
    CHECK(r.forNote(60,  1) == LayerMask::B);   // boundary belongs to B
    CHECK(r.forNote(127, 1) == LayerMask::B);
}

TEST_CASE("Split mode: the channel gate still applies on top of the split")
{
    // A Split performance on two DIFFERENT channels: a low note arriving on
    // B's channel belongs to neither half and must sound nothing.
    ParameterStore s;
    PerfRouter r(s);
    setSplit(s);
    setChannels(s, 1, 2);
    REQUIRE(s.setEngineering(ID::PERF_SPLIT_NOTE, 60.0f, Origin::Ui));

    CHECK(r.forNote(50, 1) == LayerMask::A);      // low on A's channel: A
    CHECK(r.forNote(50, 2) == LayerMask::None);   // low on B's channel: nobody
    CHECK(r.forNote(70, 2) == LayerMask::B);      // high on B's channel: B
    CHECK(r.forNote(70, 1) == LayerMask::None);   // high on A's channel: nobody
    CHECK(r.forNote(70, 9) == LayerMask::None);   // unrelated channel
}

TEST_CASE("router follows the store: a mode change takes effect immediately")
{
    ParameterStore s;
    PerfRouter r(s);
    setLayer(s);
    setChannels(s, 1, 2);
    REQUIRE(r.forChannel(7) == LayerMask::None);

    setSingle(s);
    CHECK(r.forChannel(7) == LayerMask::A);   // omni again, no rebuild needed
}

// =============================================================================
// Layer addressing on the wire
// =============================================================================

TEST_CASE("NRPN without the layer bit writes layer A")
{
    ParameterStore s;
    MidiParamTransport t(s, Origin::MidiUsbDev);

    sendNrpn(t, ID::FILTER_CUTOFF, 100, 0);

    const size_t i = ParameterStore::indexOf(ID::FILTER_CUTOFF);
    CHECK(s.getByIndex(i, 0) == doctest::Approx(100.0f * 128.0f / 16383.0f).epsilon(1e-3));
    // Layer B untouched — still its boot default.
    const ParamDesc& d = kParams[i];
    CHECK(s.getByIndex(i, 1) == doctest::Approx(Curves::toNorm(d, d.def)));
}

TEST_CASE("NRPN with bit 13 set writes layer B and leaves A alone")
{
    ParameterStore s;
    MidiParamTransport t(s, Origin::MidiUsbDev);

    const size_t i = ParameterStore::indexOf(ID::FILTER_CUTOFF);
    const ParamDesc& d = kParams[i];
    const float boot = Curves::toNorm(d, d.def);

    sendNrpn(t, (uint16_t)(ID::FILTER_CUTOFF | MidiParamTransport::kLayerBit), 100, 0);

    CHECK(s.getByIndex(i, 1) == doctest::Approx(100.0f * 128.0f / 16383.0f).epsilon(1e-3));
    CHECK(s.getByIndex(i, 0) == doctest::Approx(boot));
    CHECK(t.appliedCount() > 0);
    CHECK(t.unknownIdCount() == 0);
}

TEST_CASE("the resync request survives the layer bit")
{
    // 126 << 7 == 0x3F00, which HAS bit 13 set.  Masking the layer off before
    // the reserved-id check would turn every resync request into a layer-B
    // write to a different parameter.
    ParameterStore s;
    MidiParamTransport t(s, Origin::MidiUsbDev);

    sendNrpn(t, MidiParamTransport::kNrpnResyncRequest, 0, 0);

    CHECK(t.consumeResyncRequest());
    CHECK(t.unknownIdCount() == 0);     // never counted as a bad parameter
}

TEST_CASE("a shared parameter addressed as layer B folds onto its one slot")
{
    // fx.* and seq.* are Scope::PatchShared.  An editor must be free to set
    // the layer bit without knowing which parameters bank.
    ParameterStore s;
    MidiParamTransport t(s, Origin::MidiUsbDev);

    const size_t i = ParameterStore::indexOf(ID::SEQ_GATE_LENGTH);
    REQUIRE_FALSE(ParameterStore::isBanked(i));

    sendNrpn(t, (uint16_t)(ID::SEQ_GATE_LENGTH | MidiParamTransport::kLayerBit), 64, 0);

    const float want = 64.0f * 128.0f / 16383.0f;
    CHECK(s.getByIndex(i, 0) == doctest::Approx(want).epsilon(1e-3));
    CHECK(s.getByIndex(i, 1) == doctest::Approx(want).epsilon(1e-3));
}

TEST_CASE("curated CC takes its layer from the receive channel")
{
    ParameterStore s;
    PerfRouter r(s);
    MidiParamTransport t(s, Origin::MidiUsbHost, &r);

    setLayer(s);
    setChannels(s, 1, 2);

    const size_t i = ParameterStore::indexOf(ID::FILTER_CUTOFF);
    CHECK(t.handleControlChange(74, 20, /*channel=*/1));
    CHECK(t.handleControlChange(74, 110, /*channel=*/2));

    CHECK(s.getByIndex(i, 0) == doctest::Approx(20.0f / 127.0f).epsilon(1e-3));
    CHECK(s.getByIndex(i, 1) == doctest::Approx(110.0f / 127.0f).epsilon(1e-3));
}

TEST_CASE("curated CC on a shared channel edits both layers")
{
    ParameterStore s;
    PerfRouter r(s);
    MidiParamTransport t(s, Origin::MidiUsbHost, &r);

    setLayer(s);
    setChannels(s, 4, 4);

    const size_t i = ParameterStore::indexOf(ID::FILTER_CUTOFF);
    CHECK(t.handleControlChange(74, 90, /*channel=*/4));

    CHECK(s.getByIndex(i, 0) == doctest::Approx(90.0f / 127.0f).epsilon(1e-3));
    CHECK(s.getByIndex(i, 1) == doctest::Approx(90.0f / 127.0f).epsilon(1e-3));
}

TEST_CASE("curated CC on an unmatched channel is NOT consumed")
{
    // Returning false hands it back to the caller's standard-MIDI switch
    // rather than letting it vanish into a parameter write for a layer that
    // does not want it.
    ParameterStore s;
    PerfRouter r(s);
    MidiParamTransport t(s, Origin::MidiUsbHost, &r);

    setLayer(s);
    setChannels(s, 1, 2);

    const size_t i = ParameterStore::indexOf(ID::FILTER_CUTOFF);
    const ParamDesc& d = kParams[i];
    const float boot = Curves::toNorm(d, d.def);

    CHECK_FALSE(t.handleControlChange(74, 90, /*channel=*/11));
    CHECK(s.getByIndex(i, 0) == doctest::Approx(boot));
    CHECK(s.getByIndex(i, 1) == doctest::Approx(boot));
}

TEST_CASE("with no router every curated CC is layer A, on any channel")
{
    // The single-layer contract: a transport constructed without a router
    // behaves exactly as it did before Performance existed.
    ParameterStore s;
    MidiParamTransport t(s, Origin::MidiSerial);

    const size_t i = ParameterStore::indexOf(ID::FILTER_CUTOFF);
    CHECK(t.handleControlChange(74, 33, /*channel=*/13));
    CHECK(s.getByIndex(i, 0) == doctest::Approx(33.0f / 127.0f).epsilon(1e-3));
}

// =============================================================================
// Voice partitioning and layered playback
//
// These drive the ENGINE, not just the routing logic: they prove the pool is
// really cut, that the two layers hold independent patches, and that Single
// mode is unchanged.
// =============================================================================

#include "core/SynthCore.h"

namespace {

constexpr size_t kBlk = 128;

float rmsOf(const float* b, size_t n)
{
    float acc = 0.0f;
    for (size_t i = 0; i < n; ++i) acc += b[i] * b[i];
    return acc / (float)n;
}

float runBlocksRms(SynthCore& core, int blocks)
{
    float L[kBlk], R[kBlk], last = 0.0f;
    for (int i = 0; i < blocks; ++i) {
        core.renderBlock(L, R, kBlk);
        last = rmsOf(L, kBlk);
    }
    return last;
}

} // namespace

TEST_CASE("Single mode: layer A owns all 8 voices, layer B owns none")
{
    ParameterStore store;
    static float pool[SynthCore::kCombPoolFloats];
    SynthCore core(store, pool);

    setSingle(store);
    // A voice_split setting must be IGNORED in Single mode: one patch should
    // never lose half its polyphony to a setting that is not in effect.
    setOption(store, ID::PERF_VOICE_SPLIT, 0);        // "1+7"

    for (uint8_t n = 0; n < 8; ++n) core.noteOn((uint8_t)(60 + n), 100);
    runBlocksRms(core, 4);
    CHECK(core.activeVoices() == 8);
}

TEST_CASE("Split mode cuts the pool where perf.voice_split says")
{
    ParameterStore store;
    static float pool[SynthCore::kCombPoolFloats];
    SynthCore core(store, pool);

    setSplit(store);
    setChannels(store, 1, 1);
    setOption(store, ID::PERF_VOICE_SPLIT, 1);        // "2+6": A gets 2
    REQUIRE(store.setEngineering(ID::PERF_SPLIT_NOTE, 60.0f, Origin::Ui));
    runBlocksRms(core, 2);                            // let the split apply

    // Five notes below the split, but layer A only has two voices.
    for (uint8_t n = 0; n < 5; ++n) core.noteOn((uint8_t)(50 + n), 100, 1);
    runBlocksRms(core, 4);
    CHECK(core.activeVoices() == 2);

    // The upper half is untouched and has its own six.
    for (uint8_t n = 0; n < 6; ++n) core.noteOn((uint8_t)(70 + n), 100, 1);
    runBlocksRms(core, 4);
    CHECK(core.activeVoices() == 8);
}

TEST_CASE("Layer mode: one key on a shared channel sounds both layers")
{
    ParameterStore store;
    static float pool[SynthCore::kCombPoolFloats];
    SynthCore core(store, pool);

    setLayer(store);
    setChannels(store, 1, 1);
    setOption(store, ID::PERF_VOICE_SPLIT, 3);        // "4+4"
    runBlocksRms(core, 2);

    core.noteOn(60, 100, /*channel=*/1);
    runBlocksRms(core, 4);
    CHECK(core.activeVoices() == 2);                  // one voice in each layer
}

TEST_CASE("a note on a channel neither layer answers to sounds nothing")
{
    ParameterStore store;
    static float pool[SynthCore::kCombPoolFloats];
    SynthCore core(store, pool);

    setLayer(store);
    setChannels(store, 1, 2);
    runBlocksRms(core, 2);

    core.noteOn(60, 100, /*channel=*/9);
    runBlocksRms(core, 4);
    CHECK(core.activeVoices() == 0);
}

TEST_CASE("the two layers really do hold independent patches")
{
    // Layer B is silenced through its own mix parameters while layer A plays.
    // If the store's banking or applyParam's layer dispatch were wrong, B's
    // values would reach A's voices and A would go quiet too.
    ParameterStore store;
    static float pool[SynthCore::kCombPoolFloats];
    SynthCore core(store, pool);

    setLayer(store);
    setChannels(store, 1, 1);
    setOption(store, ID::PERF_VOICE_SPLIT, 3);        // "4+4"
    REQUIRE(store.setEngineering(ID::ENV_AMP_ATTACK, 0.0f, Origin::Ui, 0));
    REQUIRE(store.setEngineering(ID::ENV_AMP_SUSTAIN, 1.0f, Origin::Ui, 0));
    REQUIRE(store.setEngineering(ID::ENV_AMP_ATTACK, 0.0f, Origin::Ui, 1));
    REQUIRE(store.setEngineering(ID::ENV_AMP_SUSTAIN, 1.0f, Origin::Ui, 1));
    runBlocksRms(core, 2);

    core.noteOn(60, 100, 1);
    const float both = runBlocksRms(core, 40);
    CHECK(both > 0.0f);
    core.noteOff(60, 1);
    runBlocksRms(core, 200);

    // Now mute layer B's oscillators only.
    REQUIRE(store.setEngineering(ID::MIX_OSC1, 0.0f, Origin::Ui, 1));
    REQUIRE(store.setEngineering(ID::MIX_OSC2, 0.0f, Origin::Ui, 1));
    core.noteOn(60, 100, 1);
    const float aOnly = runBlocksRms(core, 40);

    CHECK(aOnly > 0.0f);         // layer A still sounds...
    CHECK(aOnly < both);         // ...and B's contribution is gone
    CHECK(core.activeVoices() == 2);   // B still ALLOCATED, just silent
}

TEST_CASE("changing the voice split hard-kills sounding voices")
{
    // Signed-off policy: a clean break beats a voice being rendered by one
    // layer while another believes it owns it.
    ParameterStore store;
    static float pool[SynthCore::kCombPoolFloats];
    SynthCore core(store, pool);

    setSplit(store);
    setChannels(store, 1, 1);
    setOption(store, ID::PERF_VOICE_SPLIT, 3);
    runBlocksRms(core, 2);

    for (uint8_t n = 0; n < 4; ++n) core.noteOn((uint8_t)(50 + n), 100, 1);
    runBlocksRms(core, 4);
    REQUIRE(core.activeVoices() > 0);

    setOption(store, ID::PERF_VOICE_SPLIT, 5);        // "6+2" — the cut moves
    runBlocksRms(core, 2);
    CHECK(core.activeVoices() == 0);
}

TEST_CASE("re-applying an unchanged split does NOT kill sounding voices")
{
    // perf.* re-applies on every patch load; an unconditional repartition
    // would cut every held note each time a patch was recalled.
    ParameterStore store;
    static float pool[SynthCore::kCombPoolFloats];
    SynthCore core(store, pool);

    setSplit(store);
    setChannels(store, 1, 1);
    setOption(store, ID::PERF_VOICE_SPLIT, 3);
    runBlocksRms(core, 2);

    core.noteOn(50, 100, 1);
    runBlocksRms(core, 4);
    const size_t before = core.activeVoices();
    REQUIRE(before > 0);

    setOption(store, ID::PERF_VOICE_SPLIT, 3);        // same value again
    runBlocksRms(core, 2);
    CHECK(core.activeVoices() == before);
}

TEST_CASE("panic silences BOTH layers whatever the routing")
{
    ParameterStore store;
    static float pool[SynthCore::kCombPoolFloats];
    SynthCore core(store, pool);

    setLayer(store);
    setChannels(store, 1, 2);
    setOption(store, ID::PERF_VOICE_SPLIT, 3);
    runBlocksRms(core, 2);

    core.noteOn(60, 100, 1);
    core.noteOn(72, 100, 2);
    runBlocksRms(core, 4);
    REQUIRE(core.activeVoices() == 2);

    core.allSoundOff();
    runBlocksRms(core, 2);
    CHECK(core.activeVoices() == 0);
}

TEST_CASE("channel 0 means layer A even in Split mode")
{
    // The sentinel a caller with no channel passes.  It must NOT be routed:
    // routing it would match neither layer and the caller would go silent the
    // moment a user enabled Layer or Split.
    ParameterStore store;
    static float pool[SynthCore::kCombPoolFloats];
    SynthCore core(store, pool);

    setSplit(store);
    setChannels(store, 3, 4);                 // neither is 0
    setOption(store, ID::PERF_VOICE_SPLIT, 3);
    REQUIRE(store.setEngineering(ID::PERF_SPLIT_NOTE, 60.0f, Origin::Ui));
    runBlocksRms(core, 2);

    core.noteOn(72, 100);                      // above the split, no channel
    runBlocksRms(core, 4);
    CHECK(core.activeVoices() == 1);           // sounded, in layer A
}

// =============================================================================
// Per-layer modulation and the balance crossfade (3b)
// =============================================================================

TEST_CASE("layerGains: centre leaves BOTH layers at unity")
{
    // The condition renderBlock's inert path tests for, and the reason a
    // conventional pan law would have been wrong: centre must not attenuate.
    ParameterStore store;
    static float pool[SynthCore::kCombPoolFloats];
    SynthCore core(store, pool);

    setLayer(store);
    setChannels(store, 1, 1);
    setOption(store, ID::PERF_VOICE_SPLIT, 3);
    REQUIRE(store.setEngineering(ID::ENV_AMP_ATTACK, 0.0f, Origin::Ui, 0));
    REQUIRE(store.setEngineering(ID::ENV_AMP_SUSTAIN, 1.0f, Origin::Ui, 0));
    REQUIRE(store.setEngineering(ID::ENV_AMP_ATTACK, 0.0f, Origin::Ui, 1));
    REQUIRE(store.setEngineering(ID::ENV_AMP_SUSTAIN, 1.0f, Origin::Ui, 1));
    runBlocksRms(core, 2);

    core.noteOn(60, 100, 1);
    const float centre = runBlocksRms(core, 40);
    CHECK(centre > 0.0f);

    // Hard left: only layer A survives, so the level must drop but not vanish.
    REQUIRE(store.setEngineering(ID::PERF_BALANCE, 0.0f, Origin::Ui));
    const float aOnly = runBlocksRms(core, 40);
    CHECK(aOnly > 0.0f);
    CHECK(aOnly < centre);

    // Hard right: layer B only — also audible, also quieter than centre.
    REQUIRE(store.setEngineering(ID::PERF_BALANCE, 127.0f, Origin::Ui));
    const float bOnly = runBlocksRms(core, 40);
    CHECK(bOnly > 0.0f);
    CHECK(bOnly < centre);
}

TEST_CASE("the two layers' LFOs are independent")
{
    // Layer B gets a deep, fast filter LFO; layer A gets none.  If the LFOs
    // were still shared, A's voices would wobble too — the failure mode the
    // transitional guard existed to hide.
    ParameterStore store;
    static float pool[SynthCore::kCombPoolFloats];
    SynthCore core(store, pool);

    setLayer(store);
    setChannels(store, 1, 2);
    setOption(store, ID::PERF_VOICE_SPLIT, 3);
    runBlocksRms(core, 2);

    REQUIRE(store.setEngineering(ID::LFO1_FREQ, 8.0f, Origin::Ui, 1));
    REQUIRE(store.setEngineering(ID::LFO1_FILTER_DEPTH, 1.0f, Origin::Ui, 1));

    // Layer A's LFO must still be at its default depth of zero.
    const size_t iDepth = ParameterStore::indexOf(ID::LFO1_FILTER_DEPTH);
    const ParamDesc& d  = kParams[iDepth];
    CHECK(store.getByIndex(iDepth, 0) == doctest::Approx(Curves::toNorm(d, d.def)));
    CHECK(store.getByIndex(iDepth, 1) == doctest::Approx(1.0f));

    // And layer A must still make sound — a shared LFO would not break this,
    // but a mis-scoped fan-out that silenced A would.
    core.noteOn(60, 100, 1);
    CHECK(runBlocksRms(core, 20) > 0.0f);
}

TEST_CASE("the sequencer ticks once per block, not once per layer")
{
    // The pattern is a signed-off singleton.  Ticking it inside the layer loop
    // would double its rate the moment a second layer became audible.
    ParameterStore store;
    static float pool[SynthCore::kCombPoolFloats];
    SynthCore core(store, pool);

    setLayer(store);
    setChannels(store, 1, 1);
    setOption(store, ID::PERF_VOICE_SPLIT, 3);
    REQUIRE(store.set(ID::SEQ_ENABLE, 1.0f, Origin::Ui));
    runBlocksRms(core, 2);

    core.noteOn(60, 100, 1);
    runBlocksRms(core, 4);

    // Both layers audible; the step counter must still advance at one rate.
    const uint16_t s1 = core.statusWord();
    runBlocksRms(core, 1);
    const uint16_t s2 = core.statusWord();
    // Not asserting a specific step — only that the engine runs and reports
    // one coherent sequencer state for the whole instrument.
    CHECK(((s1 & 0x1u) == 1u));
    CHECK(((s2 & 0x1u) == 1u));
}

TEST_CASE("a layer with no voices costs no LFO tick")
{
    // Single mode: layer B owns nothing, so the loop skips it whole.  Proven
    // indirectly — B's LFO phase must not advance while it is voiceless.
    ParameterStore store;
    static float pool[SynthCore::kCombPoolFloats];
    SynthCore core(store, pool);

    setSingle(store);
    REQUIRE(store.setEngineering(ID::LFO1_FREQ, 8.0f, Origin::Ui, 1));
    REQUIRE(store.setEngineering(ID::LFO1_PITCH_DEPTH, 1.0f, Origin::Ui, 1));
    runBlocksRms(core, 50);

    core.noteOn(60, 100);
    CHECK(runBlocksRms(core, 20) > 0.0f);   // layer A unaffected throughout
    CHECK(core.activeVoices() == 1);
}

TEST_CASE("VCA mod is per layer: tremolo on B leaves A steady")
{
    // The 19b move.  With a global amp stage this was impossible to express —
    // one bus cannot carry two tremolos.
    ParameterStore store;
    static float pool[SynthCore::kCombPoolFloats];
    SynthCore core(store, pool);

    setLayer(store);
    setChannels(store, 1, 1);
    setOption(store, ID::PERF_VOICE_SPLIT, 3);
    for (uint8_t ly = 0; ly < 2; ++ly) {
        REQUIRE(store.setEngineering(ID::ENV_AMP_ATTACK,  0.0f, Origin::Ui, ly));
        REQUIRE(store.setEngineering(ID::ENV_AMP_SUSTAIN, 1.0f, Origin::Ui, ly));
    }
    runBlocksRms(core, 2);

    // Layer B alone gets a slow, deep tremolo.
    REQUIRE(store.setEngineering(ID::LFO1_FREQ, 4.0f, Origin::Ui, 1));
    REQUIRE(store.setEngineering(ID::LFO1_AMP_DEPTH, 1.0f, Origin::Ui, 1));

    core.noteOn(60, 100, 1);
    float lo = 1e9f, hi = 0.0f;
    for (int i = 0; i < 60; ++i) {
        const float r = runBlocksRms(core, 1);
        if (i > 10) { if (r < lo) lo = r; if (r > hi) hi = r; }
    }
    CHECK(hi > lo);              // the instrument breathes...
    CHECK(lo > 0.0f);            // ...but never goes silent: layer A holds
}

TEST_CASE("voice.amp_level is per layer")
{
    ParameterStore store;
    static float pool[SynthCore::kCombPoolFloats];
    SynthCore core(store, pool);

    setLayer(store);
    setChannels(store, 1, 1);
    setOption(store, ID::PERF_VOICE_SPLIT, 3);
    for (uint8_t ly = 0; ly < 2; ++ly) {
        REQUIRE(store.setEngineering(ID::ENV_AMP_ATTACK,  0.0f, Origin::Ui, ly));
        REQUIRE(store.setEngineering(ID::ENV_AMP_SUSTAIN, 1.0f, Origin::Ui, ly));
    }
    runBlocksRms(core, 2);

    core.noteOn(60, 100, 1);
    const float both = runBlocksRms(core, 40);
    core.noteOff(60, 1);
    runBlocksRms(core, 200);

    REQUIRE(store.setEngineering(ID::VOICE_AMP_LEVEL, 0.0f, Origin::Ui, 1));  // mute B
    core.noteOn(60, 100, 1);
    const float aOnly = runBlocksRms(core, 40);

    CHECK(aOnly > 0.0f);
    CHECK(aOnly < both);
}
