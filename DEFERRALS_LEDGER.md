# DEFERRALS_LEDGER.md

Formally deferred items and retired behaviour. Anything recorded here is a
deliberate decision, not an oversight. Entries never silently disappear — they
are closed with a note saying what closed them.

---

## Retired in the arp/sequencer review pass

### R-1 — Step CURSOR parameters retired (engine ignores them)

**Retired:** `seq.step_select` (0x068A), `seq.step_value` (0x068B),
`seq.aux_step_select` (0x068E), `seq.aux_step_value` (0x068F),
`arp.step_select` (0x0889), `arp.step_onoff` (0x088A),
`arp.step_accent` (0x088B), `arp.step_ratchet` (0x088C).

**Why.** These addressed a sixteen-step pattern through a moving cursor, so the
ParameterStore held ONE scalar per lane for SIXTEEN values. Three consequences,
all silent:

* a patch saved the last edited step, never the pattern;
* a full resync sent one value, so a second editor could never learn the
  pattern;
* the ESP32 kept patterns in local RAM only, and nothing else could see them.

**Replacement.** 80 explicit per-step parameters, decoded arithmetically in
`SynthCore::applyParam` from contiguous ID blocks:

| Lane | IDs | Count |
|---|---|---|
| `seq.step_1..16` | 0x0690–0x069F | 16 |
| `seq.aux_step_1..16` | 0x06A0–0x06AF | 16 |
| `arp.step_on_1..16` | 0x088D–0x089C | 16 |
| `arp.step_accent_1..16` | 0x089D–0x08AC | 16 |
| `arp.step_ratchet_1..16` | 0x08AD–0x08BC | 16 |

**Why the rows still exist.** ParamIDs are permanent and the generator enforces
contiguous, position-derived indices within a section, so a retired row cannot
be deleted without renumbering everything after it. The rows stay in the table
and fall through to `default:` in `applyParam`, which consumes them silently.

**Consumer obligation — PARTLY CLOSED.** The ESP32 controller has been migrated
(see D-1, closed). The JUCE editor has NOT — see D-9.

---

## Open deferrals

### D-1 — ESP32 step editing — CLOSED

Migrated. `ViewController` now resolves five 16-entry ordinal tables
(`ordSeqStep_`, `ordSeqAuxStep_`, `ordArpAccent_`, `ordArpOn_`,
`ordArpRatchet_`) and a grid tap is ONE write to ONE slot. The
select-then-value wire dance, the `seqSelStep_` cursor mirror and the
`setForce` collision workaround are all gone. `SeqPanel`'s three local caches
are gone too: `draw()` takes the active lane's 16 ordinals and reads the store
directly, so the bars cannot drift from the sound.

Side effect worth noting: a patch load now repaints the grid correctly. It
never did before — nothing populated those caches on load, so the grid showed
whatever the last manual edit had left there.

### D-2 — Arp playhead consumer — CLOSED

`ViewController`'s rx trampoline decodes `0x3FFE` into `arpPlayStep_` /
`arpRunning_`, and the SEQ page picks its playhead per lane: the arp lane
follows the arp transport, the gate and aux lanes follow the sequencer's.

### D-9 — JUCE editor step editing not yet migrated (OPEN)

`JT8000_Controller` still drives the retired cursor parameters and needs a
regenerated parameter set (`emit_juce.py` / `emit_bridge.py` from the updated
`params.yaml`), plus a grid binding onto the 80 explicit step params. Until
then, step editing from the plugin does nothing.

### D-10 — ESP32 saved patches are invalidated by the parameter-count change

`PatchStore` stamps `paramCount` into every patch header and rejects a file
whose count does not match `JT::Params::kParamCount` (`PatchStore.cpp:108`).
That count moved 161 -> 243, so every patch already on the controller's FFat
will be refused on load. The refusal is graceful — it logs and returns false,
it does not crash — but the patches are unreadable.

This is the ESP32 mirror of the "old patch breakage accepted" decision taken on
the JUCE side. If any of those patches matter, the migration is mechanical: read
the v2 header, map the 161 old ordinals onto the new table by ParamID, and
rewrite with the new count. Not attempted here because no sign-off was given
for it.

### D-3 — Aux `Drive` is inaudible while `fx.drive` is OFF

`SeqAuxDest::Drive` (index 5) modulates the saturator INPUT gain, and
`FxChain::applySaturation` bypasses entirely when `_driveMode == 0`. The aux
lane cannot conjure a saturator that is not running. Asserted deliberately in
`test_fxchain.cpp` so the limitation stays honest rather than becoming a bug
report. Options if it ever matters: force a minimum drive mode when the
destination is selected, or surface it in the UI as a disabled row.

### D-4 — Swing polarity flips at the wrap on an ODD step count

`Arpeggiator::stepDurationFor` swings on the ABSOLUTE step index parity, so a
pattern with an odd `arp.step_count` alternates long/short across the loop
boundary rather than restarting on a long step. Inherent to index-parity swing.
Documented in `Arpeggiator.h`; left as-is deliberately.

### D-5 — Gate-lane chopping retained (item 4c, signed off)

Both sequencer lanes are held at zero outside the gate window, so at the
default gate of 0.5 each lane is silent for half of every step. This is the
main reason the modulation reads as subtle. Retained as inherited v1
behaviour by explicit sign-off; a `seq.hold` toggle was offered and declined.

### D-6 — Tempo-sync for the step sequencer (pre-existing)

`SEQ_TIMING_MODE` is stored but inert: `TempoClock` exposes `freqForMode` (Hz)
but not `getTimeForMode` (ms), which v1's sequencer sync path needs.
`setTimingMode` / `updateFromClock` remain stubs. Unchanged by this pass.
Also blocks `FX_DELAY_SYNC` and external MIDI clock for the delay.

### D-7 — LFO master destination-selector (pre-existing)

Unchanged by this pass.

### D-8 — 14-bit NRPN outbound for continuous params (J4, pre-existing)

Described but not initiated. Unchanged by this pass.

---

## Verification gaps in the arp/sequencer review delivery

These are gaps in what could be PROVEN, not known defects.

* **Render baseline not run.** `tools/render_wav.cpp` is absent from the
  repository, so `make render` + `cmp` could not be executed. Every change in
  this pass is inert at defaults (new params default to no-op, both bipolar
  flags off, aux destination None, tone tilt and drive mod zero), so byte
  identity is expected by construction but is NOT demonstrated. Run the gate
  locally before shipping.
* **Three test files absent** from the repository: `test_engine.cpp`,
  `test_parameter_store.cpp`, `test_patch.cpp`. These are precisely the suites
  that would cover the store resize (273 → 403 slots) and the patch round-trip
  across 82 new parameters — the highest-risk part of this change is the part
  with the least coverage. Restore them and re-run before hardware.
