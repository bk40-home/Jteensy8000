#!/usr/bin/env bash
# =============================================================================
# remove_onboard_ui.sh — JT-8000 streamlining
# -----------------------------------------------------------------------------
# Deletes the onboard TFT UI subsystem (encoders + TFT + touch + screens),
# already-dead JP8000 .syx loader, the parallel Performance/Patch preset struct
# (UI-only consumer), and stray build artifacts from src/.
#
# AFTER running this, drop in the replacement Jteensy8000.cpp and Mapping.h
# from the same delivery (they remove the matching code references).
#
# Run from the PROJECT ROOT (the folder that contains src/ and platformio.ini):
#     bash remove_onboard_ui.sh
#
# It refuses to run if src/Jteensy8000.cpp is missing, to avoid wiping the
# wrong folder. Uses `git rm` when inside a git repo so history is preserved;
# falls back to plain rm otherwise.
# =============================================================================
set -euo pipefail

if [[ ! -f src/Jteensy8000.cpp ]]; then
  echo "ERROR: run this from the project root (src/Jteensy8000.cpp not found)." >&2
  exit 1
fi

# Pick git rm if this is a git working tree, else plain rm.
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  RM=(git rm -f --)
  RMDIR=(git rm -rf --)
  echo "git repo detected — using 'git rm' (history preserved)."
else
  RM=(rm -f --)
  RMDIR=(rm -rf --)
  echo "not a git repo — using plain 'rm'."
fi

cd src

# --- Group A: already-dead JP8000 .syx loader (zero references anywhere) ------
"${RM[@]}" JP8000SyxLoader.cpp JP8000SyxLoader.h JP8000SyxMap.h

# --- Group B: onboard UI subsystem -------------------------------------------
# Input layer (2 encoders + buttons)
"${RM[@]}" HardwareInterface_MicroDexed.cpp HardwareInterface_MicroDexed.h
# Capacitive touch
"${RM[@]}" TouchInput.cpp TouchInput.h Adafruit_FT6206.cpp Adafruit_FT6206.h
# ILI9341 TFT driver (vendored) + fonts + SPI-share guard
"${RM[@]}" ILI9341_t3n.cpp ILI9341_t3n.h ILI9341_fonts.h glcdfont.c DisplaySharedSPIStatus.h
# UI manager + screens + widgets
"${RM[@]}" UIManager_TFT.cpp UIManager_TFT.h
"${RM[@]}" HomeScreen.cpp HomeScreen.h PresetBrowser.cpp PresetBrowser.h
"${RM[@]}" TFTWidgets.cpp TFTWidgets.h MiniEnvelope.cpp MiniEnvelope.h
# UI theme + section layout tables + oscilloscope feed
"${RM[@]}" JT8000Colours.h JT8000_Sections.h AudioScopeTap.h

# --- Group C: newly-dead once UI is gone (sole consumer was UIManager_TFT) ----
"${RM[@]}" Performance.h Patch.h Patch.cpp

# --- Group D: build artifacts that don't belong in src/ -----------------------
"${RMDIR[@]}" Firmware Juce 2>/dev/null || true

cd ..
echo
echo "Done. Now copy the replacement files over the originals:"
echo "    cp Jteensy8000.cpp src/Jteensy8000.cpp"
echo "    cp Mapping.h       src/Mapping.h"
echo
echo "Then rebuild:  pio run -e default"
