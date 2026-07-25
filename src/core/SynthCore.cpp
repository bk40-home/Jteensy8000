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
      _alloc(_voices, VoiceAllocator::kMaxVoices)
{
    // Attach the caller-owned reverb delay pool (PSRAM on Teensy, heap on host)
    // and apply v1 GlobalFX's ctor one-shot tank defaults.  Null pool => the
    // reverb is inert (processBlock bails) — legal for tests that ignore it.
    _reverb.begin(reverbPool);

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

void SynthCore::noteOn(uint8_t note, uint8_t velocity) { pushEvent(kEvOn, note, velocity); }
void SynthCore::noteOff(uint8_t note)                  { pushEvent(kEvOff, note, 0); }
void SynthCore::sustain(bool pedalDown)                { pushEvent(kEvSustain, pedalDown ? 1 : 0, 0); }
void SynthCore::allNotesOff()                          { pushEvent(kEvNotesOff, 0, 0); }
void SynthCore::allSoundOff()                          { pushEvent(kEvSoundOff, 0, 0); }

// Pitch bend: split the 14-bit value across the event's two payload bytes
// (a = high 7 bits, b = low 7 bits) so it rides the existing 3-byte ring
// unchanged, then recombine in the drain (spec §4.3).
void SynthCore::pitchBend(uint16_t value14)
{
    pushEvent(kEvBend, (uint8_t)((value14 >> 7) & 0x7F), (uint8_t)(value14 & 0x7F));
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
        switch (e.type) {
            case kEvOn:
                _alloc.noteOn(e.a, e.b);
                // v1's shared, global JP-8000 retrigger (spec §1.3): ANY
                // note-on restarts BOTH LFOs' delay ramps, not just the
                // voice that's stealing/reusing hardware.
                _lfo1.osc.retrigger();
                _lfo2.osc.retrigger();
                // Phase 7: a note-on restarts the sequencer to step 0 when
                // SEQ_RETRIGGER is on (v1 SynthEngine.cpp:470) — phase-locks the
                // pattern to played notes.  Off => the running position is left
                // untouched (free-running clock).
                if (_seq.retrigger()) _seq.reset();
                break;
            case kEvOff:      _alloc.noteOff(e.a);          break;
            case kEvSustain:  _alloc.sustain(e.a != 0);     break;
            case kEvNotesOff: _alloc.allNotesOff();         break;
            case kEvSoundOff: _alloc.allSoundOff();         break;
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
                const float semis      = normalised * _bendRange;
                for (Voice& v : _voices) v.setBendSemis(semis);
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

void SynthCore::applyParam(size_t index, float norm)
{
    using namespace Params;
    const ParamDesc& d = kParams[index];

    // Engineering conversion happens once, here (brief §4.3).  Select-type
    // parameters need the option INDEX, not a raw engineering float.
    const float eng = Curves::toEngineering(d, norm);
    const int   opt = (int)Curves::toOptionIndex(d, norm);

    // Fan a per-unit setter across all voices: units 0/1 share handlers
    // because OSC1/OSC2 param blocks are index-shifted twins in the table.
    auto oscs = [&](int unit, auto&& fn) {
        for (Voice& v : _voices) fn(v.oscSection(), unit);
    };

    switch (d.id) {
        // ------------- master / filter / amp env (Phase 1 set) -------------
        case ID::MASTER_VOLUME:    _masterTarget = eng; break;
        // Cutoff/resonance pass the NORM, not engineering Hz: the section
        // owns v1's per-type shape rows (fc range + res γ), so the knob
        // position is the canonical input — exactly what v1's CC was.
        case ID::FILTER_CUTOFF:
            // Both views of the knob travel: VA shapes the norm per type,
            // OBXa consumes the Hz (see FilterSection::setCutoff).
            for (Voice& v : _voices) v.filter().setCutoff(norm, eng);
            break;
        case ID::FILTER_RESONANCE:
            for (Voice& v : _voices) v.filter().setResonanceNorm(norm);
            break;
        case ID::FILTER_ENGINE:
            for (Voice& v : _voices) v.filter().setEngine(opt);
            break;
        case ID::FILTER_VA_TYPE:
            for (Voice& v : _voices) v.filter().setVaType(opt);
            break;
        case ID::FILTER_MODE:
            for (Voice& v : _voices) v.filter().setObxaMode(opt);
            break;
        case ID::FILTER_OBXA_MULTIMODE:
            for (Voice& v : _voices) v.filter().setObxaMultimode(eng);
            break;
        case ID::FILTER_OBXA_XPANDER_MODE:
            for (Voice& v : _voices) v.filter().setObxaXpanderMode(opt);
            break;
        // FILTER_OBXA_RES_MOD_DEPTH: resonance mod-bus depth (LFO/seq -> res),
        // consumed by the later mod wiring.
        case ID::ENV_AMP_ATTACK:
            for (Voice& v : _voices) v.ampEnv().setAttackMs(eng);
            break;
        case ID::ENV_AMP_DECAY:
            for (Voice& v : _voices) v.ampEnv().setDecayMs(eng);
            break;
        case ID::ENV_AMP_SUSTAIN:
            for (Voice& v : _voices) v.ampEnv().setSustain(eng);
            break;
        case ID::ENV_AMP_RELEASE:
            for (Voice& v : _voices) v.ampEnv().setReleaseMs(eng);
            break;
        case ID::ENV_AMP_ATTACK_CURVE:
            for (Voice& v : _voices) v.ampEnv().setAttackSlope(eng);
            break;
        case ID::ENV_AMP_DECAY_CURVE:
            for (Voice& v : _voices) v.ampEnv().setDecaySlope(eng);
            break;
        case ID::ENV_AMP_RELEASE_CURVE:
            for (Voice& v : _voices) v.ampEnv().setReleaseSlope(eng);
            break;

        // ------------- filter cutoff modulation (Pass 6) -------------------
        // Depths for the block-rate cutoff mod that FilterSection combines
        // (env + key track, scaled by octave control).  env_amount/key_track
        // are bipolar -1..+1 (eng already signed).  octave_control is a 0..1
        // knob in the table; v1 scaled its CC by ×10 to octaves
        // (SynthEngine FILTER_OCTAVE_CONTROL: o = norm × 10) — reproduced here.
        case ID::FILTER_ENV_AMOUNT:
            for (Voice& v : _voices) v.filter().setEnvAmount(eng);
            break;
        case ID::FILTER_KEY_TRACK:
            for (Voice& v : _voices) v.filter().setKeyTrackAmount(eng);
            break;
        case ID::FILTER_OCTAVE_CONTROL:
            for (Voice& v : _voices) v.filter().setOctaveControl(eng * 10.0f);
            break;

        // ------------- filter envelope (Pass 6) ----------------------------
        // Same EnvGen class as the amp env; destination is cutoff, not gain.
        case ID::ENV_FILTER_ATTACK:
            for (Voice& v : _voices) v.filterEnv().setAttackMs(eng);
            break;
        case ID::ENV_FILTER_DECAY:
            for (Voice& v : _voices) v.filterEnv().setDecayMs(eng);
            break;
        case ID::ENV_FILTER_SUSTAIN:
            for (Voice& v : _voices) v.filterEnv().setSustain(eng);
            break;
        case ID::ENV_FILTER_RELEASE:
            for (Voice& v : _voices) v.filterEnv().setReleaseMs(eng);
            break;
        case ID::ENV_FILTER_ATTACK_CURVE:
            for (Voice& v : _voices) v.filterEnv().setAttackSlope(eng);
            break;
        case ID::ENV_FILTER_DECAY_CURVE:
            for (Voice& v : _voices) v.filterEnv().setDecaySlope(eng);
            break;
        case ID::ENV_FILTER_RELEASE_CURVE:
            for (Voice& v : _voices) v.filterEnv().setReleaseSlope(eng);
            break;

        // ------------- pitch envelope (Pass 7) -----------------------------
        // Third EnvGen per voice; destination is oscillator pitch, applied as a
        // per-sample ramp through the FM input (non-zipped — see Voice/OscSection).
        // Depth is the table's bipolar -1..+1 knob scaled to ±24 semitones: v1
        // constrained the pitch-env depth to ±24 st (2 octaves) and summed it
        // into the FM mixer at unity gain.
        case ID::ENV_PITCH_DEPTH:
            for (Voice& v : _voices) v.setPitchEnvDepthSemis(eng * 24.0f);
            break;
        case ID::ENV_PITCH_ATTACK:
            for (Voice& v : _voices) v.pitchEnv().setAttackMs(eng);
            break;
        case ID::ENV_PITCH_DECAY:
            for (Voice& v : _voices) v.pitchEnv().setDecayMs(eng);
            break;
        case ID::ENV_PITCH_SUSTAIN:
            for (Voice& v : _voices) v.pitchEnv().setSustain(eng);
            break;
        case ID::ENV_PITCH_RELEASE:
            for (Voice& v : _voices) v.pitchEnv().setReleaseMs(eng);
            break;
        case ID::ENV_PITCH_ATTACK_CURVE:
            for (Voice& v : _voices) v.pitchEnv().setAttackSlope(eng);
            break;
        case ID::ENV_PITCH_DECAY_CURVE:
            for (Voice& v : _voices) v.pitchEnv().setDecaySlope(eng);
            break;
        case ID::ENV_PITCH_RELEASE_CURVE:
            for (Voice& v : _voices) v.pitchEnv().setReleaseSlope(eng);
            break;

        // ------------- velocity sensitivity (Pass 8) -----------------------
        // Three 0..1 knobs, consumed at the voice's NEXT noteOn (v1 applied all
        // three per-note in VoiceBlock::noteOn — static DC, not per-block).  eng
        // == norm here (Curve::Lin, 0..1).  amp-sens curves velocity→gain;
        // filter-sens/env-sens derive per-note filter DC (see Voice::noteOn).
        case ID::VELOCITY_AMP_SENS:
            for (Voice& v : _voices) v.setVelAmpSens(eng);
            break;
        case ID::VELOCITY_FILTER_SENS:
            for (Voice& v : _voices) v.setVelFilterSens(eng);
            break;
        case ID::VELOCITY_ENV_SENS:
            for (Voice& v : _voices) v.setVelEnvSens(eng);
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
            _arbBank[0] = WavetableLib::bankFromNorm(norm);
            applyArbTable(0);
            break;
        case ID::OSC2_ARB_BANK:
            _arbBank[1] = WavetableLib::bankFromNorm(norm);
            applyArbTable(1);
            break;
        case ID::OSC1_ARB_INDEX:
            _arbIndex[0] = WavetableLib::indexFromNorm(norm, _arbBank[0]);
            applyArbTable(0);
            break;
        case ID::OSC2_ARB_INDEX:
            _arbIndex[1] = WavetableLib::indexFromNorm(norm, _arbBank[1]);
            applyArbTable(1);
            break;

        // ------------- section mixer (Pass 4) -------------------------------
        case ID::MIX_OSC1:      for (Voice& v : _voices) v.oscSection().setMixOsc1(eng); break;
        case ID::MIX_OSC2:      for (Voice& v : _voices) v.oscSection().setMixOsc2(eng); break;
        case ID::MIX_SUB:       for (Voice& v : _voices) v.oscSection().setMixSub(eng); break;
        case ID::MIX_NOISE:     for (Voice& v : _voices) v.oscSection().setMixNoise(eng); break;
        case ID::MIX_BALANCE:   for (Voice& v : _voices) v.oscSection().setBalance(eng); break;
        case ID::MIX_CROSS_MOD: for (Voice& v : _voices) v.oscSection().setCrossMod(eng); break;
        case ID::MIX_OSC_SYNC:  for (Voice& v : _voices) v.oscSection().setSyncEnabled(eng >= 0.5f); break;
        // G1: JP-8000 "LFO1 & ENV Destination" — the voice steers the
        // (LFO1-pitch + pitch-env) lane by this each block (Voice::render).
        case ID::MIX_PITCH_MOD_DEST:
            for (Voice& v : _voices) v.setPitchModDest((uint8_t)opt);
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
        case ID::LFO1_WAVEFORM:     _lfo1.osc.setWave(opt); break;
        case ID::LFO1_FREQ:
            _lfo1.freeHz = eng;
            if (_lfo1.syncMode == TempoClock::kFree) _lfo1.osc.setRateHz(eng);
            break;
        case ID::LFO1_SYNC:
            _lfo1.syncMode = opt;
            applyLfoRate(_lfo1);
            break;
        case ID::LFO1_DELAY:       _lfo1.osc.setDelayMs(eng * 4000.0f); break;
        case ID::LFO1_PITCH_DEPTH:  _lfo1.depthPitch  = eng; break;
        case ID::LFO1_FILTER_DEPTH: _lfo1.depthFilter = eng; break;
        case ID::LFO1_PWM_DEPTH:    _lfo1.depthPwm    = eng; break;
        case ID::LFO1_AMP_DEPTH:    _lfo1.depthAmp    = eng; break;

        case ID::LFO2_WAVEFORM:     _lfo2.osc.setWave(opt); break;
        case ID::LFO2_FREQ:
            _lfo2.freeHz = eng;
            if (_lfo2.syncMode == TempoClock::kFree) _lfo2.osc.setRateHz(eng);
            break;
        case ID::LFO2_SYNC:
            _lfo2.syncMode = opt;
            applyLfoRate(_lfo2);
            break;
        case ID::LFO2_DELAY:       _lfo2.osc.setDelayMs(eng * 4000.0f); break;
        case ID::LFO2_PITCH_DEPTH:  _lfo2.depthPitch  = eng; break;
        case ID::LFO2_FILTER_DEPTH: _lfo2.depthFilter = eng; break;
        case ID::LFO2_PWM_DEPTH:    _lfo2.depthPwm    = eng; break;
        case ID::LFO2_AMP_DEPTH:    _lfo2.depthAmp    = eng; break;

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

        // ------------- performance (Phase 4, ParamTable §11) ---------------
        // Glide, poly/mono/unison, unison detune, bend range, amp level.
        // Details + v1 provenance in docs/PHASE4_PERFORMANCE_SPEC.md.
        case ID::GLIDE_ENABLE:
            // Toggle: eng is 0/1.  Fans to every voice (glide is per-voice).
            for (Voice& v : _voices) v.setGlideEnabled(eng >= 0.5f);
            break;
        case ID::GLIDE_TIME: {
            // Table row is a NORM knob (0..1); map with v1's log ms law then
            // v1's 1/samples rate (block-rate quirk preserved — see helper).
            const float rate = glideRateFromNorm(norm);
            for (Voice& v : _voices) v.setGlideRate(rate);
        } break;
        case ID::VOICE_POLY_MODE:
            // opt is the frozen index 0/1/2 == PolyMode Poly/Mono/Unison; the
            // allocator kills sounding voices on an actual change (v1 parity).
            _alloc.setPolyMode((PolyMode)opt);
            break;
        case ID::VOICE_UNISON_DETUNE:
            _alloc.setUnisonDetune(eng);            // 0..1; re-spreads if Unison
            break;
        case ID::VOICE_BEND_RANGE:
            // eng arrives already in 0..24 st (table range) — v1 clamped to
            // PITCH_BEND_MAX_SEMITONES(24); the table bounds guarantee it.
            _bendRange = eng;
            break;
        case ID::VOICE_AMP_LEVEL:
            // v1 AMP_MOD_FIXED_LEVEL: the DC BASE of the VCA-mod signal (spec
            // §1.4).  Replaces the hardcoded 1.0 base in renderBlock's
            // ampMulTarget; LFO tremolo still adds on top.  Default 1.0 = unity.
            _ampFixedLevel = eng;
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
            _seq.setRate(0.1f * powf(200.0f, norm)); break;             // 0.1..20 Hz exp
        case ID::SEQ_TIMING_MODE:
            // D-1: tempo-sync deferred (TempoClock has no getTimeForMode(ms)
            // yet).  Stored inert; sequencer stays free-running at SEQ_RATE.
            _seq.setTimingMode(opt);
            break;
        case ID::SEQ_STEP_SELECT:
            _seqEditStep = (int)lroundf(norm * 15.0f); break;           // 0..15
        case ID::SEQ_STEP_VALUE:
            _seq.setStepValue(_seqEditStep, (uint8_t)lroundf(norm * 127.0f)); break;

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
    // Both, unconditionally: a Free LFO harmlessly re-asserts freeHz (a
    // no-op — same value it already has), so no branch on syncMode is
    // needed here.  Only reached on a clock edit (BPM/source), which is
    // rare compared to block rate.
    applyLfoRate(_lfo1);
    applyLfoRate(_lfo2);
}

void SynthCore::applyArbTable(int unit)
{
    uint16_t len = 0;
    const int16_t* table =
        WavetableLib::akwfTable(_arbBank[unit], _arbIndex[unit], len);
    // Table swap is glitch-safe (block boundary, phase preserved) and a
    // nullptr legally selects OscCore's naive-saw fallback — see the
    // guards in WavetableLib and OscCore.
    for (Voice& v : _voices)
        v.oscSection().setArbTable(unit, table, len);
}

// -----------------------------------------------------------------------------
// The block — audio plane entry point.
// -----------------------------------------------------------------------------

void SynthCore::renderBlock(float* left, float* right, size_t n)
{
    // 1. Note events queued since the last block.
    drainNoteEvents();

    // 2. Changed parameters only (see ParameterStore::takeNextDirty).
    const float* snap = _store.acquireSnapshot();
    size_t idx;
    while ((idx = _store.takeNextDirty()) != ParameterStore::kInvalidIndex)
        applyParam(idx, snap[idx]);

    // 3. Clear the bus, then add every ACTIVE voice.  Idle voices cost one
    //    branch — the v1 all-voices-always-run drain is designed out.
    memset(left,  0, n * sizeof(float));
    memset(right, 0, n * sizeof(float));

    // 3a. Phase 3 LFOs (spec §4/§5): tick each ENGAGED LFO (any destination
    // depth > 0) once, then distribute the net per-destination values to
    // every active voice.  A disengaged LFO is not ticked at all — its
    // phase stays frozen ("do not calculate if not required"); since its
    // depths are all zero anyway, the sums below are zero regardless, so
    // this also gives the spec's "one final block of zero destination
    // values" on the engaged->disengaged transition for free, with no
    // extra state to track.
    const float lfoUnit1 = _lfo1.engaged() ? _lfo1.osc.tickBlock() : 0.0f;
    const float lfoUnit2 = _lfo2.engaged() ? _lfo2.osc.tickBlock() : 0.0f;

    // G1/G2 lane split.  LFO1's pitch term travels its OWN lane so the voice
    // can steer it (with the pitch env) per mix.pitch_mod_dest; LFO1's PWM
    // term likewise, so the section can scale it per osc.  LFO2 and the
    // sequencer stay in the common lanes — the JP-8000 never routed them
    // (manual p.112).  Same adds as before, just not pre-summed, so the
    // default patch is arithmetically identical.
    float pitchSemisLfo1  = lfoUnit1 * _lfo1.depthPitch  * kLfoPitchMaxSemis;
    float pitchSemisCommon= lfoUnit2 * _lfo2.depthPitch  * kLfoPitchMaxSemis;
    float filterCutInput  = lfoUnit1 * _lfo1.depthFilter
                                 + lfoUnit2 * _lfo2.depthFilter;
    float pwmLfo1         = lfoUnit1 * _lfo1.depthPwm;
    float pwmCommon       = lfoUnit2 * _lfo2.depthPwm;
    // VCA-mod base = v1 AMP_MOD_FIXED_LEVEL (VOICE_AMP_LEVEL), default 1.0;
    // the LFO tremolo terms add on top (v1 ampModMixer slots 1/2).  At the
    // default patch this is exactly 1.0 + 0 = 1.0 → byte-identical output.
    float ampMulTarget    = _ampFixedLevel + lfoUnit1 * _lfo1.depthAmp
                                                  + lfoUnit2 * _lfo2.depthAmp;

    // Sequencer (Phase 7, PHASE7_SEQUENCER_SPEC.md §3): one block-rate tick,
    // output routed to ONE of the four modulation accumulators — the same lanes
    // the LFOs feed.  Ticks always (so the anti-click ramp completes even right
    // after disable), but emits 0 when disabled/idle, so a disabled sequencer
    // adds nothing and the default patch is byte-identical (Q4).  No per-voice
    // destination-change cleanup is needed (D-4): the accumulators are rebuilt
    // from scratch each block, so a stale lane cannot persist.  The four terms
    // are non-const so the sequencer can add into them in place; the amp term
    // is carried in ampMulTarget so the master-stage consumer below picks it up.
    _seq.tick(kBlockMs);
    const float seqVal = _seq.getOutput();      // ±depth, 0 when disabled/idle
    switch (_seq.destination()) {
        case SeqDest::Pitch:  pitchSemisCommon += seqVal * kLfoPitchMaxSemis; break; // Q3: same const as LFO; common lane — seq is never routed
        case SeqDest::Filter: filterCutInput  += seqVal;                     break;
        case SeqDest::Pwm:    pwmCommon         += seqVal;                    break;
        case SeqDest::Amp:    ampMulTarget     += seqVal;                     break;
        case SeqDest::None:
        default:                                                             break;
    }

    for (Voice& v : _voices) {
        if (!v.isActive()) continue;
        v.setLfoPitchSemis(pitchSemisCommon);
        v.setRoutedLfoPitchSemis(pitchSemisLfo1);
        v.filter().setLfoCutoff(filterCutInput);
        v.oscSection().setLfoPwm(pwmLfo1, pwmCommon);
    }

    for (Voice& v : _voices)
        if (v.isActive()) v.render(left, right, n);

    // 3a-bis. Per-patch FX chain (Phase 6, PHASE6_FXCHAIN_SPEC.md §3): the
    //     JP-8000 JPFX chain (saturation -> tone EQ -> mod -> delay -> limiter),
    //     an in-place stereo processor on the summed bus, AFTER the voice sum
    //     and BEFORE the global reverb (v1 order: FXChainBlock output feeds the
    //     GlobalFX reverb send).  Skipped entirely unless engaged (drive/mod/
    //     delay active), so the all-OFF default patch never touches it and the
    //     output stays byte-identical to pre-Phase-6 (Q6).  The chain mono-sums
    //     the bus (v1's single mono input), processes to stereo, and blends the
    //     wet result back against the dry bus via FX_DRY_MIX / FX_JPFX_MIX.
    if (_fxEngaged)
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
    //    a per-sample ramp so even a hard CC7 jump is a short fade.  The
    //    Phase 3 amp LFO (tremolo, global post-mix — spec §3 decision #6)
    //    rides the SAME per-sample loop, ramped independently from last
    //    block's factor to this block's target: one multiply per sample
    //    total instead of a second pass over the buffer.  At the default
    //    patch ampMulTarget is always exactly 1.0, so this ramp is a
    //    permanent no-op and the output stays byte-identical to before
    //    Phase 3.
    const float start = _masterCur;
    _masterCur += (_masterTarget - _masterCur) * 0.25f;
    const float masterStep = (_masterCur - start) / (float)n;

    const float ampStart = _ampModCur;
    _ampModCur = ampMulTarget;
    const float ampStep = (_ampModCur - ampStart) / (float)n;

    float g = start;
    float a = ampStart;
    for (size_t i = 0; i < n; ++i) {
        g += masterStep;
        a += ampStep;
        const float total = g * a;
        left[i]  *= total;
        right[i] *= total;
    }
}

} // namespace JT
