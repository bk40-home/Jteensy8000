# teensy4-usb-audio — Teensy 4 core overlay for 48 kHz USB audio

**What:** the core files from [alex6679/teensy-4-usbAudio](https://github.com/alex6679/teensy-4-usbAudio)
(MIT, see `LICENSE`), vendored verbatim at commit `f73182118092e041f90f3ef680905e55b5f80ca2`
(2026-05-20). `core.sha256` records their checksums.

**Why:** the stock Teensy core's USB device audio is fixed at 44.1 kHz (UAC1). The
JT-8000 engine runs at 48 kHz (`src/core/AudioConfig.h`), so laptop → Teensy audio
arrived ~8 % short per block and came out garbled; the Teensy → DAW record path was
rate-inconsistent the same way. This overlay switches USB audio to UAC2 at the rate
given by the `AUDIO_*` build flags, with feedback so the host sends exactly what the
audio graph consumes.

**How it is used:** `pio_core_overlay.py` (a PlatformIO `pre:` script, hooked in
`platformio.ini`) merges the installed framework's `cores/teensy4` with `core/` into
`.pio/teensy4-core-usb-audio/` and builds the core from there. The PlatformIO install
is never edited. Same class names as stock (`AudioInputUSB`, `AudioOutputUSB`), plus
`getStatus()` for under/overrun counters.

**Files replaced in the core:** `AudioStream.h`, `usb.c`, `usb_audio.cpp/.h`,
`usb_desc.c/.h`. **Added:** `usb_audio_interface.cpp/.h`, `util/LastCall.h`.

**Verified against:** Teensyduino 1.60 and 1.62 — none of the six replaced stock
files differ between them. The script refuses any framework other than 1.162.x.

**Updating the framework:** diff the six replaced files between the old and new
Teensyduino tags in [PaulStoffregen/cores](https://github.com/PaulStoffregen/cores).
If any changed, re-sync from alex6679 (or merge by hand), refresh `core.sha256`,
then change `EXPECTED_FW_MAJOR` in the script and the pin in `platformio.ini`.

**Visible changes on the PC side:** USB audio is now UAC2 (Windows 10 1703+ has the
driver built in), and the MIDI interface number moved from 2 to 5 (USB 2 requires
it to follow the audio interfaces). Windows may need the old device entry removed
once; see the bench notes in the delivery doc.

**Known upstream notes:** the record path (Teensy → PC) occasionally duplicates or
drops a single sample to hold its buffer centred; `usb_audio_interface.h:119` raises
one harmless `-Wdouble-promotion` warning under the project's flags (left verbatim).
