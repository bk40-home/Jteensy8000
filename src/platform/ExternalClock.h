// =============================================================================
// ExternalClock.h — 24-PPQN MIDI-clock measurement for JT-8000 v2 (Phase 9)
// =============================================================================
//
// ROLE
//   Turns incoming MIDI real-time bytes into a BPM and transport events, then
//   hands them to SynthCore (setExternalBpm / transportStart/Stop/Continue).
//   This is the ONLY place micros() timing lives — the Arduino-free core and
//   TempoClock must never gain a micros() dependency (it would break the host
//   render/test harness), so all measurement is quarantined here in the
//   platform layer alongside AudioSynthBlockF32.
//
// WHY HERE, NOT IN TempoClock
//   TempoClock is pure math (BPM -> Hz) and host-testable.  Measurement needs a
//   wall clock and lives on the Arduino side.  ExternalClock measures; it then
//   pushes a finished BPM into TempoClock via SynthCore's lock-free setter, so
//   the whole synth (LFOs + seq + arp) follows one clock with no core changes.
//
// MEASUREMENT MODEL (docs/PHASE9_ARP_SPEC.md — BPM-follow + Start-resets-phase)
//   MIDI clock is 24 pulses per quarter-note (PPQN).  We timestamp each 0xF8
//   with micros() and average the interval over a whole beat (24 pulses) to
//   reject per-pulse jitter, then:  BPM = 60e6 / (avgIntervalUs * 24).
//   A short warm-up (ignore the first beat) avoids a wild first estimate.  The
//   derived BPM is clamped to TempoClock's 40..300 range by the consumer.
//
//   Sample-accurate per-pulse phase-lock is deliberately NOT done (it would add
//   per-pulse cross-plane traffic to a block-rate engine for no audible gain on
//   this hardware).  Phase is re-anchored only on transport Start — which is
//   what most hardware does and what the arp needs to lock to a DAW downbeat.
//
// PORT ADAPTERS
//   usbMIDI and FortySevenEffects expose no-arg per-message handlers
//   (setHandleClock/Start/Stop/Continue) -> call onClockPulse/onStart/onStop/
//   onContinue directly.  USBHost_t36's MIDIDevice instead exposes ONE
//   setHandleRealTimeSystem(uint8_t) -> route it through onRealtimeByte(), which
//   decodes the status byte and forwards to the same four entry points.  All
//   three ports can share one ExternalClock instance (the transport is global).
//
// THREADING
//   The handlers run in loop() context (Teensy MIDI .read() is polled, not a
//   true ISR here), the same context that already calls core().noteOn() etc.
//   ExternalClock only calls SynthCore's lock-free producers, so there is no
//   new cross-plane hazard beyond what the note path already has.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>

namespace JT {

class SynthCore;   // fwd — ExternalClock pushes BPM/transport into it

class ExternalClock {
public:
    // MIDI real-time status bytes.
    enum : uint8_t { kByteClock = 0xF8, kByteStart = 0xFA,
                     kByteContinue = 0xFB, kByteStop = 0xFC };

    // 24 MIDI clocks per quarter-note (the MIDI spec constant).
    static constexpr uint32_t kPPQN = 24;

    // `core` is borrowed for the object's lifetime; ExternalClock never owns it.
    // `nowUs` is a function pointer to the platform micros() (injected so this
    // header carries no Arduino dependency and can be unit-reasoned about).
    explicit ExternalClock(SynthCore& core, uint32_t (*nowUs)())
        : _core(core), _nowUs(nowUs) {}

    // ---- No-arg entry points (usbMIDI / FortySevenEffects) ----------------
    void onClockPulse();     // 0xF8
    void onStart();          // 0xFA
    void onStop();           // 0xFC
    void onContinue();       // 0xFB

    // ---- Raw-byte entry point (USBHost_t36 setHandleRealTimeSystem) --------
    // Decodes the status byte and forwards to the matching entry point above.
    void onRealtimeByte(uint8_t status);

    // ---- Debug instrumentation (Phase 9 sync bring-up) --------------------
    // Cheap monotonic counters so a 1 Hz status line can prove which stage of
    // the clock chain is alive.  Read-only; zero cost unless queried.
    uint32_t debugPulseCount() const { return _dbgPulses; }    // total 0xF8 seen
    float    debugLastBpm()    const { return _dbgLastBpm; }   // last derived BPM (0=none)
    bool     debugRunning()    const { return _running; }      // Start..Stop

private:
    void resetMeasurement();

    SynthCore&  _core;
    uint32_t  (*_nowUs)();              // platform micros()

    uint32_t    _lastPulseUs = 0;       // timestamp of the previous 0xF8
    uint32_t    _accumUs     = 0;       // summed intervals across the current beat
    uint32_t    _pulseCount  = 0;       // pulses seen in the current beat window
    bool        _haveLast    = false;   // false until the first pulse timestamp
    bool        _warmedUp    = false;   // ignore the very first beat's estimate
    bool        _running     = false;   // between Start/Continue and Stop

    // Debug-only counters (see debug accessors above).
    uint32_t    _dbgPulses   = 0;       // every 0xF8 that reached onClockPulse
    float       _dbgLastBpm  = 0.0f;    // last BPM handed to the core
};

} // namespace JT
