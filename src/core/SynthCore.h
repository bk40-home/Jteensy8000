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
#include "core/Voice.h"
#include "core/VoiceAllocator.h"
#include "core/dsp/Lfo.h"
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
    bool engaged() const
    {
        return depthPitch > 0.0f || depthFilter > 0.0f ||
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
    void noteOn(uint8_t note, uint8_t velocity);
    void noteOff(uint8_t note);
    void sustain(bool pedalDown);
    void allNotesOff();     // CC 123
    void allSoundOff();     // CC 120

    // Phase 4 pitch bend (spec §4.3).  value14 is the raw 14-bit MIDI bend
    // (0..16383, centre 8192); the platform's onPitchBend handler forwards it
    // here (mirrors v1 jt8000.cpp onPitchBend -> LayerManager::handlePitchBend).
    // Queued like note events so it can never mutate voice pitch mid-render.
    void pitchBend(uint16_t value14);

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

    // Diagnostics for the bring-up console.
    size_t activeVoices() const { return _alloc.activeCount(); }

    // ---- Status word for the controller's HOME/SEQ displays (Phase F5) ----
    // 13 bits packed into one 14-bit NRPN payload, sent on address 0x3FFF
    // (reserved: never a ParamID) by ParamBroadcast when it CHANGES:
    //
    //     [13..5] unused|voiceMask:8   [4..1] playStep:4   [0] running:1
    //
    // Loop()-context read of ISR-mutated state — same telemetry semantics as
    // activeMask() above: a momentarily torn word costs one redundant send.
    uint16_t statusWord() const {
        const uint16_t mask = _alloc.activeMask();
        const uint16_t step = static_cast<uint16_t>(_seq.currentStep()) & 0x0F;
        const uint16_t run  = _seq.enabled() ? 1u : 0u;
        return static_cast<uint16_t>((mask << 5) | (step << 1) | run);
    }

#ifdef JT_TESTING
    // Test-only: the effective rate of LFO 0 (LFO1) or 1 (LFO2) — the Hz that
    // applyLfoRate() resolved (free knob or clock division).  Lets test_bpmclock
    // assert sync resolution exactly (PHASE3_BPMCLOCK_SPEC §7).  Firmware-free.
    float debugLfoRateHz(int which) const
    { return (which == 0 ? _lfo1 : _lfo2).osc.debugRateHz(); }
#endif

private:
    // One handled-parameter fan-out.  Pass 4 wires the oscillator/mixer
    // sections on top of the Phase 1 set; filter-bank, envelope and mod
    // params still pending are deliberately ignored (their dirty flags
    // clear, so nothing accumulates) and land with Passes 5-6.
    void applyParam(size_t index, float norm);
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
    void refreshSyncedLfos();

    // --- note event ring (control -> audio, single producer / consumer) ---
    // 32 events is > two blocks of the densest realistic MIDI input; on
    // overflow the OLDEST unconsumed event is dropped and counted, which
    // degrades gracefully (a lost note-on) rather than blocking the ISR.
    struct NoteEvent { uint8_t type; uint8_t a; uint8_t b; };
    enum : uint8_t { kEvOn, kEvOff, kEvSustain, kEvNotesOff, kEvSoundOff, kEvBend };
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
    Voice           _voices[VoiceAllocator::kMaxVoices];
    VoiceAllocator  _alloc;

    // Arbitrary-wavetable selection is PATCH-level state (all voices share
    // one table per unit), so the bank/index pair lives here and fans the
    // resolved pointer to the voices whenever either knob moves.
    void applyArbTable(int unit);
    int _arbBank[2]  = { 0, 0 };
    int _arbIndex[2] = { 0, 0 };

    // Master volume: one-pole smoothed at block rate (~12 ms time constant)
    // then ramped per sample — a CC7 jump is a fade, not a click.
    float _masterTarget = 0.8f;
    float _masterCur    = 0.8f;

    // Phase 3: the two global LFOs.  Seeds are arbitrary but fixed, so S&H/
    // NOISE waveforms and renders stay deterministic across runs.
    LfoState _lfo1{ 0x1234567u };
    LfoState _lfo2{ 0x89ABCDEu };
    // Phase 3 subsystem 2: the internal BPM clock (PHASE3_BPMCLOCK_SPEC.md
    // §3 decision #7).  Control-plane only — read/written from applyParam,
    // NEVER from the audio inner loop; its output only reaches the voices
    // indirectly, via applyLfoRate()'s Lfo::setRateHz() calls.
    TempoClock _clock;
    // Tremolo (LFO -> amp, global post-mix): last block's applied gain, so
    // renderBlock can ramp toward this block's target per sample instead of
    // stepping (spec §3 decision #6).  1.0 = inert (default patch, no amp
    // depth wired anywhere).
    float _ampModCur = 1.0f;

    // Phase 4 performance (spec §4).  All defaults are the no-op values so the
    // default patch stays byte-identical: glide off, bend range 2 st but no
    // bend sent (0 semis), amp-level base 1.0 (v1 AMP_MOD_FIXED_LEVEL default).
    float _bendRange     = 2.0f;   // VOICE_BEND_RANGE, 0..24 st (v1 default 2)
    float _ampFixedLevel = 1.0f;   // VOICE_AMP_LEVEL = v1 AMP_MOD_FIXED_LEVEL base

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
    Arpeggiator   _arp;

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
