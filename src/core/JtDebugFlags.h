// JtDebugFlags.h — single definition point for temporary bring-up debug
// switches, so headers and translation units agree on their value.
//
// JT_DEBUG_NOTEKILL: per-port note-kill counters, raw Serial1 RX probe,
// one-shot hex capture, and the per-CC NRPN assembler trace (fault-2 hunt).
// Set to 0 to compile ALL of it out for release/byte-identical renders.
//
// This exists because the flag is read by both main.cpp AND
// MidiParamTransport.h (for its debug-only state getters); a #define inside
// main.cpp alone would not be visible to the header, silently disabling the
// getters and breaking the trace call sites.  One header, one truth.

#ifndef JT_DEBUG_FLAGS_H
#define JT_DEBUG_FLAGS_H

#ifndef JT_DEBUG_NOTEKILL
#define JT_DEBUG_NOTEKILL 1
#endif

#endif // JT_DEBUG_FLAGS_H
