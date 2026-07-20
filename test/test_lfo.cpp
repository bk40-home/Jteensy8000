// =============================================================================
// test_lfo.cpp — proofs for the Phase 3 LFO + modulation subsystem
// =============================================================================
// Two layers, per docs/PHASE3_LFO_SPEC.md §7:
//   UNIT   — the Lfo class in isolation: rate, the six waveforms, the delay
//            ramp.  All of this is new v2-only math (no v1 reference for the
//            oscillator itself — only the NET SCALINGS at the destinations
//            are v1-verified), so these are ordinary correctness proofs.
//   INTEG  — through ParameterStore -> SynthCore: each of the four
//            destinations (pitch, filter, PWM, amp) actually moves; the
//            master LFO*_DEPTH/LFO*_DESTINATION unwired-guard (Decision #2);
//            and the byte-identical default-patch guarantee.
// Zero-crossing / energy techniques mirror test_pitch_env.cpp and
// test_filter_env.cpp — same instruments, new subsystem.
// =============================================================================
#include "doctest.h"

#include <cmath>
#include <vector>

#include "core/dsp/Lfo.h"
#include "core/FilterSection.h"
#include "core/SynthCore.h"
#include "core/dsp/OscCore.h"
#include "core/dsp/Curves.h"
#include "gen/ParamTable.h"

using namespace JT;
using namespace JT::Params;

namespace {

// Rising zero-crossings over a mono buffer -> Hz, same technique as
// test_pitch_env.cpp / test_osc_section.cpp.
size_t risingCrossings(const std::vector<float>& v, size_t begin, size_t end)
{
    size_t c = 0;
    for (size_t i = (begin == 0 ? 1 : begin); i < end; ++i)
        if (v[i - 1] <= 0.0f && v[i] > 0.0f) ++c;
    return c;
}

float freqHz(const std::vector<float>& v, size_t begin, size_t end)
{
    const size_t cycles = risingCrossings(v, begin, end);
    const double secs   = (double)(end - begin) / (double)kSampleRate;
    return (float)((double)cycles / secs);
}

// RMS aggregated over a GROUP of blocks (chunkBlocks x kBlockSize samples).
// A single 128-sample block is NOT an integer number of periods of most
// notes, so per-block RMS alone carries a windowing artifact (a slow beat
// tied to how the block boundary drifts through the waveform's cycle, NOT
// to any real modulation) that can swamp a slow LFO's genuine effect if
// measured block-by-block — a physics-vs-test-premise lesson learned while
// writing this file (per CLAUDE.md).  Aggregating several blocks per sample
// averages that artifact out while still resolving an LFO whose period is
// many chunks long.
std::vector<float> windowedRms(SynthCore& core, int windows, int chunkBlocks)
{
    std::vector<float> out;
    out.reserve((size_t)windows);
    float L[kBlockSize], R[kBlockSize];
    for (int w = 0; w < windows; ++w) {
        double acc = 0.0;
        for (int b = 0; b < chunkBlocks; ++b) {
            core.renderBlock(L, R, kBlockSize);
            for (size_t k = 0; k < kBlockSize; ++k) acc += (double)L[k] * (double)L[k];
        }
        out.push_back((float)std::sqrt(acc / (double)(chunkBlocks * (int)kBlockSize)));
    }
    return out;
}

// Render 'blocks' blocks of a fresh default-boot engine, returning the
// interleaved L/R stream.  Used by the byte-identical guard tests.
std::vector<float> renderLR(SynthCore& core, int blocks)
{
    std::vector<float> out;
    float L[kBlockSize], R[kBlockSize];
    for (int b = 0; b < blocks; ++b) {
        core.renderBlock(L, R, kBlockSize);
        for (size_t k = 0; k < kBlockSize; ++k) { out.push_back(L[k]); out.push_back(R[k]); }
    }
    return out;
}

} // namespace

// =============================================================================
// UNIT — the Lfo class in isolation
// =============================================================================

TEST_CASE("Lfo: block-rate ticking reproduces the set rate (zero-crossing count)")
{
    Lfo lfo(1);
    lfo.setWave(Lfo::kSin);
    lfo.setRateHz(5.0f);

    const int blocks = (int)(kBlocksPerSec + 0.5f);   // ~1 second of blocks
    std::vector<float> v(blocks);
    for (int i = 0; i < blocks; ++i) v[i] = lfo.tickBlock();

    int crossings = 0;
    for (int i = 1; i < blocks; ++i)
        if (v[i - 1] <= 0.0f && v[i] > 0.0f) ++crossings;

    CHECK(crossings >= 4);
    CHECK(crossings <= 6);
}

TEST_CASE("Lfo: SAW ramps monotonically then wraps")
{
    Lfo lfo(2);
    lfo.setWave(Lfo::kSaw);
    lfo.setRateHz(kBlocksPerSec / 10.0f);   // exactly 10 blocks/cycle

    std::vector<float> v(10);
    for (float& x : v) x = lfo.tickBlock();

    for (int i = 1; i < 9; ++i) CHECK(v[i] > v[i - 1]);   // monotonic climb
    CHECK(v[9] < v[8]);                                    // the wrap: a drop
}

TEST_CASE("Lfo: SQR is exactly +-1")
{
    Lfo lfo(3);
    lfo.setWave(Lfo::kSqr);
    lfo.setRateHz(kBlocksPerSec / 10.0f);

    for (int i = 0; i < 20; ++i) {
        const float v = lfo.tickBlock();
        CHECK((v == doctest::Approx(1.0f) || v == doctest::Approx(-1.0f)));
    }
}

TEST_CASE("Lfo: TRI peaks at p=0.5 (mid-cycle) and troughs at wrap")
{
    Lfo lfo(4);
    lfo.setWave(Lfo::kTri);
    lfo.setRateHz(kBlocksPerSec / 10.0f);   // phase lands on exactly 0.5 at tick 5

    float v = 0.0f;
    for (int i = 0; i < 5; ++i) v = lfo.tickBlock();
    CHECK(v == doctest::Approx(1.0f).epsilon(0.02));       // p == 0.5: peak

    for (int i = 0; i < 5; ++i) v = lfo.tickBlock();       // ticks 6..10: wrap at 10
    CHECK(v == doctest::Approx(-1.0f).epsilon(0.02));      // p == 0.0: trough
}

TEST_CASE("Lfo: S&H holds within a cycle, redraws at wrap, deterministic given seed")
{
    auto makeLfo = []() {
        Lfo lfo(42);
        lfo.setWave(Lfo::kSampleHold);
        lfo.setRateHz(kBlocksPerSec / 10.0f);   // 10 blocks/cycle
        return lfo;
    };

    Lfo a = makeLfo();
    std::vector<float> va(20);
    for (float& x : va) x = a.tickBlock();

    for (int i = 1; i < 9; ++i)  CHECK(va[i] == doctest::Approx(va[0]));   // held
    CHECK(va[9] != doctest::Approx(va[0]));                                // redrawn at wrap
    for (int i = 10; i < 18; ++i) CHECK(va[i] == doctest::Approx(va[9]));  // held again

    Lfo b = makeLfo();                              // same seed+params
    for (int i = 0; i < 20; ++i) CHECK(b.tickBlock() == doctest::Approx(va[i]));
}

TEST_CASE("Lfo: NOISE changes every block, deterministic given seed, mean near 0")
{
    Lfo a(7);
    a.setWave(Lfo::kNoise);
    a.setRateHz(1.0f);   // irrelevant to NOISE — not phase-gated

    std::vector<float> va(2000);
    for (float& x : va) x = a.tickBlock();

    for (size_t i = 1; i < va.size(); ++i) CHECK(va[i] != va[i - 1]);

    double sum = 0.0;
    for (float x : va) sum += (double)x;
    CHECK(std::fabs(sum / (double)va.size()) < 0.05);

    Lfo b(7);
    b.setWave(Lfo::kNoise);
    b.setRateHz(1.0f);
    for (size_t i = 0; i < va.size(); ++i) CHECK(b.tickBlock() == doctest::Approx(va[i]));
}

TEST_CASE("Lfo: delay ramp fades in over delayMs; delayMs=0 is instant")
{
    Lfo lfo(9);
    lfo.setWave(Lfo::kSqr);      // exactly +-1 so |output| reads delayGain directly
    lfo.setRateHz(1000.0f);      // fast — only the delay envelope is under test
    lfo.setDelayMs(100.0f);
    lfo.retrigger();

    const float firstBlock = lfo.tickBlock();
    CHECK(std::fabs(firstBlock) < 0.1f);            // still near 0 right after retrigger

    const int blocksToFull = (int)std::ceil(100.0f / kBlockMs) + 2;
    float last = firstBlock;
    for (int i = 1; i < blocksToFull; ++i) last = lfo.tickBlock();
    CHECK(std::fabs(last) == doctest::Approx(1.0f).epsilon(0.02));

    Lfo instant(9);
    instant.setWave(Lfo::kSqr);
    instant.setRateHz(1000.0f);
    instant.setDelayMs(0.0f);
    instant.retrigger();
    CHECK(std::fabs(instant.tickBlock()) == doctest::Approx(1.0f).epsilon(0.01));
}

// =============================================================================
// UNIT — FilterSection's LFO term (formula-level, same style as
// test_filter_env.cpp/test_velocity.cpp: direct debugModOctaves() assertion)
// =============================================================================

TEST_CASE("FilterSection: setLfoCutoff joins the (keyDC+envDC+lfoCut)*octaveCtrl sum")
{
    FilterSection f;
    f.setCutoff(0.5f, 500.0f);
    f.setKeyTrackAmount(0.0f);
    f.setEnvAmount(0.0f);
    f.setOctaveControl(2.0f);       // 2 octaves of depth

    float buf[kBlockSize] = { 0.0f };
    f.setLfoCutoff(1.0f);           // full positive: modOct = 1*2 = 2 -> x4
    f.process(buf, kBlockSize);
    CHECK(f.debugModOctaves() == doctest::Approx(2.0f).epsilon(1e-4));
    CHECK(f.debugCutoffHz() == doctest::Approx(500.0f * 4.0f).epsilon(0.02));

    f.setLfoCutoff(-1.0f);          // full negative: modOct = -2 -> /4
    f.process(buf, kBlockSize);
    CHECK(f.debugModOctaves() == doctest::Approx(-2.0f).epsilon(1e-4));
    CHECK(f.debugCutoffHz() == doctest::Approx(500.0f / 4.0f).epsilon(0.02));
}

TEST_CASE("FilterSection: LFO cutoff term is silent at octaveControl 0 (v1-faithful)")
{
    FilterSection f;
    f.setCutoff(0.5f, 500.0f);
    f.setOctaveControl(0.0f);
    f.setLfoCutoff(1.0f);

    float buf[kBlockSize] = { 0.0f };
    f.process(buf, kBlockSize);
    CHECK(f.debugModOctaves() == doctest::Approx(0.0f));
    CHECK(f.debugCutoffHz() == doctest::Approx(500.0f).epsilon(0.02));
}

// =============================================================================
// INTEGRATION — through ParameterStore -> SynthCore
// =============================================================================

TEST_CASE("LFO->pitch: LFO1_PITCH_DEPTH swings the note frequency around base")
{
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);
    float L[kBlockSize], R[kBlockSize];

    store.setEngineering(ID::MIX_OSC2, 0.0f, Origin::Ui);
    const size_t idxW = ParameterStore::indexOf(ID::OSC1_WAVE);
    store.set(ID::OSC1_WAVE, Curves::toNorm(kParams[idxW], (float)(int)Wave::Sine), Origin::Ui);
    store.setEngineering(ID::ENV_AMP_ATTACK, 0.0f, Origin::Ui);
    store.setEngineering(ID::ENV_AMP_SUSTAIN, 1.0f, Origin::Ui);

    store.setEngineering(ID::LFO1_FREQ, 1.0f, Origin::MidiUsbDev);          // 1 Hz
    store.setEngineering(ID::LFO1_PITCH_DEPTH, 1.0f, Origin::MidiUsbDev);   // full: +-7 st

    core.noteOn(69, 100);                                             // A4 = 440 Hz
    std::vector<float> mono;
    mono.reserve(44100);
    while (mono.size() < 44100) {
        core.renderBlock(L, R, kBlockSize);
        for (size_t k = 0; k < kBlockSize; ++k) mono.push_back(L[k]);
    }

    // 1 Hz LFO, 1 s render: quarter period (~250 ms) sits near +1 (sharp),
    // three-quarter period (~750 ms) sits near -1 (flat).
    const float peak   = freqHz(mono, 9025, 13025);
    const float trough = freqHz(mono, 31075, 35075);
    CHECK(peak > 500.0f);
    CHECK(trough < 400.0f);
    CHECK(peak > trough * 1.2f);
}

TEST_CASE("LFO->pitch: depth 0 leaves the note at base frequency")
{
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);
    float L[kBlockSize], R[kBlockSize];

    store.setEngineering(ID::MIX_OSC2, 0.0f, Origin::Ui);
    const size_t idxW = ParameterStore::indexOf(ID::OSC1_WAVE);
    store.set(ID::OSC1_WAVE, Curves::toNorm(kParams[idxW], (float)(int)Wave::Sine), Origin::Ui);
    store.setEngineering(ID::ENV_AMP_ATTACK, 0.0f, Origin::Ui);
    store.setEngineering(ID::ENV_AMP_SUSTAIN, 1.0f, Origin::Ui);
    store.setEngineering(ID::LFO1_FREQ, 1.0f, Origin::MidiUsbDev);          // LFO runs...
    // ...but LFO1_PITCH_DEPTH stays at its 0 default: no shift should reach OSC.

    core.noteOn(69, 100);
    std::vector<float> mono;
    mono.reserve(22050);
    while (mono.size() < 22050) {
        core.renderBlock(L, R, kBlockSize);
        for (size_t k = 0; k < kBlockSize; ++k) mono.push_back(L[k]);
    }
    CHECK(freqHz(mono, 4000, 22000) == doctest::Approx(440.0f).epsilon(0.02));
}

TEST_CASE("LFO->filter: through the store, LFO1_FILTER_DEPTH audibly moves the cutoff")
{
    // octaveControl > 0 is required (v1-faithful: silent at 0, proven at the
    // FilterSection level above).  Windowed RMS (see windowedRms) rather than
    // per-block RMS: the LFO's genuine cutoff sweep must be measured over a
    // span long compared to the block-windowing artifact.
    auto rmsSeries = [](float depth) {
        ParameterStore store;
        static float combPool[SynthCore::kCombPoolFloats];
        SynthCore core(store, combPool);

        store.setEngineering(ID::MIX_OSC2, 0.0f, Origin::Ui);
        store.setEngineering(ID::ENV_AMP_SUSTAIN, 1.0f, Origin::Ui);
        store.set(ID::FILTER_ENGINE, 1.0f, Origin::MidiUsbDev);              // VA
        store.setEngineering(ID::FILTER_CUTOFF, 200.0f, Origin::MidiUsbDev); // dark base
        store.setEngineering(ID::FILTER_OCTAVE_CONTROL, 0.3f, Origin::MidiUsbDev); // 3 oct
        store.setEngineering(ID::LFO1_FREQ, 2.0f, Origin::MidiUsbDev);
        store.setEngineering(ID::LFO1_FILTER_DEPTH, depth, Origin::MidiUsbDev);

        core.noteOn(45, 127);
        (void)windowedRms(core, 3, 16);     // settle: note-on + filter warm-up
        return windowedRms(core, 16, 16);   // ~730 ms, ~1.5 LFO cycles @ 2 Hz
    };

    const auto swept = rmsSeries(1.0f);
    float swMin = swept[0], swMax = swept[0];
    for (float r : swept) { if (r < swMin) swMin = r; if (r > swMax) swMax = r; }
    CHECK(swMax > swMin * 1.3f);                     // clearly sweeping

    const auto flat = rmsSeries(0.0f);
    float flMin = flat[0], flMax = flat[0];
    for (float r : flat) { if (r < flMin) flMin = r; if (r > flMax) flMax = r; }
    CHECK(flMax < flMin * 1.1f);                     // depth 0: essentially flat
}

TEST_CASE("LFO->amp: LFO1_AMP_DEPTH tremolos the (windowed) RMS; depth 0 stays flat")
{
    auto rmsSeries = [](float depth) {
        ParameterStore store;
        static float combPool[SynthCore::kCombPoolFloats];
        SynthCore core(store, combPool);

        store.setEngineering(ID::MIX_OSC2, 0.0f, Origin::Ui);
        store.setEngineering(ID::ENV_AMP_ATTACK, 0.0f, Origin::Ui);
        store.setEngineering(ID::ENV_AMP_SUSTAIN, 1.0f, Origin::Ui);
        store.setEngineering(ID::LFO1_FREQ, 2.0f, Origin::MidiUsbDev);
        store.setEngineering(ID::LFO1_AMP_DEPTH, depth, Origin::MidiUsbDev);

        core.noteOn(57, 110);
        (void)windowedRms(core, 3, 16);     // settle: attack + master-gain ramp
        return windowedRms(core, 16, 16);   // ~730 ms, ~1.5 LFO cycles @ 2 Hz
    };

    const auto tremolo = rmsSeries(1.0f);
    float trMin = tremolo[0], trMax = tremolo[0];
    for (float r : tremolo) { if (r < trMin) trMin = r; if (r > trMax) trMax = r; }
    CHECK(trMax > trMin * 1.3f);                     // clearly modulating

    const auto flat = rmsSeries(0.0f);
    float flMin = flat[0], flMax = flat[0];
    for (float r : flat) { if (r < flMin) flMin = r; if (r > flMax) flMax = r; }
    CHECK(flMax < flMin * 1.1f);                     // depth 0: essentially flat
}

TEST_CASE("LFO->PWM: LFO1_PWM_DEPTH moves the duty cycle of a pulse osc over time")
{
    ParameterStore store;
    static float combPool[SynthCore::kCombPoolFloats];
    SynthCore core(store, combPool);
    float L[kBlockSize], R[kBlockSize];

    store.setEngineering(ID::MIX_OSC2, 0.0f, Origin::Ui);
    const size_t idxW = ParameterStore::indexOf(ID::OSC1_WAVE);
    store.set(ID::OSC1_WAVE, Curves::toNorm(kParams[idxW], (float)(int)Wave::Pulse), Origin::Ui);
    store.setEngineering(ID::ENV_AMP_ATTACK, 0.0f, Origin::Ui);
    store.setEngineering(ID::ENV_AMP_SUSTAIN, 1.0f, Origin::Ui);
    store.setEngineering(ID::LFO1_FREQ, 1.0f, Origin::MidiUsbDev);           // 1 Hz
    store.setEngineering(ID::LFO1_PWM_DEPTH, 1.0f, Origin::MidiUsbDev);

    core.noteOn(45, 110);
    std::vector<float> mono;
    mono.reserve(44100);
    while (mono.size() < 44100) {
        core.renderBlock(L, R, kBlockSize);
        for (size_t k = 0; k < kBlockSize; ++k) mono.push_back(L[k]);
    }

    // Duty measured as the +/high sample fraction in two 100 ms windows a
    // quarter-period apart — the LFO should have visibly shifted the width.
    auto dutyOf = [&](size_t begin, size_t end) {
        size_t high = 0;
        for (size_t i = begin; i < end; ++i) if (mono[i] > 0.0f) ++high;
        return (float)high / (float)(end - begin);
    };
    const float dutyA = dutyOf(2000, 6410);        // ~45-145 ms
    const float dutyB = dutyOf(13230, 17640);      // ~300-400 ms (quarter period later)
    CHECK(std::fabs(dutyA - dutyB) > 0.03f);
}

TEST_CASE("LFO Decision #2 guard: master LFO1_DEPTH and LFO1_DESTINATION are unwired")
{
    auto render = [](bool writeUnwired) {
        ParameterStore store;
        static float combPool[SynthCore::kCombPoolFloats];
        SynthCore core(store, combPool);
        if (writeUnwired) {
            store.setEngineering(ID::LFO1_DEPTH, 1.0f, Origin::Ui);
            const size_t idxD = ParameterStore::indexOf(ID::LFO1_DESTINATION);
            store.set(ID::LFO1_DESTINATION, Curves::toNorm(kParams[idxD], 2.0f), Origin::Ui); // "Filter"
        }
        core.noteOn(57, 100);
        return renderLR(core, 100);
    };

    const auto a = render(false);
    const auto b = render(true);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) CHECK(a[i] == b[i]);
}

TEST_CASE("LFO default patch (all depths 0) is byte-identical to never touching LFO params")
{
    auto render = [](bool touchLfoParams) {
        ParameterStore store;
        static float combPool[SynthCore::kCombPoolFloats];
        SynthCore core(store, combPool);
        if (touchLfoParams) {
            store.setEngineering(ID::LFO1_PITCH_DEPTH, 0.0f, Origin::Ui);
            store.setEngineering(ID::LFO1_FILTER_DEPTH, 0.0f, Origin::Ui);
            store.setEngineering(ID::LFO1_PWM_DEPTH, 0.0f, Origin::Ui);
            store.setEngineering(ID::LFO1_AMP_DEPTH, 0.0f, Origin::Ui);
            store.setEngineering(ID::LFO2_PITCH_DEPTH, 0.0f, Origin::Ui);
            store.setEngineering(ID::LFO2_FILTER_DEPTH, 0.0f, Origin::Ui);
            store.setEngineering(ID::LFO2_PWM_DEPTH, 0.0f, Origin::Ui);
            store.setEngineering(ID::LFO2_AMP_DEPTH, 0.0f, Origin::Ui);
            store.setEngineering(ID::LFO1_FREQ, 5.0f, Origin::Ui);   // running LFO...
            store.setEngineering(ID::LFO2_FREQ, 9.0f, Origin::Ui);   // ...but 0 depth everywhere
        }
        core.noteOn(57, 100);
        core.noteOn(52, 90);
        return renderLR(core, 200);
    };

    const auto a = render(false);
    const auto b = render(true);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) CHECK(a[i] == b[i]);
}
