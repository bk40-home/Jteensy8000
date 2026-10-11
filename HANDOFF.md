# Pattern presets + sequencer tempo sync — handoff

## What changed

### Jteensy8000 (firmware)
| File | Change |
|---|---|
| `src/core/dsp/StepSequencer.h/.cpp` | Tempo sync: `setTimingMode(mode, clock)` and a real `updateFromClock()`. Step ms = `1000 / TempoClock::freqForMode`, cached as `_syncHz`. `setRate()` stores without applying while synced (Free fallback). |
| `src/core/SynthCore.cpp` | 2 hunks: `SEQ_TIMING_MODE` passes `_clock`; `refreshSyncedRates()` calls `_seq.updateFromClock(_clock)`. Internal BPM and external MIDI clock both reach the sequencer. |
| `src/gen/ParamTable.h` | Regenerated. Only change: `seq.rate` gets `visible_when` (Free). |
| `test/test_sequencer.cpp` | Inert D-1 test replaced by 7 sync tests (unit + engine + external clock). |
| `test/test_pattern_presets.cpp` | **New.** Applies every generated preset on the real engine. **Add it to `SRCS` in the Makefile.** |
| `DEFERRALS_LEDGER.md` | D-6 closed. |

### jt8000-params
| File | Change |
|---|---|
| `params.yaml` | `seq.rate` `visible_when`; new `pattern_presets:` block (13 arp, 6 seq-gate). |
| `tools/emit_presets.py` | **New.** Validates presets, emits `gen/PatternPresets.h`. |
| `tools/gen_params.py` | Calls the emitter; validates before any file is written. |
| `gen/ParamTable.h`, `gen/PatternPresets.h` | Generated. |

### JtMidiController (ESP32)
| File | Change |
|---|---|
| `PatternPresetApply.h/.cpp` | **New.** `JtPresets::apply(store, family, index)`: engineering → `toNorm` → `Store::setById`. No Arduino dependency. |
| `SelectPopup.h/.cpp` | List mode: `openList(tag, title, names, count, current)`, `isListMode()`. Param mode is unchanged (`desc_` replaced by `title_`/`names_`). |
| `ViewController.h/.cpp` | Long-press (≥ `LONG_PRESS_MS`) on the SEQ grid parks a preset request: Gate lane → SEQ GATE, Arp lane → ARP PATTERN, Aux → nothing. Adds `applyPatternPreset()`. |
| `JtMidiController.ino` | Collects the request and opens the list popup. One `commitPopup()` serves both touch and encoder. |
| `PatternPresets.h`, `ParamTable.h` | Copied from jt8000-params/gen. |

**Apply as diffs** (`firmware.diff`, `controller.diff`), not by overwriting. The full files are built on the public `master`, which looks older than your local tree (see below).

## Verification (host)
- **Sequencer:** 21/21 sequencer cases pass. This includes the "default patch byte-identical" check.
- **Presets on the engine:** 4/4 preset cases pass, covering all 19 presets.
  - Gate 16ths measured: rest steps RMS 0.0, hit steps 0.088.
  - Arp rest step: 0.0.
- **Whole suite:** no new failures compared with the baseline (282 cases).
- **Generator:** `gen_params.py --check` clean. 15 negative validator cases are all rejected with clear messages.
- **ESP32 apply:** host test passes for all 19 presets:
  - every value round-trips exactly through `toNorm` → `toEng`;
  - re-applying the same preset sends 0;
  - Straight 16 → Offbeat 8 sends 25 of 55.
- **ESP32 UI:** `SelectPopup` was host-run in both modes. `ViewController.cpp` and the `.ino` fragments pass a `-Wall -Wextra` syntax check against stub headers.
- **Firmware:** `make attrcheck` is OK.

**Not run here:** `pio run` (no Teensy toolchain), the ESP32 Arduino build, `make render` (`tools/render_wav.cpp` isn't in the public repo), and JUCE (private repo).

## Public master is inconsistent — check against your local tree
- `Makefile` has unresolved stash conflict markers.
- `TempoClock.h` lacks the dotted modes that the tests and `params.yaml` use. I added them locally only to build; that change is **not** delivered.
- `test_parameter_store/patch/engine.cpp` are missing.
- **Real bug:** `SynthCore::kCombPoolFloats` hardcodes `221u`. At 48 kHz `FeedbackComb::kDelaySamples` is 240, so ASan reports a heap overflow in `FeedbackComb::reset()`. On Teensy that would be an OCRAM overrun.
  - Suggested fix: `2u * kMaxVoices * FeedbackComb::kDelaySamples`.
  - Not delivered; it needs sign-off.
- 18 baseline failures, mostly oscillator pitch and the 48 kHz migration. They exist before and after this change.
- The ESP32 public `ParamTable.h` has 250 params against 252 in the YAML (missing `GLIDE_MODE`, `LFO2_MOD_WHEEL`). If your local copy is also 250, updating it triggers **D-10** (saved-patch invalidation).

## Flagged deviations / limits
1. **Sequencer sync is BPM-follow only, not bar-phase-locked.** MIDI Start resets to step 0, the same model as the arp.
2. **Seq gate uses an inverted Amp encoding** (depth −1, rest = 1.0). Because of that:
   - The grid shows rests as tall bars.
   - It assumes `voice.amp_level` = 1.0. Below that, a rest's gain goes negative and leaks an inverted signal.
   - Suggested follow-up (needs sign-off): clamp `ampMul ≥ 0` in `SynthCore`.
3. **Duty cycle for seq gates comes from rate, not gate length.** 1/32 gives a half-bar loop where `X.` = one 16th at 50%.
4. **Arp presets set `arp.enable` true** (family `fixed`). They don't touch latch or free rate.
5. **JUCE not done.** Apply the same tables directly to APVTS in engineering units. Steps go through the existing `ArpParamId` / seq step path.

## Hardware test plan
1. **Firmware sync**
   - Set SEQ dest Filter, depth +, sync 1/16, BPM 120. Steps land on the grid against a metronome.
   - Change BPM: it follows.
   - Ext MIDI clock from Cubase at 140: it follows. Transport Start → step 1.
2. **Free fallback**
   - Set sync to Free: RATE knob reappears with its old value.
   - Turning RATE while synced has no effect.
3. **ESP32 seq gate**
   - SEQ page, Gate lane: hold the grid ~1 s and release. The SEQ GATE list opens; pick "Gate 16ths".
   - Hold a pad: clean 16th chop, no clicks.
   - Pick "Trance Chop": a full-bar pattern at 1/16.
4. **ESP32 arp**
   - Flip to the Arp lane, long-press, pick "Offbeat 8". Hold a chord: offbeat stabs.
   - Pick "Stutter Tail": ratchets on the last beat.
   - Pick "Straight 16" after "Tresillo": swing is back to straight.
5. **Layers.** In Layer mode with edit layer B, apply an arp preset. Only B changes.
6. **Edge cases.**
   - A short tap on the grid still writes one step.
   - A long-press on the Aux lane does nothing.
   - A long-press, then tapping outside the popup, cancels with no writes.
7. **Wire load.** Apply presets back to back. There should be no dropped NRPNs at 115,200 baud: the worst case is 55 writes ≈ 80 ms.
