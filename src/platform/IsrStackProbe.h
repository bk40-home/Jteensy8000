// =============================================================================
// IsrStackProbe.h — measures how much interrupt stack the firmware actually uses
// =============================================================================
//
// WHY THIS EXISTS
//   TeensyAtomThreads, pulled in as a submodule of teensy4_usbhost, repoints
//   the Main Stack Pointer at a fixed 2 KB buffer during startup
//   (atomport.cpp __switchStack: "static uint8_t handlerStack[2048]", then
//   "msr MSP").  The thread keeps the Teensy's normal large stack on PSP, but
//   on Cortex-M every exception and interrupt runs on MSP — so from that
//   moment the audio ISR, and any context switch nesting on top of it, share
//   2 KB.
//
//   For a mouse driver that is ample.  For an eight-voice synthesiser whose
//   ISR runs the entire render and FX chain it may not be, and overflowing it
//   corrupts whatever sits below rather than failing cleanly.
//
// HOW IT MEASURES
//   In thread mode nothing uses MSP, so MSP still holds the top of that
//   buffer.  Reading it gives the top address without needing the symbol,
//   which is a function-local static and not reachable from here.  begin()
//   fills the region below it with a pattern; used() finds the first word
//   that has been disturbed.  That is the true high-water mark, not an
//   estimate.
//
//   Scanning is a few hundred word compares, cheap at 1 Hz and off every hot
//   path.  Remove the whole probe once the size question is settled.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>

namespace JT {

// Bytes to fill and watch.  Matches the buffer TeensyAtomThreads installs.
// If the stack in use is actually larger than this, the measurement simply
// covers the deepest 2 KB of it, which is the part that matters.
inline constexpr uint32_t kIsrStackWatchBytes = 2048u;

class IsrStackProbe
{
public:
    // Call once, early in setup(), before the audio graph starts so that no
    // interrupt has had a chance to use the stack yet.  Fills with interrupts
    // masked: nothing can be executing on MSP while it is being written.
    void begin(uint32_t watchBytes = kIsrStackWatchBytes);

    // Deepest excursion seen so far, in bytes from the top of the stack.
    // Equal to capacity() means the pattern is gone entirely and the true
    // figure is at least this — treat it as overflowed, not as exactly full.
    uint32_t used(void) const;

    uint32_t capacity(void) const { return watched; }
    bool valid(void) const { return watched != 0u; }

    // Top of the interrupt stack, for reporting alongside a fault address.
    uint32_t top(void) const { return topAddress; }

private:
    uint32_t topAddress = 0u;
    uint32_t watched = 0u;
};

extern IsrStackProbe gIsrStackProbe;

} // namespace JT
