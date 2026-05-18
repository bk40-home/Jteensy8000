#pragma once
// =============================================================================
// LayerManager.h — JP-8080-style dual-layer performance coordinator
// =============================================================================
//
// Sits between MIDI input and one or two SynthEngine instances.
// In SINGLE mode, only Layer A is active — identical to pre-layer behaviour.
// In LAYER mode, both layers receive all notes with split voice allocation.
// In SPLIT mode, notes are routed by a split point.
//
// AUDIO TOPOLOGY:
//   Layer A stereo out ──► _perfMixerL/R ch0 ──┐
//                                               ├──► DAC
//   Layer B stereo out ──► _perfMixerL/R ch1 ──┘
//
//   In SINGLE mode, Layer B's mixer channels are zeroed and its engine
//   receives no MIDI — zero CPU contribution from idle voices.
//
// VOICE ALLOCATION:
//   Total is always MAX_VOICES (8). A CC sets how many go to Layer A;
//   Layer B gets the remainder. In SINGLE mode, Layer A gets all 8.
//
// LAYER BALANCE:
//   CC 0..127, centre=64.  At centre, both layers are equal gain.
//   At 0, only Layer A is heard. At 127, only Layer B is heard.
//   This is an output gain crossfade — not a CC morph (that comes later).
//
// EDIT TARGET:
//   Determines which layer receives CC edits from the controller/editor.
//   A = Layer A only, B = Layer B only, BOTH = forwarded to both engines.
//
// CC ASSIGNMENTS (internal-only, above MIDI range — not automatable via MIDI):
//   CC 140 = PERF_MODE         (0=Single, 1=Layer, 2=Split)
//   CC 141 = PERF_VOICE_SPLIT  (0..7 = voices for Layer A; B gets remainder)
//   CC 142 = PERF_SPLIT_NOTE   (0..127 = MIDI note split point, Split mode)
//   CC 143 = PERF_BALANCE      (0..127, 64=centre, layer output crossfade)
//   CC 144 = PERF_EDIT_TARGET  (0=A, 1=B, 2=Both)
//
// These are compile-time / firmware-UI only until the alternative CC routing
// branch is merged. Use setCC()-style direct calls from TFT UI or SysEx.
// =============================================================================

#include <Arduino.h>
#include "SynthEngine.h"
#include "VoicePool.h"
#include "GlobalFX.h"
#include "CCDefs.h"
#include "CCCache.h"      // Phase 2 — per-layer/per-scope CC cache

class SysExAdapter;       // Phase 1 — forward decl for setSysExSnoop()

// Performance mode — how layers are used
enum class PerfMode : uint8_t {
    SINGLE = 0,   // Layer A only, all voices
    LAYER  = 1,   // Both layers, all notes to both, voice count split
    SPLIT  = 2,   // Both layers, notes split by split point
};

// Edit target — which layer receives CC changes
enum class EditTarget : uint8_t {
    LAYER_A = 0,
    LAYER_B = 1,
    BOTH    = 2,
};

class LayerManager {
public:
    LayerManager();

    // =========================================================================
    // Lifecycle — call from setup() AFTER AudioMemory()
    // =========================================================================
    // Wires the performance mixer and calls begin() on both engines.
    void begin();

    // Call from loop() — forwards to both engines' update().
    void update();

    // =========================================================================
    // MIDI routing — call these from the .ino MIDI callbacks
    // =========================================================================
    void noteOn(byte channel, byte note, float velocity);
    void noteOff(byte channel, byte note);
    void handleControlChange(byte channel, byte control, byte value);
    void handlePitchBend(byte channel, int16_t value);

    // =========================================================================
    // Performance mode
    // =========================================================================
    void     setPerfMode(PerfMode mode);
    PerfMode getPerfMode() const { return _perfMode; }

    // =========================================================================
    // Voice allocation split
    // =========================================================================
    // voicesA = how many voices Layer A gets (1..MAX_VOICES-1 in dual modes,
    //           MAX_VOICES in SINGLE mode). Layer B gets MAX_VOICES - voicesA.
    void    setVoiceSplit(uint8_t voicesA);
    uint8_t getVoiceSplit() const { return _voicesA; }

    // =========================================================================
    // Split point (SPLIT mode only)
    // =========================================================================
    void    setSplitNote(uint8_t note);
    uint8_t getSplitNote() const { return _splitNote; }

    // =========================================================================
    // Layer balance — output crossfade
    // =========================================================================
    // 0 = Layer A only, 64 = equal, 127 = Layer B only.
    void    setBalance(uint8_t value);
    uint8_t getBalance() const { return _balance; }

    // =========================================================================
    // Edit target — which layer receives CC edits
    // =========================================================================
    void       setEditTarget(EditTarget t);
    EditTarget getEditTarget() const { return _editTarget; }

    // =========================================================================
    // Per-layer MIDI channel (JP-8000 convention: 1..16, no omni).
    //
    // Setters clamp out-of-range values to channel 1 so malformed preset
    // data never leaves the manager in a broken state. Channel 0 input is
    // explicitly rejected: on the JP-8000 a Part always has an explicit
    // channel assignment.
    //
    // Behaviour summary (strict multitimbral):
    //   - Notes, pitch bend, and CCs from live MIDI are accepted by an
    //     engine only if the incoming channel equals that engine's channel.
    //   - Setting both channels to the same value = "Dual" behaviour
    //     (both engines match, split/layer rules then decide routing).
    //   - Preset/patch loads bypass this filter entirely because they
    //     call engine.handleControlChange() directly rather than going
    //     through LayerManager::handleControlChange.
    // =========================================================================
    void    setMidiChannelA(uint8_t ch1to16);
    void    setMidiChannelB(uint8_t ch1to16);
    uint8_t getMidiChannelA() const { return _midiChannelA; }
    uint8_t getMidiChannelB() const { return _midiChannelB; }

    // =========================================================================
    // Engine access — for UI, display, presets, audio wiring
    // =========================================================================
    SynthEngine&       layerA()       { return _engineA; }
    SynthEngine&       layerB()       { return _engineB; }
    const SynthEngine& layerA() const { return _engineA; }
    const SynthEngine& layerB() const { return _engineB; }

    // Active engine for UI display — returns the edit target layer (A if BOTH)
    SynthEngine&       activeEngine();
    const SynthEngine& activeEngine() const;

    // Is Layer B currently producing audio?
    bool isLayerBActive() const { return _perfMode != PerfMode::SINGLE; }

    // =========================================================================
    // Audio graph outputs — performance mixer feeds the DAC
    // =========================================================================
    AudioMixer4& getPerfOutL() { return _perfMixerL; }
    AudioMixer4& getPerfOutR() { return _perfMixerR; }

    // =========================================================================
    // BPM clock — forwarded to both engines
    // =========================================================================
    void setBPMClock(BPMClockManager* clock);

    // =========================================================================
    // Global FX bus — shared across layers. Performance struct reads/writes
    // via here for preset save/load. UI uses it for global reverb controls.
    // =========================================================================
    GlobalFX&       getGlobalFX()       { return _globalFx; }
    const GlobalFX& getGlobalFX() const { return _globalFx; }

    // =========================================================================
    // UI notifier — forwarded to active engine
    // =========================================================================
    void setNotifier(SynthEngine::NotifyFn fn);

    // =========================================================================
    // SysEx CC snoop (Phase 1) — registered by Jteensy8000 in setup(). When
    // set, handleControlChange() forwards every incoming CC to the adapter so
    // it can keep its SysEx-only abstraction state (FX Mod/Delay enable+
    // variation memory) consistent with whatever's coming in from hardware.
    // Pass nullptr to disable (default).
    // =========================================================================
    void setSysExSnoop(SysExAdapter* adapter) { _sysExSnoop = adapter; }

    // =========================================================================
    // Per-layer / per-scope CC cache (Phase 2) — backs SysEx GET_PARAM and
    // BANK_DUMP. Updated automatically by every CC dispatch through
    // handleControlChange(); also updated by SysExAdapter for engine-direct
    // paths that bypass the channel-filtered handler.
    //
    // cacheCC:    write a (layer, cc, value) tuple into the cache. layer is
    //             one of SyxProto::kLayer{A,B,Perf,GlobalFx}.
    // getCachedCC: read the most recent value for (layer, cc). Returns
    //             CCCache::kUnset (0xFF) if the slot has never been written.
    // =========================================================================
    void    cacheCC    (uint8_t layer, uint8_t cc, uint8_t value);
    uint8_t getCachedCC(uint8_t layer, uint8_t cc) const;

private:
    // =========================================================================
    // Shared voice pool — declared BEFORE _engineA/_engineB so it is fully
    // constructed before the engines. begin() passes &_pool to both engines
    // via setVoicePool() before calling their begin(). Pool + engines are all
    // direct members of LayerManager, so lifetimes are perfectly aligned.
    // =========================================================================
    VoicePool   _pool;

    // =========================================================================
    // Two engine instances — Layer A and Layer B. Both are full peers; each
    // owns its own AudioGraph (LFOs, step seq, amp-mod chain) and its own
    // FXChainBlock. Voices come from the shared _pool above.
    // =========================================================================
    SynthEngine _engineA;
    SynthEngine _engineB;

    // =========================================================================
    // Shared global FX bus — today just the reverb tank.
    // Fed by taps from both layers' FX outputs; wet output returns into
    // the perf mixer on its own channels (ch2/ch3). See GlobalFX.h.
    // =========================================================================
    GlobalFX _globalFx;

    // =========================================================================
    // Performance mixer — combines both layers' stereo outputs + reverb wet
    // =========================================================================
    //   ch0 = Layer A dry/JPFX
    //   ch1 = Layer B dry/JPFX
    //   ch2 = Global reverb wet L (perf mixer L only — R mixer uses R wet)
    //   ch3 = unused
    // Both perf mixers have the same channel assignment semantically; the
    // L mixer takes L-side signals and the R mixer takes R-side signals.
    AudioMixer4 _perfMixerL;
    AudioMixer4 _perfMixerR;

    // Audio patch cables — Layer A/B FX out → performance mixer
    AudioConnection* _patchAL = nullptr;   // Layer A FX out L → _perfMixerL ch0
    AudioConnection* _patchAR = nullptr;   // Layer A FX out R → _perfMixerR ch0
    AudioConnection* _patchBL = nullptr;   // Layer B FX out L → _perfMixerL ch1
    AudioConnection* _patchBR = nullptr;   // Layer B FX out R → _perfMixerR ch1

    // Reverb send taps — FX outputs ALSO feed GlobalFX's send-sum mixers.
    // Note: a single AudioStream output can feed multiple AudioConnection
    // destinations, so the Layer A/B FX mixers are simultaneously wired to
    // both the perf mixer (ch0/ch1) and GlobalFX send inputs here.
    AudioConnection* _patchATapToReverbL = nullptr;   // Layer A L → GlobalFX send L ch0
    AudioConnection* _patchATapToReverbR = nullptr;   // Layer A R → GlobalFX send R ch0
    AudioConnection* _patchBTapToReverbL = nullptr;   // Layer B L → GlobalFX send L ch1
    AudioConnection* _patchBTapToReverbR = nullptr;   // Layer B R → GlobalFX send R ch1

    // Reverb wet return — GlobalFX master-wet amps → perf mixer ch2
    AudioConnection* _patchReverbWetToPerfL = nullptr;  // GlobalFX wetL → _perfMixerL ch2
    AudioConnection* _patchReverbWetToPerfR = nullptr;  // GlobalFX wetR → _perfMixerR ch2

    // =========================================================================
    // State
    // =========================================================================
    PerfMode   _perfMode     = PerfMode::SINGLE;
    EditTarget _editTarget   = EditTarget::LAYER_A;
    uint8_t    _voicesA      = MAX_VOICES;  // In SINGLE mode, A gets all
    uint8_t    _splitNote    = 60;          // Middle C default split point
    uint8_t    _balance      = 64;          // Centre = equal mix

    // Per-layer MIDI channel (JP-8000 convention: 1..16, no omni). Default
    // to channel 1 for both — matches a bare JP-8000 coming out of the box.
    // See setMidiChannelA/B() for update rules and _channelMatches() for
    // the filter predicate applied on every incoming event.
    uint8_t    _midiChannelA = 1;
    uint8_t    _midiChannelB = 1;

    // CC notifier — stored locally so perf CCs can trigger UI refresh.
    // Also forwarded to both engines for patch CC notification.
    SynthEngine::NotifyFn _notifyFn = nullptr;

    // =========================================================================
    // Internal helpers
    // =========================================================================

    // Recalculate voice ranges on both engines based on _voicesA and _perfMode.
    void _applyVoiceSplit();

    // Recalculate mixer gains based on _balance and _perfMode.
    void _applyBalance();

    // Engage or release Engine B's full-chain FX bypass based on current
    // perf mode. In SINGLE mode, Engine B does no useful audio work — its
    // full FX chain (reverb + JPFX tone/mod/delay) is bypassed to save
    // ~10% CPU that would otherwise be spent processing silence through
    // the reverb tank. Called from begin() and whenever _perfMode changes.
    void _updateLayerBFXBypass();

    // Returns true if the given CC is a performance-level CC handled by
    // LayerManager itself (not forwarded to engines).
    static bool _isPerfCC(uint8_t cc);

    // Channel filter predicate — true iff an incoming event on `eventChan`
    // (1-16, as delivered by the MIDI stack) should reach an engine whose
    // configured channel is `engineChan` (1-16, stored in _midiChannelX).
    //
    // Current rule (strict multitimbral): event channel must equal engine
    // channel. Split out into a helper so the predicate has one home if
    // it ever needs to evolve (e.g. per-channel groups, channel ranges).
    static inline bool _channelMatches(uint8_t eventChan, uint8_t engineChan) {
        return eventChan == engineChan;
    }

    // Performance CC numbers — reference CC:: namespace constants from CCDefs.h.
    static constexpr uint8_t CC_PERF_MODE            = CC::PERF_MODE;
    static constexpr uint8_t CC_PERF_VOICE_SPLIT     = CC::PERF_VOICE_SPLIT;
    static constexpr uint8_t CC_PERF_SPLIT_NOTE      = CC::PERF_SPLIT_NOTE;
    static constexpr uint8_t CC_PERF_BALANCE         = CC::PERF_BALANCE;
    static constexpr uint8_t CC_PERF_EDIT_TARGET     = CC::PERF_EDIT_TARGET;
    static constexpr uint8_t CC_PERF_MIDI_CHANNEL_A  = CC::PERF_MIDI_CHANNEL_A;
    static constexpr uint8_t CC_PERF_MIDI_CHANNEL_B  = CC::PERF_MIDI_CHANNEL_B;

    // SysEx adapter back-reference for CC snoop (Phase 1). nullptr until
    // Jteensy8000 wires it in setup(). See setSysExSnoop().
    SysExAdapter* _sysExSnoop = nullptr;

    // CC cache (Phase 2) — populated by every CC dispatch through this class.
    // ~272 bytes; see CCCache.h for layout.
    CCCache::Storage _ccCache;

public:
    // =========================================================================
    // CC readback for UI — performance CCs return local state,
    // patch CCs delegate to the active engine's getCC().
    // =========================================================================
    uint8_t getCC(uint8_t cc) const;

    // Set a CC via the LayerManager (perf CCs handled locally,
    // patch CCs routed to edit target engine). Used by TFT UI encoders.
    void setCC(uint8_t cc, uint8_t value);
};
