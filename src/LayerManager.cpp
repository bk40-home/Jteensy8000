// =============================================================================
// LayerManager.cpp — JP-8080-style dual-layer performance coordinator
// =============================================================================
// See LayerManager.h for architecture notes and signal flow.
// =============================================================================

#include "LayerManager.h"
#include "DebugTrace.h"
#include "SysExAdapter.h"   // Phase 1 — for snoopCC() forward access
#include "SyxProtocol.h"    // Phase 2 — for kLayerA/B/Perf/GlobalFx constants

// =============================================================================
// Constructor — no AudioConnections here (same rule as SynthEngine).
//
// Both engines are constructed as full peers. They do NOT yet have a voice
// pool pointer — setVoicePool() is called from begin() so that the pool's
// VoiceBlock array is fully constructed (including its contained LFOs and
// other audio objects) before any engine tries to address it.
// =============================================================================
LayerManager::LayerManager()
    : _pool(), _engineA(), _engineB()
{
}

// =============================================================================
// begin() — call from setup() AFTER AudioMemory().
//
// Order is important:
//   1. Hand the shared VoicePool to both engines.
//   2. Both engines begin() — each wires its full audio graph to all 8
//      voices (per Option R3 — permanent wiring with slot-gain gating).
//   3. Wire each engine's FX chain stereo out into the performance mixer.
//   4. Apply initial perf state (SINGLE mode, all voices to A, Layer B
//      silenced via FX bypass).
// =============================================================================
void LayerManager::begin()
{
    // --- Reset cache (Phase 2) -------------------------------------------
    // CC cache starts empty. Live CC dispatch + SysEx writes populate it.
    // Boot patch loads bypass our handleControlChange() (call engine
    // handlers directly), so the editor pre-populates the cache on connect
    // by sending SET_PARAM for every UI-displayed param.
    _ccCache.reset();

    // --- Hand both engines a pointer to the shared pool -------------------
    // This replaces the old setSharedVoicePool() hack where Engine B borrowed
    // Engine A's voices. The pool is a first-class member of LayerManager
    // and both engines are peer consumers.
    _engineA.setVoicePool(&_pool);
    _engineB.setVoicePool(&_pool);

    // --- Initialise both engines ------------------------------------------
    // Each engine wires its own LFOs / step seq / voice-mixer slots to ALL
    // MAX_VOICES voices. Range gating via per-voice mixer slot gain prevents
    // the two engines' modulation from leaking across voice boundaries.
    _engineA.begin();
    _engineB.begin();

    // --- Initialise global FX bus -----------------------------------------
    // begin() wires the reverb's internal send→tank→wetAmp chain. External
    // taps (inputs) and perf-mixer returns (outputs) are wired below.
    _globalFx.begin();

    // --- Wire Layer A FX output → perf mixer ch0 (dry/JPFX path) ----------
    _patchAL = new AudioConnection(_engineA.getFXOutL(), 0, _perfMixerL, 0);
    _patchAR = new AudioConnection(_engineA.getFXOutR(), 0, _perfMixerR, 0);

    // --- Wire Layer B FX output → perf mixer ch1 (dry/JPFX path) ----------
    _patchBL = new AudioConnection(_engineB.getFXOutL(), 0, _perfMixerL, 1);
    _patchBR = new AudioConnection(_engineB.getFXOutR(), 0, _perfMixerR, 1);

    // --- Wire Layer A FX output → GlobalFX send input ch0 -----------------
    // Same stream can feed multiple destinations; the Teensy Audio Library
    // handles output-fan-out transparently.
    _patchATapToReverbL = new AudioConnection(_engineA.getFXOutL(), 0,
                                              _globalFx.getReverbSendInputL(), 0);
    _patchATapToReverbR = new AudioConnection(_engineA.getFXOutR(), 0,
                                              _globalFx.getReverbSendInputR(), 0);

    // --- Wire Layer B FX output → GlobalFX send input ch1 -----------------
    _patchBTapToReverbL = new AudioConnection(_engineB.getFXOutL(), 0,
                                              _globalFx.getReverbSendInputL(), 1);
    _patchBTapToReverbR = new AudioConnection(_engineB.getFXOutR(), 0,
                                              _globalFx.getReverbSendInputR(), 1);

    // --- Wire GlobalFX master-wet out → perf mixer ch2 --------------------
    _patchReverbWetToPerfL = new AudioConnection(_globalFx.getWetOutL(), 0,
                                                 _perfMixerL, 2);
    _patchReverbWetToPerfR = new AudioConnection(_globalFx.getWetOutR(), 0,
                                                 _perfMixerR, 2);

    // --- Perf mixer slot gains --------------------------------------------
    // ch0 and ch1 set by _applyBalance() below.
    // ch2 (reverb wet) is unity here — GlobalFX applies master-wet-level on
    // its own wet amp, so this mixer slot just passes the already-scaled
    // wet signal through.
    _perfMixerL.gain(2, 1.0f);
    _perfMixerR.gain(2, 1.0f);
    _perfMixerL.gain(3, 0.0f);   // spare
    _perfMixerR.gain(3, 0.0f);

    // --- Apply initial state: SINGLE mode, all voices to A ----------------
    // _applyVoiceSplit silences Layer B (voice count 0-or-1 with muted mixer)
    // and _applyBalance sets the perf-mixer gains. _updateLayerBFXBypass
    // engages the full-chain FX bypass on Engine B so its per-layer FX
    // produces no output and costs near-zero CPU in SINGLE.
    _applyVoiceSplit();
    _applyBalance();
    _updateLayerBFXBypass();

    JT_LOGF("[LayerMgr] begin() — SINGLE mode, %u voices on A\n",
            (unsigned)_voicesA);
}

// =============================================================================
// update() — call from loop(), forwards to both engines.
// In SINGLE mode, Layer B has no active voices so its update() is near-free
// (the voice activity loop skips all idle voices).
// =============================================================================
void LayerManager::update()
{
    _engineA.update();

    // Only update Layer B if it could have active voices
    if (_perfMode != PerfMode::SINGLE) {
        _engineB.update();
    }
}

// =============================================================================
// MIDI ROUTING — noteOn
//
// Strict multitimbral filter applied first:
//   - Compute which engines the incoming channel matches.
//   - Then apply PerfMode rules WITHIN the set of matched engines.
//
// If neither engine matches, the note is silently dropped. If both engines
// match (same channel set on A and B — "Dual" behaviour), PerfMode decides:
// SINGLE → only A, LAYER → both, SPLIT → note value chooses.
// =============================================================================
void LayerManager::noteOn(byte channel, byte note, float velocity)
{
    const bool aMatches = _channelMatches(channel, _midiChannelA);
    const bool bMatches = _channelMatches(channel, _midiChannelB);
    if (!aMatches && !bMatches) return;   // channel assigned to neither Part

    switch (_perfMode) {
        case PerfMode::SINGLE:
            // Only Layer A plays in SINGLE, and only if it accepts this channel.
            if (aMatches) _engineA.noteOn(note, velocity);
            break;

        case PerfMode::LAYER:
            // Both layers play, each gated by its own channel match.
            if (aMatches) _engineA.noteOn(note, velocity);
            if (bMatches) _engineB.noteOn(note, velocity);
            break;

        case PerfMode::SPLIT:
            // Note value chooses which Part should play. That Part still has
            // to accept the channel — otherwise the note is dropped even
            // though the other Part could technically play it. This matches
            // JP-8000 behaviour: SPLIT's zone decision is sovereign.
            if (note < _splitNote) {
                if (aMatches) _engineA.noteOn(note, velocity);
            } else {
                if (bMatches) _engineB.noteOn(note, velocity);
            }
            break;
    }
}

// =============================================================================
// MIDI ROUTING — noteOff
//
// Same filter as noteOn. A note cannot reach an engine that rejected the
// corresponding noteOn, so skipping noteOff on that engine is correct and
// avoids spurious engine.noteOff() calls (the engine's _noteToVoice[] would
// already be VOICE_NONE, but this keeps behaviour defensive and cheap).
// =============================================================================
void LayerManager::noteOff(byte channel, byte note)
{
    const bool aMatches = _channelMatches(channel, _midiChannelA);
    const bool bMatches = _channelMatches(channel, _midiChannelB);
    if (!aMatches && !bMatches) return;

    switch (_perfMode) {
        case PerfMode::SINGLE:
            if (aMatches) _engineA.noteOff(note);
            break;

        case PerfMode::LAYER:
            if (aMatches) _engineA.noteOff(note);
            if (bMatches) _engineB.noteOff(note);
            break;

        case PerfMode::SPLIT:
            // Send to whichever matched engine(s) might hold the note.
            // noteOff returns early if _noteToVoice[note] == VOICE_NONE,
            // so sending to both when both match is harmless.
            if (aMatches) _engineA.noteOff(note);
            if (bMatches) _engineB.noteOff(note);
            break;
    }
}

// =============================================================================
// MIDI ROUTING — CC
//
// Three-stage dispatch:
//   1. Perf CCs (140..146) are consumed locally by LayerManager. They
//      configure the manager itself — not channel-filtered.
//   2. Patch CCs (0..127 MIDI + internal engine CCs above 127 that aren't
//      perf CCs) are filtered BOTH by channel and by _editTarget. Strict
//      multitimbral per the design decision:
//         - channel filter gates WHICH engines can receive at all
//         - _editTarget further narrows which of the matched engines
//           actually receive. In most live scenarios _editTarget == BOTH
//           so channel match is the only effective filter.
//   3. Notifier fires once at the end for any CC that was handled locally
//      or routed to at least one engine.
// =============================================================================
void LayerManager::handleControlChange(byte channel, byte control, byte value)
{
    // --- SysEx adapter CC snoop (Phase 1) ------------------------------------
    // Runs first so the adapter sees every CC, including those generated
    // internally (e.g. by Patch::applyTo). The adapter only inspects a handful
    // of conflated CCs (103/106/107/109) for its abstraction state — branch
    // is cheap when not registered.
    if (_sysExSnoop) _sysExSnoop->snoopCC(control, value);

    // --- CC cache (Phase 2): perf and global-FX scope ------------------------
    // Both share the _isPerfCC() predicate in the current firmware (it covers
    // performance CCs 140..146 AND the reverb CCs). cacheCC() routes to the
    // right slot internally based on CC number.
    if (_isPerfCC(control)) {
        if (control >= CCCache::kPerfCcBase) {
            cacheCC(SyxProto::kLayerPerf, control, value);
        } else {
            cacheCC(SyxProto::kLayerGlobalFx, control, value);
        }
    }

    // --- Performance CCs: handled here, no channel filter --------------------
    if (_isPerfCC(control)) {
        switch (control) {
            case CC_PERF_MODE: {
                PerfMode mode;
                if      (value <= 42)  mode = PerfMode::SINGLE;
                else if (value <= 84)  mode = PerfMode::LAYER;
                else                   mode = PerfMode::SPLIT;
                setPerfMode(mode);
            } break;

            case CC_PERF_VOICE_SPLIT: {
                uint8_t voicesA = (uint8_t)constrain(
                    (int)value * 7 / 128 + 1, 1, MAX_VOICES - 1);
                setVoiceSplit(voicesA);
            } break;

            case CC_PERF_SPLIT_NOTE:
                setSplitNote(value);
                break;

            case CC_PERF_BALANCE:
                setBalance(value);
                break;

            case CC_PERF_EDIT_TARGET: {
                EditTarget t;
                if      (value <= 42)  t = EditTarget::LAYER_A;
                else if (value <= 84)  t = EditTarget::LAYER_B;
                else                   t = EditTarget::BOTH;
                setEditTarget(t);
            } break;

            case CC_PERF_MIDI_CHANNEL_A: {
                // Map 0..127 → 1..16. Formula: ((v * 16) / 128) + 1.
                // Integer division truncates, so the 128 values split into
                // 16 evenly-sized buckets of 8: 0..7 → ch1, 8..15 → ch2, etc.
                const uint8_t ch = (uint8_t)((value * 16) / 128) + 1;
                setMidiChannelA(ch);
            } break;

            case CC_PERF_MIDI_CHANNEL_B: {
                const uint8_t ch = (uint8_t)((value * 16) / 128) + 1;
                setMidiChannelB(ch);
            } break;

            // =================================================================
            // GLOBAL REVERB CCs — routed to _globalFx (Phase 3 move).
            // All value transforms match what FXChainBlock previously did
            // for these same CCs; only the destination has changed.
            // =================================================================
            case CC::FX_REVERB_SIZE:
                _globalFx.setReverbRoomSize(value / 127.0f);
                break;

            case CC::FX_REVERB_DAMP:        // hi damping
                _globalFx.setReverbHiDamping(value / 127.0f);
                break;

            case CC::FX_REVERB_LODAMP:
                _globalFx.setReverbLoDamping(value / 127.0f);
                break;

            case CC::FX_REVERB_MIX: {
                // Master wet level — both channels set to the same value so
                // the reverb comes out centred. Use setReverbMix() rather
                // than two separate setters to hit the auto-bypass check
                // exactly once per CC.
                const float m = value / 127.0f;
                _globalFx.setReverbMix(m, m);
            } break;

            case CC::FX_REVERB_BYPASS:
                // Firmware convention: toggle CCs emit 0 or 127.
                _globalFx.setReverbBypass(value >= 64);
                break;

            case CC::FX_REVERB_SHIMMER:
                _globalFx.setReverbShimmer(value / 127.0f);
                break;

            case CC::FX_REVERB_FREEZE:
                _globalFx.setReverbFreeze(value >= 64);
                break;

            case CC::FX_REVERB_LOWPASS:
                _globalFx.setReverbLowpass(value / 127.0f);
                break;

            case CC::FX_REVERB_HIPASS:
                _globalFx.setReverbHipass(value / 127.0f);
                break;
        }
        // Fire notifier so TFT repaints the control that changed
        if (_notifyFn) _notifyFn(control, value);
        return;
    }

    // --- Patch CCs: filter by channel first, then by edit target -------------
    const bool aMatches = _channelMatches(channel, _midiChannelA);
    const bool bMatches = _channelMatches(channel, _midiChannelB);
    if (!aMatches && !bMatches) return;   // channel assigned to neither Part

    // Compose the "which engines actually receive this CC" decision by
    // intersecting the channel match with the edit target selection.
    bool routeToA = false;
    bool routeToB = false;
    switch (_editTarget) {
        case EditTarget::LAYER_A: routeToA = aMatches;                 break;
        case EditTarget::LAYER_B: routeToB = bMatches;                 break;
        case EditTarget::BOTH:    routeToA = aMatches; routeToB = bMatches; break;
    }

    // Update cache (Phase 2) before dispatch so the slot reflects intent
    // even if the engine handler is later refactored to defer state changes.
    if (routeToA) {
        cacheCC(SyxProto::kLayerA, control, value);
        _engineA.handleControlChange(channel, control, value);
    }
    if (routeToB) {
        cacheCC(SyxProto::kLayerB, control, value);
        _engineB.handleControlChange(channel, control, value);
    }

    // Fire notifier once if we routed anywhere. Skipping it on "channel
    // matched but edit target rejected" is deliberate — the UI shouldn't
    // redraw for a CC that didn't modify any engine's state.
    // (Engine A still has its own notifier for CCs that bypass LayerManager,
    //  e.g. preset loads that call engine.handleControlChange directly.)
    if ((routeToA || routeToB) && _notifyFn) _notifyFn(control, value);
}

// =============================================================================
// MIDI ROUTING — Pitch bend
//
// Channel-filtered the same way as notes. A controller sending bend on
// channel 1 only affects whichever Part(s) are assigned to channel 1.
// Setting both Parts to the same channel = bend hits both (Dual behaviour).
// =============================================================================
void LayerManager::handlePitchBend(byte channel, int16_t value)
{
    const bool aMatches = _channelMatches(channel, _midiChannelA);
    const bool bMatches = _channelMatches(channel, _midiChannelB);

    if (aMatches) _engineA.handlePitchBend(channel, value);

    // In SINGLE mode Layer B has no voices, so even if its channel matches
    // the bend has nowhere to land (the engine's voice loop skips zero
    // voices). Still call through so internal state (if any) stays coherent.
    if (bMatches) _engineB.handlePitchBend(channel, value);
}

// =============================================================================
// Performance mode
// =============================================================================
void LayerManager::setPerfMode(PerfMode mode)
{
    if (_perfMode == mode) return;
    const PerfMode oldMode = _perfMode;
    _perfMode = mode;

    // When entering a dual-layer mode from SINGLE, auto-set a sensible
    // voice split if still at the single-mode default (all voices to A).
    if (oldMode == PerfMode::SINGLE && mode != PerfMode::SINGLE) {
        if (_voicesA >= MAX_VOICES) {
            _voicesA = MAX_VOICES / 2;  // 4+4 default split
        }
    }

    // Reconfigure voice allocation and mixer gains
    _applyVoiceSplit();
    _applyBalance();
    _updateLayerBFXBypass();   // engage/release Engine B's FX chain

    JT_LOGF("[LayerMgr] PerfMode → %s  voices A=%u B=%u\n",
        mode == PerfMode::SINGLE ? "SINGLE" :
        mode == PerfMode::LAYER  ? "LAYER"  : "SPLIT",
        (unsigned)_voicesA,
        (unsigned)(mode == PerfMode::SINGLE ? 0 : MAX_VOICES - _voicesA));
}

// =============================================================================
// Voice split
// =============================================================================
void LayerManager::setVoiceSplit(uint8_t voicesA)
{
    // Clamp: at least 1 voice per active layer
    if (voicesA < 1) voicesA = 1;
    if (voicesA > MAX_VOICES - 1) voicesA = MAX_VOICES - 1;

    _voicesA = voicesA;
    _applyVoiceSplit();

    JT_LOGF("[LayerMgr] VoiceSplit → A=%u B=%u\n",
            (unsigned)_voicesA, (unsigned)(MAX_VOICES - _voicesA));
}

// =============================================================================
// Split note
// =============================================================================
void LayerManager::setSplitNote(uint8_t note)
{
    _splitNote = constrain(note, 0, 127);
    JT_LOGF("[LayerMgr] SplitNote → %u\n", (unsigned)_splitNote);
}

// =============================================================================
// Layer balance
// =============================================================================
void LayerManager::setBalance(uint8_t value)
{
    _balance = value;
    _applyBalance();
}

// =============================================================================
// Edit target
// =============================================================================
void LayerManager::setEditTarget(EditTarget t)
{
    _editTarget = t;
    JT_LOGF("[LayerMgr] EditTarget → %s\n",
        t == EditTarget::LAYER_A ? "A" :
        t == EditTarget::LAYER_B ? "B" : "BOTH");
}

// =============================================================================
// Per-layer MIDI channel setters (JP-8000 convention: 1..16)
//
// Two-step operation:
//   1. Clamp the requested channel to 1..16. Values outside that range are
//      forced to 1 (a silently-ignored out-of-range input would leave the
//      layer unreachable by any live MIDI, which is a confusing bug mode).
//   2. Release any notes currently held on the affected engine. Without
//      this, a DAW sending noteOffs on the OLD channel after a channel
//      change would never reach the engine and the notes would stick
//      forever. We walk 0..127 calling engine.noteOff() — noteOff()
//      early-returns when the note isn't held, so the cost is trivial.
//
// No-op guard: if the requested channel equals the current channel, skip
// the release pass entirely so UI encoders/CCs setting the same value
// repeatedly don't cut held notes.
// =============================================================================
void LayerManager::setMidiChannelA(uint8_t ch1to16)
{
    const uint8_t newCh = (ch1to16 >= 1 && ch1to16 <= 16) ? ch1to16 : 1;
    if (newCh == _midiChannelA) return;

    // Release any held notes on Engine A before switching channels.
    // The next note on the new channel is the intended "next state".
    for (int n = 0; n < 128; ++n) _engineA.noteOff((byte)n);

    _midiChannelA = newCh;
    JT_LOGF("[LayerMgr] MIDI Ch A → %u\n", (unsigned)_midiChannelA);
}

void LayerManager::setMidiChannelB(uint8_t ch1to16)
{
    const uint8_t newCh = (ch1to16 >= 1 && ch1to16 <= 16) ? ch1to16 : 1;
    if (newCh == _midiChannelB) return;

    for (int n = 0; n < 128; ++n) _engineB.noteOff((byte)n);

    _midiChannelB = newCh;
    JT_LOGF("[LayerMgr] MIDI Ch B → %u\n", (unsigned)_midiChannelB);
}

// =============================================================================
// Engine access helpers
// =============================================================================
SynthEngine& LayerManager::activeEngine()
{
    return (_editTarget == EditTarget::LAYER_B) ? _engineB : _engineA;
}

const SynthEngine& LayerManager::activeEngine() const
{
    return (_editTarget == EditTarget::LAYER_B) ? _engineB : _engineA;
}

// =============================================================================
// BPM clock — forward to both engines
// =============================================================================
void LayerManager::setBPMClock(BPMClockManager* clock)
{
    _engineA.setBPMClock(clock);
    _engineB.setBPMClock(clock);
}

// =============================================================================
// UI notifier — forward to both engines so either triggers UI refresh
// =============================================================================
void LayerManager::setNotifier(SynthEngine::NotifyFn fn)
{
    _notifyFn = fn;           // stored locally — ALL notification goes through here
    // Neither engine gets a direct notifier. LayerManager fires _notifyFn
    // once per CC change, preventing double-fire and USB echo feedback loops.
    // Preset loads that call engine.handleControlChange() directly will not
    // trigger notification — call syncFromEngine() after preset load instead.
}

// =============================================================================
// INTERNAL: Apply voice split to both engines
// =============================================================================
void LayerManager::_applyVoiceSplit()
{
    if (_perfMode == PerfMode::SINGLE) {
        // Layer A gets all voices. Layer B gets a count of 0 — it does no
        // voice allocation, its modulation helpers write 0.0 to every slot
        // (so its LFOs don't leak into A's voices via the permanent wiring),
        // and its update() is skipped by LayerManager::update(). Genuine
        // zero-cost idle state.
        _engineA.setVoiceRange(0, MAX_VOICES);
        _engineB.setVoiceRange(0, 0);
    } else {
        // Dual mode: A gets first _voicesA, B gets the rest.
        const uint8_t voicesB = MAX_VOICES - _voicesA;
        _engineA.setVoiceRange(0, _voicesA);
        _engineB.setVoiceRange(_voicesA, voicesB);
    }
}

// =============================================================================
// INTERNAL: Apply balance gains to performance mixer
// =============================================================================
void LayerManager::_applyBalance()
{
    if (_perfMode == PerfMode::SINGLE) {
        // Layer A at unity, Layer B muted
        _perfMixerL.gain(0, 1.0f);
        _perfMixerR.gain(0, 1.0f);
        _perfMixerL.gain(1, 0.0f);
        _perfMixerR.gain(1, 0.0f);
        return;
    }

    // Crossfade: balance 0 = A only, 64 = equal, 127 = B only.
    // Use equal-power crossfade for smooth balance transitions.
    // norm: 0.0 (full A) → 1.0 (full B), 0.5 = centre.
    const float norm = (float)_balance / 127.0f;

    // Equal-power: cos/sin curve so that the total energy stays constant
    // at any balance position. Much smoother than linear crossfade.
    const float angleRad = norm * (float)(M_PI * 0.5);
    const float gainA = cosf(angleRad);
    const float gainB = sinf(angleRad);

    _perfMixerL.gain(0, gainA);
    _perfMixerR.gain(0, gainA);
    _perfMixerL.gain(1, gainB);
    _perfMixerR.gain(1, gainB);
}

// =============================================================================
// INTERNAL: Engage Engine B's full FX-chain bypass when SINGLE, release otherwise.
//
// In SINGLE mode Engine B produces no audio (voice count 0, perf-mixer ch1
// muted), but its FXChainBlock still registers with the Audio Library and
// its AudioStream::update() is called every 2.9 ms. JPFX (tone/mod/delay)
// is the only per-layer DSP cost here — reverb moved to GlobalFX (Phase 3)
// so it doesn't factor into this decision anymore.
//
// setBypass(true) zeroes the output mixer gains (dry + JPFX wet) so Engine
// B's FX chain produces silence. The cached mix values are preserved so
// the state restores exactly when the user moves to LAYER or SPLIT mode —
// no program-state loss during the SINGLE-mode idle period.
//
// JPFX itself still ticks every audio block because the Teensy library
// has no per-object bypass hook. In SINGLE, its input is silent (voices
// muted upstream) so it processes zeros at near-zero CPU cost.
// =============================================================================
void LayerManager::_updateLayerBFXBypass()
{
    const bool bypassB = (_perfMode == PerfMode::SINGLE);
    _engineB.getFXChain().setBypass(bypassB);
}

// =============================================================================
// INTERNAL: Performance CC identification
//
// Returns true for CCs that LayerManager handles locally — NOT forwarded
// to engines. Covers:
//   - The 7 perf-config CCs (mode, voice split, split note, balance,
//     edit target, MIDI channel A/B).
//   - The 9 global reverb CCs that were previously per-layer and are now
//     routed to the shared GlobalFX instance (Phase 3).
//
// If a new CC is added that LayerManager should own, it MUST be added
// here — otherwise it falls through to the patch-CC dispatch and ends
// up in the engine, which doesn't know what to do with it.
// =============================================================================
bool LayerManager::_isPerfCC(uint8_t cc)
{
    return cc == CC_PERF_MODE
        || cc == CC_PERF_VOICE_SPLIT
        || cc == CC_PERF_SPLIT_NOTE
        || cc == CC_PERF_BALANCE
        || cc == CC_PERF_EDIT_TARGET
        || cc == CC_PERF_MIDI_CHANNEL_A
        || cc == CC_PERF_MIDI_CHANNEL_B
        // --- Global reverb CCs (routed to _globalFx) ---
        || cc == CC::FX_REVERB_SIZE
        || cc == CC::FX_REVERB_DAMP
        || cc == CC::FX_REVERB_LODAMP
        || cc == CC::FX_REVERB_MIX
        || cc == CC::FX_REVERB_BYPASS
        || cc == CC::FX_REVERB_SHIMMER
        || cc == CC::FX_REVERB_FREEZE
        || cc == CC::FX_REVERB_LOWPASS
        || cc == CC::FX_REVERB_HIPASS;
}

// =============================================================================
// CC readback for UI — performance CCs return local state
// =============================================================================
uint8_t LayerManager::getCC(uint8_t cc) const {
    switch (cc) {
        case CC_PERF_MODE: {
            switch (_perfMode) {
                case PerfMode::SINGLE: return 21;   // midpoint of 0..42
                case PerfMode::LAYER:  return 63;   // midpoint of 43..84
                case PerfMode::SPLIT:  return 106;  // midpoint of 85..127
                default:               return 0;
            }
        }
        case CC_PERF_VOICE_SPLIT: {
            // Encode voicesA (1..7) back to 0..127
            return (uint8_t)constrain((_voicesA - 1) * 128 / 7, 0, 127);
        }
        case CC_PERF_SPLIT_NOTE:
            return _splitNote;
        case CC_PERF_BALANCE:
            return _balance;
        case CC_PERF_EDIT_TARGET: {
            switch (_editTarget) {
                case EditTarget::LAYER_A: return 21;
                case EditTarget::LAYER_B: return 63;
                case EditTarget::BOTH:    return 106;
                default:                  return 0;
            }
        }

        // Reverse of the CC→channel mapping used in handleControlChange:
        //   channel = ((v * 16) / 128) + 1           (set)
        //   v       = (channel - 1) * 8 + 4          (get — centre of bucket)
        // The returned value lands in the centre of the bucket that would
        // re-decode to the same channel, so round-tripping a channel through
        // setCC → getCC is stable. Clamp belt-and-braces.
        case CC_PERF_MIDI_CHANNEL_A: {
            const int v = (_midiChannelA - 1) * 8 + 4;
            return (uint8_t)constrain(v, 0, 127);
        }
        case CC_PERF_MIDI_CHANNEL_B: {
            const int v = (_midiChannelB - 1) * 8 + 4;
            return (uint8_t)constrain(v, 0, 127);
        }

        // =====================================================================
        // GLOBAL REVERB CCs — read back from _globalFx.
        //
        // These CCs are intercepted by handleControlChange() (perf branch) and
        // routed to _globalFx — they never reach any engine, so the engine's
        // _patch.ccState entry for these CCs stays 0. Without the cases below,
        // the default branch would delegate to activeEngine().getCC() and the
        // UI would read 0 for every reverb control, regardless of actual
        // reverb state.
        //
        // Each case inverts the write-path transform in handleControlChange.
        // Continuous params: lroundf keeps the round-trip stable. Toggles:
        // emit exact 0/127 to match the firmware CC convention (bucket-midpoint
        // formulas would send non-zero for "Off").
        // =====================================================================
        case CC::FX_REVERB_SIZE:
            return (uint8_t)constrain(
                lroundf(_globalFx.getReverbRoomSize() * 127.0f), 0L, 127L);

        case CC::FX_REVERB_DAMP:   // hi damping
            return (uint8_t)constrain(
                lroundf(_globalFx.getReverbHiDamping() * 127.0f), 0L, 127L);

        case CC::FX_REVERB_LODAMP:
            return (uint8_t)constrain(
                lroundf(_globalFx.getReverbLoDamping() * 127.0f), 0L, 127L);

        case CC::FX_REVERB_MIX:
            // Write path sets L and R to the same value; reading L is
            // sufficient and represents the master wet level.
            return (uint8_t)constrain(
                lroundf(_globalFx.getReverbMixL() * 127.0f), 0L, 127L);

        case CC::FX_REVERB_BYPASS:
            return _globalFx.getReverbBypass() ? 127 : 0;

        case CC::FX_REVERB_SHIMMER:
            return (uint8_t)constrain(
                lroundf(_globalFx.getReverbShimmer() * 127.0f), 0L, 127L);

        case CC::FX_REVERB_FREEZE:
            return _globalFx.getReverbFreeze() ? 127 : 0;

        case CC::FX_REVERB_LOWPASS:
            return (uint8_t)constrain(
                lroundf(_globalFx.getReverbLowpass() * 127.0f), 0L, 127L);

        case CC::FX_REVERB_HIPASS:
            return (uint8_t)constrain(
                lroundf(_globalFx.getReverbHipass() * 127.0f), 0L, 127L);

        default:
            // Patch CC — delegate to active engine
            return activeEngine().getCC(cc);
    }
}

// =============================================================================
// CC set from UI — routes perf CCs locally, patch CCs to edit target
// =============================================================================
void LayerManager::setCC(uint8_t cc, uint8_t value) {
    if (_isPerfCC(cc)) {
        handleControlChange(1, cc, value);
    } else {
        // Route to edit target engine(s)
        switch (_editTarget) {
            case EditTarget::LAYER_A:
                _engineA.handleControlChange(1, cc, value);
                break;
            case EditTarget::LAYER_B:
                _engineB.handleControlChange(1, cc, value);
                break;
            case EditTarget::BOTH:
                _engineA.handleControlChange(1, cc, value);
                _engineB.handleControlChange(1, cc, value);
                break;
        }
    }
}

// =============================================================================
// CC cache (Phase 2) — write/read a slot per layer + scope.
//
// Patch CCs (0..127) live in patchA[] / patchB[] indexed by raw CC number.
// Performance CCs (140..146) and the sparse GlobalFX CC numbers are packed
// into compact arrays via the helpers in CCCache.h. Writes outside the
// expected layout (e.g. an unrecognised CC) are silently dropped — keeps the
// API forgiving so callers don't have to care about the routing.
// =============================================================================
void LayerManager::cacheCC(uint8_t layer, uint8_t cc, uint8_t value) {
    switch (layer) {
        case SyxProto::kLayerA:
            if (cc < 128) _ccCache.patchA[cc] = value;
            break;
        case SyxProto::kLayerB:
            if (cc < 128) _ccCache.patchB[cc] = value;
            break;
        case SyxProto::kLayerPerf: {
            const uint8_t i = CCCache::perfIndex(cc);
            if (i != 0xFF) _ccCache.perf[i] = value;
        } break;
        case SyxProto::kLayerGlobalFx: {
            const uint8_t i = CCCache::gfxIndex(cc);
            if (i != 0xFF) _ccCache.gfx[i] = value;
        } break;
        default: break;
    }
}

uint8_t LayerManager::getCachedCC(uint8_t layer, uint8_t cc) const {
    switch (layer) {
        case SyxProto::kLayerA:
            return (cc < 128) ? _ccCache.patchA[cc] : CCCache::kUnset;
        case SyxProto::kLayerB:
            return (cc < 128) ? _ccCache.patchB[cc] : CCCache::kUnset;
        case SyxProto::kLayerPerf: {
            const uint8_t i = CCCache::perfIndex(cc);
            return (i != 0xFF) ? _ccCache.perf[i] : CCCache::kUnset;
        }
        case SyxProto::kLayerGlobalFx: {
            const uint8_t i = CCCache::gfxIndex(cc);
            return (i != 0xFF) ? _ccCache.gfx[i] : CCCache::kUnset;
        }
        default: return CCCache::kUnset;
    }
}