#!/usr/bin/env python3
# =============================================================================
# emit_presets.py — pattern-preset emitter for jt8000-params
# -----------------------------------------------------------------------------
# Turns the `pattern_presets:` block of params.yaml into gen/PatternPresets.h:
# constexpr tables of {ParamID, engineering value} writes, one array per preset.
#
# WHY ENGINEERING UNITS (not normalised t)
#   Both consumers already own the conversion and use it on every edit:
#     * ESP32  — JtParam::toNorm(desc, eng) then Store::setById (store is t).
#     * JUCE   — APVTS is keyed in engineering units, so the value goes in as-is.
#   Emitting t here would duplicate the Log/Seg2/select curve maths in Python —
#   a second copy of Curves.cpp that could drift.  Engineering values are what
#   the YAML author wrote, so they also read correctly in a diff.
#
# WHY THE FAMILY SEMANTICS LIVE HERE (FAMILIES below), NOT IN THE YAML
#   What a step character MEANS is a property of the engine (accent is a
#   velocity multiplier; the Amp lane is additive so a gate is inverted).  The
#   YAML holds the musical content; this file holds the one mapping from that
#   content onto lanes, with the engine reasoning next to it.
#
# VALIDATION (hard errors — a bad preset never reaches a controller)
#   * family known; name 1..kMaxName printable ASCII, unique in its family
#   * steps: 1..16 characters (spaces ignored), every one in the family charset
#   * every fixed/set key exists, belongs to the family's section, and is not a
#     generator-owned key (the lanes, the step count, the retired cursor params)
#   * every value is valid for its param: option NAME (or index) for selects,
#     bool for toggles, whole number in range for ints, number in range for
#     continuous
#
# WIRING — called from gen_params.py main(), beside the other emitters:
#     from emit_presets import emit_presets, PRESETS_PATH
#     ok &= write_if_changed(PRESETS_PATH, emit_presets(doc), args.check)
# =============================================================================

import os

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRESETS_PATH = os.path.join(_ROOT, "gen", "PatternPresets.h")

kMaxSteps = 16
kMaxName = 16          # fits SelectPopup's 360 px row at text size 2 with margin


class PresetError(Exception):
    """Validation failure — message names the family, preset and field."""


def _fail(msg):
    raise PresetError(msg)


# -----------------------------------------------------------------------------
# Family definitions — the engine semantics of each step character.
#
# lanes: list of (key_prefix, {char: engineering value}, default_for_unused_step)
#   key_prefix + str(n) (1-based) is the param key for step n.
# count_key: the param that receives the step-string length.
# section:   fixed/set keys must live in this ParamTable section.
# owned:     keys the generator writes itself — rejected in fixed/set so a
#            preset cannot fight the step string.
# -----------------------------------------------------------------------------
FAMILIES = {
    "arp": {
        "cpp": "Arp",
        "label": "ARP PATTERN",
        "section": 17,
        "charset": set("Xx.234"),
        "count_key": "arp.step_count",
        "lanes": [
            # on: '.' is a REST (melodic pointer still advances — engine note).
            ("arp.step_on_",
             {"X": True, "x": True, ".": False, "2": True, "3": True, "4": True},
             True),
            # accent: velocity MULTIPLIER 0..1.  A rest keeps 1.0 so that
            # toggling it back on by hand gives a normal-level hit.
            ("arp.step_accent_",
             {"X": 1.0, "x": 0.6, ".": 1.0, "2": 1.0, "3": 1.0, "4": 1.0},
             1.0),
            # ratchet: hits packed into the gate window.
            ("arp.step_ratchet_",
             {"X": 1, "x": 1, ".": 1, "2": 2, "3": 3, "4": 4},
             1),
        ],
        "owned_extra": ["arp.step_select", "arp.step_onoff",
                        "arp.step_accent", "arp.step_ratchet"],
    },
    "seq_gate": {
        "cpp": "SeqGate",
        "label": "SEQ GATE",
        "section": 13,
        "charset": set("Xx."),
        "count_key": "seq.steps",
        "lanes": [
            # INVERTED Amp encoding (depth -1, additive lane): 0 = open,
            # 1 = muted.  Unused steps 0.0 = open, matching the engine's
            # zero-filled default and the table default.
            ("seq.step_", {"X": 0.0, "x": 0.5, ".": 1.0}, 0.0),
        ],
        "owned_extra": ["seq.step_select", "seq.step_value",
                        "seq.aux_step_select", "seq.aux_step_value"],
    },
}


# -----------------------------------------------------------------------------
# Value checking — YAML value -> engineering float for one param
# -----------------------------------------------------------------------------

def _to_eng(p, v, where, option_sets):
    t = p["type"]
    k = p["key"]
    if t == "select":
        opts = option_sets[p["options"]]
        if isinstance(v, bool):
            _fail(f"{where}: {k} is a select — give an option name, not a bool")
        if isinstance(v, int):
            if not (0 <= v < len(opts)):
                _fail(f"{where}: {k} option index {v} outside 0..{len(opts) - 1}")
            return float(v)
        s = str(v)
        if s not in opts:
            _fail(f"{where}: {k} has no option {s!r} (options: {opts})")
        return float(opts.index(s))
    if t == "toggle":
        if not isinstance(v, bool):
            _fail(f"{where}: {k} is a toggle — give true/false, got {v!r}")
        return 1.0 if v else 0.0
    # continuous / int
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        _fail(f"{where}: {k} needs a number, got {v!r}")
    lo, hi = float(p["range"]["min"]), float(p["range"]["max"])
    if not (lo <= float(v) <= hi):
        _fail(f"{where}: {k} value {v} outside [{lo:g}, {hi:g}]")
    if t == "int" and float(v) != int(v):
        _fail(f"{where}: {k} is an int param, got {v}")
    return float(v)


def _family_owned(fam):
    keys = {fam["count_key"]}
    for prefix, _, _ in fam["lanes"]:
        keys.update(f"{prefix}{n}" for n in range(1, kMaxSteps + 1))
    keys.update(fam["owned_extra"])
    return keys


def _check_globals(block, fam, by_key, option_sets, where):
    """Validate a fixed/set mapping; return [(key, eng)] in YAML order."""
    if block is None:
        return []
    if not isinstance(block, dict):
        _fail(f"{where}: must be a mapping of param key -> value")
    owned = _family_owned(fam)
    out = []
    for k, v in block.items():
        p = by_key.get(k)
        if p is None:
            _fail(f"{where}: unknown param key {k!r}")
        if k in fam["owned_extra"]:
            _fail(f"{where}: {k} is a retired cursor param the engine ignores")
        if k in owned:
            _fail(f"{where}: {k} is written by the step string — remove it")
        if p["ui"]["section"] != fam["section"]:
            _fail(f"{where}: {k} is outside this family's section "
                  f"{fam['section']} — a pattern preset may not touch it")
        out.append((k, _to_eng(p, v, where, option_sets)))
    return out


# -----------------------------------------------------------------------------
# Model: validate everything and resolve each preset to an ordered write list
# -----------------------------------------------------------------------------

def build_presets(doc):
    """Returns [(family_name, fam_def, [(preset_name, [(key, eng)])])]."""
    by_key = {p["key"]: p for p in doc["params"]}
    option_sets = doc["option_sets"]
    block = doc.get("pattern_presets") or {}

    for fname in block:
        if fname not in FAMILIES:
            _fail(f"pattern_presets: unknown family {fname!r} "
                  f"(known: {sorted(FAMILIES)})")

    result = []
    for fname, fam in FAMILIES.items():            # fixed, deterministic order
        fblock = block.get(fname) or {}
        fixed = _check_globals(fblock.get("fixed"), fam, by_key, option_sets,
                               f"pattern_presets.{fname}.fixed")
        presets = []
        seen = set()
        for i, pr in enumerate(fblock.get("presets") or []):
            name = pr.get("name")
            where = f"pattern_presets.{fname}[{i}] {name!r}"
            if not isinstance(name, str) or not (1 <= len(name) <= kMaxName):
                _fail(f"{where}: name must be 1..{kMaxName} characters")
            if not all(32 <= ord(c) < 127 for c in name) or '"' in name or "\\" in name:
                _fail(f"{where}: name must be plain printable ASCII")
            if name in seen:
                _fail(f"{where}: duplicate name in family {fname}")
            seen.add(name)

            raw = pr.get("steps")
            if not isinstance(raw, str):
                _fail(f"{where}: steps must be a string")
            steps = raw.replace(" ", "")
            if not (1 <= len(steps) <= kMaxSteps):
                _fail(f"{where}: {len(steps)} steps — must be 1..{kMaxSteps}")
            bad = sorted(set(steps) - fam["charset"])
            if bad:
                _fail(f"{where}: characters {bad} not allowed in family "
                      f"{fname} (allowed: {''.join(sorted(fam['charset']))})")

            unknown = set(pr) - {"name", "steps", "set"}
            if unknown:
                _fail(f"{where}: unknown field(s) {sorted(unknown)}")

            # fixed first, then set overrides IN PLACE (keeps one write per key)
            merged = dict(fixed)
            order = [k for k, _ in fixed]
            for k, e in _check_globals(pr.get("set"), fam, by_key, option_sets,
                                       f"{where}.set"):
                if k not in merged:
                    order.append(k)
                merged[k] = e
            writes = [(k, merged[k]) for k in order]

            writes.append((fam["count_key"], float(len(steps))))
            for prefix, cmap, dflt in fam["lanes"]:
                for n in range(1, kMaxSteps + 1):
                    v = cmap[steps[n - 1]] if n <= len(steps) else dflt
                    k = f"{prefix}{n}"
                    if k not in by_key:
                        _fail(f"{where}: lane param {k} missing from params")
                    writes.append((k, _to_eng(by_key[k], v, where, option_sets)))
            presets.append((name, writes))
        result.append((fname, fam, presets))
    return result


# -----------------------------------------------------------------------------
# Emission
# -----------------------------------------------------------------------------

def _f(x):
    s = f"{float(x):g}"
    if "." not in s and "e" not in s:
        s += ".0"
    return s + "f"


def _q(s):
    return '"' + s + '"'


def emit_presets(doc):
    fams = build_presets(doc)
    by_key = {p["key"]: p for p in doc["params"]}
    L = []
    a = L.append
    a("// =============================================================================")
    a("// PatternPresets.h — GENERATED FILE, DO NOT EDIT")
    a("// =============================================================================")
    a("// Generated by tools/emit_presets.py from the pattern_presets block of")
    a("// params.yaml.  Edit the YAML, rerun tools/gen_params.py, commit both.")
    a("//")
    a("// A preset is a list of ordinary parameter writes {ParamID, ENGINEERING value}.")
    a("// Apply it through the normal edit path so the result is stored, sent and")
    a("// resynced like any knob:")
    a("//   ESP32 : store.setById(w.id, JtParam::toNorm(*desc, w.eng))  — the store's")
    a("//           exact-compare dirty flag makes the send diff-only for free.")
    a("//   JUCE  : APVTS is in engineering units — set the value directly.")
    a("// The Teensy firmware does not include this file.")
    a("//")
    a("// Header-only by design (CLAUDE.md rule 4 exception): constexpr tables only,")
    a("// no code — the whole file lives in flash.")
    a("// =============================================================================")
    a("#pragma once")
    a("")
    a("#include <stdint.h>")
    a("")
    a("namespace JT {")
    a("namespace PatternPresets {")
    a("")
    a("struct Write  { uint16_t id; float eng; };")
    a("struct Preset { const char* name; const Write* writes; uint16_t count; };")
    a("struct Family { const char* label; const Preset* presets;")
    a("                const char* const* names; uint8_t count; };")
    a("")
    a("// Family order is the FamilyId value.  Not stored anywhere, so it may be")
    a("// extended freely; preset ORDER within a family is display order only.")
    a("enum class FamilyId : uint8_t {")
    for i, (_, fam, _) in enumerate(fams):
        a(f"    {fam['cpp']} = {i},")
    a(f"    Count = {len(fams)}")
    a("};")
    a("")

    for fname, fam, presets in fams:
        cpp = fam["cpp"]
        a("// -----------------------------------------------------------------------------")
        a(f"// Family: {fname}  ({len(presets)} presets)")
        a("// -----------------------------------------------------------------------------")
        for i, (name, writes) in enumerate(presets):
            a(f"// {name}")
            a(f"static constexpr Write k{cpp}{i}[] = {{")
            lane_keys = {f"{pfx}{n}" for pfx, _, _ in fam["lanes"]
                         for n in range(1, kMaxSteps + 1)}
            globals_ = [(k, e) for k, e in writes if k not in lane_keys]
            lanes = [(k, e) for k, e in writes if k in lane_keys]
            for k, e in globals_:
                a(f"    {{ 0x{by_key[k]['id']:04X}, {_f(e)} }},  // {k}")
            for j in range(0, len(lanes), 4):
                chunk = lanes[j:j + 4]
                cells = ", ".join(f"{{ 0x{by_key[k]['id']:04X}, {_f(e)} }}"
                                  for k, e in chunk)
                a(f"    {cells},  // {chunk[0][0]}..")
            a("};")
        a(f"static constexpr Preset k{cpp}Presets[] = {{")
        for i, (name, writes) in enumerate(presets):
            a(f"    {{ {_q(name)}, k{cpp}{i}, {len(writes)} }},")
        if not presets:
            a("    { \"\", nullptr, 0 },   // placeholder: zero-length arrays are ill-formed")
        a("};")
        a(f"static constexpr const char* const k{cpp}Names[] = {{")
        for name, _ in presets:
            a(f"    {_q(name)},")
        if not presets:
            a("    \"\",")
        a("};")
        a("")

    a("static constexpr Family kFamilies[] = {")
    for _, fam, presets in fams:
        cpp = fam["cpp"]
        a(f"    {{ {_q(fam['label'])}, k{cpp}Presets, k{cpp}Names, {len(presets)} }},")
    a("};")
    a("")
    a("inline const Family& family(FamilyId f)")
    a("{")
    a("    return kFamilies[static_cast<uint8_t>(f) < static_cast<uint8_t>(FamilyId::Count)")
    a("                     ? static_cast<uint8_t>(f) : 0u];")
    a("}")
    a("")
    a("} // namespace PatternPresets")
    a("} // namespace JT")
    return "\n".join(L) + "\n"


if __name__ == "__main__":
    import sys
    import yaml
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    with open(os.path.join(_ROOT, "params.yaml"), encoding="utf-8") as f:
        d = yaml.safe_load(f)
    sys.stdout.write(emit_presets(d))
