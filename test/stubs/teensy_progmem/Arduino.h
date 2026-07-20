// Host stand-in for the Teensy 4 core's PROGMEM definition — used ONLY by
// `make attrcheck` to verify on the host that the AKWF tables land in the
// .progmem section exactly as they will on hardware.  Never shipped.
#pragma once
#define PROGMEM __attribute__((section(".progmem")))
