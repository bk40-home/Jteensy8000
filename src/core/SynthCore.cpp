// =============================================================================
// SynthCore.cpp — implementation (architecture in SynthCore.h)
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "core/SynthCore.h"

#include <string.h>   // memset
#include <cmath>      // powf (glide ms->rate map)

#include "core/dsp/Curves.h"
#include "gen/ParamTable.h"
#include "core/WavetableLib.h"

namespace JT {

namespace {
// v1 LFO_PITCH_MAX_SEMITONES · FM_SEMITONE_SCALE, already folded into v2's
// direct-semitone convention (spec §4): a unit-amplitude pitch LFO at full
// depth swings ±7 semitones.  Now lives in AudioConfig.h (kLfoPitchMaxSemis)
// because G1 routing needs it in Voice too.
} // namespace

SynthCore::SynthCore(ParameterStore& store, float* combPool,
                     float* reverbPool, float* fxPool)
    : _store(store),
      _router(store),
      // Boot layout is Single mode: layer A owns the entire pool, layer B owns
      // nothing.  That is the pre-Performance arrangement exactly, which is why
      // a default patch renders byte-identically to the single-layer engine.
      _layers{ Layer(_voices, VoiceAllocator::kMaxVoices, 0x1234567u, 0x89ABCDEu),
               Layer(_voices + VoiceAllocator::kMaxVoices, 0, 0x2468ACEu, 0x13579BDu) }
{
    // Attach the caller-owned reverb delay pool (PSRAM on Teensy, heap on host)
    // and apply v1 GlobalFX's ctor one-shot tank defaults.  Null pool => the
    // reverb is inert (processBlock bails) — legal for tests that ignore it.
    _reverb.begin(reverbPool);

    // Parameter smoothing bank: clears state and derives the two per-block
    // decay factors from SlewedValue.  Must run before any drain.
    initSlewBank();

    // Attach the caller-owned FX-chain delay pool (Phase 6).  Same ownership
    // model as reverb: PSRAM on Teensy, heap on host, null => inert.  The chain
    // stays disengaged (all stage-selectors OFF) until a param wires it up.
    _fx.begin(fxPool);

    // Slice the platform's pool across the 16 combs (voice-major).  A null
    // pool leaves every comb detached = feedback silently inert — legal,
    // but real builds should always provide it (see FeedbackComb.h for the
    // DTCM/OCRAM placement rationale).
    if (combPool != nullptr) {
        for (size_t v = 0; v < VoiceAllocator::kMaxVoices; ++v)
            for (int u = 0; u < 2; ++u)
                _voices[v].oscSection().attachCombStorage(
                    u, combPool + ((v * 2u) + (size_t)u)
                                * FeedbackComb::kDelaySamples);
    }
}

JT_COLD void SynthCore::disableExtmemPools()
{
    // begin(nullptr) is the engines' documented inert mode (the same one
    // host tests exercise when built without pools) — safe to re-enter.
    _reverb.begin(nullptr);
    _fx.begin(nullptr);
}

// -----------------------------------------------------------------------------
// Note event ring — single producer (control), single consumer (audio).
// Head/tail are free-running uint32 counters masked into the ring, the
// classic SPSC pattern: producer touches only _head, consumer only _tail,
// so no RMW contention exists at all.
// -----------------------------------------------------------------------------

void SynthCore::pushEvent(uint8_t type, uint8_t a, uint8_t b)
{
    const uint32_t head = _head.load(std::memory_order_relaxed);
    const uint32_t tail = _tail.load(std::memory_order_acquire);
    if (head - tail >= kRingSize) {
        // Full: drop the incoming event and count it.  Preferable to
        // overwriting in-flight slots (torn events) or blocking (control
        // plane must never wait on the audio plane).
        ++_dropped;
        return;
    }
    _ring[head % kRingSize] = NoteEvent{ type, a, b };
    _head.store(head + 1, std::memory_order_release);
}

void SynthCore::pushRouted(uint8_t type, uint8_t a, uint8_t b, LayerMask dest)
{
    // Nothing queued when the mask is empty: a note on a channel neither layer
    // answers to costs one compare and disappears here, not in the audio plane.
    if (has(dest, LayerMask::A)) pushEvent(type, a, b);
    if (has(dest, LayerMask::B)) pushEvent(static_cast<uint8_t>(type | kEvLayerB), a, b);
}

// Channel 0 is not a MIDI channel.  It is the "I have no channel" sentinel a
// bring-up console or a test harness passes, and it means LAYER A — never
// "route it and see".  Routing it would send it through forChannel(), where it
// matches neither layer and vanishes: a caller with no channel would go silent
// the moment a user enabled Layer mode, which is a trap, not a feature.
static inline JT::LayerMask routeOrDefaultA(const JT::PerfRouter& r,
                                            uint8_t note, uint8_t channel,
                                            bool haveNote)
{
    if (channel == 0u) return JT::LayerMask::A;
    return haveNote ? r.forNote(note, channel) : r.forChannel(channel);
}

void SynthCore::noteOn(uint8_t note, uint8_t velocity, uint8_t channel1to16)
{
    pushRouted(kEvOn, note, velocity,
               routeOrDefaultA(_router, note, channel1to16, true));
}

void SynthCore::noteOff(uint8_t note, uint8_t channel1to16)
{
    // Routed identically to note-on.  It has to be: if the split point or the
    // mode moved between the two, the note-off would arrive at a layer that
    // never played the note and the original would hang.  That is a real risk
    // and it is NOT fixed here — see the note in drainNoteEvents.
    pushRouted(kEvOff, note, 0,
               routeOrDefaultA(_router, note, channel1to16, true));
}

void SynthCore::sustain(bool pedalDown, uint8_t channel1to16)
{
    pushRouted(kEvSustain, pedalDown ? 1 : 0, 0,
               routeOrDefaultA(_router, 0, channel1to16, false));
}

// Panic is never routed.  CC 120/123 must silence the instrument whatever the
// performance setup is — that is the v1 bug this redesign started with.
void SynthCore::allNotesOff() { pushRouted(kEvNotesOff, 0, 0, LayerMask::Both); }
void SynthCore::allSoundOff() { pushRouted(kEvSoundOff, 0, 0, LayerMask::Both); }

size_t SynthCore::activeVoices() const
{
    size_t n = 0;
    for (const Voice& v : _voices) if (v.isActive()) ++n;
    return n;
}

uint8_t SynthCore::activeVoiceMask() const
{
    uint8_t m = 0;
    for (size_t i = 0; i < VoiceAllocator::kMaxVoices; ++i)
        if (_voices[i].isActive()) m |= static_cast<uint8_t>(1u << i);
    return m;
}

// --- external MIDI clock producers (Phase 9) -------------------------------
// Called from the platform's realtime-byte handlers.  They ONLY store into the
// lock-free atomics; renderBlock's drainExternalClock() applies them next block
// in the audio plane.  bpm is clamped to TempoClock's 40..300 range at apply
// time; here we just carry it as BPM×1000 (0 sentinel = "no external tempo").
void SynthCore::setExternalBpm(float bpm)
{
    if (bpm < 1.0f) return;                       // ignore garbage / not-yet-locked
    const uint32_t milli = (uint32_t)(bpm * 1000.0f + 0.5f);
    _extBpmMilli.store(milli, std::memory_order_release);
}

void SynthCore::transportStart()
{
    _transportAction.store(kTransStart, std::memory_order_relaxed);
    _transportSeq.fetch_add(1, std::memory_order_release);
}

void SynthCore::transportStop()
{
    _transportAction.store(kTransStop, std::memory_order_relaxed);
    _transportSeq.fetch_add(1, std::memory_order_release);
}

void SynthCore::transportContinue()
{
    _transportAction.store(kTransContinue, std::memory_order_relaxed);
    _transportSeq.fetch_add(1, std::memory_order_release);
}

// --- external MIDI clock consumer (audio plane, top of renderBlock) --------
void SynthCore::drainExternalClock()
{
    // BPM: apply only while the shared clock's source is External, so an
    // incoming DAW clock drives the whole synth (LFOs + seq + arp) but a synth
    // left on Internal ignores stray clock bytes.  refreshSyncedLfos re-resolves
    // synced LFO rates just like a CLOCK_TEMPO edit.
    if (_clock.source() == TempoClock::kExtMidi) {
        const uint32_t milli = _extBpmMilli.load(std::memory_order_acquire);
        if (milli != 0) {
            const float bpm = (float)milli * 0.001f;
            if (bpm != _clock.bpm()) { _clock.setBpm(bpm); refreshSyncedLfos(); }
        }
    }

    // Transport: apply the latest action if new since we last looked.  Start
    // resets the arp (and seq) to the downbeat; Stop silences the arp; Continue
    // resumes without a phase reset.  Gated on the source too — transport from a
    // DAW shouldn't reset a synth the user is driving from its own internal clock.
    const uint32_t seq = _transportSeq.load(std::memory_order_acquire);
    if (seq != _transportSeen) {
        _transportSeen = seq;
        if (_clock.source() == TempoClock::kExtMidi) {
            switch (_transportAction.load(std::memory_order_relaxed)) {
                case kTransStart:
                    for (Layer& lz : _layers) lz.arp.transportStart();
                    _seq.reset();
                    break;
                case kTransStop:
                    for (Layer& lz : _layers) lz.arp.transportStop();
                    for (Layer& lz : _layers) lz.arp.allNotesOff();   // release held arp notes
                    break;
                case kTransContinue:
                default:
                    break;                   // resume: nothing to reset
            }
        }
    }
}

// Pitch bend: split the 14-bit value across the event's two payload bytes
// (a = high 7 bits, b = low 7 bits) so it rides the existing 3-byte ring
// unchanged, then recombine in the drain (spec §4.3).
void SynthCore::pitchBend(uint16_t value14)
{
    pushEvent(kEvBend, (uint8_t)((value14 >> 7) & 0x7F), (uint8_t)(value14 & 0x7F));
}

void SynthCore::modWheel(uint8_t value7, uint8_t channel1to16)
{
    pushRouted(kEvModWheel, (uint8_t)(value7 & 0x7F), 0,
               routeOrDefaultA(_router, 0, channel1to16, false));
}

// GLIDE_TIME norm (0..1) -> v1 log ms (1..11880) -> v1's per-sample fraction
// 1/samples.  Applied ONCE PER BLOCK in Voice, which is v1's documented ~128×
// "quirk" (spec §1.1/§4.1), preserved so preset glide-times feel identical.
float SynthCore::glideRateFromNorm(float norm)
{
    // v1 Mapping.h: ms = msMin·(msMax/msMin)^norm, msMin=1, msMax=11880.
    const float ms      = 1.0f * powf(11880.0f, norm);
    const float samples = (ms / 1000.0f) * kSampleRate;   // v1 setGlideTime
    return (samples > 0.0f) ? (1.0f / samples) : 0.0f;
}

void SynthCore::drainNoteEvents()
{
    uint32_t tail = _tail.load(std::memory_order_relaxed);
    const uint32_t head = _head.load(std::memory_order_acquire);
    while (tail != head) {
        const NoteEvent e = _ring[tail % kRingSize];

        // The layer was resolved on the control plane when this event was
        // queued (see pushRouted).  The audio plane only unpacks it — routing
        // here would read the store from the wrong context AND could disagree
        // with the routing a matching note-on already used.
        //
        // KNOWN GAP: if perf.mode / perf.split_note / the channels move while a
        // note is held, its note-off routes to a layer that never played it and
        // the note hangs.  Panic clears it, and the split-change path below
        // silences everything anyway; a held-note re-route is deliberately not
        // attempted here.
        Layer&        L    = _layers[(e.type & kEvLayerB) ? 1u : 0u];
        const uint8_t type = static_cast<uint8_t>(e.type & kEvTypeMask);
        switch (type) {
            case kEvOn:
                // Phase 9: when the arp is ENABLED, played notes are CONSUMED —
                // they feed the arp's held-note list and only the arp sounds
                // (classic behaviour, user-signed-off).  The arp itself drives
                // _alloc from its tick() in renderBlock.  When the arp is OFF
                // this whole branch is skipped and the note goes straight to the
                // allocator exactly as before (byte-identical default path).
                if (L.arp.enabled()) {
                    L.arp.noteOn(e.a, e.b);
                } else {
                    L.alloc.noteOn(e.a, e.b);
                }
                // v1's shared, global JP-8000 retrigger (spec §1.3): ANY
                // note-on restarts BOTH LFOs' delay ramps, not just the
                // voice that's stealing/reusing hardware.  Still fires under
                // the arp so LFO delay behaves the same whether or not the arp
                // is between the keys and the voices.
                L.lfo1.osc.retrigger();
                L.lfo2.osc.retrigger();
                // Phase 7: a note-on restarts the sequencer to step 0 when
                // SEQ_RETRIGGER is on (v1 SynthEngine.cpp:470) — phase-locks the
                // pattern to played notes.  Off => the running position is left
                // untouched (free-running clock).
                if (_seq.retrigger()) _seq.reset();
                break;
            case kEvOff:
                if (L.arp.enabled()) L.arp.noteOff(e.a);
                else                L.alloc.noteOff(e.a);
                break;
            case kEvSustain:  L.alloc.sustain(e.a != 0);    break;
            case kEvNotesOff:
                // Panic clears BOTH the arp's held list and the voices, so a
                // DAW all-notes-off silences everything regardless of routing.
                L.arp.allNotesOff();
                L.alloc.allNotesOff();
                break;
            case kEvSoundOff:
                L.arp.allNotesOff();
                L.alloc.allSoundOff();
                break;
            case kEvModWheel:
                // Fixed destination until the mod matrix exists: the wheel
                // adds vibrato via LFO1's pitch lane.  Full wheel is a
                // deliberately modest kModWheelPitchDepth of the knob's full
                // range — a wheel that reaches +-7 semitones would be
                // unplayable, and the knob is still there for extremes.
                //
                // Wheel at rest writes 0.0f, so the default patch's pitch
                // depth and engaged() state are exactly what they were
                // before the wheel existed.
                L.lfo1.depthPitchMod =
                    Curves::normFrom7bit(e.a) * kModWheelPitchDepth;
                break;
            case kEvBend: {
                // Recombine the 14-bit value (raw form: 0..16383, centre 8192)
                // and convert to semitones with the current range, then push the
                // shared offset to every voice's FM path (spec §4.3).
                //
                // CONTRACT: pitchBend() takes the RAW value, so centre 8192 must
                // map to 0 st.  main.cpp converts the transports' centred wheel
                // (-8192..+8191) to this raw form with +8192 — see the long note
                // there about the recurring "centre plays sharp" bug.  The maths
                // below is numerically identical to v1's normalised=value/8192
                // applied to the centred value; it is NOT a behaviour change.
                const uint16_t value14 = (uint16_t)(((uint16_t)e.a << 7) | e.b);
                const float normalised = ((float)value14 - 8192.0f) / 8192.0f;  // -1..+1
                const float semis      = normalised * L.bendRange;
                for (Voice& v : L.voices()) v.setBendSemis(semis);
            } break;
            default:                                        break;
        }
        ++tail;
    }
    _tail.store(tail, std::memory_order_release);
}

// -----------------------------------------------------------------------------
// Parameter fan-out.  Engineering conversion happens HERE, exactly once per
// change (brief §4.3) — voices receive ready-to-use Hz/ms/levels and never
// see normalized values.
// -----------------------------------------------------------------------------

// =============================================================================
// Generic parameter smoothing
// =============================================================================
// Consumes ParamTable's smooth_ms, which until this existed was declared on 76
// parameters and read by nobody.  See the SlewBank comment in the header for
// the placement argument.
// =============================================================================

void SynthCore::initSlewBank()
{
    for (size_t i = 0; i < ParameterStore::kSlots; ++i) {
        _slew.cur[i]  = 0.0f;
        _slew.seen[i] = false;
    }
    _slew.nActive = 0;

    // Derive the two per-block decay factors FROM SlewedValue rather than
    // restating its maths here, so the ported class stays the single authority
    // on what a smooth_ms value means.  Only two distinct values exist in the
    // whole table (5 and 20), which is what makes the shared-coefficient
    // approach possible instead of one smoother object per slot.
    SlewedValue probe;
    probe.setSampleRate(kSampleRate);
    probe.setBlockSize((int)kBlockSize);
    for (int pass = 0; pass < 2; ++pass) {
        probe.setTimeMs(pass == 0 ? 5.0f : 20.0f);
        probe.reset(0.0f);
        probe.setTarget(1.0f);
        // One block from 0 toward 1 leaves exactly the decay factor behind.
        const float remaining = 1.0f - probe.tickBlock();
        (pass == 0 ? _slew.decayFast : _slew.decaySlow) = remaining;
    }
}

void SynthCore::routeParam(size_t slot, float target)
{
    const size_t  index = Params::paramOfSlot(slot);
    const uint8_t layer = Params::layerOfSlot(slot);
    const uint8_t ms    = Params::kParams[index].smoothMs;

    // Straight through when the table says step (smooth_ms 0), and ALSO on the
    // first write to a slot.  That first-write snap is deliberate: a patch load
    // must land on its values, not sweep into them from whatever the previous
    // patch left behind - which would be audible as the whole synth swooping on
    // every program change.
    if (ms == 0 || !_slew.seen[slot]) {
        _slew.cur[slot]  = target;
        _slew.seen[slot] = true;
        applyParam(index, target, layer);
        return;
    }

    if (target == _slew.cur[slot]) return;   // nowhere to go; stay quiet

    // Enrol in the active list if it is not already there.  Linear scan over at
    // most kMaxActive entries, only on an actual parameter change, never per
    // block and never per sample.
    for (uint8_t i = 0; i < _slew.nActive; ++i)
        if (_slew.active[i] == (uint16_t)slot) return;   // already gliding

    if (_slew.nActive < SlewBank::kMaxActive) {
        _slew.active[_slew.nActive++] = (uint16_t)slot;
        return;
    }

    // List full: more than kMaxActive parameters moving at once, which in
    // practice means automation rather than hands.  Apply this one immediately
    // rather than dropping it - a stepped parameter is a far smaller fault than
    // a parameter that silently never reaches its target.
    _slew.cur[slot] = target;
    applyParam(index, target, layer);
}

void SynthCore::tickSlewBank(const float* snap)
{
    if (_slew.nActive == 0) return;   // the common case: one compare per block

    uint8_t w = 0;                    // compacting write cursor
    for (uint8_t r = 0; r < _slew.nActive; ++r) {
        const uint16_t slot   = _slew.active[r];
        const size_t   index  = Params::paramOfSlot(slot);
        const uint8_t  layer  = Params::layerOfSlot(slot);
        const float    target = snap[slot];   // the store IS the target store

        const float decay = (Params::kParams[index].smoothMs >= 20)
                              ? _slew.decaySlow : _slew.decayFast;
        float cur = target + (_slew.cur[slot] - target) * decay;

        // Settle test mirrors SlewedValue::kSlewEps (1e-5, about -100 dB on a
        // 0..1 range).  Snapping the last sliver matters: without it the value
        // creeps forever, every block, and the parameter never leaves the
        // active list.
        if (fabsf(cur - target) < 1.0e-5f) {
            cur = target;                     // arrive exactly, then drop out
        } else {
            _slew.active[w++] = slot;         // still moving: keep it
        }

        _slew.cur[slot] = cur;
        applyParam(index, cur, layer);
    }
    _slew.nActive = w;
}

void SynthCore::applyParam(size_t index, float norm, uint8_t layer)
{
    using namespace Params;
    const ParamDesc& d = kParams[index];

    // Every voice fan-out below runs over ONE layer's slice.  Shared,
    // performance and global parameters always arrive with layer 0 (see
    // Params::layerOfSlot), so their handlers simply never look at L — and in
    // Single mode layer A owns the whole pool, which is why the fan-out is
    // identical to the pre-layer engine's.
    Layer& L = _layers[layer & 1u];

    // Engineering conversion happens once, here (brief §4.3).  Select-type
    // parameters need the option INDEX, not a raw engineering float.
    const float eng = Curves::toEngineering(d, norm);
    const int   opt = (int)Curves::toOptionIndex(d, norm);

    // Fan a per-unit setter across all voices: units 0/1 share handlers
    // because OSC1/OSC2 param blocks are index-shifted twins in the table.
    auto oscs = [&](int unit, auto&& fn) {
        for (Voice& v : L.voices()) fn(v.oscSection(), unit);
    };
    (void)L;   // some handlers are layer-agnostic; silence -Wunused for them

    // ---- Explicit per-step arrays (5 lanes x 16 steps) --------------------
    // Decoded ARITHMETICALLY from contiguous ParamID blocks rather than as 80
    // switch labels: the table guarantees the blocks are contiguous (the
    // generator refuses non-contiguous indices within a section), the branch
    // count stays flat, and adding a lane never touches this switch.
    //
    // These replaced a CURSOR idiom (a step_select parameter followed by a
    // value write).  One store slot cannot represent a sixteen-value pattern,
    // so patterns were never saved in a patch and never reached an editor.
    {
        const uint16_t id = d.id;
        if (id >= ID::SEQ_STEP_1 && id <= ID::SEQ_STEP_16) {
            _seq.setStepValue((int)(id - ID::SEQ_STEP_1),
                              (uint8_t)lroundf(norm * 127.0f));
            return;
        }
        if (id >= ID::SEQ_AUX_STEP_1 && id <= ID::SEQ_AUX_STEP_16) {
            _seq.setAuxStepValue((int)(id - ID::SEQ_AUX_STEP_1),
                                 (uint8_t)lroundf(norm * 127.0f));
            return;
        }
        if (id >= ID::ARP_STEP_ON_1 && id <= ID::ARP_STEP_ON_16) {
            L.arp.setStepOn((int)(id - ID::ARP_STEP_ON_1), norm >= 0.5f);
            return;
        }
        if (id >= ID::ARP_STEP_ACCENT_1 && id <= ID::ARP_STEP_ACCENT_16) {
            L.arp.setStepAccent((int)(id - ID::ARP_STEP_ACCENT_1), norm);
            return;
        }
        if (id >= ID::ARP_STEP_RATCHET_1 && id <= ID::ARP_STEP_RATCHET_16) {
            L.arp.setStepRatchet((int)(id - ID::ARP_STEP_RATCHET_1),
                                 1 + (int)lroundf(norm * 3.0f));
            return;
        }
    }

    switch (d.id) {
        // ------------- master / filter / amp env (Phase 1 set) -------------
        case ID::MASTER_VOLUME:    _masterTarget = eng; break;
        // Cutoff/resonance pass the NORM, not engineering Hz: the section
        // owns v1's per-type shape rows (fc range + res γ), so the knob
        // position is the canonical input — exactly what v1's CC was.
        case ID::FILTER_CUTOFF:
            // Both views of the knob travel: VA shapes the norm per type,
            // OBXa consumes the Hz (see FilterSection::setCutoff).
            //
            // Not smoothed here: smoothing is generic now and happens upstream
            // in the drain (see routeParam / tickSlewBank), so by the time this
            // runs `norm` is ALREADY the glided value.  Every case in this
            // switch gets that for free and none of them has to know about it.
            for (Voice& v : L.voices()) v.filter().setCutoff(norm, eng);
            break;
        case ID::FILTER_RESONANCE:
            for (Voice& v : L.voices()) v.filter().setResonanceNorm(norm);
            break;
        case ID::FILTER_ENGINE:
            for (Voice& v : L.voices()) v.filter().setEngine(opt);
            break;
        case ID::FILTER_VA_TYPE:
            for (Voice& v : L.voices()) v.filter().setVaType(opt);
            break;
        case ID::FILTER_MODE:
            for (Voice& v : L.voices()) v.filter().setObxaMode(opt);
            break;
        case ID::FILTER_OBXA_MULTIMODE:
            for (Voice& v : L.voices()) v.filter().setObxaMultimode(eng);
            break;
        case ID::FILTER_OBXA_XPANDER_MODE:
            for (Voice& v : L.voices()) v.filter().setObxaXpanderMode(opt);
            break;
        // Drive scales the signal INTO the VA topology so it meets that
        // section's output saturator harder.  There is deliberately NO output
        // compensation — the demo bank had none (AudioFilterVABank.cpp:334 is
        // a bare `x *= _drive`), and an earlier 1/√drive make-up here was
        // measured to turn drive into a fader on self-oscillation and nothing
        // else.  Level is allowed to rise with drive, as the demo's did.
        //
        // `eng` is the multiplier itself, not a norm: the table's range is
        // min 1 / max 4 linear, so this is literally 1.0 + 3.0×norm and the
        // knob's zero position is unity.
        //
        // Passing `eng` rather than `norm` is deliberate and load-bearing —
        // setDrive() tests for exact equality with 1.0f to decide whether the
        // drive path is active at all, and a norm would make that test fire on
        // the wrong end of the knob.
        //
        // No effect under the OBXa engine: the demo bank was VA-only, so OBXa
        // has no drive path here.  Safe to push unconditionally to every voice.
        case ID::FILTER_DRIVE:
            for (Voice& v : L.voices()) v.filter().setDrive(eng);
            break;
        // FILTER_OBXA_RES_MOD_DEPTH: resonance mod-bus depth (LFO/seq -> res),
        // consumed by the later mod wiring.
        case ID::ENV_AMP_ATTACK:
            for (Voice& v : L.voices()) v.ampEnv().setAttackMs(eng);
            break;
        case ID::ENV_AMP_DECAY:
            for (Voice& v : L.voices()) v.ampEnv().setDecayMs(eng);
            break;
        case ID::ENV_AMP_SUSTAIN:
            for (Voice& v : L.voices()) v.ampEnv().setSustain(eng);
            break;
        case ID::ENV_AMP_RELEASE:
            for (Voice& v : L.voices()) v.ampEnv().setReleaseMs(eng);
            break;
        case ID::ENV_AMP_ATTACK_CURVE:
            for (Voice& v : L.voices()) v.ampEnv().setAttackSlope(eng);
            break;
        case ID::ENV_AMP_DECAY_CURVE:
            for (Voice& v : L.voices()) v.ampEnv().setDecaySlope(eng);
            break;
        case ID::ENV_AMP_RELEASE_CURVE:
            for (Voice& v : L.voices()) v.ampEnv().setReleaseSlope(eng);
            break;

        // ------------- filter cutoff modulation (Pass 6) -------------------
        // Depths for the block-rate cutoff mod that FilterSection combines
        // (env + key track, scaled by octave control).  env_amount/key_track
        // are bipolar -1..+1 (eng already signed).  octave_control is a 0..1
        // knob in the table; v1 scaled its CC by ×10 to octaves
        // (SynthEngine FILTER_OCTAVE_CONTROL: o = norm × 10) — reproduced here.
        case ID::FILTER_ENV_AMOUNT:
            for (Voice& v : L.voices()) v.filter().setEnvAmount(eng);
            break;
        case ID::FILTER_KEY_TRACK:
            for (Voice& v : L.voices()) v.filter().setKeyTrackAmount(eng);
            break;
        case ID::FILTER_OCTAVE_CONTROL:
            for (Voice& v : L.voices()) v.filter().setOctaveControl(eng * 10.0f);
            break;

        // ------------- filter envelope (Pass 6) ----------------------------
        // Same EnvGen class as the amp env; destination is cutoff, not gain.
        case ID::ENV_FILTER_ATTACK:
            for (Voice& v : L.voices()) v.filterEnv().setAttackMs(eng);
            break;
        case ID::ENV_FILTER_DECAY:
            for (Voice& v : L.voices()) v.filterEnv().setDecayMs(eng);
            break;
        case ID::ENV_FILTER_SUSTAIN:
            for (Voice& v : L.voices()) v.filterEnv().setSustain(eng);
            break;
        case ID::ENV_FILTER_RELEASE:
            for (Voice& v : L.voices()) v.filterEnv().setReleaseMs(eng);
            break;
        case ID::ENV_FILTER_ATTACK_CURVE:
            for (Voice& v : L.voices()) v.filterEnv().setAttackSlope(eng);
            break;
        case ID::ENV_FILTER_DECAY_CURVE:
            for (Voice& v : L.voices()) v.filterEnv().setDecaySlope(eng);
            break;
        case ID::ENV_FILTER_RELEASE_CURVE:
            for (Voice& v : L.voices()) v.filterEnv().setReleaseSlope(eng);
            break;

        // ------------- pitch envelope (Pass 7) -----------------------------
        // Third EnvGen per voice; destination is oscillator pitch, applied as a
        // per-sample ramp through the FM input (non-zipped — see Voice/OscSection).
        // Depth is the table's bipolar -1..+1 knob scaled to ±24 semitones: v1
        // constrained the pitch-env depth to ±24 st (2 octaves) and summed it
        // into the FM mixer at unity gain.
        case ID::ENV_PITCH_DEPTH:
            for (Voice& v : L.voices()) v.setPitchEnvDepthSemis(eng * 24.0f);
            break;
        case ID::ENV_PITCH_ATTACK:
            for (Voice& v : L.voices()) v.pitchEnv().setAttackMs(eng);
            break;
        case ID::ENV_PITCH_DECAY:
            for (Voice& v : L.voices()) v.pitchEnv().setDecayMs(eng);
            break;
        case ID::ENV_PITCH_SUSTAIN:
            for (Voice& v : L.voices()) v.pitchEnv().setSustain(eng);
            break;
        case ID::ENV_PITCH_RELEASE:
            for (Voice& v : L.voices()) v.pitchEnv().setReleaseMs(eng);
            break;
        case ID::ENV_PITCH_ATTACK_CURVE:
            for (Voice& v : L.voices()) v.pitchEnv().setAttackSlope(eng);
            break;
        case ID::ENV_PITCH_DECAY_CURVE:
            for (Voice& v : L.voices()) v.pitchEnv().setDecaySlope(eng);
            break;
        case ID::ENV_PITCH_RELEASE_CURVE:
            for (Voice& v : L.voices()) v.pitchEnv().setReleaseSlope(eng);
            break;

        // ------------- velocity sensitivity (Pass 8) -----------------------
        // Three 0..1 knobs, consumed at the voice's NEXT noteOn (v1 applied all
        // three per-note in VoiceBlock::noteOn — static DC, not per-block).  eng
        // == norm here (Curve::Lin, 0..1).  amp-sens curves velocity→gain;
        // filter-sens/env-sens derive per-note filter DC (see Voice::noteOn).
        case ID::VELOCITY_AMP_SENS:
            for (Voice& v : L.voices()) v.setVelAmpSens(eng);
            break;
        case ID::VELOCITY_FILTER_SENS:
            for (Voice& v : L.voices()) v.setVelFilterSens(eng);
            break;
        case ID::VELOCITY_ENV_SENS:
            for (Voice& v : L.voices()) v.setVelEnvSens(eng);
            break;

        // ------------- oscillator units (Pass 4) ---------------------------
        // Engineering-unit scaling per the v1 CC-edge conversions, verified
        // in source: fine ±100 cents, detune ±12 st, freq DC ±24 st.
        case ID::OSC1_WAVE:            oscs(0, [&](OscSection& s, int u){ s.setWave(u, opt); }); break;
        case ID::OSC2_WAVE:            oscs(1, [&](OscSection& s, int u){ s.setWave(u, opt); }); break;
        case ID::OSC1_PITCH_OFFSET:    oscs(0, [&](OscSection& s, int u){ s.setPitchOffset(u, opt); }); break;
        case ID::OSC2_PITCH_OFFSET:    oscs(1, [&](OscSection& s, int u){ s.setPitchOffset(u, opt); }); break;
        case ID::OSC1_FINE_TUNE:       oscs(0, [&](OscSection& s, int u){ s.setFineTuneCents(u, eng * 100.0f); }); break;
        case ID::OSC2_FINE_TUNE:       oscs(1, [&](OscSection& s, int u){ s.setFineTuneCents(u, eng * 100.0f); }); break;
        case ID::OSC1_DETUNE:          oscs(0, [&](OscSection& s, int u){ s.setDetuneSemis(u, eng * 12.0f); }); break;
        case ID::OSC2_DETUNE:          oscs(1, [&](OscSection& s, int u){ s.setDetuneSemis(u, eng * 12.0f); }); break;
        case ID::OSC1_FREQ_DC:         oscs(0, [&](OscSection& s, int u){ s.setFreqDcSemis(u, eng * 24.0f); }); break;
        case ID::OSC2_FREQ_DC:         oscs(1, [&](OscSection& s, int u){ s.setFreqDcSemis(u, eng * 24.0f); }); break;
        case ID::OSC1_SHAPE_DC:        oscs(0, [&](OscSection& s, int u){ s.setShapeDc(u, eng); }); break;
        case ID::OSC2_SHAPE_DC:        oscs(1, [&](OscSection& s, int u){ s.setShapeDc(u, eng); }); break;
        case ID::OSC1_SUPERSAW_DETUNE: oscs(0, [&](OscSection& s, int u){ s.setSupersawDetune(u, eng); }); break;
        case ID::OSC2_SUPERSAW_DETUNE: oscs(1, [&](OscSection& s, int u){ s.setSupersawDetune(u, eng); }); break;
        case ID::OSC1_SUPERSAW_MIX:    oscs(0, [&](OscSection& s, int u){ s.setSupersawMix(u, eng); }); break;
        case ID::OSC2_SUPERSAW_MIX:    oscs(1, [&](OscSection& s, int u){ s.setSupersawMix(u, eng); }); break;
        case ID::OSC1_RING_MIX:        oscs(0, [&](OscSection& s, int u){ s.setRingMix(u, eng); }); break;
        case ID::OSC2_RING_MIX:        oscs(1, [&](OscSection& s, int u){ s.setRingMix(u, eng); }); break;
        case ID::OSC1_FEEDBACK_AMOUNT: oscs(0, [&](OscSection& sec, int u){ sec.setFeedbackAmount(u, eng); }); break;
        case ID::OSC2_FEEDBACK_AMOUNT: oscs(1, [&](OscSection& sec, int u){ sec.setFeedbackAmount(u, eng); }); break;
        case ID::OSC1_FEEDBACK_MIX:    oscs(0, [&](OscSection& sec, int u){ sec.setFeedbackMix(u, eng); }); break;
        case ID::OSC2_FEEDBACK_MIX:    oscs(1, [&](OscSection& sec, int u){ sec.setFeedbackMix(u, eng); }); break;
        // Arbitrary wavetables (AKWF): the two knobs bucket per v1's laws
        // (bank across 10 banks, index against the CURRENT bank's count),
        // and either change re-resolves the table pointer for all voices.
        case ID::OSC1_ARB_BANK:
            L.arbBank[0] = WavetableLib::bankFromNorm(norm);
            applyArbTable(0, layer);
            break;
        case ID::OSC2_ARB_BANK:
            L.arbBank[1] = WavetableLib::bankFromNorm(norm);
            applyArbTable(1, layer);
            break;
        case ID::OSC1_ARB_INDEX:
            L.arbIndex[0] = WavetableLib::indexFromNorm(norm, L.arbBank[0]);
            applyArbTable(0, layer);
            break;
        case ID::OSC2_ARB_INDEX:
            L.arbIndex[1] = WavetableLib::indexFromNorm(norm, L.arbBank[1]);
            applyArbTable(1, layer);
            break;

        // ------------- section mixer (Pass 4) -------------------------------
        case ID::MIX_OSC1:      for (Voice& v : L.voices()) v.oscSection().setMixOsc1(eng); break;
        case ID::MIX_OSC2:      for (Voice& v : L.voices()) v.oscSection().setMixOsc2(eng); break;
        case ID::MIX_SUB:       for (Voice& v : L.voices()) v.oscSection().setMixSub(eng); break;
        case ID::MIX_NOISE:     for (Voice& v : L.voices()) v.oscSection().setMixNoise(eng); break;
        case ID::MIX_BALANCE:   for (Voice& v : L.voices()) v.oscSection().setBalance(eng); break;
        case ID::MIX_CROSS_MOD: for (Voice& v : L.voices()) v.oscSection().setCrossMod(eng); break;
        case ID::MIX_OSC_SYNC:  for (Voice& v : L.voices()) v.oscSection().setSyncEnabled(eng >= 0.5f); break;
        // G1: JP-8000 "LFO1 & ENV Destination" — the voice steers the
        // (LFO1-pitch + pitch-env) lane by this each block (Voice::render).
        case ID::MIX_PITCH_MOD_DEST:
            for (Voice& v : L.voices()) v.setPitchModDest((uint8_t)opt);
            break;
        // G2: per-osc share of LFO1's PWM (JP-8000 SQR Control 2).
        case ID::OSC1_PWM_LFO1_DEPTH: oscs(0, [&](OscSection& s, int u){ s.setPwmLfo1Scale(u, eng); }); break;
        case ID::OSC2_PWM_LFO1_DEPTH: oscs(1, [&](OscSection& s, int u){ s.setPwmLfo1Scale(u, eng); }); break;

        // ------------- LFO + modulation (Phase 3) --------------------------
        // Only the FOUR per-destination depths are wired (Decision #2,
        // docs/PHASE3_LFO_SPEC.md §3): master LFO*_DEPTH and LFO*_DESTINATION
        // were early ideas superseded by these independent amounts.
        // LFO1/2_SYNC (Phase 3 subsystem 2, PHASE3_BPMCLOCK_SPEC.md §6):
        // FREQ now only drives the oscillator while the LFO is Free — it
        // always updates freeHz so a later switch back to Free is exact
        // (Decision #6).  SYNC picks the branch via applyLfoRate.
        case ID::LFO1_WAVEFORM:     L.lfo1.osc.setWave(opt); break;
        case ID::LFO1_FREQ:
            L.lfo1.freeHz = eng;
            if (L.lfo1.syncMode == TempoClock::kFree) L.lfo1.osc.setRateHz(eng);
            break;
        case ID::LFO1_SYNC:
            L.lfo1.syncMode = opt;
            applyLfoRate(L.lfo1);
            break;
        case ID::LFO1_DELAY:       L.lfo1.osc.setDelayMs(eng * 4000.0f); break;
        case ID::LFO1_PITCH_DEPTH:  L.lfo1.depthPitch  = eng; break;
        case ID::LFO1_FILTER_DEPTH: L.lfo1.depthFilter = eng; break;
        case ID::LFO1_PWM_DEPTH:    L.lfo1.depthPwm    = eng; break;
        case ID::LFO1_AMP_DEPTH:    L.lfo1.depthAmp    = eng; break;

        case ID::LFO2_WAVEFORM:     L.lfo2.osc.setWave(opt); break;
        case ID::LFO2_FREQ:
            L.lfo2.freeHz = eng;
            if (L.lfo2.syncMode == TempoClock::kFree) L.lfo2.osc.setRateHz(eng);
            break;
        case ID::LFO2_SYNC:
            L.lfo2.syncMode = opt;
            applyLfoRate(L.lfo2);
            break;
        case ID::LFO2_DELAY:       L.lfo2.osc.setDelayMs(eng * 4000.0f); break;
        case ID::LFO2_PITCH_DEPTH:  L.lfo2.depthPitch  = eng; break;
        case ID::LFO2_FILTER_DEPTH: L.lfo2.depthFilter = eng; break;
        case ID::LFO2_PWM_DEPTH:    L.lfo2.depthPwm    = eng; break;
        case ID::LFO2_AMP_DEPTH:    L.lfo2.depthAmp    = eng; break;

        // ------------- internal BPM clock (Phase 3 subsystem 2) ------------
        // CLOCK_TEMPO only changes the internal BPM on an actual knob edit
        // (no per-block clock refresh — Decision #5): re-resolve both LFOs'
        // synced rates right here, once.  CLOCK_CLOCK_SOURCE is stored for
        // forward-compat with the external-clock pass but is INERT this
        // pass (Decision #2) — internal BPM keeps driving regardless of the
        // selection; refreshSyncedLfos() here is a harmless no-op while
        // Internal (source doesn't feed freqForMode) and keeps behaviour
        // correct once External lands.
        case ID::CLOCK_TEMPO:
            _clock.setBpm(eng);
            refreshSyncedLfos();
            break;
        case ID::CLOCK_CLOCK_SOURCE:
            _clock.setSource(opt);   // kExtMidi accepted but inert (Decision #2)
            refreshSyncedLfos();
            break;

        // ------------- performance / layering ------------------------------
        // These decide how the 8-voice pool is CUT between the two layers.
        // Only the cut is handled here: which layer a note reaches is settled
        // on the control plane by PerfRouter, before the event is queued.
        //
        // Both handlers funnel into repartitionVoices(), which no-ops unless
        // the cut actually moves — perf.* re-applies on every patch load and
        // an unconditional repartition would hard-kill sounding voices each
        // time a patch was recalled.
        case ID::PERF_MODE:
        case ID::PERF_VOICE_SPLIT:
            repartitionVoices();
            break;

        // perf.split_note and the two channel assignments have NO engine
        // consumer by design: PerfRouter reads them straight from the store at
        // routing time.  Listed explicitly rather than left to fall through
        // 'default', so that reading this switch tells you they are handled
        // and not forgotten — which is exactly how all seven perf.* params
        // came to be silently discarded in the first place.
        case ID::PERF_BALANCE:
            // Stored raw; the gain law and the per-sample ramp live in
            // renderBlock, so a balance sweep is smoothed once, in one place.
            _balance = eng;
            break;

        case ID::PERF_MIDI_CHANNEL_A:
        case ID::PERF_MIDI_CHANNEL_B:
        case ID::PERF_SPLIT_NOTE:
        case ID::PERF_EDIT_TARGET:      // editor-only: never engine state
            break;

        // ------------- performance (Phase 4, ParamTable §11) ---------------
        // Glide, poly/mono/unison, unison detune, bend range, amp level.
        // Details + v1 provenance in docs/PHASE4_PERFORMANCE_SPEC.md.
        case ID::GLIDE_ENABLE:
            // Toggle: eng is 0/1.  Fans to every voice (glide is per-voice).
            for (Voice& v : L.voices()) v.setGlideEnabled(eng >= 0.5f);
            break;
        case ID::GLIDE_TIME: {
            // Table row is a NORM knob (0..1); map with v1's log ms law then
            // v1's 1/samples rate (block-rate quirk preserved — see helper).
            const float rate = glideRateFromNorm(norm);
            for (Voice& v : L.voices()) v.setGlideRate(rate);
        } break;
        case ID::VOICE_POLY_MODE:
            // opt is the frozen index 0/1/2 == PolyMode Poly/Mono/Unison; the
            // allocator kills sounding voices on an actual change (v1 parity).
            L.alloc.setPolyMode((PolyMode)opt);
            break;
        case ID::VOICE_UNISON_DETUNE:
            L.alloc.setUnisonDetune(eng);            // 0..1; re-spreads if Unison
            break;
        case ID::VOICE_BEND_RANGE:
            // eng arrives already in 0..24 st (table range) — v1 clamped to
            // PITCH_BEND_MAX_SEMITONES(24); the table bounds guarantee it.
            L.bendRange = eng;
            break;
        case ID::VOICE_AMP_LEVEL:
            // v1 AMP_MOD_FIXED_LEVEL: the DC BASE of the VCA-mod signal (spec
            // §1.4).  Replaces the hardcoded 1.0 base in renderBlock's
            // ampMulTarget; LFO tremolo still adds on top.  Default 1.0 = unity.
            L.ampFixedLevel = eng;
            break;

        // ---- [15] Global Reverb (Phase 5, PHASE5_REVERB_SPEC.md §1.2) ----
        // v1 routed these through GlobalFX setters after a value/127 CC step;
        // v2 passes the full-float `norm` straight in (spec §1.1a / sign-off
        // Q3) — the tank setters are float-native, so this is faithful to the
        // DSP AND full display resolution, with no 7-bit re-quantisation.
        // Toggles use `norm >= 0.5f` (reproduces v1's `value >= 64`).
        case ID::REVERB_SIZE:    _reverb.setSize(norm);    break;
        case ID::REVERB_DAMP:    _reverb.setHiDamp(norm);  break;   // "hi damp"
        case ID::REVERB_LODAMP:  _reverb.setLoDamp(norm);  break;
        case ID::REVERB_MIX:
            // Master wet level + the auto-bypass re-evaluation (v1 setReverbMix
            // -> updateReverbBypass).  L==R (v1 always centred it).
            _reverbMix = norm;
            recomputeReverbBypass();
            break;
        case ID::REVERB_BYPASS:
            _reverbManualBypass = (norm >= 0.5f);
            recomputeReverbBypass();
            break;
        case ID::REVERB_SHIMMER: _reverb.setShimmer(norm); break;
        case ID::REVERB_FREEZE:  _reverb.setFreeze(norm >= 0.5f); break;
        case ID::REVERB_LOWPASS: _reverb.setLowpass(norm); break;
        case ID::REVERB_HIPASS:  _reverb.setHipass(norm);  break;

        // ---------------- [9] Per-patch FX chain (Phase 6) ----------------
        // Norm→engineering mappings reproduce v1 SynthEngine.cpp:2055-2137
        // EXACTLY (spec §1.3 / §4).  Stage-selector params (drive/mod/delay
        // effect) also recompute the engaged-gate so renderBlock knows whether
        // to run the chain at all (Q6).
        //
        // D-4 (flagged): the ParamTable defaults FX_BASS/TREBLE to norm 0.0,
        // which maps to -12 dB here (not v1's flat 0 dB default).  This is inert
        // at boot — tone only runs once the chain is engaged AND a gain delta is
        // non-zero — but is a real v1→v2 default difference when the chain is
        // engaged with tone untouched.  Honouring the frozen table by sign-off
        // (Q1/OQ-2); a flat-by-default table is a separate optional regen.
        case ID::FX_BASS_GAIN:
            _fx.setBassGain(norm * 24.0f - 12.0f);   break;   // 0..1 -> -12..+12 dB
        case ID::FX_TREBLE_GAIN:
            _fx.setTrebleGain(norm * 24.0f - 12.0f); break;
        case ID::FX_DRIVE:                                     // D-1: Select {OFF,Soft,Hard}
            _fx.setDriveMode(opt); recomputeFxEngaged(); break;
        case ID::FX_MOD_EFFECT:                               // Q3: opt-1 = v1 ModEffectType
            _fx.setModEffect(opt - 1); recomputeFxEngaged(); break;
        case ID::FX_MOD_MIX:
            _fx.setModMix(norm); break;
        case ID::FX_MOD_RATE:
            _fx.setModRate(norm * 20.0f); break;             // 0..20 Hz (0 = preset)
        case ID::FX_MOD_FEEDBACK:                            // D-5: norm 0 = use preset (-1)
            _fx.setModFeedback(norm <= 0.0f ? -1.0f : norm * 0.99f); break;
        case ID::FX_DELAY_EFFECT:                            // Q3: opt-1 = v1 DelayEffectType
            _fx.setDelayEffect(opt - 1); recomputeFxEngaged(); break;
        case ID::FX_DELAY_TIME:
            _fx.setDelayTime(norm * 1500.0f); break;         // 0..1500 ms (0 = preset)
        case ID::FX_DELAY_MIX:
            _fx.setDelayMix(norm); break;                    // D-6: 0..1, no phase invert
        case ID::FX_DELAY_FEEDBACK:                          // D-5
            _fx.setDelayFeedback(norm <= 0.0f ? -1.0f : norm * 0.99f); break;
        case ID::FX_DELAY_SYNC:
            // D-2: tempo-sync deferred (Phase 3 deferral list).  No-op — the
            // dirty flag clears, nothing accumulates.  Wiring it intersects the
            // internal BPM clock (already ported) and is a bounded follow-up.
            break;
        case ID::FX_DRY_MIX:
            _fx.setDryMix(norm); break;
        case ID::FX_JPFX_MIX:
            _fx.setJpfxMix(norm); break;

        // ---------------- [13] Step sequencer (Phase 7) ----------------
        // Norm→engineering mappings reproduce v1 SynthEngine.cpp:2504-2600
        // EXACTLY (spec §1.3 / §4).  Sequencer is disabled by default, so none
        // of this perturbs the default patch (Q4 guard test proves it).
        case ID::SEQ_ENABLE:
            _seq.setEnabled(norm >= 0.5f); break;
        case ID::SEQ_STEPS:
            _seq.setStepCount(1 + (int)lroundf(norm * 15.0f)); break;   // 1..16
        case ID::SEQ_GATE_LENGTH:
            _seq.setGateLength(norm); break;
        case ID::SEQ_SLIDE:
            _seq.setSlide(norm); break;
        case ID::SEQ_DIRECTION:
            _seq.setDirection((SeqDir)opt); break;                      // 0..3
        case ID::SEQ_DESTINATION:
            _seq.setDestination((SeqDest)opt); break;                   // 0..4
        case ID::SEQ_DEPTH:
            _seq.setDepth(eng); break;                                  // table bipolar -1..+1
        case ID::SEQ_RETRIGGER:
            _seq.setRetrigger(norm >= 0.5f); break;
        case ID::SEQ_RATE:
            // 0.02..50 Hz, logarithmic.  Widened from 0.1..20 Hz, which could
            // reach NEITHER synced extreme (1/32 @ 300 BPM = 40 Hz; 4 bars @
            // 40 BPM = 0.0417 Hz), so switching sync off lost range at both
            // ends.  exp2f rather than powf: same curve, cheaper call.
            // log2(50 / 0.02) = 11.2877124.
            _seq.setRate(StepSequencer::kFreeHzMin * exp2f(norm * 11.2877124f));
            break;
        case ID::SEQ_TIMING_MODE:
            // D-1: tempo-sync deferred (TempoClock has no getTimeForMode(ms)
            // yet).  Stored inert; sequencer stays free-running at SEQ_RATE.
            _seq.setTimingMode(opt);
            break;
        // SEQ_STEP_SELECT / SEQ_STEP_VALUE and their aux twins are RETIRED.
        // ParamIDs are permanent so the rows stay in the table, but the engine
        // ignores them: the explicit SEQ_STEP_n / SEQ_AUX_STEP_n arrays decoded
        // above are the only step path now.  Falling through to `default`
        // consumes them silently, which is what a retired row should do.
        case ID::SEQ_STEP_BIPOLAR:
            _seq.setStepBipolar(norm >= 0.5f); break;
        case ID::SEQ_AUX_BIPOLAR:
            _seq.setAuxBipolar(norm >= 0.5f); break;

        // ---- Aux lane (Stage B).  Shares the gate lane's clock; carries its
        //      own dest/depth/steps.  Stage B routes None+Filter; Pan/Delay
        //      All six destinations are wired: None, Filter, Pan, DelaySend,
        //      Tone (the +/-6 dB EQ tilt) and Drive (saturator input gain). ----
        case ID::SEQ_AUX_DESTINATION:
            _seq.setAuxDestination((SeqAuxDest)opt); break;             // 0..5
        case ID::SEQ_AUX_DEPTH:
            _seq.setAuxDepth(eng); break;                              // table bipolar -1..+1

        // ------------- Arpeggiator (ParamTable section 17) -----------------
        // Independent clock, shared BPM.  ARP_ENABLE off by default => notes go
        // straight to _alloc and tick() early-returns => byte-identical.  The
        // three step LANES arrive through the explicit ARP_STEP_ON_n /
        // _ACCENT_n / _RATCHET_n arrays decoded above; ARP_STEP_SELECT and the
        // three cursor writes are RETIRED and fall through to `default`.
        // Rate reuses the shared 12-entry timing set; ARP_RATE opt 0 (Free)
        // falls back to ARP_FREE_HZ.
        case ID::ARP_ENABLE: {
            // Toggling the arp re-routes where held keys go, so the SIDE that
            // is losing them must be cleared or its voices hang: a key pressed
            // before the toggle sends its note-off to the other side, which
            // never knew about it.
            const bool on = (norm >= 0.5f);
            if (on != L.arp.enabled()) {
                if (on) L.alloc.allNotesOff();   // keys move keyboard -> arp
                else    L.arp.allNotesOff();     // keys move arp -> keyboard
            }
            L.arp.setEnabled(on);
            break;
        }
        case ID::ARP_MODE:
            L.arp.setMode((ArpMode)opt); break;                         // 0..6
        case ID::ARP_OCTAVES:
            L.arp.setOctaves(1 + opt); break;                           // opt 0..3 -> 1..4
        case ID::ARP_LATCH:
            L.arp.setLatch(norm >= 0.5f); break;
        case ID::ARP_RATE:
            // opt is the TempoClock::Mode index (0==Free).  The arp IS tempo-
            // synced (freqForMode is sufficient — unlike the seq's D-1 defer).
            L.arp.setRateMode(opt); break;
        case ID::ARP_FREE_HZ:
            // Engine exp-maps over Arpeggiator::kFreeHzMin..kFreeHzMax
            // (0.02..50 Hz), chosen to cover the synced extremes.
            L.arp.setFreeHz(norm); break;
        case ID::ARP_GATE_LENGTH:
            L.arp.setGateLength(norm); break;
        case ID::ARP_SWING:
            L.arp.setSwing(norm); break;
        case ID::ARP_STEP_COUNT:
            L.arp.setStepCount(1 + (int)lroundf(norm * 15.0f)); break;  // 1..16

        default:
            // Not yet handled (remaining FX/sequencer) or deliberately unwired
            // above: consumed silently by design — see header note.
            break;
    }
}

// -----------------------------------------------------------------------------
// Tempo-sync helpers (PHASE3_BPMCLOCK_SPEC.md §4/§6).  Control-plane only —
// called from applyParam on an actual dirty param, never per-block.
// -----------------------------------------------------------------------------

void SynthCore::applyLfoRate(LfoState& lfo)
{
    // freqForMode() returns <=0 for kFree ("not synced"); every other mode
    // returns the clock-derived Hz (corrected divide, see TempoClock.h).
    const float synced = _clock.freqForMode(lfo.syncMode);
    lfo.osc.setRateHz(synced > 0.0f ? synced : lfo.freeHz);
}

void SynthCore::refreshSyncedLfos()
{
    // All four, unconditionally: a Free LFO harmlessly re-asserts freeHz (a
    // no-op — same value it already has), so no branch on syncMode is needed
    // here.  Only reached on a clock edit (BPM/source), which is rare compared
    // to block rate.
    //
    // BOTH layers, because the clock is shared: a tempo change must re-resolve
    // every synced LFO in the instrument, not just the audible layer's — layer
    // B may become audible on the very next block.
    for (Layer& lz : _layers) {
        applyLfoRate(lz.lfo1);
        applyLfoRate(lz.lfo2);
    }
}

// -----------------------------------------------------------------------------
// Re-cut the voice pool between the layers.  Audio plane (applyParam context),
// which is what makes the hard-kill below safe: voices are only ever mutated
// from here and from drainNoteEvents, in the same context.
//
// POLICY (signed off): a split change hard-kills every voice, then rebinds.
// The alternatives — letting sounding voices finish, or killing only voices
// that fall outside their new slice — both leave a voice being rendered by one
// layer while another layer believes it owns it, which is how stuck notes and
// double-triggered envelopes happen.  A split change is a deliberate,
// non-real-time action; a clean break is the honest behaviour.
// -----------------------------------------------------------------------------
void SynthCore::repartitionVoices()
{
    using namespace Params;

    const int mode = _router.mode();

    // Single mode: layer A owns everything and voice_split is ignored — one
    // patch should never be limited to half the polyphony because of a setting
    // that is not in effect.
    uint8_t countA;
    if (mode == PerfRouter::kModeSingle) {
        countA = VoiceAllocator::kMaxVoices;
    } else {
        // The 'voice_split' option set is "1+7", "2+6", ... so option index + 1
        // is layer A's share and the remainder is layer B's.
        const size_t iSplit = ParameterStore::indexOf(ID::PERF_VOICE_SPLIT);
        const int    opt    = Curves::toOptionIndex(kParams[iSplit],
                                                    _store.getByIndex(iSplit));
        const int    a      = opt + 1;
        countA = static_cast<uint8_t>(
            (a < 1) ? 1 : (a > (int)VoiceAllocator::kMaxVoices - 1)
                              ? (int)VoiceAllocator::kMaxVoices - 1
                              : a);
    }

    if (countA == _splitCountA) return;      // cut unchanged: do nothing at all
    _splitCountA = countA;

    // Silence the WHOLE pool before either allocator is rebound.  Doing it per
    // layer would let the first rebind hand voices to A that B then silences.
    for (Voice& v : _voices) v.hardKill();
    for (Layer& lz : _layers) lz.arp.allNotesOff();

    _layers[0].setSlice(_voices, countA);
    _layers[1].setSlice(_voices + countA,
                        VoiceAllocator::kMaxVoices - countA);
}

// -----------------------------------------------------------------------------
// perf.balance -> the two bus gains.  "Full at centre", NOT an equal-power or
// linear pan law:
//
//     balance   0 ....... 64 ....... 127
//     layer A   1.0 ..... 1.0 ..... 0.0
//     layer B   0.0 ..... 1.0 ..... 1.0
//
// Centre therefore leaves BOTH layers at unity — which is what a layered patch
// should sound like out of the box, and is also the condition renderBlock
// tests to skip its mixing stage entirely.  A conventional pan law would put
// centre at ~0.7 on each side and quietly halve the level of every existing
// patch the moment Performance was switched on.
// -----------------------------------------------------------------------------
void SynthCore::layerGains(float balance, float& gA, float& gB)
{
    constexpr float kCentre = 64.0f;
    constexpr float kSpan   = 127.0f - kCentre;   // 63

    const float b = (balance < 0.0f) ? 0.0f : (balance > 127.0f) ? 127.0f : balance;

    gA = (b <= kCentre) ? 1.0f : 1.0f - (b - kCentre) / kSpan;
    gB = (b >= kCentre) ? 1.0f : b / kCentre;
}

void SynthCore::applyArbTable(int unit, uint8_t layer)
{
    Layer& L = _layers[layer & 1u];
    uint16_t len = 0;
    const int16_t* table =
        WavetableLib::akwfTable(L.arbBank[unit], L.arbIndex[unit], len);
    // Table swap is glitch-safe (block boundary, phase preserved) and a
    // nullptr legally selects OscCore's naive-saw fallback — see the
    // guards in WavetableLib and OscCore.
    for (Voice& v : L.voices())
        v.oscSection().setArbTable(unit, table, len);
}

// -----------------------------------------------------------------------------
// The block — audio plane entry point.
// -----------------------------------------------------------------------------

void SynthCore::renderBlock(float* left, float* right, size_t n)
{
    // 1. Note events queued since the last block.
    drainNoteEvents();

    // 1b. External MIDI clock (Phase 9): apply any BPM / transport handed off by
    //     the platform's realtime-byte handlers.  No-op unless the clock source
    //     is External and a tempo/transport actually arrived → byte-identical
    //     for the internal-clock default.
    drainExternalClock();

    // 2. Changed parameters only (see ParameterStore::takeNextDirty).
    //
    //    The store hands back a SLOT, which carries both the parameter and its
    //    layer; the engine now consumes both.  Shared parameters (FX chain,
    //    sequencer) report layer 0 whichever layer wrote them, so they reach
    //    their singleton exactly once — never twice, never for the wrong one.
    //
    //    In Single mode layer B owns no voices, so its fan-out loops execute
    //    zero times: B's parameters cost the switch dispatch and nothing else,
    //    and they stay coherent for the moment the user switches to Layer.
    const float* snap = _store.acquireSnapshot();
    size_t slot;
    while ((slot = _store.takeNextDirty()) != ParameterStore::kInvalidIndex)
        routeParam(slot, snap[slot]);

    // Advance anything still gliding.  This is what turns a stepped knob into a
    // continuous one; see the SlewBank comment in the header for why it lives
    // at the drain rather than in the store or the DSP objects.
    tickSlewBank(snap);

    // 3. Clear the bus, then add every ACTIVE voice.  Idle voices cost one
    //    branch — the v1 all-voices-always-run drain is designed out.
    memset(left,  0, n * sizeof(float));
    memset(right, 0, n * sizeof(float));

    // 3a. Shared, once-per-block modulation sources.
    //
    //     The sequencer and the arpeggiator clock are SINGLETONS by design
    //     (signed off): one global sequencer, one global BPM clock.  They are
    //     therefore ticked ONCE, here, before either layer is rendered — a
    //     per-layer tick would advance the pattern twice as fast the moment a
    //     second layer became audible.
    _seq.tick(kBlockMs);
    const float seqVal = _seq.getOutput();      // ±depth, 0 when disabled/idle
    const float auxVal = _seq.getAuxOutput();

    // Aux lane (Stage B/C/D): a second block-rate output on the same clock,
    // routed to its own destination.  Emits 0 while dest==None/idle, so an
    // unused aux lane leaves every accumulator untouched → byte-identical.
    //
    // Pan and the two FX mods are BUS-level: they belong to the shared chain,
    // not to either layer, so they are resolved out here.  Stage D FX mods are
    // stateful on FxChain and must be written EVERY block — to auxVal when
    // targeted, to 0 otherwise — or a stale mod persists after a dest change.
    float panTarget       = 0.0f;
    float auxToneTiltMod  = 0.0f;
    float auxDriveMod     = 0.0f;
    float auxDelaySendMod = 0.0f;
    switch (_seq.auxDestination()) {
        case SeqAuxDest::Pan:       panTarget      += auxVal;  break;
        case SeqAuxDest::Tone:      auxToneTiltMod  = auxVal;  break;  // EQ tilt
        case SeqAuxDest::Drive:     auxDriveMod     = auxVal;  break;  // saturator input
        case SeqAuxDest::DelaySend: auxDelaySendMod = auxVal;  break;  // additive on mix
        case SeqAuxDest::Filter:                               break;  // per-layer, below
        case SeqAuxDest::None:
        default:                                               break;
    }
    _fx.setToneTiltMod(auxToneTiltMod);
    _fx.setDriveAmountMod(auxDriveMod);
    _fx.setDelayMixMod(auxDelaySendMod);

    // 3b. Bus routing for perf.balance.
    //
    //     THE INERT PATH IS THE POINT.  At centre balance both gains are
    //     exactly 1.0 and no ramp is in flight, so layer B is pointed at the
    //     caller's buffers too: both layers accumulate into one bus in pool
    //     order, exactly as the single-layer engine did, and the mixing stage
    //     below is skipped entirely.  The scratch buffers are reserved but
    //     never touched, and the render stays byte-identical.
    //
    //     Voices are contiguous and layer A owns the low slice, so rendering A
    //     then B preserves the pool-order float accumulation the baseline had.
    //     Changing that order would change the sum's rounding.
    float gainA = 1.0f, gainB = 1.0f;
    layerGains(_balance, gainA, gainB);

    // The VCA-mod half of the gain is only KNOWN inside the layer loop (it
    // needs the LFO tick), but the routing decision has to be made before it.
    // ampModActive() answers "could it move off unity?" from state alone, so
    // the choice is made on a cheap conservative predicate rather than by
    // ticking the LFOs early and having to remember not to tick them again.
    const bool ampCouldMove = _layers[0].ampModActive() ||
                              _layers[1].ampModActive() ||
                              _seq.destination() == SeqDest::Amp;

    const bool balanceInert = (gainA >= 1.0f && gainB >= 1.0f && !ampCouldMove &&
                               _layers[0].gainCur >= 1.0f &&
                               _layers[1].gainCur >= 1.0f);

    // Per-layer bus gain TARGETS: balance x that layer's VCA-mod factor.  The
    // two are multiplied into one number because they are applied at the same
    // point by the same ramp — a separate ramp each would cost a second pass
    // over the buffer for no audible benefit.
    float gainTarget[2] = { gainA, gainB };

    float* busL[2] = { left, balanceInert ? left  : _busBL };
    float* busR[2] = { right, balanceInert ? right : _busBR };
    if (!balanceInert) {
        memset(_busBL, 0, n * sizeof(float));
        memset(_busBR, 0, n * sizeof(float));
    }

    // 3c. Per layer: tick its LFOs, distribute its modulation, render it.
    for (size_t li = 0; li < 2; ++li) {
        Layer& lz = _layers[li];

        // A layer with no voices (layer B in Single mode) is skipped whole:
        // no LFO tick, no fan-out, no render.  "Do not calculate if not
        // required" — and it is what makes Single mode cost what it always did.
        if (lz.voiceCount() == 0) continue;

        // filter.cutoff smoothing.  Advanced BEFORE the voice work so this block
        // renders with the new value, and fanned out to EVERY voice of the
        // layer - including idle ones.  Pushing to idle voices is the point:
        // renderBlock skips them, so if they were not kept in step, a note
        // triggered mid-sweep would start on a stale cutoff while its siblings
        // sat on the current one.
        //
        // tickBlock() is the closed-form N-sample advance - one multiply, exact,
        // not an approximation of the per-sample recurrence.  At smooth_ms = 5
        // it covers ~44% of the remaining distance per block, spreading a detent
        // over roughly three blocks (~9 ms) instead of landing whole.  When
        // settled it is a single bool test, so a static filter costs nothing.
        // Phase 3 LFOs (spec §4/§5): tick each ENGAGED LFO (any destination
        // depth > 0) once, then distribute the net per-destination values to
        // every active voice of THIS layer.  A disengaged LFO is not ticked at
        // all — its phase stays frozen; since its depths are zero the sums are
        // zero regardless, which also gives the spec's "one final block of zero
        // destination values" on the engaged->disengaged transition for free.
        const float u1 = lz.lfo1.engaged() ? lz.lfo1.osc.tickBlock() : 0.0f;
        const float u2 = lz.lfo2.engaged() ? lz.lfo2.osc.tickBlock() : 0.0f;

        // G1/G2 lane split.  LFO1's pitch term travels its OWN lane so the
        // voice can steer it (with the pitch env) per mix.pitch_mod_dest;
        // LFO1's PWM term likewise, so the section can scale it per osc.  LFO2
        // and the sequencer stay in the common lanes — the JP-8000 never routed
        // them (manual p.112).
        //
        // pitchDepthTotal() == depthPitch when the mod wheel is at rest, so the
        // default patch is arithmetically unchanged (see LfoState).
        float pitchSemisLfo1   = u1 * lz.lfo1.pitchDepthTotal() * kLfoPitchMaxSemis;
        float pitchSemisCommon = u2 * lz.lfo2.depthPitch * kLfoPitchMaxSemis;
        float filterCutInput   = u1 * lz.lfo1.depthFilter + u2 * lz.lfo2.depthFilter;
        float pwmLfo1          = u1 * lz.lfo1.depthPwm;
        float pwmCommon        = u2 * lz.lfo2.depthPwm;

        // The shared sequencer feeds BOTH layers' lanes — one pattern
        // modulating the whole instrument is what "global sequencer" means.
        switch (_seq.destination()) {
            case SeqDest::Pitch:  pitchSemisCommon += seqVal * kLfoPitchMaxSemis; break;
            case SeqDest::Filter: filterCutInput   += seqVal;                     break;
            case SeqDest::Pwm:    pwmCommon        += seqVal;                     break;
            case SeqDest::Amp:                                                    break; // global stage
            case SeqDest::None:
            default:                                                              break;
        }
        if (_seq.auxDestination() == SeqAuxDest::Filter) filterCutInput += auxVal;

        // VCA mod, PER LAYER (v1 topology): base = v1 AMP_MOD_FIXED_LEVEL
        // (voice.amp_level, default 1.0) with the two LFO tremolo terms adding
        // on top (v1 ampModMixer slots 1/2), plus the shared sequencer when it
        // is routed to Amp.
        //
        // Applied to THIS LAYER'S BUS, which puts it BEFORE the FX chain and
        // reverb — where v1 had it, and the only place two layers can carry two
        // different tremolos.  The consequence is deliberate and worth knowing:
        // tremolo now modulates what is FED to the delay and reverb rather than
        // their output, so tails are no longer chopped by it.
        //
        // At the default patch this is exactly 1.0 + 0 + 0, so the gain stays
        // unity, the mixing stage stays inert, and the render is unchanged.
        float ampMul = lz.ampFixedLevel + u1 * lz.lfo1.depthAmp
                                        + u2 * lz.lfo2.depthAmp;
        if (_seq.destination() == SeqDest::Amp) ampMul += seqVal;
        gainTarget[li] *= ampMul;

        // Arpeggiator (Phase 9): one block-rate tick, driving THIS layer's
        // allocator and reading the SHARED clock for its synced rate.  When
        // disabled it early-returns (releasing any lingering arp note), so a
        // disabled arp costs one bool test and leaves the allocator untouched.
        // Ticked in the same audio-plane context as drainNoteEvents, so its
        // noteOn/off run exactly where the note ring already mutates voices.
        lz.arp.tick(kBlockMs, lz.alloc, _clock);

        for (Voice& v : lz.voices()) {
            if (!v.isActive()) continue;
            v.setLfoPitchSemis(pitchSemisCommon);
            v.setRoutedLfoPitchSemis(pitchSemisLfo1);
            v.filter().setLfoCutoff(filterCutInput);
            v.oscSection().setLfoPwm(pwmLfo1, pwmCommon);
        }

        for (Voice& v : lz.voices())
            if (v.isActive()) v.render(busL[li], busR[li], n);
    }

    // 3d. Mix the layers with a per-sample gain ramp.  Skipped entirely on the
    //     inert path, where both layers already wrote the same bus.
    //
    //     Ramped rather than stepped for the same reason master volume is: a
    //     balance knob swept by hand delivers a staircase of block-rate values,
    //     and stepping a bus gain is an audible zipper.
    if (!balanceInert) {
        const float stepA = (gainTarget[0] - _layers[0].gainCur) / (float)n;
        const float stepB = (gainTarget[1] - _layers[1].gainCur) / (float)n;
        float cA = _layers[0].gainCur;
        float cB = _layers[1].gainCur;
        for (size_t i = 0; i < n; ++i) {
            left[i]  = left[i]  * cA + _busBL[i] * cB;
            right[i] = right[i] * cA + _busBR[i] * cB;
            cA += stepA;
            cB += stepB;
        }
        _layers[0].gainCur = gainTarget[0];
        _layers[1].gainCur = gainTarget[1];
    }

    // VCA-mod base = v1 AMP_MOD_FIXED_LEVEL (VOICE_AMP_LEVEL), default 1.0;
    // the LFO tremolo terms add on top (v1 ampModMixer slots 1/2).  At the
    // default patch this is exactly 1.0 + 0 = 1.0 → byte-identical output.
    //
    // STILL GLOBAL, and fed from layer A: this stage sits AFTER the FX chain
    // and reverb, so moving it per layer means moving it PRE-FX, which changes
    // where tremolo sits relative to the delay and reverb tails.  That is
    // audible for any patch with amp depth, so it is not done unasked.

    // 3a-bis. Per-patch FX chain (Phase 6, PHASE6_FXCHAIN_SPEC.md §3): the
    //     JP-8000 JPFX chain (saturation -> tone EQ -> mod -> delay -> limiter),
    //     an in-place stereo processor on the summed bus, AFTER the voice sum
    //     and BEFORE the global reverb (v1 order: FXChainBlock output feeds the
    //     GlobalFX reverb send).  Skipped entirely unless engaged (drive/mod/
    //     delay active), so the all-OFF default patch never touches it and the
    //     output stays byte-identical to pre-Phase-6 (Q6).  The chain mono-sums
    //     the bus (v1's single mono input), processes to stereo, and blends the
    //     wet result back against the dry bus via FX_DRY_MIX / FX_JPFX_MIX.
    // _fxEngaged is recomputed only when a stage SELECTOR moves, so it cannot
    // see the aux lane.  The tone tilt colours the bus whatever the drive mode
    // is, so it has to be able to engage the chain by itself — otherwise
    // choosing the aux Tone destination on an all-OFF chain did nothing at
    // all.  One bool OR per block; zero when the lane is idle.
    if (_fxEngaged || _fx.toneTiltActive())
        _fx.processBlock(left, right, n);

    // 3b. Global reverb (Phase 5, spec §3): a single post-mix stereo processor
    //     on the summed bus, BEFORE master volume so CC7 rides dry+wet together
    //     (v1 topology — reverb return is pre-fader).  Skipped entirely when
    //     bypassed (manual || mix<=1e-3), so the all-zero default patch never
    //     touches it and output stays byte-identical to pre-Phase-5.  The tank
    //     is 100% wet internally; processBlock does the dry+mix*wet blend.
    if (!_reverbBypassed)
        _reverb.processBlock(left, right, n, _reverbMix);

    // 4. Master volume: one-pole toward target at block rate (~12 ms), then
    //    a per-sample ramp so even a hard CC7 jump is a short fade.
    //
    //    The Phase 3 amp LFO used to ride this same loop as a second global
    //    factor.  It no longer does: tremolo is per layer now and is applied on
    //    the layer bus, pre-FX (see the layer loop above).
    const float start = _masterCur;
    _masterCur += (_masterTarget - _masterCur) * 0.25f;
    const float masterStep = (_masterCur - start) / (float)n;

    // Global bus pan (Stage C): centre-normalised equal-power law, computed
    // ONCE per block (the only sinf/cosf here), then ramped per-sample in the
    // loop below.  Skipped entirely when centred AND already centred last block
    // (panTarget == 0, gains == 1) so the default patch pays nothing and stays
    // byte-identical.  Law: θ = (pan+1)·π/4 → cos/sin, ×√2 so centre = 1.0 both
    // channels (no −3 dB dip sweeping through centre); edges → √2 / 0.
    const bool panActive = (panTarget != 0.0f) || (_panLCur != 1.0f) || (_panRCur != 1.0f);
    float panLStart = _panLCur, panRStart = _panRCur, panLStep = 0.0f, panRStep = 0.0f;
    if (panActive) {
        const float p    = panTarget < -1.0f ? -1.0f : (panTarget > 1.0f ? 1.0f : panTarget);
        const float theta= (p + 1.0f) * 0.7853981634f;         // π/4
        constexpr float kSqrt2 = 1.4142135624f;
        const float tgtL = std::cos(theta) * kSqrt2;           // centre → 1.0
        const float tgtR = std::sin(theta) * kSqrt2;           // centre → 1.0
        panLStart = _panLCur; panRStart = _panRCur;
        _panLCur = tgtL; _panRCur = tgtR;
        panLStep = (_panLCur - panLStart) / (float)n;
        panRStep = (_panRCur - panRStart) / (float)n;
    }

    float g = start;
    if (panActive) {
        float pl = panLStart, pr = panRStart;
        for (size_t i = 0; i < n; ++i) {
            g  += masterStep;
            pl += panLStep;
            pr += panRStep;
            left[i]  *= g * pl;
            right[i] *= g * pr;
        }
    } else {
        for (size_t i = 0; i < n; ++i) {
            g += masterStep;
            left[i]  *= g;
            right[i] *= g;
        }
    }
}

} // namespace JT
