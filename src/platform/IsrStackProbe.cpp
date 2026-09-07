// =============================================================================
// IsrStackProbe.cpp — see IsrStackProbe.h.
// =============================================================================
#include "platform/IsrStackProbe.h"

#include <Arduino.h>

namespace JT {

namespace {

// Any value works provided it is unlikely to occur naturally.  Stack frames
// are full of addresses, small integers and floats; a repeating 0xA5 byte is
// none of those.
constexpr uint32_t kPattern = 0xA5A5A5A5u;

} // namespace

IsrStackProbe gIsrStackProbe;

void IsrStackProbe::begin(uint32_t watchBytes)
{
    // In thread mode the Main Stack Pointer is idle and still holds the top of
    // the interrupt stack, so this reads the buffer's top address without
    // needing its symbol.
    uint32_t msp;
    asm volatile("mrs %0, msp" : "=r"(msp));

    // Word-align downward; a misaligned base would make the scan meaningless.
    msp &= ~3u;
    if (msp < watchBytes) {
        // Implausible address: measure nothing rather than write somewhere
        // arbitrary.
        watched = 0u;
        return;
    }

    topAddress = msp;
    watched = watchBytes & ~3u;

    // Masked while filling: an interrupt taken here would be executing on the
    // very memory being overwritten.
    noInterrupts();
    uint32_t* base = reinterpret_cast<uint32_t*>(topAddress - watched);
    const uint32_t words = watched / 4u;
    for (uint32_t i = 0u; i < words; ++i) {
        base[i] = kPattern;
    }
    interrupts();
}

uint32_t IsrStackProbe::used(void) const
{
    if (watched == 0u) {
        return 0u;
    }

    // The stack grows downward, so the deepest excursion is the LOWEST
    // disturbed word.  Scan up from the bottom until the pattern reappears.
    const uint32_t* base = reinterpret_cast<const uint32_t*>(topAddress - watched);
    const uint32_t words = watched / 4u;

    uint32_t i = 0u;
    while ((i < words) && (base[i] == kPattern)) {
        ++i;
    }
    return (words - i) * 4u;
}

} // namespace JT
