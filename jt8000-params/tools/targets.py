#!/usr/bin/env python3
# =============================================================================
# targets.py — where every generated artefact is copied on the laptop layout
# =============================================================================
#
# ROLE
#   gen_params.py writes each artefact once, into this repo (jt8000-params/),
#   then MIRRORS it byte-for-byte to every consumer listed here, so a single
#   `python tools/gen_params.py` leaves all three codebases in step — no hand
#   copies.  `--check` verifies the mirrors the same way (line-ending blind).
#
# LAYOUT (laptop masters, 2026-10-25)
#   C:\GitHub\Jteensy8000\                firmware   (this repo is jt8000-params/ inside it)
#   C:\GitHub\JT8000_Controller\          JUCE plugin
#   C:\GitHub\JtMidiController\           ESP32 controller
#   Paths below are relative to jt8000-params/ (the repo root, parent of
#   tools/).  ../ is Jteensy8000, ../../ is the folder holding all three.
#   Override the sibling-repo base with JT_GITHUB_ROOT if the layout differs.
#
# MISSING REPOS
#   A mirror whose REPO folder does not exist (e.g. CI, or a sibling repo not
#   checked out) is skipped with a note, never a failure.  A missing FILE in a
#   repo that does exist is written (or reported stale by --check).
#
# © 2026 Kris Bishop — MIT licensed.
# =============================================================================

import os

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))     # jt8000-params/
_FIRMWARE = os.path.dirname(_ROOT)                                      # Jteensy8000/
_GITHUB = os.environ.get("JT_GITHUB_ROOT") or os.path.dirname(_FIRMWARE)

# Each mirror: (repo folder that must exist, path of the copy).
def _fw(*p):   return (_FIRMWARE, os.path.join(_FIRMWARE, *p))
def _sib(repo, *p):
    base = os.path.join(_GITHUB, repo)
    return (base, os.path.join(base, *p))

# Source artefact (relative to jt8000-params/) -> its consumer copies.
MIRRORS = {
    os.path.join("gen", "ParamTable.h"): [
        _fw("src", "gen", "ParamTable.h"),               # firmware build (-I src)
        _fw("gen", "ParamTable.h"),                       # firmware repo top-level copy
        _sib("JtMidiController", "ParamTable.h"),        # ESP32 controller
    ],
    os.path.join("gen", "PatternPresets.h"): [
        _sib("JtMidiController", "PatternPresets.h"),    # ESP32 controller
    ],
    os.path.join("gen", "JtParamTable.h"): [
        _fw("gen", "JtParamTable.h"),                     # firmware repo top-level copy
        _sib("JT8000_Controller", "Source", "gen", "JtParamTable.h"),   # JUCE plugin
    ],
}
