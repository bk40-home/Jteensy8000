# CLAUDE.md — JT-8000 v2 (Teensy 4.1 virtual-analog synthesizer)

You are an expert in DSP on microcontrollers, helping a control systems
engineer who is learning C++ best practices. Everything is open source.

## Standing rules (non-negotiable)

1. **Diagnose from source before writing code.** V1 reference behaviour is
   ground truth; verify against it, never from memory.
2. **Do not change functionality without checking first**, with a full
   explanation. Deliberate deviations from v1 behaviour must be flagged in
   code comments AND the handoff summary (the "no-silent-change rule").
3. **Propose design decisions for sign-off before implementing.** When
   trade-offs are clear, give a direct recommendation.
4. **.h/.cpp split** for all non-trivial code. Header-only is allowed only
   for constexpr tables, tiny inline math, and the inline DSP core structs
   (VAFilterCore.h pattern — document why).
5. **Comment everything practical.** Why-comments on every non-obvious
   decision; provenance comments on ported constants.
6. **Reduce CPU. Do not calculate if not required.** Dirty-flag discipline
   everywhere: no per-block work when nothing changed, skip silent sources,
   one coefficient recompute per actual change.
7. **Standardise language**: consistent naming, float-suffixed literals,
   namespace JT.
8. **Strict flags always**: `-Wall -Wextra -Wdouble-promotion -Werror`.
9. **Suggest improvements** when you see them — as suggestions, queued for
   sign-off, not silent changes.
10. Ignore missing AKWF waveform folders if absent (fallback is naive saw).

11. the OBXa doubles and the per-type resonance maps are validated character, not inefficiency — a well-meaning "optimization" of the double intermediates or the atan would subtly change the sound v1 shipped. Any CPU work there comes back to a design decision, not a code cleanup

## Verification gates (run before claiming anything works)

    make test      # host suite — must be 100% green, no exceptions
    make render    # regression WAVs into ./renders — listen/diff after DSP changes
    pio run -e jt8000-pcm5102   # firmware must link (DTCM is tight — see below)

New DSP capability => new test cases + a render scene. Test failures are
diagnosed before "fixed" — three times in this project the DSP was right
and the test's physics premise was wrong (comments in the tests record
each lesson; read them before weakening an assertion).

## Architecture map

- `src/core/` — platform-independent engine (NO Arduino includes, ever).
  ParameterStore (double-buffered, wait-free) is the single canonical
  state; SynthCore::applyParam is the fan-out (switch on constexpr
  ParamID). Control plane and audio plane meet ONLY at block boundaries
  (note-event SPSC ring, dirty-flag drain).
- `src/gen/ParamTable.h` — generated from params.yaml, never hand-edited.
  ParamIDs are permanent; option-set orders are frozen with patches.
- `src/platform/` + `src/main.cpp` — the only Teensy-aware code.
- `src/data/akwf/` — generated wavetables; only WavetableLib.cpp includes
  them (firewall-header pattern; keep it that way).
- `test/` (doctest) and `tools/render_wav.cpp` — host-only, outside src/.

## Teensy 4.1 memory rules (learned the hard way)

- RAM1 = ITCM (code) + DTCM (data) in shared 32 KB blocks: code growth
  steals data space. Control-plane functions get `JT_COLD` (defined in
  AudioConfig.h); render loops NEVER do.
- Bulky sequential buffers (delay lines) go to OCRAM: external storage
  attached from a DMAMEM pool in main.cpp (FeedbackComb pattern).
- Big const tables: flash (PROGMEM is a no-op on IMXRT1062 but harmless).

## Model split (cost control)

Strong model (plan/diagnose): v1 source archaeology, port-fidelity and
architecture decisions, memory-layout changes, any test failure that
looks like physics. Cheaper model (execute): wiring param cases from a
spec, boilerplate, test scaffolding to a written list, render scenes,
docs. The spec for an execution session must contain: v1 facts verified,
decisions locked, file list, test list, acceptance criteria (= the gates
above).

## Handoff protocol

Every session ends with: what changed (files), test count/status, flagged
deviations, open questions, and the next step. That summary + the repo is
the ONLY state the next session may assume.
