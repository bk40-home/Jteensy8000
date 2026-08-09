// =============================================================================
// ExternalClock.cpp — 24-PPQN MIDI-clock measurement implementation
// =============================================================================
// See ExternalClock.h for the design contract.  The measurement math is
// Arduino-free (micros() is injected), so it host-compiles and is unit-tested
// in test/test_external_clock.cpp against a synthetic pulse stream.
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#include "platform/ExternalClock.h"

#include "core/SynthCore.h"

namespace JT {

void ExternalClock::resetMeasurement()
{
    _lastPulseUs = 0;
    _accumUs     = 0;
    _pulseCount  = 0;
    _haveLast    = false;
    _warmedUp    = false;
}

// ---------------------------------------------------------------------------
// onClockPulse — one 0xF8.  Accumulate the interval since the previous pulse;
// once a full beat (24 pulses) of intervals is gathered, derive BPM and hand it
// to the core, then slide to the next beat window.
// ---------------------------------------------------------------------------
void ExternalClock::onClockPulse()
{
    ++_dbgPulses;                        // debug: prove 0xF8 bytes are arriving
    const uint32_t now = _nowUs();

    if (!_haveLast) {                    // first pulse: no interval yet
        _lastPulseUs = now;
        _haveLast    = true;
        return;
    }

    // Unsigned subtraction is wrap-safe on the 32-bit micros() counter (it wraps
    // every ~71.6 min; the difference stays correct across a single wrap).
    const uint32_t dt = now - _lastPulseUs;
    _lastPulseUs = now;

    // Guard against absurd intervals (a dropped/duplicated byte, or a paused
    // then resumed clock): ignore anything outside a sane 40..300 BPM window.
    //   40 BPM  -> 62500 us/pulse ;  300 BPM -> 8333 us/pulse.
    if (dt < 6000u || dt > 70000u) {     // a little slack beyond 40..300
        // Treat as a discontinuity: restart the beat window from here.
        _accumUs    = 0;
        _pulseCount = 0;
        return;
    }

    _accumUs += dt;
    ++_pulseCount;

    if (_pulseCount >= kPPQN) {          // a whole beat of intervals gathered
        const uint32_t avg = _accumUs / _pulseCount;   // mean us/pulse
        _accumUs    = 0;
        _pulseCount = 0;

        if (!_warmedUp) {                // discard the first beat's estimate
            _warmedUp = true;
            return;
        }

        // BPM = 60,000,000 us-per-min / (avg us-per-pulse * 24 pulses-per-beat).
        if (avg > 0u) {
            const float bpm = 60000000.0f / ((float)avg * (float)kPPQN);
            _dbgLastBpm = bpm;           // debug: prove measurement produced a number
            _core.setExternalBpm(bpm);   // consumer clamps to 40..300
        }
    }
}

void ExternalClock::onStart()
{
    resetMeasurement();
    _running = true;
    _core.transportStart();              // arp/seq re-anchor to the downbeat
}

void ExternalClock::onContinue()
{
    // Resume without a phase reset; keep measuring from the next pulse.
    _haveLast   = false;                 // next pulse re-seeds the interval base
    _accumUs    = 0;
    _pulseCount = 0;
    _running    = true;
    _core.transportContinue();
}

void ExternalClock::onStop()
{
    _running = false;
    _core.transportStop();
}

void ExternalClock::onRealtimeByte(uint8_t status)
{
    switch (status) {
        case kByteClock:    onClockPulse(); break;
        case kByteStart:    onStart();      break;
        case kByteContinue: onContinue();   break;
        case kByteStop:     onStop();       break;
        default:            /* Active Sensing 0xFE, Reset 0xFF, etc. */ break;
    }
}

} // namespace JT
