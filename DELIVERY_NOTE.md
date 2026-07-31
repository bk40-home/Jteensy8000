# JT-8000 v2 — Phase 9 Delivery: Arpeggiator + External MIDI Clock

One combined delivery. Additive; the default patch (arp off, internal clock) is
byte-identical to the prior baseline.

## What's in this pass

- **Arpeggiator** (`src/core/dsp/Arpeggiator.{h,cpp}`) — 7 modes (Up, Down,
  UpDn inc/exc, AsPlayed, Random, Chord), 1–4 octaves, 16-step pattern layer
  (on/off + accent + ratchet, ratchet subdivides the gate), swing, latch, gate
  length. Independent clock, shared BPM. Drives voices directly (B-note).
  Classic note-consumption (arp on ⇒ only the arp sounds).
- **External MIDI clock** (`src/platform/ExternalClock.{h,cpp}`) — 24-PPQN
  measurement → BPM, transport Start/Stop/Continue; drives the whole synth
  (LFOs + seq + arp) via the shared TempoClock while source == External.
- **SynthCore integration** — note routing in `drainNoteEvents`, arp tick in
  `renderBlock`, 13 param cases, lock-free external-clock atomics + drain,
  transport.
- **main.cpp** — ExternalClock instance, micros bridge, realtime handlers on all
  three MIDI ports.
- **params.yaml** — section 17 (Arpeggiator), regenerated, synced to src/gen.
- **Tests** — `test/test_arpeggiator.cpp`, 5 cases / 23 assertions, all green.
- **Spec** — `docs/PHASE9_ARP_SPEC.md` (full diagnosis, param map, deferrals).

## Apply order

1. Drop the `src/`, `test/`, `docs/`, `Makefile` files into the firmware repo.
2. Drop `jt8000-params/` files into the params repo (params.yaml + regenerated
   gen/ParamTable.h + docs/ParamMap.md). If you prefer, re-run
   `python3 tools/gen_params.py` yourself and diff — output matches.
3. `make test` (arp suite green; see note on pre-existing failures) and
   `make render` (byte-identical).

## Verification captured

- `-Wall -Wextra -Wdouble-promotion -Werror` clean on all new/edited core.
- `make test`: 187 cases; arp 5/5. The 2 failing cases
  (`test_velocity.cpp`, `test_osc_section.cpp`) are PRE-EXISTING, not from this
  pass — flagged for separate triage.
- `make render`: all 16 baseline WAVs byte-identical.

## Two things to confirm on hardware

1. USBHost_t36 handler name `setHandleRealTimeSystem(uint8_t)` — verify against
   your installed version (only API I couldn't host-compile).
2. Chord mode = rhythmic chord stabs driven by the step pattern (not a strum).
   Flag if you want a strum variant.

## Deferred (logged in the spec §7)

JP-8080 named beat-pattern presets; MIDI arp-note-out; per-step transpose;
sample-accurate per-pulse phase-lock; the TFT/controller UI (dropped by request).
