// =============================================================================
// SynthCore.h — the platform-independent synthesis engine of JT-8000 v2
// =============================================================================
//
// ROLE IN THE ARCHITECTURE (brief §3, §6.1)
//   Everything between the ParameterStore and the output samples: voices,
//   allocator, parameter application, master gain.  It has ZERO Arduino
//   dependencies — the Teensy platform layer (AudioSynthBlockF32) is a thin
//   wrapper that calls renderBlock() from the audio ISR, and the host test
//   harness calls the SAME function to run proofs and render WAV files.
//   One engine, two worlds; that is the whole Phase 1 testing strategy.
//
// EXECUTION CONTEXTS
//   Control plane: noteOn/noteOff/sustain/panic — these do NOT touch voice
//   state directly.  They push events into a small lock-free ring that the
//   audio plane drains at the top of each block.  Same discipline as the
//   ParameterStore: the two planes meet only at block boundaries, so a
//   note-on can never mutate an envelope mid-render.  (v1 called engine
//   methods straight from MIDI handlers and lived with the races.)
//   Audio plane: renderBlock() — drains notes, applies dirty parameters,
//   renders active voices, applies the smoothed master gain.
//
// PER-BLOCK COST WHEN IDLE ("do not calculate if not required")
//   note ring empty-check + 6 dirty-word loads + 8 isActive() branches +
//   one buffer clear.  No voice math, no conversions, no envelope ticks.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>
#include <atomic>

#include "core/AudioConfig.h"
#include "core/ParameterStore.h"
#include "core/PerfRouter.h"
#include "core/Voice.h"
#include "core/VoiceAllocator.h"
#include "core/dsp/Lfo.h"
#include "core/dsp/SlewedValue.h"
#include "core/dsp/TempoClock.h"
#include "core/dsp/PlateReverb.h"
#include "core/dsp/FxChain.h"
#include "core/dsp/StepSequencer.h"
#include "core/dsp/Arpeggiator.h"

namespace JT {

// One global LFO's block-rate state (Phase 3 §5/§6 of PHASE3_LFO_SPEC.md):
// the oscillator plus the FOUR per-destination depths.  Master LFO*_DEPTH
// and LFO*_DESTINATION are deliberately NOT modelled here — Decision #2
// left them unwired (see SynthCore::applyParam).
struct LfoState {
    explicit LfoState(uint32_t seed) : osc(seed) {}
    Lfo   osc;
    float depthPitch  = 0.0f;
    float depthFilter = 0.0f;
    float depthPwm    = 0.0f;
    float depthAmp    = 0.0f;

    // Additive pitch depth from a PERFORMANCE control (today: the mod wheel).
    // Kept separate from depthPitch rather than added into it, because
    // depthPitch is patch state owned by the parameter store: folding the
    // wheel in would make the wheel overwrite the patch, the editor would
    // echo the overwritten value back, and the wheel and the knob would
    // fight.  This field is engine-only and never serialised.
    //
    // ADDITIVE, not a multiplier: the JP-8000's default patch has
    // lfo1.pitch_depth == 0, and a multiplier would leave the wheel dead
    // until the user first raised the knob.
    float depthPitchMod = 0.0f;

    // Tempo-sync state (Phase 3 subsystem 2, PHASE3_BPMCLOCK_SPEC.md §3
    // decision #6): freeHz is the last LFO*_FREQ knob value, kept alive
    // even while synced so switching back to Free restores it exactly
    // (mirrors v1's setLFOnTimingMode).  syncMode is a TempoClock::Mode
    // option index; kFree (0) means "use freeHz", matching the LFO*_FREQ
    // table default (0.03 Hz).
    int   syncMode = TempoClock::kFree;
    float freeHz   = 0.03f;

    // "Engaged" (spec §4): any destination depth > 0.  Gates whether this
    // block bothers to tick the oscillator at all.
    // Total pitch depth actually applied, clamped so wheel-plus-knob cannot
    // exceed the range a full knob already reaches.
    float pitchDepthTotal() const
    {
        const float d = depthPitch + depthPitchMod;
        return (d > 1.0f) ? 1.0f : d;
    }

    bool engaged() const
    {
        // depthPitchMod is included, or raising the wheel on the default
        // patch would leave the oscillator un-ticked and do nothing.
        return depthPitch > 0.0f || depthPitchMod > 0.0f ||
               depthFilter > 0.0f ||
               depthPwm > 0.0f  || depthAmp > 0.0f;
    }
};

class SynthCore {
public:
    // Size of the feedback-comb pool the OWNER must provide: 2 units x 8
    // voices x one delay line.  On Teensy declare it DMAMEM (OCRAM); on
    // the host any static/stack array works.  See FeedbackComb.h.
    static constexpr size_t kCombPoolFloats =
        2u * VoiceAllocator::kMaxVoices * 221u;   // == FeedbackComb line

    // Global-reverb delay memory the OWNER must provide (Phase 5).  ~39707
    // floats ≈ 155 KB — declare it EXTMEM (PSRAM) on Teensy, heap on host.
    // A null pool leaves the reverb inert.  See PlateReverb.h.
    static constexpr size_t kReverbPoolFloats = PlateReverb::kPoolFloats;

    // Per-patch FX-chain delay memory the OWNER must provide (Phase 6).  ~136718
    // floats ≈ 534 KB — declare it EXTMEM (PSRAM) on Teensy, heap on host.  A
    // null pool leaves the FX chain inert (processBlock bails).  See FxChain.h.
    static constexpr size_t kFxPoolFloats = FxChain::kPoolFloats;

    // reverbPool / fxPool default to null so existing call sites / tests that
    // don't exercise those subsystems keep compiling; real builds pass pools.
    SynthCore(ParameterStore& store, float* combPool,
              float* reverbPool = nullptr, float* fxPool = nullptr);

    // Detach the EXTMEM-backed engines (plate reverb + FX chain), leaving
    // them inert — their processBlock()s bail on a null pool.  Called by
    // the platform layer when PSRAM detection fails at boot: unbacked
    // EXTMEM reads return bus garbage, so "no FX" is the correct fallback
    // and "noise" is the bug this prevents.  Comb feedback is unaffected
    // (its pool is DTCM/OCRAM, not PSRAM).
    void disableExtmemPools();

    // --- control plane: note & pedal events (queued, applied next block) ---
    // 'channel1to16' is what Performance routing keys on.  It defaults to 0,
    // a value no MIDI channel has, meaning "unrouted — send it to layer A".
    // That default is what keeps every existing caller and test working
    // unchanged, and it is also the honest answer for a caller that genuinely
    // has no channel (the bring-up console, a test harness).
    //
    // In Single mode routing is omni, so a real channel and the sentinel give
    // the same answer; the distinction only matters in Layer/Split.
    void noteOn(uint8_t note, uint8_t velocity, uint8_t channel1to16 = 0);
    void noteOff(uint8_t note, uint8_t channel1to16 = 0);
    void sustain(bool pedalDown, uint8_t channel1to16 = 0);

    // Routing is resolved on the control plane, at the moment the event is
    // queued, because PerfRouter reads the store's control-plane view.  The
    // resolved layer travels with the event; the audio plane never routes.
    const PerfRouter& router() const { return _router; }
    void allNotesOff();     // CC 123
    void allSoundOff();     // CC 120

    // Phase 4 pitch bend (spec §4.3).  value14 is the raw 14-bit MIDI bend
    // (0..16383, centre 8192); the platform's onPitchBend handler forwards it
    // here (mirrors v1 jt8000.cpp onPitchBend -> LayerManager::handlePitchBend).
    // Queued like note events so it can never mutate voice pitch mid-render.
    void pitchBend(uint16_t value14);

    // Mod wheel (CC 1).  Until the mod matrix lands this has ONE fixed
    // destination — LFO1 pitch depth, i.e. vibrato — which is what the wheel
    // does on a JP-8000 out of the box.  'value7' is the raw 0..127 CC.
    //
    // Queued through the same ring as notes and bend: the wheel arrives on
    // the control plane, and the depth it drives is read by the audio plane
    // once per block.  A plain store would be a race for the sake of one
    // float.
    // Routed like any other live performance control: LFO1 is per layer now,
    // so the wheel must reach the layer(s) its channel addresses.  Channel 0
    // is the same "no channel -> layer A" sentinel the note handlers use.
    void modWheel(uint8_t value7, uint8_t channel1to16 = 0);

    // --- external MIDI clock (Phase 9) -------------------------------------
    // main.cpp's realtime-byte handlers (loop/ISR context) measure 24-PPQN
    // clock into a BPM and detect transport, then call these.  They only STORE
    // into lock-free atomics; the values are consumed once per block at the top
    // of renderBlock (audio plane), exactly like the note ring — so no engine
    // state is mutated across planes.  setExternalBpm updates the shared clock
    // only while the source is External, so LFOs, seq and arp all follow; the
    // transport calls reset/clear the arp on the downbeat.  When no clock
    // arrives and the source stays Internal, none of this takes effect and
    // behaviour is byte-identical.
    void setExternalBpm(float bpm);     // derived from 0xF8 pulse interval
    void transportStart();              // 0xFA — reset arp phase to step 0
    void transportStop();               // 0xFC — silence the arp
    void transportContinue();           // 0xFB — resume without phase reset

    // --- audio plane: render one stereo block (n == kBlockSize) ---
    void renderBlock(float* left, float* right, size_t n);

    // Diagnostics for the bring-up console.  Counted over the whole pool, not
    // per layer: "how loaded is the synth" is a property of the hardware.
    size_t activeVoices() const;

    // Bit i set = pool voice i sounding.  Built from the POOL, not by OR-ing
    // the two allocators' masks — each allocator numbers bits from its own
    // slice, so layer B's bit 0 is really pool voice 4 or wherever the cut
    // currently falls.
    uint8_t activeVoiceMask() const;

    // ---- Status word for the controller's HOME/SEQ displays (Phase F5) ----
    // 13 bits packed into one 14-bit NRPN payload, sent on address 0x3FFF
    // (reserved: never a ParamID) by ParamBroadcast when it CHANGES:
    //
    //     [13..5] unused|voiceMask:8   [4..1] playStep:4   [0] running:1
    //
    // Loop()-context read of ISR-mutated state — same telemetry semantics as
    // activeMask() above: a momentarily torn word costs one redundant send.
    uint16_t statusWord() const {
        const uint16_t mask = activeVoiceMask();
        const uint16_t step = static_cast<uint16_t>(_seq.currentStep()) & 0x0F;
        const uint16_t run  = _seq.enabled() ? 1u : 0u;
        return static_cast<uint16_t>((mask << 5) | (step << 1) | run);
    }

    // ── Generic parameter smoothing ────────────────────────────────────────
    // Every param whose table row declares smooth_ms > 0 glides to its new
    // value instead of stepping to it.  All 76 of them, from one mechanism.
    //
    // WHY IT LIVES HERE, at the drain, rather than in ParameterStore or in the
    // individual DSP objects:
    //   * Not the store: the store is a lock-free dual-plane structure with a
    //     published snapshot; adding mutable per-block state to it would put
    //     control-plane writes and audio-plane ticks on the same cells.
    //   * Not the DSP objects: renderBlock skips inactive voices, so a
    //     per-voice smoother goes stale on an idle voice and a note triggered
    //     mid-sweep starts from a stale value while its siblings do not.  It
    //     would also cost 8x the instances for an identical result.
    //   * Here, applyParam is already the single fan-out point for every
    //     parameter, so one mechanism covers all of them and no DSP object
    //     needs to know smoothing exists.
    //
    // WHY NOT A SlewedValue PER SLOT: 272 of them would be ~8.7 kB and would
    // tick 272 times a block to move at most a handful.  There are only TWO
    // distinct time constants in the whole table (5 ms and 20 ms), so the
    // per-sample coefficient is shared: the bank keeps one float of state per
    // slot plus a short list of which slots are actually moving.  Cost when
    // nothing is moving is one integer compare for the whole block.
    //
    // The decay factors are derived FROM SlewedValue at construction rather
    // than rewritten here, so the maths stays single-sourced and the ported
    // class remains the authority on what smooth_ms means.
    struct SlewBank {
        static constexpr size_t kMaxActive = 24;   // simultaneous gliding params
        float    cur[ParameterStore::kSlots];      // last value handed to applyParam
        bool     seen[ParameterStore::kSlots];     // false until first write
        uint16_t active[kMaxActive];
        uint8_t  nActive = 0;
        float    decayFast = 0.0f;                 // smooth_ms 5, per BLOCK
        float    decaySlow = 0.0f;                 // smooth_ms 20, per BLOCK
    };
    SlewBank _slew;
    void initSlewBank();
    // Route one drained parameter: glide it, or apply it straight through.
    void routeParam(size_t slot, float target);
    // Advance every gliding parameter by one block and re-apply it.
    void tickSlewBank(const float* snap);

    // Clock introspection for bring-up/debug (hardware-visible, not test-gated).
    // debugClockBpm() is the tempo the shared clock is currently running at;
    // debugClockSourceExternal() reports whether the External gate in
    // drainExternalClock() is OPEN — the usual reason incoming MIDI clock is
    // ignored is that this is false (source still Internal).
    float debugClockBpm()            const { return _clock.bpm(); }
    bool  debugClockSourceExternal() const { return _clock.source() == TempoClock::kExtMidi; }
    // The arp's rate mode index (TempoClock::Mode): kFree(0) means the arp is on
    // its free-run knob and will NOT follow tempo at all.
    int   debugArpRateMode()         const { return _layers[0].arp.debugRateModeOrMinus1(); }

#ifdef JT_TESTING
    // Test-only: the effective rate of LFO 0 (LFO1) or 1 (LFO2) — the Hz that
    // applyLfoRate() resolved (free knob or clock division).  Lets test_bpmclock
    // assert sync resolution exactly (PHASE3_BPMCLOCK_SPEC §7).  Firmware-free.
    // Bring-up/test: layer A's current SMOOTHED cutoff norm, i.e. what the
    // voices were actually given this block, not the knob target.
    // Bring-up/test: the SMOOTHED value a parameter is currently sitting at,
    // i.e. what the engine was last handed - not the knob target in the store.
    float debugSlewCur(uint16_t id, uint8_t layer = 0) const
    { return _slew.cur[Params::slotFor(ParameterStore::indexOf(id), layer)]; }
    uint8_t debugSlewActiveCount() const { return _slew.nActive; }

    float debugLfoRateHz(int which) const
    { return (which == 0 ? _layers[0].lfo1 : _layers[0].lfo2).osc.debugRateHz(); }
#endif

private:
    // One handled-parameter fan-out.  Pass 4 wires the oscillator/mixer
    // sections on top of the Phase 1 set; filter-bank, envelope and mod
    // params still pending are deliberately ignored (their dirty flags
    // clear, so nothing accumulates) and land with Passes 5-6.
    // 'layer' selects which layer's voices and per-layer state this parameter
    // reaches.  Shared, performance and global parameters always arrive with
    // layer 0 — Params::layerOfSlot() guarantees it — so their handlers below
    // may ignore the argument entirely.
    void applyParam(size_t index, float norm, uint8_t layer);
    void drainNoteEvents();
    // Apply any external-clock BPM / transport handed off since the last block.
    // Runs at the top of renderBlock (audio plane); no-op when no clock present.
    void drainExternalClock();

    // Tempo-sync helpers (PHASE3_BPMCLOCK_SPEC.md §6).  applyLfoRate pushes
    // the correct Hz (free knob or clock division) into one LfoState's
    // oscillator; refreshSyncedLfos re-runs it for both LFOs whenever the
    // clock itself changes (BPM or source).  Both are control-plane only,
    // called from applyParam on an actual dirty param — no per-block clock
    // work (rule 6 / spec decision #5).
    void applyLfoRate(LfoState& lfo);
    void refreshSyncedLfos();   // both LFOs of BOTH layers

    // --- note event ring (control -> audio, single producer / consumer) ---
    // 32 events is > two blocks of the densest realistic MIDI input; on
    // overflow the OLDEST unconsumed event is dropped and counted, which
    // degrades gracefully (a lost note-on) rather than blocking the ISR.
    struct NoteEvent { uint8_t type; uint8_t a; uint8_t b; };
    // Bit 7 of 'type' carries the destination LAYER.  Types run 0..6, so the
    // bit is free and the event stays three bytes — no ring growth, no extra
    // cost on the single-layer path where the bit is always 0.
    //
    // An event bound for BOTH layers is pushed TWICE, once per layer, rather
    // than encoded as a third destination: the drain then has no mask to
    // decode, and a full ring degrades by dropping one layer's copy instead of
    // silently halving a chord.
    enum : uint8_t { kEvOn, kEvOff, kEvSustain, kEvNotesOff, kEvSoundOff, kEvBend,
                     kEvModWheel };
    static constexpr uint8_t kEvLayerB   = 0x80;
    static constexpr uint8_t kEvTypeMask = 0x7F;

    // Queue one event per layer named by 'dest'.
    void pushRouted(uint8_t type, uint8_t a, uint8_t b, LayerMask dest);
    static constexpr size_t kRingSize = 32;          // power of two
    void pushEvent(uint8_t type, uint8_t a, uint8_t b);

    NoteEvent             _ring[kRingSize];
    std::atomic<uint32_t> _head { 0 };   // producer writes (control plane)
    std::atomic<uint32_t> _tail { 0 };   // consumer writes (audio plane)

    // --- external MIDI clock hand-off (Phase 9) ---------------------------
    // Written by the platform's realtime-byte handlers (setExternalBpm /
    // transport*), consumed once per block at the top of renderBlock.  Lock-
    // free scalars with release/acquire ordering — same discipline as the note
    // ring, no mutex.  _extBpm carries the latest derived tempo (0 == none yet);
    // _transport is a monotonic counter of transport events with the 2 low bits
    // encoding the last action, so a block that misses none still applies the
    // final state.  All default to the no-op values, so with no clock present
    // renderBlock's drain is a couple of relaxed loads and does nothing.
    std::atomic<uint32_t> _extBpmMilli { 0 };   // BPM×1000, 0 = no external tempo
    std::atomic<uint32_t> _transportSeq { 0 };  // bumped on each transport event
    uint32_t              _transportSeen { 0 };  // audio-plane copy (last applied)
    enum : uint8_t { kTransNone = 0, kTransStart, kTransStop, kTransContinue };
    std::atomic<uint8_t>  _transportAction { kTransNone };
    uint32_t              _dropped = 0;

    // --- engine state ---
    ParameterStore& _store;

    // Owned rather than injected: routing is a property of this engine's
    // store, and one owner means the transports and the note handlers cannot
    // disagree about where a channel goes.
    // Constructed from _store, so it must be declared AFTER it — member init
    // order is declaration order, not initialiser-list order.
    PerfRouter      _router;

    Voice           _voices[VoiceAllocator::kMaxVoices];

    // -------------------------------------------------------------------------
    // LAYERS
    //
    // v1 layered by owning two SynthEngine objects, because in v1 an engine WAS
    // a patch.  v2 keeps ONE engine and gives it a layer dimension that mirrors
    // the store's: the 8 voices are a single pool, and each layer owns a
    // contiguous SLICE of it (perf.voice_split decides where the cut falls).
    //
    // What lives here is exactly what has to differ per layer AND is owned by
    // the voices: the allocator, and the patch state that fans into them.  The
    // LFOs, arpeggiator, sequencer, clock, FX and reverb are still singletons
    // on SynthCore — some permanently (they are shared by design), some only
    // until the per-layer modulation stage lands.  See the notes at each.
    // -------------------------------------------------------------------------
    struct VoiceSpan {
        Voice* b;
        Voice* e;
        Voice* begin() const { return b; }
        Voice* end()   const { return e; }
        size_t size()  const { return static_cast<size_t>(e - b); }
    };

    struct Layer {
        Layer(Voice* first, size_t count, uint32_t lfoSeed1, uint32_t lfoSeed2)
            : alloc(first, count), lfo1(lfoSeed1), lfo2(lfoSeed2),
              _first(first), _count(count) {}

        VoiceAllocator alloc;

        // Per-layer modulation.  Each layer is a whole patch, and an LFO rate,
        // shape and depth set are patch state — sharing them would make one
        // layer's vibrato follow the other's knob.  Seeds differ between the
        // layers so two S&H/NOISE LFOs on the same rate do not phase-lock into
        // sounding like one; layer A keeps the original seeds, which is what
        // holds the render baseline byte-identical.
        LfoState lfo1;
        LfoState lfo2;

        // Per-layer arpeggiator: it drives THIS layer's allocator, so in Split
        // mode the lower half can arpeggiate while the upper half plays
        // normally — the reason to have two at all.
        Arpeggiator arp;

        // Post-voice bus gain: perf.balance x this layer's VCA-mod factor,
        // ramped per sample.  Held here rather than in a pair of arrays so a
        // layer's gain cannot drift out of step with the voices it belongs to.
        // 1.0 = unity.
        float gainCur = 1.0f;

        // Would this layer's VCA mod move the bus gain off unity?  Answerable
        // WITHOUT ticking the LFOs, which is what lets renderBlock choose its
        // bus routing before the layer loop runs — the tick that produces the
        // actual modulation value happens inside it.
        //
        // Depths are 0..1 and amp_level defaults to exactly 1.0, so the default
        // patch answers false and the mixing stage is skipped entirely.
        bool ampModActive() const
        {
            return ampFixedLevel != 1.0f ||
                   lfo1.depthAmp > 0.0f  ||
                   lfo2.depthAmp > 0.0f;
        }

        // Patch state that fans to this layer's voices.  Defaults are the
        // no-op values, identical to the pre-layer single-instance members
        // they replace, so a Single-mode patch is arithmetically unchanged.
        float bendRange     = 2.0f;   // VOICE_BEND_RANGE, 0..24 st (v1 default 2)
        float ampFixedLevel = 1.0f;   // VOICE_AMP_LEVEL (v1 AMP_MOD_FIXED_LEVEL)

        // Arbitrary-wavetable selection is PATCH-level state (every voice in
        // the layer shares one table per oscillator), so the bank/index pair
        // lives here and the resolved pointer fans out when either knob moves.
        int arbBank[2]  = { 0, 0 };
        int arbIndex[2] = { 0, 0 };

        // The slice of the shared pool this layer currently owns.  A layer may
        // legitimately own ZERO voices (layer B in Single mode), in which case
        // every fan-out loop below simply does not execute — no branch needed
        // at any of the ~50 call sites.
        VoiceSpan voices() const { return { _first, _first + _count }; }
        size_t    voiceCount() const { return _count; }

        void setSlice(Voice* first, size_t count)
        {
            _first = first;
            _count = count;
            alloc.setPool(first, count);
        }

    private:
        Voice* _first;
        size_t _count;
    };

    Layer _layers[2];

    // Layer B's scratch bus.  Layer A always renders straight into the caller's
    // buffers, so only ONE extra stereo buffer is needed, not two.
    //
    // At the default balance (centre, both gains unity) this is not used at
    // all: layer B renders into the caller's buffers too and the whole mixing
    // stage is skipped — see renderBlock's 'balance inert' path.  The buffers
    // are reserved but untouched, which is what keeps a Single-mode render
    // byte-identical to the pre-layer engine.
    float _busBL[kBlockSize];
    float _busBR[kBlockSize];

    // perf.balance, 0..127 with 64 = centre.  Kept raw so the gain law lives
    // in exactly one place (layerGains).
    float _balance = 64.0f;

    // Resolve perf.balance into the two bus gains.  "Full at centre": A is at
    // unity for 0..64 and fades to silence by 127; B mirrors it.  Centre gives
    // 1.0/1.0 — the pre-Performance behaviour, and the condition the inert
    // path tests for.
    static void layerGains(float balance, float& gA, float& gB);

    // Re-cut the voice pool from perf.mode + perf.voice_split.  Audio plane
    // (called from applyParam): it silences voices.
    void repartitionVoices();


    // Cached so repartitionVoices() only does the work when the cut actually
    // moves — perf.* params re-apply on every patch load and mode change, and
    // an unconditional repartition would hard-kill sounding voices each time.
    uint8_t _splitCountA = VoiceAllocator::kMaxVoices;

    void applyArbTable(int unit, uint8_t layer);

    // Master volume: one-pole smoothed at block rate (~12 ms time constant)
    // then ramped per sample — a CC7 jump is a fade, not a click.
    float _masterTarget = 0.8f;
    float _masterCur    = 0.8f;

    // The two LFOs are per layer now — see Layer.  Seeds stay fixed so S&H /
    // NOISE waveforms and renders remain deterministic across runs.
    // Phase 3 subsystem 2: the internal BPM clock (PHASE3_BPMCLOCK_SPEC.md
    // §3 decision #7).  Control-plane only — read/written from applyParam,
    // NEVER from the audio inner loop; its output only reaches the voices
    // indirectly, via applyLfoRate()'s Lfo::setRateHz() calls.
    TempoClock _clock;
    // Tremolo used to be a global post-mix factor cached here.  It is now per
    // layer and applied on the layer bus (Layer::gainCur), so no global copy
    // exists to go stale.

    // Phase 4 performance (spec §4) now lives per layer — see Layer above.
    // Bend range is fully per-layer.
    //
    // Amp level and LFO tremolo are per layer and applied on the layer bus —
    // see Layer::ampFixedLevel and renderBlock.  Nothing global remains.

    // GLIDE_TIME norm -> v1 log ms -> per-sample fraction (spec §4.1).  Static:
    // no per-instance state, and constexpr-friendly for the compiler.
    static float glideRateFromNorm(float norm);

    // Phase 5 global reverb (PHASE5_REVERB_SPEC.md).  The tank plus the cached
    // control state v1's GlobalFX held: master wet mix + manual bypass, and the
    // derived effective-bypass flag (manual || mix<=threshold).  Post-voice-sum,
    // pre-master (spec §3).  Defaults are all-zero (user sign-off Q1): mix 0 =>
    // effectively bypassed => the tank never runs on the default patch, so the
    // output stays byte-identical to the pre-Phase-5 engine.
    PlateReverb _reverb;
    float _reverbMix           = 0.0f;    // REVERB_MIX (master wet), 0..1
    bool  _reverbManualBypass  = false;   // REVERB_BYPASS toggle
    bool  _reverbBypassed      = true;    // manual || mix<=kReverbMixThreshold
    // v1 GLOBAL_REVERB_MIX_THRESHOLD (GlobalFX.h): below this the tank is
    // skipped to save CPU (and the wet contribution is inaudible anyway).
    static constexpr float kReverbMixThreshold = 0.001f;
    void recomputeReverbBypass()
    {
        _reverbBypassed = _reverbManualBypass || (_reverbMix <= kReverbMixThreshold);
    }

    // Phase 6 per-patch FX chain (PHASE6_FXCHAIN_SPEC.md).  In-place stereo
    // processor on the summed bus, AFTER the voice sum and BEFORE the global
    // reverb (v1 order: FXChainBlock output feeds GlobalFX send).  Gated by
    // _fxEngaged: skipped entirely unless a stage is active (drive/mod/delay),
    // so the default patch never touches it and output stays byte-identical
    // (spec §3 / Q6).  _fxEngaged is recomputed only when a stage-selector
    // param changes (never per-block — rule 6).
    FxChain _fx;
    bool    _fxEngaged = false;
    void recomputeFxEngaged()
    {
        _fxEngaged = _fx.driveActive() || _fx.modActive() || _fx.delayActive();
    }

    // Phase 7 step sequencer (PHASE7_SEQUENCER_SPEC.md).  Control-plane: ticks
    // once per renderBlock and adds its routed output to one of the four LFO
    // modulation accumulators (pitch/filter/pwm/amp).  No pool, no per-sample
    // work.  Disabled by default (SEQ_ENABLE off) => emits 0 => the four
    // accumulators are unchanged => default patch stays byte-identical.
    // _seqEditStep is the "currently selected step for editing" (v1
    // seqSelectedStep): SEQ_STEP_SELECT sets it, SEQ_STEP_VALUE writes it.
    StepSequencer _seq;
    int           _seqEditStep = 0;
    int           _seqAuxEditStep = 0;   // aux-lane edit cursor (Stage B)

    // Arpeggiator (Phase 9, PHASE9_ARP_SPEC.md).  Independent clock, but reads
    // the SHARED _clock BPM so internal-tempo and external-MIDI-clock changes
    // move it with the LFOs and seq.  Disabled by default (ARP_ENABLE off) =>
    // drainNoteEvents keeps feeding the keyboard straight to _alloc, and tick()
    // early-returns => default patch stays byte-identical.  When ENABLED, played
    // notes are CONSUMED into the arp's held-note list (classic behaviour) and
    // only the arp sounds — see drainNoteEvents.
    // The arpeggiator is per layer — see Layer.

    // Global bus pan (Stage C): ramped per-channel gains for the master stage.
    // Centre = 1.0 both (centre-normalised equal-power), so the default patch
    // (aux dest != Pan, or centred) stays byte-identical and the pan multiply
    // is skipped entirely when centred.
    float         _panLCur = 1.0f;
    float         _panRCur = 1.0f;

    // applyParam dispatches on the parameter's permanent ID via a switch —
    // the IDs are constexpr, so the compiler builds a jump table.  With 40+
    // handled parameters this beats the Phase 1 cached-index if-chain on
    // both readability and cycles.  (Refactor flagged and delivered with
    // Pass 4 Step 3 — no behavioural change for the Phase 1 set.)
};

} // namespace JT
