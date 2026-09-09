// =============================================================================
// IsrStack.cpp — see IsrStack.h.
// =============================================================================
#include "platform/IsrStack.h"

#include <Arduino.h>

namespace JT {

namespace {

// The stack itself.  Ordinary .bss, so DTCM: an interrupt stack is touched on
// every exception and wants the fastest memory available.  Eight-byte aligned
// because the exception frame requires it, and a whole number of words so the
// top lands on an aligned address.
uint32_t g_isrStack[kIsrStackBytes / 4u] __attribute__((aligned(8)));

// Any value unlikely to occur naturally.  Stack frames hold addresses, small
// integers and floats; a repeating 0xA5 byte is none of those.
constexpr uint32_t kPattern = 0xA5A5A5A5u;

// CONTROL bit 1 (SPSEL): set means thread mode is running on PSP.
constexpr uint32_t kControlSpsel = 2u;

} // namespace

IsrStack gIsrStack;

void IsrStack::install(void)
{
    uint32_t control;
    asm volatile("mrs %0, control" : "=r"(control));

    if ((control & kControlSpsel) == 0u) {
        // Thread mode is on MSP, which means AtomThreads never ran — a build
        // without the USB host stack.  Repointing MSP here would discard the
        // call stack this function is standing on.  Leave everything alone;
        // the Teensy's own arrangement has always been sufficient.
        installed = false;
        return;
    }

    const uint32_t words = kIsrStackBytes / 4u;

    // Masked for the whole operation: an exception taken between filling the
    // buffer and installing it would run on the old stack, which is harmless,
    // but one taken midway through the fill would be writing to memory being
    // overwritten.
    noInterrupts();

    for (uint32_t i = 0u; i < words; ++i) {
        g_isrStack[i] = kPattern;
    }

    // The stack grows downward, so MSP starts at the top.
    topAddress = reinterpret_cast<uint32_t>(&g_isrStack[words]);
    asm volatile("msr msp, %0" :: "r"(topAddress) : "memory");

    interrupts();

    installed = true;
}

uint32_t IsrStack::used(void) const
{
    if (!installed) {
        return 0u;
    }

    // Deepest excursion is the LOWEST disturbed word, so scan up from the
    // bottom until the pattern reappears.
    const uint32_t words = kIsrStackBytes / 4u;
    uint32_t i = 0u;
    while ((i < words) && (g_isrStack[i] == kPattern)) {
        ++i;
    }
    return (words - i) * 4u;
}

} // namespace JT
