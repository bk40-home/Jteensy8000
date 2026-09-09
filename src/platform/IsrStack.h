// =============================================================================
// IsrStack.h — the firmware provides its own interrupt stack
// =============================================================================
//
// WHY
//   TeensyAtomThreads (a submodule of teensy4_usbhost) repoints MSP at a
//   2048-byte static buffer during startup, in __switchStack().  On Cortex-M
//   every exception runs on MSP, and the Teensy Audio Library calls
//   AudioStream::update_all() — this synth's entire DSP render — from a
//   software interrupt.  With the FPU in use each stacked frame is 104 bytes
//   rather than 32, and 2048 is not enough: it overflows, overwrites the
//   EXC_RETURN that atomPendSV_ISR pushed, and the processor returns to a
//   nonsense address.  See DEFERRALS_LEDGER.md D-13.
//
//   Patching the library was the obvious fix and a poor one — a local fork of
//   somebody else's threading code, carried forever.  The library's author
//   suggested this instead: allocate the stack here and overwrite the MSP the
//   library set up.  It needs no change to the library at all, so we track
//   upstream cleanly.
//
// WHEN
//   install() must run before any interrupt goes deep — first statement in
//   setup() is early enough, since the audio graph is not started and the USB
//   host stack is not begun until later in that function.
//
// SAFETY
//   Overwriting MSP is only safe because AtomThreads has already moved the
//   running context onto PSP; MSP is idle and nothing of ours is on it.  If
//   the USB host stack were ever compiled out, thread mode would still be
//   using MSP and repointing it would throw away our own call stack.
//   install() therefore checks CONTROL.SPSEL and declines rather than
//   corrupting the system.  A firmware built without the host port keeps the
//   Teensy's normal arrangement, which was always fine.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>

namespace JT {

// Size of the replacement stack.
//
// EMPIRICAL, and deliberately far above the measured requirement.  The probe
// below reports about 460 bytes idle and 1104 with eight voices sounding and
// both USB streams running — yet 4096 still faults, so something nests deeper
// than a high-water mark sampled at 1 Hz can catch.  2048 crashes, 4096
// crashes, 16384 is stable under sustained hard playing.
//
// Do not trim this toward the measured figure.  If DTCM pressure ever forces
// it down, bisect it on hardware.
inline constexpr uint32_t kIsrStackBytes = 16384u;

class IsrStack
{
public:
    // Installs the stack and fills it with a pattern.  Call as the first
    // statement in setup().  Does nothing, safely, if the running context is
    // not on PSP.
    void install(void);

    // True when the stack was installed and is being measured.
    bool valid(void) const { return installed; }

    // Deepest excursion since install(), in bytes.  Reaching capacity() means
    // the pattern is gone entirely: the real figure is at least that, and the
    // stack has overflowed.
    uint32_t used(void) const;

    uint32_t capacity(void) const { return installed ? kIsrStackBytes : 0u; }

    // Top of the stack, for sanity-checking against a fault address.
    uint32_t top(void) const { return topAddress; }

private:
    uint32_t topAddress = 0u;
    bool installed = false;
};

extern IsrStack gIsrStack;

} // namespace JT
