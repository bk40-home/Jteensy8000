#!/usr/bin/env python3
# =============================================================================
# gen_params.py — JT-8000 v2 parameter table generator
# =============================================================================
#
# THE RULE: params.yaml is the single source of truth.  This script turns it
# into the C++ tables and documentation every codebase consumes.  Generated
# files are committed to git; CI runs `gen_params.py --check` so a stale or
# hand-edited generated file fails the build.  Drift between the three
# codebases is therefore structurally impossible — the failure mode of v1's
# sync_cc_defs.py (three hand-maintained copies, drift merely *detected*)
# is designed out.
#
# OUTPUTS
#   gen/ParamTable.h   — constexpr parameter table (FLASH only, zero RAM).
#                        Consumed by the Teensy firmware now; the ESP32 and
#                        JUCE emitters are added in Phase 5 and will reuse
#                        the same loaded/validated model (build_model()).
#   docs/ParamMap.md   — human-readable MIDI implementation chart, generated
#                        so it can never lie about the firmware.
#
# USAGE
#   python3 tools/gen_params.py            # regenerate outputs in place
#   python3 tools/gen_params.py --check    # CI gate: exit 1 if outputs stale
#
# VALIDATION (all hard errors — the generator refuses to emit a bad table)
#   * ParamID unique, ≤ 0x3FFF (14-bit), and consistent with (section<<7)|index
#     derived from position — appends only, no renumbering.
#   * ParamID not in retired_ids.
#   * key / name unique.
#   * CC bindings unique and never from the RESERVED set (NRPN machinery,
#     mod sources, sustain, bank select, channel-mode 120–127).
#   * select params reference an existing option set.
#   * ranges sane: min < max; curve 'log' requires min > 0;
#     curve 'seg2' requires 'mid' strictly between min and max.
#   * defaults inside range / valid option index / boolean.
#
# DESIGN NOTES
#   * Output is emitted with LF endings and a trailing newline, then compared
#     byte-for-byte in --check mode.  Deterministic output is a feature:
#     the same YAML always produces the same bytes, so git diffs are honest.
#   * We deliberately do NOT use yaml.dump or f-string templating libraries —
#     plain line assembly keeps the output format under exact control.
# =============================================================================

import argparse
import os
import re
import sys

try:
    import yaml
except ImportError:
    sys.exit("gen_params.py requires PyYAML:  pip install pyyaml")

# Paths are relative to the repo root (parent of tools/), so the script works
# from any CWD — important for CI runners.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
YAML_PATH = os.path.join(ROOT, "params.yaml")
HEADER_PATH = os.path.join(ROOT, "gen", "ParamTable.h")
DOC_PATH = os.path.join(ROOT, "docs", "ParamMap.md")

# CCs a parameter may NEVER bind to (design brief §5.1).  These carry their
# standard MIDI meaning and are handled by dedicated firmware paths:
#   0/32  Bank Select            6/38  NRPN Data Entry MSB/LSB
#   1     Mod Wheel (mod source) 11    Expression (mod source)
#   64    Sustain (voice alloc)  96/97 Data Inc/Dec
#   98/99 NRPN LSB/MSB           100/101 RPN LSB/MSB
#   120–127 Channel Mode (panic must actually panic)
RESERVED_CCS = {0, 1, 6, 11, 32, 38, 64, 96, 97, 98, 99, 100, 101,
                120, 121, 122, 123, 124, 125, 126, 127}

# "int": a stepped whole-number parameter (e.g. seq.steps 1..16). Same wire
# semantics as continuous (norm 0..1 over range min..max, lin curve), but the
# UI displays/steps it as an integer and it binds to an encoder by default.
VALID_TYPES = {"continuous", "select", "toggle", "int"}
VALID_CONTROLS = {"pot", "encoder", "switch"}
VALID_SCOPES = {"patch", "performance", "global"}
VALID_CURVES = {"lin", "log", "seg2"}
VALID_WIDGETS = {"envelope", "grid"}


# -----------------------------------------------------------------------------
# Model loading + validation
# -----------------------------------------------------------------------------

class GenError(Exception):
    """Validation failure — message carries the offending param key."""


def fail(msg):
    raise GenError(msg)


def build_model(path=YAML_PATH):
    """Load params.yaml and validate every rule.  Returns the checked model.

    Kept separate from emission so future emitters (ESP32, JUCE — Phase 5)
    validate identically by reusing this one function."""
    with open(path, encoding="utf-8") as f:
        doc = yaml.safe_load(f)

    for req in ("schema_version", "option_sets", "sections", "params"):
        if req not in doc:
            fail(f"params.yaml missing top-level key '{req}'")

    retired = set(doc.get("retired_ids") or [])
    option_sets = doc["option_sets"]
    sections = {s["index"]: s for s in doc["sections"]}

    # Key -> param, so visible_when can be validated against the parameter it
    # depends on. Built before the validation loop because a rule may reference
    # a parameter declared LATER in the file.
    by_key = {p["key"]: p for p in doc["params"]}
    params = doc["params"]

    seen_ids, seen_keys, seen_names, seen_ccs = {}, set(), set(), {}
    per_section_next = {}  # enforces contiguous, position-derived indices

    for p in params:
        k = p.get("key", "<missing key>")

        # --- identity ---
        pid = p.get("id")
        if not isinstance(pid, int) or not (0 <= pid <= 0x3FFF):
            fail(f"{k}: id must be an int in 0..0x3FFF, got {pid!r}")
        if pid in retired:
            fail(f"{k}: id 0x{pid:04X} is retired and may never be reused")
        if pid in seen_ids:
            fail(f"{k}: id 0x{pid:04X} already used by {seen_ids[pid]}")
        seen_ids[pid] = k

        sec, idx = pid >> 7, pid & 0x7F
        if sec not in sections:
            fail(f"{k}: id section {sec} has no entry in 'sections'")
        expect = per_section_next.get(sec, 0)
        if idx != expect:
            fail(f"{k}: id index {idx} in section {sec} — expected {expect}. "
                 f"Params must appear in index order; append only, never renumber.")
        per_section_next[sec] = idx + 1

        ui = p.get("ui", {})
        if ui.get("section") != sec:
            fail(f"{k}: ui.section {ui.get('section')} disagrees with id section {sec}")
        grp = ui.get("group", 0)
        if grp >= len(sections[sec]["groups"]):
            fail(f"{k}: ui.group {grp} out of range for section {sec}")

        # ui.control — OVERRIDE for the default binding policy.
        #
        # The policy is: continuous -> pot, select -> encoder, toggle -> encoder
        # switch. It is derived from `type`, so the panel layout is generated and
        # cannot drift from this file. An override is for the rare case where
        # ergonomics beat the type rule — e.g. env_pitch.depth is continuous but
        # binds to an encoder so the three envelope pages share a pot layout.
        #
        # Keep overrides RARE. Every one is a place the panel stops being
        # predictable from the parameter's type alone.
        # ---- visible_when -------------------------------------------------
        # Conditional visibility. A parameter that cannot do anything in the
        # current engine state is not DRAWN — but it is NOT disabled: the engine
        # keeps its value and a patch still stores it. Switching filter engine
        # back and forth therefore does not destroy your settings.
        #
        # Validated HARD here, because a rule that names an option which does
        # not exist would silently hide the row FOREVER, and that is
        # indistinguishable from the parameter having been lost.
        vw = p.get("visible_when")
        if vw is not None:
            dep = vw.get("param")
            if dep not in by_key:
                fail(f"{k}: visible_when.param '{dep}' is not a parameter")
            dp = by_key[dep]
            if dp["type"] != "select":
                fail(f"{k}: visible_when.param '{dep}' must be a select "
                     f"(it is {dp['type']})")
            opts = option_sets[dp["options"]]

            want = []
            if "is" in vw:
                want = [vw["is"]]
            elif "in" in vw:
                want = list(vw["in"])
            else:
                fail(f"{k}: visible_when needs an 'is' or an 'in'")

            for w in want:
                if w not in opts:
                    fail(f"{k}: visible_when value '{w}' is not an option of "
                         f"'{dep}' (options: {opts})")

        ctl = ui.get("control")
        if ctl is not None and ctl not in VALID_CONTROLS:
            fail(f"{k}: ui.control '{ctl}' not one of {sorted(VALID_CONTROLS)}")
        if ctl == "switch" and p["type"] != "toggle":
            fail(f"{k}: ui.control 'switch' requires type: toggle")

        if k in seen_keys:
            fail(f"duplicate key '{k}'")
        seen_keys.add(k)
        nm = p.get("name", "")
        if nm in seen_names:
            fail(f"{k}: duplicate name '{nm}'")
        seen_names.add(nm)

        # --- type-specific rules ---
        t = p.get("type")
        if t not in VALID_TYPES:
            fail(f"{k}: type '{t}' not in {sorted(VALID_TYPES)}")
        if p.get("scope") not in VALID_SCOPES:
            fail(f"{k}: scope '{p.get('scope')}' not in {sorted(VALID_SCOPES)}")
        w = p.get("widget")
        if w is not None and w not in VALID_WIDGETS:
            fail(f"{k}: widget '{w}' not in {sorted(VALID_WIDGETS)}")

        if t == "select":
            oref = p.get("options")
            if oref not in option_sets:
                fail(f"{k}: options set '{oref}' not defined in option_sets")
            n = len(option_sets[oref])
            d = p.get("default")
            if not isinstance(d, int) or not (0 <= d < n):
                fail(f"{k}: default {d!r} not a valid index into '{oref}' (0..{n-1})")
        elif t == "toggle":
            if not isinstance(p.get("default"), bool):
                fail(f"{k}: toggle default must be true/false")
        else:  # continuous or int
            r = p.get("range")
            if not r:
                fail(f"{k}: {t} param needs a range")
            lo, hi = float(r["min"]), float(r["max"])
            curve = r.get("curve", "lin")
            if curve not in VALID_CURVES:
                fail(f"{k}: curve '{curve}' not in {sorted(VALID_CURVES)}")
            if not lo < hi:
                fail(f"{k}: range min {lo} must be < max {hi}")
            if curve == "log" and lo <= 0:
                fail(f"{k}: log curve requires min > 0 (got {lo})")
            if t == "int":
                # The display shows min + round(norm*(max-min)); non-integer
                # bounds would put the shown numbers off-lattice from what
                # the engine computes from the same norm.
                if lo != int(lo) or hi != int(hi):
                    fail(f"{k}: int range bounds must be whole numbers")
                if curve != "lin":
                    fail(f"{k}: int params must use a lin curve")
                dflt = float(p.get("default", lo))
                if dflt != int(dflt) or not (lo <= dflt <= hi):
                    fail(f"{k}: int default must be a whole number in range")
            if curve == "seg2":
                mid = r.get("mid")
                if mid is None or not (lo < float(mid) < hi):
                    fail(f"{k}: seg2 curve requires min < mid < max")
            d = float(p.get("default", lo))
            if not (lo <= d <= hi):
                fail(f"{k}: default {d} outside range [{lo}, {hi}]")

        # --- CC binding ---
        cc = p.get("cc")
        if cc is not None:
            if not isinstance(cc, int) or not (0 <= cc <= 127):
                fail(f"{k}: cc must be 0..127, got {cc!r}")
            if cc in RESERVED_CCS:
                fail(f"{k}: cc {cc} is reserved (standard MIDI meaning — brief §5.1)")
            if cc in seen_ccs:
                fail(f"{k}: cc {cc} already bound to {seen_ccs[cc]}")
            seen_ccs[cc] = k

        # --- smoothing ---
        sm = p.get("smooth_ms", 0)
        if not isinstance(sm, int) or not (0 <= sm <= 255):
            fail(f"{k}: smooth_ms must be an int 0..255")

    return doc


# -----------------------------------------------------------------------------
# C++ emission helpers
# -----------------------------------------------------------------------------

def cpp_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def cpp_float(x):
    """Emit a float literal with an 'f' suffix — the whole project compiles
    with -Wdouble-promotion, so bare double literals are a build error."""
    s = f"{float(x):g}"
    if "." not in s and "e" not in s and "inf" not in s and "nan" not in s:
        s += ".0"
    return s + "f"


def ident(key):
    """params.yaml key -> C++ identifier:  filter.cutoff -> FILTER_CUTOFF"""
    return re.sub(r"[.\-]", "_", key).upper()


CURVE_ENUM = {"lin": "Curve::Lin", "log": "Curve::Log", "seg2": "Curve::Seg2"}
TYPE_ENUM = {"continuous": "Type::Continuous", "select": "Type::Select",
             "toggle": "Type::Toggle", "int": "Type::Int"}
CONTROL_ENUM = {"pot": "Control::Pot", "encoder": "Control::Encoder",
                "switch": "Control::Switch"}
SCOPE_ENUM = {"patch": "Scope::Patch", "performance": "Scope::Performance",
              "global": "Scope::Global"}
WIDGET_ENUM = {None: "Widget::Knob", "envelope": "Widget::Envelope",
               "grid": "Widget::Grid"}


def emit_header(doc):
    """Build gen/ParamTable.h as a single string (LF endings)."""
    opt_sets = doc["option_sets"]
    sections = sorted(doc["sections"], key=lambda s: s["index"])
    params = doc["params"]
    L = []
    a = L.append

    a("// =============================================================================")
    a("// ParamTable.h — GENERATED FILE, DO NOT EDIT")
    a("// =============================================================================")
    a("//")
    a("// Generated by tools/gen_params.py from params.yaml.")
    a("// To change a parameter: edit params.yaml, rerun the generator, commit both.")
    a("// CI runs `gen_params.py --check` — hand edits here fail the build.")
    a("//")
    a("// Everything below is constexpr: the entire table lives in FLASH,")
    a("// costs zero RAM, and is available at compile time for static asserts.")
    a("// =============================================================================")
    a("#pragma once")
    a("")
    a("#include <stdint.h>")
    a("#include <stddef.h>")
    a("")
    a("namespace JT {")
    a("namespace Params {")
    a("")
    a(f"inline constexpr uint16_t kSchemaVersion = {doc['schema_version']};")
    a("")

    # ---- enums ----
    a("// --- Behavioural enums ------------------------------------------------------")
    a("enum class Type   : uint8_t { Continuous, Select, Toggle, Int };")
    a("enum class Scope  : uint8_t { Patch, Performance, Global };")
    a("enum class Widget : uint8_t { Knob, Envelope, Grid };  // UI rendering hint")
    a("")
    a("// Curve maps a normalized control position t (0..1) to engineering units:")
    a("//   Lin : min + t*(max-min)")
    a("//   Log : min * pow(max/min, t)              (perceptual freq/time sweeps)")
    a("//   Seg2: two linear segments meeting at 'mid' when t == 0.5")
    a("//         (v1 envelope-slope mapping, Mapping.h cc_to_curve)")
    a("enum class Curve  : uint8_t { Lin, Log, Seg2 };")
    a("")
    a("// Which physical control a parameter binds to.")
    a("//")
    a("// DERIVED from `type` by default:")
    a("//     Continuous -> Pot        (a value you sweep)")
    a("//     Select     -> Encoder    (a detented list)")
    a("//     Toggle     -> Switch     (encoder push)")
    a("//")
    a("// A parameter may override this via `ui.control` in params.yaml when")
    a("// ergonomics beat the type rule — e.g. env_pitch.depth is continuous but")
    a("// binds to an encoder so the three envelope pages share a pot layout.")
    a("// Overrides are deliberately rare: each one is a place the panel stops")
    a("// being predictable from the parameter's type alone.")
    a("enum class Control : uint8_t { Pot = 0, Encoder = 1, Switch = 2 };")
    a("")
    a("// ParamDesc::visIf sentinel for \"no condition, always visible\".")
    a("// MUST NOT be 0: ParamID 0x0000 is osc1.wave, a real parameter that other")
    a("// rows genuinely depend on.")
    a("inline constexpr uint16_t kNoVisDep = 0xFFFF;")
    a("")

    # ---- option string arrays ----
    a("// --- Option sets (display strings for Select params) -------------------------")
    for name, opts in opt_sets.items():
        cname = "kOpt_" + re.sub(r"[^0-9A-Za-z]", "_", name)
        a(f"static constexpr const char* const {cname}[] JT_TABLE_FLASH = {{ "
          + ", ".join(cpp_str(o) for o in opts) + " };")
    a("")

    # ---- section / group names ----
    a("// --- Section & group names (NRPN MSB == section index) -----------------------")
    max_sec = sections[-1]["index"]
    a(f"inline constexpr uint8_t kSectionCount = {max_sec + 1};")
    a("static constexpr const char* const kSectionNames[kSectionCount] JT_TABLE_FLASH = {")
    by_index = {s["index"]: s for s in sections}
    for i in range(max_sec + 1):
        nm = by_index[i]["name"] if i in by_index else ""
        a(f"    {cpp_str(nm)},")
    a("};")
    a("")
    max_groups = max(len(s["groups"]) for s in sections)
    a(f"inline constexpr uint8_t kMaxGroups = {max_groups};")
    a("inline constexpr const char* kGroupNames[kSectionCount][kMaxGroups] = {")
    for i in range(max_sec + 1):
        gs = by_index[i]["groups"] if i in by_index else []
        cells = ", ".join(cpp_str(g) for g in gs)
        pad = ", ".join(["nullptr"] * (max_groups - len(gs)))
        row = ", ".join(x for x in (cells, pad) if x)
        a(f"    {{ {row} }},")
    a("};")
    a("")

    # ---- ParamDesc ----
    a("// --- One row per parameter ----------------------------------------------------")
    a("struct ParamDesc {")
    a("    uint16_t    id;          // permanent 14-bit ParamID == NRPN number")
    a("    const char* key;         // dotted identifier, e.g. \"filter.cutoff\"")
    a("    const char* name;        // full display name")
    a("    const char* label;       // short panel label")
    a("    Type        type;")
    a("    Scope       scope;       // who owns / saves it (patch vs perf vs global)")
    a("    Widget      widget;      // UI rendering hint (knob / envelope / grid)")
    a("    Curve       curve;       // normalized→engineering mapping")
    a("    float       min;         // engineering range")
    a("    float       mid;         // Seg2 midpoint value (unused otherwise: 0)")
    a("    float       max;")
    a("    float       def;         // default in engineering units;")
    a("                             // Select: option index, Toggle: 0/1")
    a("    const char* unit;        // display unit; \"norm\" = engine owns scaling")
    a("    bool        bipolarUi;   // display centred (does not change the math)")
    a("    uint8_t     smoothMs;    // audio-plane slew class: 0 step / 5 fast / 20 slow")
    a("    int16_t     cc;          // curated performance CC, -1 = NRPN only")
    a("    uint8_t     optionCount; // Select only")
    a("    const char* const* options;")
    a("    uint8_t     section;     // == id >> 7   (kept explicit for UI loops)")
    a("    uint8_t     group;")
    a("    Control     control;     // which physical control this binds to")
    a("")
    a("    // ---- conditional visibility ----------------------------------------")
    a("    // A parameter that cannot do anything in the current engine state is")
    a("    // not DRAWN. It is NOT disabled: the engine keeps its value and a")
    a("    // patch still stores it, so toggling filter engine (say) does not")
    a("    // destroy your OBXa settings.")
    a("    //")
    a("    // visIf == kNoVisDep means ALWAYS VISIBLE. Otherwise the row is shown")
    a("    // only when parameter `visIf` currently holds one of the `visCount`")
    a("    // option indices in `visOpts`.")
    a("    //")
    a("    // The sentinel is 0xFFFF, NOT 0 -- 0x0000 is a REAL ParamID")
    a("    // (osc1.wave, the first row in the table). Using 0 as the sentinel")
    a("    // silently disabled every rule that depended on it.")
    a("    uint16_t       visIf;      // ParamID this depends on (0 = always show)")
    a("    const uint8_t* visOpts;    // option indices that make it visible")
    a("    uint8_t        visCount;")
    a("};")
    a("")

    # ---- ID constants ----
    a("// --- ParamID constants (use these, never raw numbers) -------------------------")
    a("namespace ID {")
    for p in params:
        a(f"inline constexpr uint16_t {ident(p['key']):<28} = 0x{p['id']:04X};")
    a("} // namespace ID")
    a("")

    # ---- visible_when option-index arrays ------------------------------------
    # Emitted as INDICES, not strings: the runtime compares against the option
    # index it already has, so no string compare in the render loop.
    # Local key index — build_model()'s is out of scope here.
    by_key = {p["key"]: p for p in params}

    a("// visible_when: which option indices of the dependency make a row visible.")
    for p in params:
        vw = p.get("visible_when")
        if not vw:
            continue
        dep = by_key[vw["param"]]
        opts = opt_sets[dep["options"]]
        want = [vw["is"]] if "is" in vw else list(vw["in"])
        idx = [opts.index(w) for w in want]
        a(f"static constexpr uint8_t kVis_{ident(p['key'])}[] JT_TABLE_FLASH = "
          f"{{ {', '.join(str(i) for i in idx)} }};   "
          f"// {dep['key']} in {want}")
    a("")

    # ---- the table ----
    a("// --- The table -----------------------------------------------------------------")
    a("static constexpr ParamDesc kParams[] JT_TABLE_FLASH = {")
    cur_sec = -1
    for p in params:
        sec = p["id"] >> 7
        if sec != cur_sec:
            cur_sec = sec
            a(f"    // ---- [{sec}] {by_index[sec]['name']} ----")
        t = p["type"]
        if t == "select":
            oref = p["options"]
            n = len(opt_sets[oref])
            lo, mid, hi = 0.0, 0.0, float(n - 1)
            d = float(p["default"])
            curve, unit = "lin", "index"
            optc = n
            opta = "kOpt_" + re.sub(r"[^0-9A-Za-z]", "_", oref)
        elif t == "toggle":
            lo, mid, hi = 0.0, 0.0, 1.0
            d = 1.0 if p["default"] else 0.0
            curve, unit = "lin", "bool"
            optc, opta = 0, "nullptr"
        else:
            r = p["range"]
            lo, hi = float(r["min"]), float(r["max"])
            mid = float(r.get("mid", 0.0))
            d = float(p["default"])
            curve = r.get("curve", "lin")
            unit = p.get("unit", "norm")
            optc, opta = 0, "nullptr"

        cc = p.get("cc", -1)

        # ---- Binding policy: derive the physical control from the type -------
        # continuous -> Pot | select -> Encoder | toggle -> Switch.
        # ui.control overrides it (see the validation block for when that is
        # justified). This is what makes the panel layout a FUNCTION of this
        # file rather than a hand-maintained table that can drift from it.
        ctl_override = p["ui"].get("control")
        if ctl_override:
            ctl = CONTROL_ENUM[ctl_override]
        else:
            ctl = CONTROL_ENUM[{"continuous": "pot",
                                "select":     "encoder",
                                "toggle":     "switch",
                                # stepped whole numbers belong on a detented
                                # control, not a pot
                                "int":        "encoder"}[t]]

        # visible_when -> (depId, optionIndexArray, count)
        vw = p.get("visible_when")
        if vw:
            dep = by_key[vw["param"]]
            want = [vw["is"]] if "is" in vw else list(vw["in"])
            vis = (f"ID::{ident(dep['key'])}, kVis_{ident(p['key'])}, "
                   f"{len(want)}")
        else:
            vis = "kNoVisDep, nullptr, 0"

        row = (f"    {{ ID::{ident(p['key'])}, {cpp_str(p['key'])}, "
               f"{cpp_str(p['name'])}, {cpp_str(p['label'])}, "
               f"{TYPE_ENUM[t]}, {SCOPE_ENUM[p['scope']]}, "
               f"{WIDGET_ENUM[p.get('widget')]}, {CURVE_ENUM[curve]}, "
               f"{cpp_float(lo)}, {cpp_float(mid)}, {cpp_float(hi)}, {cpp_float(d)}, "
               f"{cpp_str(unit)}, {'true' if p.get('bipolar_display') else 'false'}, "
               f"{p.get('smooth_ms', 0)}, {cc}, {optc}, {opta}, "
               f"{sec}, {p['ui'].get('group', 0)}, {ctl}, {vis} }},")
        a(row)
    a("};")
    a("")
    a("inline constexpr size_t kParamCount = sizeof(kParams) / sizeof(kParams[0]);")
    a("")

    # ---- lookup helpers ----
    a("// --- Lookups -------------------------------------------------------------------")
    a("// Linear scans are constexpr-friendly and fine at control rate (UI, MIDI");
    a("// dispatch).  NOTHING in the audio ISR looks parameters up by id — the")
    a("// engine caches direct indices at boot (ParameterStore, Phase 1).")
    a("")
    a("inline constexpr const ParamDesc* find(uint16_t id) {")
    a("    for (size_t i = 0; i < kParamCount; ++i)")
    a("        if (kParams[i].id == id) return &kParams[i];")
    a("    return nullptr;")
    a("}")
    a("")
    a("inline constexpr const ParamDesc* findByCC(uint8_t cc) {")
    a("    for (size_t i = 0; i < kParamCount; ++i)")
    a("        if (kParams[i].cc == static_cast<int16_t>(cc)) return &kParams[i];")
    a("    return nullptr;")
    a("}")
    a("")
    a("// NRPN number == ParamID by construction (brief §5.2).")
    a("inline constexpr uint8_t nrpnMsb(uint16_t id) { return static_cast<uint8_t>((id >> 7) & 0x7F); }")
    a("inline constexpr uint8_t nrpnLsb(uint16_t id) { return static_cast<uint8_t>(id & 0x7F); }")
    a("inline constexpr uint16_t idFromNrpn(uint8_t msb, uint8_t lsb) {")
    a("    return static_cast<uint16_t>((static_cast<uint16_t>(msb & 0x7F) << 7) | (lsb & 0x7F));")
    a("}")
    a("")
    a("// Compile-time guarantees — a bad regeneration cannot even compile.")
    a(f"static_assert(kParamCount == {len(params)}, \"param count changed — regenerate consumers\");")
    a("// Dereferencing in a constant expression: if find() ever returned nullptr")
    a("// this line would be a hard compile error — stronger than a null compare,")
    a("// and clean under -Waddress (GCC can prove the pointer is never null).")
    a("static_assert(find(ID::FILTER_CUTOFF)->id == ID::FILTER_CUTOFF, \"table self-check failed\");")
    a("")
    a("} // namespace Params")
    a("} // namespace JT")
    a("")
    return "\n".join(L)


# -----------------------------------------------------------------------------


# -----------------------------------------------------------------------------
# Flash string pool (Teensy 4.x memory rule).
#
# On the IMXRT1062 the linker copies .rodata — including every string
# LITERAL — into DTCM, the scarcest RAM.  Attributing the arrays alone
# does not move the literals they point at.  This pass therefore:
#   1. collects every string literal appearing in table rows, option
#      arrays and the section-name array,
#   2. emits each unique one as a named char array carrying the
#      JT_TABLE_FLASH attribute,
#   3. rewrites the rows/arrays to reference the pool.
# Only data-emitting lines are touched — literals inside comments and
# code examples stay verbatim.  On non-Teensy builds the attribute is
# empty and the pool is ordinary const data: zero behavioural change.
# -----------------------------------------------------------------------------
_STR_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')

def pool_strings(text):
    data_line = re.compile(
        r'^(    \{ ID::|inline constexpr const char\* const kOpt_'
        r'|inline constexpr const char\* const kSectionNames'
        r'|    "?)')
    lines = text.split("\n")

    # Which lines carry table data?  Rows, kOpt_ arrays, kSectionNames and
    # its continuation lines (entries indented under the array opener).
    in_secnames = False
    targets = []
    for i, ln in enumerate(lines):
        if ln.startswith("inline constexpr const char* const kSectionNames"):
            in_secnames = True
            targets.append(i)
            continue
        if in_secnames:
            targets.append(i)
            if "};" in ln:
                in_secnames = False
            continue
        if ln.startswith("    { ID::") or \
           ln.startswith("inline constexpr const char* const kOpt_"):
            targets.append(i)

    pool = {}                     # literal (with quotes) -> identifier
    def intern(m):
        lit = m.group(0)
        if lit not in pool:
            pool[lit] = f"s{len(pool)}"
        return "Str::" + pool[lit]

    for i in targets:
        lines[i] = _STR_RE.sub(intern, lines[i])

    decls = ["// --- Flash string pool (see the pass comment in gen_params.py) ----------------",
             "namespace Str {"]
    for lit, ident_ in pool.items():
        decls.append(f"static constexpr char {ident_}[] JT_TABLE_FLASH = {lit};")
    decls.append("} // namespace Str")
    decls.append("")

    macro = [
        "// -------------------------------------------------------------------------",
        "// FLASH PLACEMENT (Teensy 4.x): const/.rodata data — string literals",
        "// included — is COPIED INTO DTCM by default on the IMXRT1062.  RAM1 is",
        "// the scarce resource; this table and its strings cost tens of KB.",
        "// JT_TABLE_FLASH pins them in flash, which is directly addressable and",
        "// cached (no AVR pgm_read needed).  Control-plane lookups only.",
        "// LINKAGE: `static` (not `inline`) ON PURPOSE — inline variables are",
        "// COMDAT symbols and GCC refuses COMDAT + plain data in one named",
        "// section (a real linker error found this).  Each including .cpp",
        "// carries its own flash copy (~25 KB x ~6 TUs); flash is 7.75 MB and",
        "// DTCM is the resource being defended, so the trade is deliberate.",
        "// Self-contained guard: this header is consumed by three codebases.",
        "// -------------------------------------------------------------------------",
        "#if defined(__IMXRT1062__)",
        '#define JT_TABLE_FLASH __attribute__((section(".progmem")))',
        "#else",
        "#define JT_TABLE_FLASH",
        "#endif",
        "",
    ]

    # Insert macro + pool immediately before the option-set section (the
    # first consumer of both).
    for i, ln in enumerate(lines):
        if ln.startswith("// --- Option sets"):
            return "\n".join(lines[:i] + macro + decls + lines[i:])
    raise SystemExit("pool_strings: option-set anchor not found")

# Documentation emission — the MIDI implementation chart writes itself.
# -----------------------------------------------------------------------------

def fmt_range(p):
    t = p["type"]
    if t == "select":
        return f"0–{p['_optcount'] - 1} (index)"
    if t == "toggle":
        return "off / on"
    r = p["range"]
    unit = p.get("unit", "norm")
    curve = r.get("curve", "lin")
    return f"{r['min']:g}–{r['max']:g} {unit} ({curve})"


def fmt_default(p):
    t = p["type"]
    if t == "select":
        return f"{p['default']} ({p['_optnames'][p['default']]})"
    if t == "toggle":
        return "on" if p["default"] else "off"
    return f"{float(p['default']):g} {p.get('unit', '')}".strip()


def emit_doc(doc):
    opt_sets = doc["option_sets"]
    sections = sorted(doc["sections"], key=lambda s: s["index"])
    by_index = {s["index"]: s for s in sections}
    L = []
    a = L.append
    a("# JT-8000 v2 — MIDI Implementation Chart & Parameter Map")
    a("")
    a("*Generated by `tools/gen_params.py` from `params.yaml` — do not edit.*")
    a("")
    a("Every parameter is addressable by **NRPN** (CC 99 = MSB, CC 98 = LSB,")
    a("CC 6 = Data Entry MSB, CC 38 = Data Entry LSB).  NRPN number = ParamID;")
    a("MSB is the section index, LSB the index within the section.  Data Entry")
    a("MSB alone gives a 7-bit coarse set; MSB+LSB gives 14-bit resolution.")
    a("Select/toggle parameters take **direct indices** as data.")
    a("")
    a("A curated set of standard CCs mirrors key performance parameters (the")
    a("**CC** column).  Reserved CCs keep their standard MIDI meaning:")
    a("1 mod wheel, 5/65 portamento, 7 volume, 11 expression, 64 sustain,")
    a("98/99/6/38/96/97/100/101 (N)RPN machinery, 120–127 channel mode.")
    a("")
    total = len(doc["params"])
    a(f"**Schema version {doc['schema_version']} — {total} parameters.**")
    a("")
    for sec in sections:
        i = sec["index"]
        rows = [p for p in doc["params"] if (p["id"] >> 7) == i]
        if not rows:
            continue
        a(f"## [{i}] {sec['name']}  *(NRPN MSB {i})*")
        a("")
        a("| ParamID | LSB | Name | Key | Type | Range | Default | Smooth | CC | Scope |")
        a("|---|---|---|---|---|---|---|---|---|---|")
        for p in rows:
            if p["type"] == "select":
                p["_optnames"] = opt_sets[p["options"]]
                p["_optcount"] = len(p["_optnames"])
            cc = p.get("cc")
            a(f"| `0x{p['id']:04X}` | {p['id'] & 0x7F} | {p['name']} | `{p['key']}` "
              f"| {p['type']} | {fmt_range(p)} | {fmt_default(p)} "
              f"| {p.get('smooth_ms', 0)} ms | {cc if cc is not None else '—'} "
              f"| {p['scope']} |")
        a("")
    a("### Option sets")
    a("")
    for name, opts in opt_sets.items():
        a(f"- **{name}**: " + ", ".join(f"`{o}`" for o in opts))
    a("")
    return "\n".join(L)


# -----------------------------------------------------------------------------
# Entry point — regenerate or --check
# -----------------------------------------------------------------------------

def write_if_changed(path, content, check_only):
    """In --check mode compare byte-for-byte; otherwise write atomically."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    old = None
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8", newline="") as f:
            old = f.read()
    # Compare with line endings normalised: files are committed CRLF (project
    # standard, so one byte-identical ParamTable.h serves all three repos),
    # while `content` is assembled with plain newlines in memory.
    if old is not None:
        old = old.replace("\r\n", "\n")
    if check_only:
        if old != content:
            print(f"STALE: {os.path.relpath(path, ROOT)} does not match params.yaml "
                  f"— run tools/gen_params.py and commit the result.")
            return False
        print(f"ok: {os.path.relpath(path, ROOT)}")
        return True
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\r\n") as f:
        f.write(content)
    os.replace(tmp, path)  # atomic on POSIX & Windows — no half-written files
    print(f"wrote {os.path.relpath(path, ROOT)} "
          f"({'unchanged' if old == content else 'updated'})")
    return True


def main():
    ap = argparse.ArgumentParser(description="JT-8000 v2 parameter generator")
    ap.add_argument("--check", action="store_true",
                    help="verify generated files match params.yaml (CI gate)")
    args = ap.parse_args()

    try:
        doc = build_model()
    except GenError as e:
        sys.exit(f"params.yaml INVALID: {e}")

    header = emit_header(doc)
    docmd = emit_doc(doc)

    ok = True
    header = pool_strings(header)
    ok &= write_if_changed(HEADER_PATH, header, args.check)
    ok &= write_if_changed(DOC_PATH, docmd, args.check)
    if not ok:
        sys.exit(1)
    print(f"validated {len(doc['params'])} parameters, "
          f"{len(doc['option_sets'])} option sets, "
          f"{len(doc['sections'])} sections.")


if __name__ == "__main__":
    main()
