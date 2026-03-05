#include "BPMClockManager.h"

#include "BPMClockManager.h"

// Human-readable names for UI display
extern const char* TimingModeNames[NUM_TIMING_MODES] = {
    "Free",    // TIMING_FREE
    "4 Bars",  // TIMING_4_BARS
    "2 Bars",  // TIMING_2_BARS
    "1 Bar",   // TIMING_1_BAR
    "1/2",     // TIMING_1_2
    "1/4",     // TIMING_1_4
    "1/8",     // TIMING_1_8
    "1/16",    // TIMING_1_16
    "1/32",    // TIMING_1_32
    "1/4T",    // TIMING_1_4T
    "1/8T",    // TIMING_1_8T
    "1/16T"    // TIMING_1_16T
};

// ═════════════════════════════════════════════════════════════════
// Constructor
// ═════════════════════════════════════════════════════════════════

BPMClockManager::BPMClockManager()
    : _clockSource(CLOCK_INTERNAL)
    , _internalBPM(120.0f)
    , _currentBPM(120.0f)
    , _externalClockRunning(false)
    , _lastClockTime(0)
    , _clockPulseCount(0)
    , _lastQuarterNoteTime(0)
    , _measuredBPM(120.0f)
    , _bpmHistoryIndex(0)
{
    // Initialize BPM history for smoothing
    for (int i = 0; i < BPM_SMOOTH_SAMPLES; i++) {
        _bpmHistory[i] = 120.0f;
    }
    
    // Pre-calculate beat multipliers for default BPM
    updateBeatMultipliers();
}

// ═════════════════════════════════════════════════════════════════
// Clock Source Management
// ═════════════════════════════════════════════════════════════════

void BPMClockManager::setClockSource(ClockSource source) {
    _clockSource = source;
    
    // When switching to internal, use the stored internal BPM
    if (source == CLOCK_INTERNAL) {
        _currentBPM = _internalBPM;
        updateBeatMultipliers();
    }
    // When switching to external, keep last measured BPM until new data arrives
    // (This prevents sudden jumps - the _currentBPM is already tracking external)
}

// ═════════════════════════════════════════════════════════════════
// Internal BPM Control
// ═════════════════════════════════════════════════════════════════

void BPMClockManager::setInternalBPM(float bpm) {
    // Clamp to reasonable range
    bpm = constrain(bpm, 40.0f, 300.0f);
    
    _internalBPM = bpm;
    
    // If currently using internal clock, update immediately
    if (_clockSource == CLOCK_INTERNAL) {
        _currentBPM = bpm;
        updateBeatMultipliers();  // Recalculate multipliers
    }
}

// ═════════════════════════════════════════════════════════════════
// MIDI Clock Message Handling
// ═════════════════════════════════════════════════════════════════

void BPMClockManager::handleMIDIClock() {
    // Only process if external clock is selected
    if (_clockSource != CLOCK_EXTERNAL_MIDI) return;
    
    uint32_t now = micros();  // High-precision timestamp
    
    // First pulse - just record timestamp
    if (_clockPulseCount == 0) {
        _lastClockTime = now;
        _lastQuarterNoteTime = now;
        _clockPulseCount = 1;
        _externalClockRunning = true;
        return;
    }
    
    _clockPulseCount++;
    
    // Every 24 pulses = 1 quarter note
    if (_clockPulseCount % MIDI_CLOCK_PPQN == 0) {
        // Calculate time for this quarter note
        uint32_t quarterNoteDuration = now - _lastQuarterNoteTime;
        
        // Calculate BPM: 60,000,000 microseconds per minute
        // BPM = 60,000,000 / microseconds_per_quarter_note
        if (quarterNoteDuration > 0) {
            float instantBPM = 60000000.0f / quarterNoteDuration;
            
            // Smooth BPM using running average
            _bpmHistory[_bpmHistoryIndex] = instantBPM;
            _bpmHistoryIndex = (_bpmHistoryIndex + 1) % BPM_SMOOTH_SAMPLES;
            
            // Calculate average
            float sum = 0.0f;
            for (int i = 0; i < BPM_SMOOTH_SAMPLES; i++) {
                sum += _bpmHistory[i];
            }
            _measuredBPM = sum / BPM_SMOOTH_SAMPLES;
            
            // Update current BPM and recalculate multipliers
            _currentBPM = _measuredBPM;
            updateBeatMultipliers();
        }
        
        _lastQuarterNoteTime = now;
    }
    
    _lastClockTime = now;
}

void BPMClockManager::handleMIDIStart() {
    if (_clockSource != CLOCK_EXTERNAL_MIDI) return;
    
    // Reset all tracking state
    _clockPulseCount = 0;
    _lastClockTime = micros();
    _lastQuarterNoteTime = _lastClockTime;
    _externalClockRunning = true;
    
    // Reset BPM history for clean start
    for (int i = 0; i < BPM_SMOOTH_SAMPLES; i++) {
        _bpmHistory[i] = _currentBPM;  // Use last known BPM
    }
    _bpmHistoryIndex = 0;
}

void BPMClockManager::handleMIDIStop() {
    if (_clockSource != CLOCK_EXTERNAL_MIDI) return;
    _externalClockRunning = false;
    // Note: We keep _currentBPM so LFOs/delays maintain last tempo
}

void BPMClockManager::handleMIDIContinue() {
    if (_clockSource != CLOCK_EXTERNAL_MIDI) return;
    _externalClockRunning = true;
    _lastClockTime = micros();
}

// ═════════════════════════════════════════════════════════════════
// Timing Conversions
// ═════════════════════════════════════════════════════════════════

float BPMClockManager::getFrequencyForMode(TimingMode mode) const {
    if (mode == TIMING_FREE) return -1.0f;  // Indicates free-running mode
    
    // Use pre-calculated multiplier
    float multiplier = _beatMultipliers[mode];
    
    // frequency = (BPM / 60) * multiplier
    // Example: 120 BPM, quarter note → (120/60) * 1.0 = 2.0 Hz
    return (_currentBPM / 60.0f) * multiplier;
}

float BPMClockManager::getTimeForMode(TimingMode mode) const {
    if (mode == TIMING_FREE) return -1.0f;  // Indicates free-running mode
    
    // Use pre-calculated multiplier
    float multiplier = _beatMultipliers[mode];
    
    // time_ms = (60000 / BPM) * multiplier
    // Example: 120 BPM, quarter note → (60000/120) * 1.0 = 500 ms
    return (60000.0f / _currentBPM) * multiplier;
}

// ═════════════════════════════════════════════════════════════════
// Status and Diagnostics
// ═════════════════════════════════════════════════════════════════

uint32_t BPMClockManager::getTimeSinceLastClock() const {
    if (!_externalClockRunning) return 0;
    return (micros() - _lastClockTime) / 1000;  // Convert to milliseconds
}

// ═════════════════════════════════════════════════════════════════
// Internal Helpers
// ═════════════════════════════════════════════════════════════════

void BPMClockManager::updateBeatMultipliers() {
    // Pre-calculate multipliers for all timing modes
    // This is CPU-efficient: only called when BPM changes
    
    _beatMultipliers[TIMING_FREE]   = 0.0f;      // N/A
    _beatMultipliers[TIMING_4_BARS] = 16.0f;     // 4 bars = 16 quarter notes
    _beatMultipliers[TIMING_2_BARS] = 8.0f;      // 2 bars = 8 quarter notes
    _beatMultipliers[TIMING_1_BAR]  = 4.0f;      // 1 bar = 4 quarter notes
    _beatMultipliers[TIMING_1_2]    = 2.0f;      // Half note
    _beatMultipliers[TIMING_1_4]    = 1.0f;      // Quarter note (reference)
    _beatMultipliers[TIMING_1_8]    = 0.5f;      // Eighth note
    _beatMultipliers[TIMING_1_16]   = 0.25f;     // Sixteenth note
    _beatMultipliers[TIMING_1_32]   = 0.125f;    // 32nd note
    _beatMultipliers[TIMING_1_4T]   = 0.6667f;   // Quarter triplet (2/3)
    _beatMultipliers[TIMING_1_8T]   = 0.3333f;   // Eighth triplet (1/3)
    _beatMultipliers[TIMING_1_16T]  = 0.1667f;   // Sixteenth triplet (1/6)
}

float BPMClockManager::getBeatMultiplier(TimingMode mode) const {
    if (mode < 0 || mode >= NUM_TIMING_MODES) return 1.0f;
    return _beatMultipliers[mode];
}