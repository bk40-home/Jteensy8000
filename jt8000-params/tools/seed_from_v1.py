#!/usr/bin/env python3
# =============================================================================
# seed_from_v1.py — ONE-OFF provenance tool for JT-8000 v2 Phase 0
# =============================================================================
#
# WHY THIS EXISTS
#   The v2 parameter definition file (params.yaml) must contain every one of
#   the 139 parameters in the v1 firmware.  Hand-transcribing 139 rows invites
#   silent omissions, so instead this script PARSES the v1 ParamDefs.h source
#   and emits params.yaml mechanically.  Anything it cannot derive from v1
#   (engineering ranges, curves, units, smoothing classes) comes from the
#   ENRICH table below, which encodes the curves found in v1 Mapping.h.
#
#   After the seed is generated and reviewed, this script is RETIRED.
#   params.yaml becomes the single source of truth; gen_params.py is the
#   ongoing tool.  Keep this file in tools/ purely as provenance — a record
#   of exactly how the v2 IDs were derived from v1.
#
# WHAT IT DOES
#   1. Parse v1 CC constants        (namespace CC { ... })
#   2. Parse v1 option string arrays (kXxxOptions[])
#   3. Parse v1 section/group names
#   4. Parse the v1 kParams[] master table (139 rows)
#   5. Assign PERMANENT v2 ParamIDs:  id = (section << 7) | index
#      where index = order of appearance within the section.
#      *** IDs assigned here are frozen forever (brief §4.2). ***
#   6. Apply the ENRICH table (ranges/curves/units from v1 Mapping.h)
#   7. Apply the curated performance-CC set (brief §5.1) — all other v1
#      CC numbers are dropped; those params become NRPN-only.
#   8. Emit params.yaml with the schema documented in gen_params.py.
#
# USAGE
#   python3 seed_from_v1.py <path-to-v1-ParamDefs.h> <output params.yaml>
# =============================================================================

import re
import sys
import math

# -----------------------------------------------------------------------------
# Curve helpers — mirror v1 Mapping.h so seeded DEFAULTS are converted from
# v1 CC defaults into engineering units using the *same* math the firmware
# used.  This preserves the v1 init-patch sound exactly.
# -----------------------------------------------------------------------------

def cc_to_lin(cc, lo, hi):
    """v1 cc_to_norm() then linear scale — Mapping.h line 61."""
    return lo + (min(cc, 127) / 127.0) * (hi - lo)

def cc_to_log(cc, lo, hi):
    """v1 exponential mapping, e.g. cc_to_cutoff_hz / cc_to_time_ms."""
    t = min(cc, 127) / 127.0
    return lo * math.pow(hi / lo, t)

def cc_to_seg2(cc, lo, mid, hi):
    """v1 cc_to_curve(): two linear segments, value `mid` at CC 64."""
    if cc <= 64:
        return lo + (cc / 64.0) * (mid - lo)
    return mid + ((cc - 64) / 63.0) * (hi - mid)

# -----------------------------------------------------------------------------
# ENRICH — engineering ranges/curves/units per v1 CC constant name.
# Sources are cited per entry.  Anything absent falls back to normalized
# 0..1 (or -1..+1 when the v1 row is flagged bipolar); the exact engine
# scaling for those is confirmed one-by-one during the Phase 2 DSP port,
# guaranteeing functional parity (the engine owns final scaling, as in v1).
#
# Fields: lo, hi, curve ('lin'|'log'|'seg2'), unit, smooth (ms), [mid]
# -----------------------------------------------------------------------------

ENV_TIME = dict(lo=1.0, hi=11880.0, curve="log", unit="ms", smooth=0)      # Mapping.h msMin/msMax
ENV_SLOPE = dict(lo=0.15, hi=5.0, mid=1.0, curve="seg2", unit="slope", smooth=0)  # Mapping.h cc_to_curve
LFO_HZ = dict(lo=0.03, hi=39.0, curve="log", unit="Hz", smooth=0)          # Mapping.h cc_to_lfo_hz (0.03*1300^t)

ENRICH = {
    # --- Filter core (Mapping.h cc_to_cutoff_hz / resonance) ---
    "FILTER_CUTOFF":       dict(lo=20.0, hi=20000.0, curve="log", unit="Hz", smooth=5),
    "FILTER_RESONANCE":    dict(lo=0.0, hi=1.0, curve="lin", unit="norm", smooth=5),
    # NOTE: OBXa engine clamps its own resonance to 0.97 internally
    # (Mapping.h OBXA_RES_MAX) — that is an ENGINE limit, not a param limit.

    # --- Envelope times (Mapping.h cc_to_time_ms) ---
    "AMP_ATTACK": ENV_TIME, "AMP_DECAY": ENV_TIME, "AMP_RELEASE": ENV_TIME,
    "FILTER_ENV_ATTACK": ENV_TIME, "FILTER_ENV_DECAY": ENV_TIME, "FILTER_ENV_RELEASE": ENV_TIME,
    "PITCH_ENV_ATTACK": ENV_TIME, "PITCH_ENV_DECAY": ENV_TIME, "PITCH_ENV_RELEASE": ENV_TIME,

    # --- Envelope slopes (Mapping.h cc_to_curve) ---
    "AMP_ATTACK_CURVE": ENV_SLOPE, "AMP_DECAY_CURVE": ENV_SLOPE, "AMP_RELEASE_CURVE": ENV_SLOPE,
    "FILTER_ATTACK_CURVE": ENV_SLOPE, "FILTER_DECAY_CURVE": ENV_SLOPE, "FILTER_RELEASE_CURVE": ENV_SLOPE,
    "PITCH_ATTACK_CURVE": ENV_SLOPE, "PITCH_DECAY_CURVE": ENV_SLOPE, "PITCH_RELEASE_CURVE": ENV_SLOPE,

    # --- LFO rates (Mapping.h cc_to_lfo_hz) ---
    "LFO1_FREQ": LFO_HZ, "LFO2_FREQ": LFO_HZ,

    # --- Clock (BPMClockManager.h "40-300 range") ---
    "BPM_TEMPO":           dict(lo=40.0, hi=300.0, curve="lin", unit="BPM", smooth=0),

    # --- Performance (SynthEngine.cpp CC 0..127 → 0..24 semitones) ---
    "PITCH_BEND_RANGE":    dict(lo=0.0, hi=24.0, curve="lin", unit="st", smooth=0),
    "PERF_SPLIT_NOTE":     dict(lo=0.0, hi=127.0, curve="lin", unit="note", smooth=0),
}

# Params where per-block smoothing must be OFF even though they are continuous
# (stepped/integer-like values where slewing would smear discrete jumps).
NO_SMOOTH = {
    "PERF_MIDI_CHANNEL_A", "PERF_MIDI_CHANNEL_B", "PERF_VOICE_SPLIT",
    "SEQ_STEPS", "SEQ_STEP_SELECT", "SEQ_STEP_VALUE",
    "OSC1_ARB_BANK", "OSC1_ARB_INDEX", "OSC2_ARB_BANK", "OSC2_ARB_INDEX",
}

# -----------------------------------------------------------------------------
# Curated performance CCs (brief §5.1, signed off).  ALL other v1 CC numbers
# are dropped — those parameters become NRPN-only.  Standard meanings only:
#   74 Brightness→Cutoff, 71 Harmonic Content→Resonance,
#   5 Portamento Time→Glide Time, 65 Portamento On/Off→Glide Enable.
# -----------------------------------------------------------------------------
CURATED_CC = {
    "FILTER_CUTOFF":    74,
    "FILTER_RESONANCE": 71,
    "GLIDE_TIME":       5,
    "GLIDE_ENABLE":     65,
}

# -----------------------------------------------------------------------------
# Key naming — deterministic conversion of v1 CC constant → dotted v2 key.
# Overrides handle names whose prefix rules would read badly; everything
# else falls through the PREFIX_RULES.  Keys are for humans; ParamIDs rule.
# -----------------------------------------------------------------------------

KEY_OVERRIDES = {
    "OSC_MIX_BALANCE": "mix.balance",      "OSC1_MIX": "mix.osc1",
    "OSC2_MIX": "mix.osc2",                "SUB_MIX": "mix.sub",
    "NOISE_MIX": "mix.noise",              "OSC_CROSS_MOD_DEPTH": "mix.cross_mod",
    "OSC_SYNC_ENABLE": "mix.osc_sync",
    "RING1_MIX": "osc1.ring_mix",          "RING2_MIX": "osc2.ring_mix",
    # Envelopes — group by envelope, not by the FILTER_/AMP_/PITCH_ prefix soup
    "AMP_ATTACK": "env_amp.attack",        "AMP_DECAY": "env_amp.decay",
    "AMP_SUSTAIN": "env_amp.sustain",      "AMP_RELEASE": "env_amp.release",
    "AMP_ATTACK_CURVE": "env_amp.attack_curve",
    "AMP_DECAY_CURVE": "env_amp.decay_curve",
    "AMP_RELEASE_CURVE": "env_amp.release_curve",
    "FILTER_ENV_ATTACK": "env_filter.attack",   "FILTER_ENV_DECAY": "env_filter.decay",
    "FILTER_ENV_SUSTAIN": "env_filter.sustain", "FILTER_ENV_RELEASE": "env_filter.release",
    "FILTER_ATTACK_CURVE": "env_filter.attack_curve",
    "FILTER_DECAY_CURVE": "env_filter.decay_curve",
    "FILTER_RELEASE_CURVE": "env_filter.release_curve",
    "PITCH_ENV_ATTACK": "env_pitch.attack",     "PITCH_ENV_DECAY": "env_pitch.decay",
    "PITCH_ENV_SUSTAIN": "env_pitch.sustain",   "PITCH_ENV_RELEASE": "env_pitch.release",
    "PITCH_ENV_DEPTH": "env_pitch.depth",
    "PITCH_ATTACK_CURVE": "env_pitch.attack_curve",
    "PITCH_DECAY_CURVE": "env_pitch.decay_curve",
    "PITCH_RELEASE_CURVE": "env_pitch.release_curve",
    # Voice / performance section 11
    "POLY_MODE": "voice.poly_mode",        "UNISON_DETUNE": "voice.unison_detune",
    "PITCH_BEND_RANGE": "voice.bend_range","AMP_MOD_FIXED_LEVEL": "voice.amp_level",
    "VA_FILTER_TYPE": "filter.va_type",
}

PREFIX_RULES = [  # (v1 prefix, v2 dotted prefix) — first match wins
    ("FX_REVERB_", "reverb."), ("FX_JPFX_DELAY_", "fx.delay_"),
    ("FX_MOD_", "fx.mod_"), ("FX_DELAY_", "fx.delay_"), ("FX_", "fx."),
    ("SUPERSAW1_", "osc1.supersaw_"), ("SUPERSAW2_", "osc2.supersaw_"),
    ("OSC1_", "osc1."), ("OSC2_", "osc2."),
    ("FILTER_OBXA_", "filter.obxa_"), ("FILTER_", "filter."),
    ("LFO1_", "lfo1."), ("LFO2_", "lfo2."),
    ("SEQ_", "seq."), ("BPM_", "clock."), ("VELOCITY_", "velocity."),
    ("GLIDE_", "glide."), ("PERF_", "perf."),
]

def make_key(const_name: str) -> str:
    if const_name in KEY_OVERRIDES:
        return KEY_OVERRIDES[const_name]
    for pre, rep in PREFIX_RULES:
        if const_name.startswith(pre):
            return rep + const_name[len(pre):].lower()
    return const_name.lower().replace("_", ".", 1)

# -----------------------------------------------------------------------------
# v1 header parsing
# -----------------------------------------------------------------------------

def parse_v1(src: str):
    # 1) CC constants:  static constexpr uint8_t NAME = N;
    cc_consts = dict(re.findall(
        r"static constexpr uint8_t\s+(\w+)\s*=\s*(\d+);", src))
    cc_consts = {k: int(v) for k, v in cc_consts.items()}

    # 2) Option arrays: kXxxOptions[] = { "a", "b", ... };
    option_sets = {}
    for name, body in re.findall(
            r'static constexpr const char\*\s+(k\w+Options)\[\]\s*=\s*\{(.*?)\};',
            src, re.S):
        option_sets[name] = re.findall(r'"([^"]*)"', body)

    # 3) Section names (16 entries, in order)
    sec_block = re.search(
        r'kSectionNames\[NUM_SECTIONS\]\s*=\s*\{(.*?)\};', src, re.S).group(1)
    sections = re.findall(r'"([^"]*)"', sec_block)

    # 4) Group names — one row per section: { "a", "b", nullptr },
    grp_block = re.search(
        r'kGroupNames\[\]\[8\]\s*=\s*\{(.*?)\n\};', src, re.S).group(1)
    groups = []
    for row in re.findall(r'\{([^{}]*)\}', grp_block):
        groups.append(re.findall(r'"([^"]*)"', row))

    # 5) kParams rows
    row_re = re.compile(
        r'\{\s*CC::(\w+)\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*(\w+)\s*,\s*(\w+)\s*,'
        r'\s*(\d+)\s*,\s*(true|false)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}')
    rows = []
    for m in row_re.finditer(src):
        rows.append(dict(
            const=m.group(1), name=m.group(2), label=m.group(3),
            vtype=m.group(4), scope=m.group(5), cc_default=int(m.group(6)),
            bipolar=(m.group(7) == "true"), options=m.group(9),
            section=int(m.group(10)), group=int(m.group(11)),
        ))
    return cc_consts, option_sets, sections, groups, rows

# -----------------------------------------------------------------------------
# YAML emission — hand-formatted (not yaml.dump) so the file carries comments
# and a stable field order that humans can review and git can diff cleanly.
# -----------------------------------------------------------------------------

def yq(s):  # minimal YAML string quoting
    return '"' + s.replace('"', '\\"') + '"'

def emit(cc_consts, option_sets, sections, groups, rows, out_path):
    lines = []
    a = lines.append

    a("# =============================================================================")
    a("# params.yaml — JT-8000 v2 SINGLE SOURCE OF TRUTH for all parameters")
    a("# =============================================================================")
    a("#")
    a("# Seeded mechanically from v1 ParamDefs.h by tools/seed_from_v1.py.")
    a("# From this point on, THIS FILE is authoritative.  Firmware, ESP32")
    a("# controller and JUCE editor tables are GENERATED from it by")
    a("# tools/gen_params.py — never hand-edit the generated files.")
    a("#")
    a("# RULES (design brief v1.0 §4.2):")
    a("#   * ParamIDs are PERMANENT.  id = (section << 7) | index, 14-bit.")
    a("#     NRPN number == ParamID (MSB = section, LSB = index).")
    a("#     Never renumber.  Deleting a param: move its id to retired_ids.")
    a("#   * New params append at the END of their section (next free index).")
    a("#   * 'cc' bindings come only from the curated performance set (§5.1).")
    a("#   * range.curve: lin | log | seg2   (log needs min > 0;")
    a("#     seg2 is two linear segments meeting at 'mid' when the control")
    a("#     is at its physical centre — used for v1's envelope slopes).")
    a("#   * unit 'norm' means the engine owns final scaling (v1 parity is")
    a("#     confirmed per-param during the Phase 2 DSP port).")
    a("#   * smooth_ms is the audio-plane slew class: 0 = step, 5 = fast, 20 = slow.")
    a("")
    a("schema_version: 1")
    a("")
    a("retired_ids: []          # ParamIDs may enter this list; they never leave")
    a("")

    # ---- option sets ----
    a("# -----------------------------------------------------------------------------")
    a("# Option sets — canonical display strings for select parameters.")
    a("# Adding an option APPENDS (indices are stored in patches).")
    a("# -----------------------------------------------------------------------------")
    a("option_sets:")
    name_map = {}
    for v1name, opts in option_sets.items():
        # kOscWaveOptions -> osc_wave
        short = re.sub(r'^k|Options$', '', v1name)
        short = re.sub(r'(?<!^)(?=[A-Z])', '_', short).lower()
        name_map[v1name] = short
        a(f"  {short}: [" + ", ".join(yq(o) for o in opts) + "]")
    a("")

    # ---- sections ----
    a("# -----------------------------------------------------------------------------")
    a("# Sections — index IS the NRPN MSB.  Order is frozen (from v1).")
    a("# Section 16 MASTER is new in v2 (holds the CC-7 master volume).")
    a("# -----------------------------------------------------------------------------")
    a("sections:")
    for i, s in enumerate(sections):
        gnames = ", ".join(yq(g) if g else '"Main"' for g in groups[i]) or '"Main"'
        a(f"  - {{index: {i}, name: {yq(s)}, groups: [{gnames}]}}")
    a(f'  - {{index: 16, name: "Master", groups: ["Output"]}}')
    a("")

    # ---- parameters ----
    a("# -----------------------------------------------------------------------------")
    a("# Parameters — 139 seeded from v1 + 1 new (master.volume).")
    a("# Defaults were converted from v1 CC defaults through the SAME curves the")
    a("# v1 firmware used (Mapping.h), so the init patch is bit-identical in intent.")
    a("# -----------------------------------------------------------------------------")
    a("params:")

    next_index = {}          # per-section running index -> permanent ParamID
    scope_map = {"PATCH": "patch", "PERF": "performance",
                 "GLOBAL_FX": "global", "INTERNAL": "performance"}
    type_map = {"CONTINUOUS": "continuous", "SELECT": "select",
                "TOGGLE": "toggle", "ENVELOPE": "continuous", "GRID": "continuous"}
    widget_map = {"ENVELOPE": "envelope", "GRID": "grid"}

    cur_section = -1
    for r in rows:
        sec = r["section"]
        if sec != cur_section:
            cur_section = sec
            a("")
            a(f"  # ---- [{sec}] {sections[sec]} ----")
        idx = next_index.get(sec, 0)
        next_index[sec] = idx + 1
        pid = (sec << 7) | idx

        # engineering range from ENRICH, else normalized fallback
        e = ENRICH.get(r["const"])
        if e:
            lo, hi, curve, unit = e["lo"], e["hi"], e["curve"], e["unit"]
            mid = e.get("mid")
            smooth = e["smooth"]
        else:
            lo, hi = (-1.0, 1.0) if r["bipolar"] else (0.0, 1.0)
            curve, unit, mid = "lin", "norm", None
            smooth = 0 if (r["vtype"] != "CONTINUOUS"
                           or r["const"] in NO_SMOOTH) else 5
        if r["const"] in NO_SMOOTH:
            smooth = 0

        vtype = type_map[r["vtype"]]
        widget = widget_map.get(r["vtype"])

        # default: convert the v1 CC default through the matching curve
        ccd = r["cc_default"]
        if vtype == "select":
            opts = option_sets[r["options"]]
            default = (ccd * len(opts)) // 128          # v1 bucket decode
            default_s = str(default)
        elif vtype == "toggle":
            default_s = "true" if ccd >= 64 else "false"
        else:
            if curve == "log":
                d = cc_to_log(ccd, lo, hi)
            elif curve == "seg2":
                d = cc_to_seg2(ccd, lo, mid, hi)
            else:
                d = cc_to_lin(ccd, lo, hi)
            # v1 quirk: "centre" was CC 64, which is NOT the midpoint of 0..127
            # (63.5 is), so bipolar centres landed at +0.0079 instead of 0.
            # Snap CC-64 linear defaults to the exact midpoint.  The delta is
            # below v1's own 7-bit resolution, so this is not a behaviour change
            # — it is the asymmetry v2 exists to remove.
            if ccd == 64 and curve == "lin":
                d = (lo + hi) / 2.0
            default_s = f"{d:.6g}"

        cc = CURATED_CC.get(r["const"])

        a(f"  - id: 0x{pid:04X}            # v1: CC::{r['const']}"
          + (f" (was CC {cc_consts.get(r['const'], '?')})" if r['const'] in cc_consts else ""))
        a(f"    key: {make_key(r['const'])}")
        a(f"    name: {yq(r['name'])}")
        a(f"    label: {yq(r['label'])}")
        a(f"    type: {vtype}")
        if widget:
            a(f"    widget: {widget}")
        a(f"    scope: {scope_map[r['scope']]}")
        if vtype == "select":
            a(f"    options: {name_map[r['options']]}")
        elif vtype == "continuous":
            rng = f"{{min: {lo:g}, max: {hi:g}, curve: {curve}"
            if mid is not None:
                rng += f", mid: {mid:g}"
            rng += "}"
            a(f"    range: {rng}")
            a(f"    unit: {yq(unit)}")
            if r["bipolar"]:
                a(f"    bipolar_display: true")
        a(f"    default: {default_s}")
        a(f"    smooth_ms: {smooth}")
        if cc is not None:
            a(f"    cc: {cc}              # curated performance CC (brief §5.1)")
        a(f"    ui: {{section: {sec}, group: {r['group']}}}")

    # ---- the single new v2 param ----
    a("")
    a("  # ---- [16] Master (NEW in v2 — covered by brief §5.1 sign-off) ----")
    a("  - id: 0x0800            # section 16, index 0")
    a("    key: master.volume")
    a('    name: "Master Volume"')
    a('    label: "VOLUME"')
    a("    type: continuous")
    a("    scope: global")
    a("    range: {min: 0, max: 1, curve: lin}")
    a('    unit: "norm"')
    a("    default: 0.8")
    a("    smooth_ms: 20")
    a("    cc: 7               # standard MIDI Channel Volume")
    a("    ui: {section: 16, group: 0}")
    a("")

    with open(out_path, "w", newline="\n") as f:
        f.write("\n".join(lines))
    print(f"Wrote {out_path}: {sum(next_index.values()) + 1} parameters "
          f"({sum(next_index.values())} seeded from v1 + 1 new)")

def main():
    if len(sys.argv) != 3:
        sys.exit("usage: seed_from_v1.py <v1 ParamDefs.h> <out params.yaml>")
    with open(sys.argv[1], encoding="utf-8", errors="replace") as f:
        src = f.read()
    emit(*parse_v1(src), sys.argv[2])

if __name__ == "__main__":
    main()
