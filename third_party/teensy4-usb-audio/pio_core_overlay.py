# =============================================================================
# pio_core_overlay.py — PlatformIO pre-build script: Teensy 4 core + USB audio
# =============================================================================
# WHAT IT DOES
#   Builds the Teensy 4 core from a MERGED copy instead of the installed one:
#     1. copies the installed framework's cores/teensy4 into
#        .pio/teensy4-core-usb-audio/  (only when something changed),
#     2. overlays the files in third_party/teensy4-usb-audio/core/
#        (alex6679/teensy-4-usbAudio: UAC2 USB audio at a selectable rate),
#     3. points the core library build and the include path at that copy.
#   Your PlatformIO install is NEVER modified.  Delete .pio/ to force a fresh
#   merge; a normal build re-merges by itself when the framework version or any
#   overlay file changes (content-hashed stamp).
#
# WHY A WHOLE MERGED COPY AND NOT JUST THE 9 FILES ON THE INCLUDE PATH
#   Core sources include their own headers with quotes ("usb_desc.h"), and a
#   quoted include always finds the file NEXT TO the source first.  With only an
#   -I override, the stock core .c files would silently compile against the
#   STOCK usb_desc.h (different interface numbers) while usb_desc.c used the new
#   one.  A complete merged directory makes every quoted include consistent.
#
# SAFETY GUARD
#   The overlay was diffed against Teensyduino 1.60 and 1.62: none of the six
#   stock files it replaces changed between those versions.  Any other framework
#   version stops the build with a message instead of mixing core versions.
#   platformio.ini also pins the framework to ~1.162.0.
#
# Hooked from platformio.ini:   extra_scripts = pre:third_party/teensy4-usb-audio/pio_core_overlay.py
# =============================================================================
Import("env")  # noqa: F821  (provided by PlatformIO/SCons)

import hashlib
import os
import shutil
import sys
from os.path import isdir, join, normcase, normpath, relpath

SCRIPT_VERSION    = "1"        # bump to force a re-merge after editing this file
EXPECTED_FW_MAJOR = "1.162."   # framework-arduinoteensy 1.162.x == Teensyduino 1.62
MERGED_DIR_NAME   = "teensy4-core-usb-audio"
STAMP_NAME        = ".overlay_stamp"
TAG               = "[usb-audio core]"


def _fail(msg):
    """Stop the build with a clear, single-line reason."""
    sys.stderr.write("%s ERROR: %s\n" % (TAG, msg))
    env.Exit(1)  # noqa: F821


def _same_path(a, b):
    """Windows-safe path comparison (case and separator insensitive)."""
    return normcase(normpath(a)) == normcase(normpath(b))


def _overlay_files(overlay_dir):
    """All overlay files as sorted (relative path, absolute path) pairs."""
    out = []
    for root, _dirs, files in os.walk(overlay_dir):
        for name in files:
            full = join(root, name)
            out.append((relpath(full, overlay_dir), full))
    return sorted(out)


def _stamp(fw_version, files):
    """Content hash of everything the merged copy depends on."""
    h = hashlib.sha256()
    h.update(("v%s|%s|" % (SCRIPT_VERSION, fw_version)).encode())
    for rel, full in files:
        h.update(rel.replace("\\", "/").encode())
        with open(full, "rb") as fp:
            h.update(hashlib.sha256(fp.read()).digest())
    return h.hexdigest()


# --- 1. Locate everything and check the framework version -------------------
platform   = env.PioPlatform()  # noqa: F821
fw_dir     = platform.get_package_dir("framework-arduinoteensy")
fw_version = platform.get_package_version("framework-arduinoteensy") or "?"
core_name  = env.BoardConfig().get("build.core", "")  # noqa: F821

if core_name != "teensy4":
    _fail("this overlay is for Teensy 4.x boards only (build.core=%r)" % core_name)
if not fw_dir or not isdir(join(fw_dir, "cores", core_name)):
    _fail("framework-arduinoteensy not installed; run the build once online")
if not fw_version.startswith(EXPECTED_FW_MAJOR):
    _fail("framework-arduinoteensy %s found, overlay verified only against %sx. "
          "Re-diff third_party/teensy4-usb-audio/core against the new core "
          "before changing EXPECTED_FW_MAJOR." % (fw_version, EXPECTED_FW_MAJOR))

stock_core  = join(fw_dir, "cores", core_name)
project_dir = env.subst("$PROJECT_DIR")  # noqa: F821
overlay_dir = join(project_dir, "third_party", "teensy4-usb-audio", "core")
merged_core = join(env.subst("$PROJECT_WORKSPACE_DIR"), MERGED_DIR_NAME)  # noqa: F821

if not isdir(overlay_dir):
    _fail("overlay folder missing: %s" % overlay_dir)

# --- 2. (Re)build the merged copy only when its inputs changed --------------
files      = _overlay_files(overlay_dir)
want_stamp = _stamp(fw_version, files)
stamp_path = join(merged_core, STAMP_NAME)

have_stamp = None
if os.path.isfile(stamp_path):
    with open(stamp_path) as fp:
        have_stamp = fp.read().strip()

if have_stamp != want_stamp:
    if isdir(merged_core):
        shutil.rmtree(merged_core)
    shutil.copytree(stock_core, merged_core)
    for rel, full in files:
        dst = join(merged_core, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(full, dst)
    with open(stamp_path, "w") as fp:
        fp.write(want_stamp)
    print("%s merged: framework %s + %d overlay files -> %s"
          % (TAG, fw_version, len(files), merged_core))
else:
    print("%s up to date (framework %s, %d overlay files)"
          % (TAG, fw_version, len(files)))

# --- 3. Headers: the merged core is searched before the stock one -----------
# The framework script APPENDS the stock core path later, so prepending here
# keeps the merged copy first for every <angle-bracket> include in src/ and
# in libraries (Audio, OpenAudio, ...).
env.Prepend(CPPPATH=[merged_core])  # noqa: F821

# --- 4. Sources: build the core library from the merged copy ----------------
# BuildLibrary is a PlatformIO method on the environment.  The wrapper changes
# ONLY the call whose source is the stock core folder; every other library is
# passed straight through, on whichever (possibly cloned) environment called it.
_wrapper = getattr(env, "BuildLibrary", None)  # noqa: F821
_orig    = getattr(_wrapper, "method", None)    # unbound function inside SCons' MethodWrapper
if _orig is None:
    _fail("cannot hook env.BuildLibrary (PlatformIO internals changed)")


def _build_library(e, variant_dir, src_dir, *args, **kwargs):
    if _same_path(e.subst(src_dir), stock_core):
        src_dir = merged_core
    return _orig(e, variant_dir, src_dir, *args, **kwargs)


env.AddMethod(_build_library, "BuildLibrary")  # noqa: F821
