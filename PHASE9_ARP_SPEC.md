# PHASE9_ARP_SPEC.md — Arpeggiator + External MIDI Clock

JT-8000 v2, Phase 9. Adds a playable arpeggiator (classic + trance-era) and the
external-MIDI-clock subsystem it (and the LFOs and step sequencer) sync to.

This document is the sign-off record and delivery note for the pass. It follows
the diagnose-first / no-silent-change rules in `CLAUDE.md`.

---

## 1. Scope (as signed off)

**In:**

- 7 note-ordering modes: Up, Down, Up/Down inclusive, Up/Down exclusive,
  As-Played, Random, Chord.
- Octave span 1–4.
- 16-step **pattern layer**: per step `{on/off, accent 0–127, ratchet 1–4}`.
  Ratchet subdivides the **gate portion** of the step, so gate-length still
  shapes staccato within each ratchet sub-hit.
- Rate: shared 12-entry musical-division set (`timing_mode`); `Free` falls back
  to `arp.free_hz` (exp-mapped 0.1–20 Hz).
- Gate length, swing (0.5 straight, >0.5 shuffle delaying odd steps), latch.
- Independent clock (own division/phase) reading the **shared** `TempoClock`
  BPM — so internal-tempo and external-clock changes move the arp, LFOs and seq
  together.
- **Classic note-consumption**: while enabled, played notes are consumed into
  the held-note list and only the arp sounds.
- Voice triggering is **B-note** — the arp drives `VoiceAllocator::noteOn/off`
  directly, the same API the keyboard uses.
- **External MIDI clock**: 24-PPQN measurement → BPM (BPM-follow), transport
  Start resets phase to the downbeat; Stop silences; Continue resumes. Applied
  to the whole synth via the existing clock plumbing.

**Deferred (logged §7):** JP-8080-style named beat-pattern *presets*;
MIDI arp-note-out; per-step transpose; sample-accurate per-pulse phase-lock
(BPM-follow + Start-reset shipped instead); the TFT/controller UI (dropped from
this pass by request).

---

## 2. Architecture & diagnosis (file:line the build follows)

The arp is a **control-plane note generator**, a sibling of `StepSequencer`,
ticked once per block from `renderBlock`.

- **Note entry** — `main.cpp` `onNoteOn/onNoteOff` → `SynthCore::noteOn/noteOff`
  push onto the lock-free `NoteEvent` ring; the audio plane drains it in
  `SynthCore::drainNoteEvents()`. That drain is the interception point: when
  `arp.enabled()`, `kEvOn/kEvOff` route to `_arp` instead of `_alloc`.
- **Voice trigger** — `VoiceAllocator::noteOn(note,vel)/noteOff(note)`.
- **Tick site** — `renderBlock`, immediately before `_seq.tick()`.
- **Threading discovery** — `applyParam` runs **inside `renderBlock`** (audio
  plane): `renderBlock` drains the param dirty-queue itself. So external-clock
  hand-off from `main.cpp` (control plane) uses lock-free atomics drained at the
  top of `renderBlock`, the same discipline as the note ring — no cross-plane
  mutation.
- **Clock** — `TempoClock::freqForMode(mode)` returns the synced Hz; the arp
  computes `stepDurationMs = 1000 / freqForMode`. Unlike the sequencer (whose
  tempo-sync was deferred as D-1 pending a ms accessor), the arp **is**
  tempo-synced — `freqForMode(Hz)` is all it needs.

### External clock

Measurement needs a wall clock and must **not** enter the Arduino-free core
(that would break the host render/test harness). So it lives in
`src/platform/ExternalClock.{h,cpp}`, with `micros()` **injected** as a function
pointer — which keeps the header Arduino-free and lets the measurement math be
host-unit-tested. It measures 24 PPQN, averages the interval over a beat to
reject jitter, discards the first beat (warm-up), and pushes a finished BPM into
`SynthCore::setExternalBpm()`. The core applies it to the shared `TempoClock`
**only while the clock source is External**, so a synth left on Internal ignores
stray clock bytes.

---

## 3. Parameters (section 17, base 0x0880)

Section 16 (Master) uses only 0x0800; section 17 is free. All `params.yaml`,
regenerated via `gen_params.py`, synced to `src/gen/ParamTable.h`.

| id     | key               | type       | notes |
|--------|-------------------|------------|-------|
| 0x0880 | arp.enable        | toggle     | default OFF → byte-identical default patch |
| 0x0881 | arp.mode          | select(7)  | `arp_mode`; order frozen with `ArpMode` |
| 0x0882 | arp.octaves       | select(4)  | opt 0..3 → 1..4 octaves |
| 0x0883 | arp.latch         | toggle     | hold pattern after keys release |
| 0x0884 | arp.rate          | select(12) | shared `timing_mode`; default 1/16; opt 0 = Free |
| 0x0885 | arp.free_hz       | continuous | fallback rate when rate==Free (exp 0.1–20 Hz) |
| 0x0886 | arp.gate_length   | continuous | fraction of step |
| 0x0887 | arp.swing         | continuous | 0.5 straight; >0.5 shuffle |
| 0x0888 | arp.step_count    | int 1..16  | pattern length |
| 0x0889 | arp.step_select   | int 1..16  | edit cursor (0..15 internal) |
| 0x088A | arp.step_onoff    | toggle     | writes step at cursor (default ON) |
| 0x088B | arp.step_accent   | continuous | writes step at cursor (default full) |
| 0x088C | arp.step_ratchet  | int 1..4   | writes step at cursor |

Step params use the **select-then-value** NRPN idiom, identical to the
sequencer: `arp.step_select` moves the cursor, the three `arp.step_*` writes
target it.

The arp has **no clock-source param of its own** — it follows the global
`clock.clock_source`. (An earlier mirror param was removed; see §5.)

---

## 4. Behaviour notes worth knowing

- **Chord mode** fires the held chord as rhythmic stabs driven by the step
  pattern (re-triggering on each active step), advancing one octave-slice per
  step. This is the trance-gate-on-a-chord behaviour; it is *not* a strum.
- **Rest steps** (`step_onoff` off) skip firing but still advance the melodic
  pointer, so pattern rhythm and note sequence stay independent.
- **Random** never repeats the immediately previous note when >1 note is
  available.
- **Sounding-note tracking**: the arp releases only notes *it* triggered, never
  a key the player physically holds — so nothing sticks and player notes are
  never stolen by the arp's note-offs.
- **Transport Start** re-anchors phase to step 0 (locks to a DAW downbeat).

---

## 5. Deliberate change flagged (no-silent-change rule)

**Removed a mirror `arp.clock_source` param during development.** The first
draft exposed a per-arp clock-source select that also wrote the shared
`TempoClock` source. Because every param is dirty at init and applied in table
order, the mirror's `Internal` default overwrote an `External` selection made by
`clock.clock_source` **every block** — silently disabling external clock. Two
params owning one piece of state is the bug. The arp now follows the single
global source, which was always the intended model. Caught by the end-to-end
external-clock test before delivery.

---

## 6. Verification

- Compiles clean under `-Wall -Wextra -Wdouble-promotion -Werror`:
  `Arpeggiator.cpp`, `ExternalClock.cpp`, edited `SynthCore.cpp`.
- `main.cpp` and the USBHost realtime binding are Teensy-only; verified
  structurally (brace/paren balance, static construction order: `gSynth` before
  `gExtClock`, `core()` returns a live reference).
- `make test`: **187 cases, arp suite 5/5 green (23 assertions).** The 2 failing
  cases (`test_velocity.cpp`, `test_osc_section.cpp`) are **pre-existing** and
  unrelated — present in the baseline before this pass. `CLAUDE.md`'s 100%-green
  gate is currently blocked *only* by those two; they should be triaged
  separately (their comments warn the physics premise may be the issue).
- `make render`: **all 16 baseline WAVs byte-identical** (arp off + internal
  clock ⇒ every path falls through to prior behaviour).

New coverage: `test/test_arpeggiator.cpp` — orderings, octave expansion, latch,
no-stuck-notes through disable/clear, note-consumption, and external-clock BPM
tracking via a synced LFO readback.

---

## 7. Deferrals ledger (this pass)

| id   | item | rationale |
|------|------|-----------|
| A-1  | JP-8080 named beat-pattern presets | they are just saved step-layer patterns; add as preset data later |
| A-2  | MIDI arp-note-out | not required for internal-voice B-note; needs an out-path design |
| A-3  | Per-step transpose | extra per-step lane; out of v1 scope |
| A-4  | Sample-accurate per-pulse phase-lock | BPM-follow + Start-reset shipped; per-pulse adds cross-plane traffic for no audible gain on this block-rate engine |
| A-5  | TFT / controller UI for the arp page | dropped from this pass by request; params + NRPN idiom are ready for it |

---

## 8. Open items to confirm on hardware

1. **USBHost_t36 handler name** — used `midiHost.setHandleRealTimeSystem(uint8_t)`.
   Confirm against the installed USBHost_t36 version when building on the Teensy.
2. Confirm Chord-mode stab behaviour (§4) matches intent, or flag for a strum
   variant.
