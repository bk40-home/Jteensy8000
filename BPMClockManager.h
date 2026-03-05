#pragma once
#include <Arduino.h>

// MIDI clock standard: 24 pulses per quarter note (PPQN)
#define MIDI_CLOCK_PPQN 24

// Musical note division types
enum TimingMode {
    TIMING_FREE,      // Free-running Hz (not synced)
    TIMING_4_BARS,    // 16 quarter notes
    TIMING_2_BARS,    // 8 quarter notes
    TIMING_1_BAR,     // 4 quarter notes
    TIMING_1_2,       // Half note
    TIMING_1_4,       // Quarter note (1 beat)
    TIMING_1_8,       // Eighth note
    TIMING_1_16,      // Sixteenth note
    TIMING_1_32,      // 32nd note
    TIMING_1_4T,      // Quarter triplet
    TIMING_1_8T,      // Eighth triplet
    TIMING_1_16T,     // Sixteenth triplet
    NUM_TIMING_MODES
};

// Human-readable names for UI display
extern const char* TimingModeNames[NUM_TIMING_MODES];

// Clock source selection
enum ClockSource {
    CLOCK_INTERNAL,
    CLOCK_EXTERNAL_MIDI,
    NUM_CLOCK_SOURCES
};

/**
 * @brief BPM clock manager for tempo-synced modulation and effects
 * 
 * Features:
 * - Internal/external MIDI clock support
 * - MIDI clock (0xF8) message handling (24 PPQN)
 * - BPM calculation and smoothing
 * - Musical note division conversion
 * - CPU-efficient: calculations only when BPM changes
 */
class BPMClockManager {
public:
    BPMClockManager();
    
    // ─────────────────────────────────────────────────────────────
    // Clock Source Management
    // ─────────────────────────────────────────────────────────────
    
    /**
     * @brief Set the clock source (internal or external MIDI)
     * @param source CLOCK_INTERNAL or CLOCK_EXTERNAL_MIDI
     */
    void setClockSource(ClockSource source);
    
    /**
     * @brief Get the current clock source
     * @return Current ClockSource
     */
    ClockSource getClockSource() const { return _clockSource; }
    
    // ─────────────────────────────────────────────────────────────
    // Internal BPM Control
    // ─────────────────────────────────────────────────────────────
    
    /**
     * @brief Set internal BPM (used when clock source is internal)
     * @param bpm Target BPM (40-300 range recommended)
     */
    void setInternalBPM(float bpm);
    
    /**
     * @brief Get current effective BPM
     * @return Current BPM (internal or calculated from external MIDI clock)
     */
    float getCurrentBPM() const { return _currentBPM; }
    
    // ─────────────────────────────────────────────────────────────
    // MIDI Clock Message Handling (External Clock)
    // ─────────────────────────────────────────────────────────────
    
    /**
     * @brief Handle incoming MIDI clock pulse (0xF8)
     * Call this from MIDI handler for clock messages
     * Automatically calculates BPM from pulse timing (24 PPQN)
     */
    void handleMIDIClock();
    
    /**
     * @brief Handle MIDI Start message (0xFA)
     * Resets clock state and begins tracking
     */
    void handleMIDIStart();
    
    /**
     * @brief Handle MIDI Stop message (0xFC)
     * Stops external clock tracking
     */
    void handleMIDIStop();
    
    /**
     * @brief Handle MIDI Continue message (0xFB)
     * Resumes external clock tracking
     */
    void handleMIDIContinue();
    
    // ─────────────────────────────────────────────────────────────
    // Timing Conversions (Core Calculation Methods)
    // ─────────────────────────────────────────────────────────────
    
    /**
     * @brief Convert musical timing mode to frequency in Hz
     * @param mode Musical division (TIMING_1_4, TIMING_1_8, etc.)
     * @return Frequency in Hz, or -1.0 if TIMING_FREE
     * 
     * Example: At 120 BPM:
     *   TIMING_1_4  → 2.0 Hz (quarter note = 500ms)
     *   TIMING_1_8  → 4.0 Hz (eighth note = 250ms)
     *   TIMING_1_16 → 8.0 Hz (16th note = 125ms)
     */
    float getFrequencyForMode(TimingMode mode) const;
    
    /**
     * @brief Convert musical timing mode to time in milliseconds
     * @param mode Musical division
     * @return Time in milliseconds, or -1.0 if TIMING_FREE
     * 
     * Example: At 120 BPM:
     *   TIMING_1_4  → 500 ms
     *   TIMING_1_8  → 250 ms
     *   TIMING_1_16 → 125 ms
     */
    float getTimeForMode(TimingMode mode) const;
    
    // ─────────────────────────────────────────────────────────────
    // Status and Diagnostics
    // ─────────────────────────────────────────────────────────────
    
    /**
     * @brief Check if external clock is currently running
     * @return true if receiving MIDI clock messages
     */
    bool isExternalClockRunning() const { return _externalClockRunning; }
    
    /**
     * @brief Get time since last MIDI clock pulse
     * @return Milliseconds since last pulse (for timeout detection)
     */
    uint32_t getTimeSinceLastClock() const;
    
private:
    // ─────────────────────────────────────────────────────────────
    // Internal State
    // ─────────────────────────────────────────────────────────────
    
    ClockSource _clockSource;           // Current clock source
    float _internalBPM;                 // User-set internal BPM
    float _currentBPM;                  // Effective BPM (internal or external)
    
    // External MIDI clock tracking
    bool _externalClockRunning;         // Is MIDI clock active?
    uint32_t _lastClockTime;            // Timestamp of last MIDI clock (micros)
    uint32_t _clockPulseCount;          // Pulses since start (24 per quarter)
    uint32_t _lastQuarterNoteTime;      // Time of last quarter note boundary
    float _measuredBPM;                 // Calculated BPM from MIDI timing
    
    // BPM smoothing for stable external clock
    static const int BPM_SMOOTH_SAMPLES = 4;  // Average over N quarter notes
    float _bpmHistory[BPM_SMOOTH_SAMPLES];
    int _bpmHistoryIndex;
    
    // Cached multipliers for efficiency (updated only when BPM changes)
    float _beatMultipliers[NUM_TIMING_MODES];
    
    // ─────────────────────────────────────────────────────────────
    // Internal Helper Methods
    // ─────────────────────────────────────────────────────────────
    
    /**
     * @brief Update cached multiplier table (call when BPM changes)
     * Reduces CPU by pre-calculating beat multipliers
     */
    void updateBeatMultipliers();
    
    /**
     * @brief Get beat multiplier for a timing mode
     * @param mode Musical division
     * @return Multiplier (1.0 = quarter note, 0.5 = eighth, etc.)
     */
    float getBeatMultiplier(TimingMode mode) const;
    
    /**
     * @brief Update BPM from external clock measurements
     * Applies smoothing and validation
     */
    void updateExternalBPM();
};