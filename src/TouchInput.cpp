/**
 * TouchInput.cpp
 *
 * Implementation of touch input and gesture recognition.
 *
 * CRASH FIX (2026-02):
 *   Removed Serial.print() calls from update().  They were firing on EVERY
 *   loop() iteration while the screen was touched (~1 kHz+), flooding the
 *   USB-serial TX buffer faster than the host could drain it.  The result is
 *   USB descriptor re-enumeration failure -> the Teensy appears to power-cycle.
 *   To re-enable touch coordinate logging, uncomment the DEBUG block below and
 *   only do it once per finger-down event, not every poll.
 */

#include "TouchInput.h"

// ============================================================================
// Constructor
// ============================================================================

TouchInput::TouchInput()
    : _isTouched(false)
    , _detectedGesture(GESTURE_NONE)
    , _touchStartTime(0)
    , _touchEndTime(0)
{}

// ============================================================================
// begin() — hardware initialisation
// ============================================================================

bool TouchInput::begin() {
    Serial.print("TouchInput: Initializing FT6206 (capacitive)... ");
    if (!_touchController.begin(40)) {
        Serial.println("FAILED - Not found on I2C");
        return false;
    }
    Serial.println("SUCCESS");
    return true;
}

// ============================================================================
// update() — poll hardware, maintain state, fire gesture detection.
//
// PERFORMANCE RULES FOR THIS FUNCTION:
//   - Called every loop() at ~1 kHz or faster.
//   - NO Serial.print() here — USB TX flood will crash USB enumeration.
//   - NO blocking calls (delay, Wire timeout loops).
//   - Keep total execution under ~50 us.
// ============================================================================

void TouchInput::update() {
    bool    nowTouched = false;
    int16_t rawX = 0, rawY = 0;

    if (_touchController.touched()) {
        TS_Point p = _touchController.getPoint();
        rawX       = p.x;
        rawY       = p.y;
        nowTouched = true;
    }

    if (nowTouched) {
        _currentPoint = mapCoordinates(rawX, rawY);

        if (!_isTouched) {
            // ---- Finger just landed ----
            _isTouched       = true;
            _gestureStart    = _currentPoint;
            _touchStartTime  = millis();
            _detectedGesture = GESTURE_NONE;

            // DEBUG: log ONCE per touch-down only.
            // Uncomment only during calibration - leave commented for production.
            // Serial.printf("Touch DOWN  raw(%d,%d) -> screen(%d,%d)\n",
            //               rawX, rawY, _currentPoint.x, _currentPoint.y);
        }
        _lastPoint = _currentPoint;

    } else {
        if (_isTouched) {
            // ---- Finger just lifted ----
            _isTouched    = false;
            _touchEndTime = millis();
            detectGesture();
        }
    }
}

// ============================================================================
// Public accessors
// ============================================================================

TouchInput::Point TouchInput::getTouchPoint() const {
    return _currentPoint;
}

TouchInput::Gesture TouchInput::getGesture() {
    Gesture g        = _detectedGesture;
    _detectedGesture = GESTURE_NONE;   // Consume — fires once per gesture only
    return g;
}

bool TouchInput::hitTest(int16_t x, int16_t y, int16_t w, int16_t h) const {
    if (!_isTouched) return false;
    return (_currentPoint.x >= x && _currentPoint.x < x + w &&
            _currentPoint.y >= y && _currentPoint.y < y + h);
}

// ============================================================================
// detectGesture() — classify a completed touch into a Gesture enum value.
//   Called exactly once per lift event from update().
// ============================================================================

void TouchInput::detectGesture() {
    const uint32_t duration = _touchEndTime - _touchStartTime;

    // Signed 16-bit displacement — fits the 320x240 screen without overflow
    const int16_t dx       = _lastPoint.x - _gestureStart.x;
    const int16_t dy       = _lastPoint.y - _gestureStart.y;
    // sqrtf result always <=~385 px on this screen, fits int16_t safely
    const int16_t distance = (int16_t)sqrtf((float)(dx * dx + dy * dy));

    if (distance < 10 && duration < TAP_MAX_DURATION) {
        _detectedGesture = GESTURE_TAP;

    } else if (distance < 10 && duration >= HOLD_MIN_DURATION) {
        _detectedGesture = GESTURE_HOLD;

    } else if (distance >= SWIPE_MIN_DISTANCE && duration < SWIPE_MAX_DURATION) {
        if (abs(dx) > abs(dy)) {
            _detectedGesture = (dx > 0) ? GESTURE_SWIPE_RIGHT : GESTURE_SWIPE_LEFT;
        } else {
            _detectedGesture = (dy > 0) ? GESTURE_SWIPE_DOWN  : GESTURE_SWIPE_UP;
        }

    } else {
        _detectedGesture = GESTURE_NONE;
    }
}

// ============================================================================
// mapCoordinates() — raw FT6206 portrait coordinates -> landscape screen pixels
//
// FT6206 native portrait resolution: 240 wide x 320 tall.
// In landscape (ILI9341 rotation=3), both axes are inverted.
//
// Confirmed by hardware measurement:
//   raw (  0,   0) -> screen top-right    (320, 240)
//   raw (239, 319) -> screen bottom-left  (  0,   0)
// ============================================================================

TouchInput::Point TouchInput::mapCoordinates(int16_t rawX, int16_t rawY) {
    // Landscape rotation=3: swap axes, invert X mapping
    const int16_t x = (int16_t)(320 - rawY);   // raw Y (0-319) -> screen X (320..0)
    const int16_t y = rawX;                      // raw X (0-239) -> screen Y (0..239)
    return Point(x, y);
}
