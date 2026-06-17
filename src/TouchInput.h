/**
 * TouchInput.h
 *
 * Capacitive/resistive touch input for ILI9341 + FT6206.
 *
 * Gesture detection: tap, hold, swipe (up/down/left/right).
 * Hit-test helpers for parameter rows and buttons.
 *
 * IMPORTANT — implementations live in TouchInput.cpp.
 * Do NOT put function bodies in this header; doing so causes
 * "multiple definition" linker errors when more than one .cpp
 * file includes this header.
 */

#pragma once
#include <Arduino.h>
#include <Wire.h>


#define MICRODEXED_ADAFRUIT_DISPLAY  // Adafruit capacitive touch (FT6206)
#include "Adafruit_FT6206.h"


class TouchInput {
public:
    // -------------------------------------------------------------------------
    // Touch point
    // -------------------------------------------------------------------------
    struct Point {
        int16_t x;
        int16_t y;
        bool    valid;

        Point()                        : x(0),  y(0),  valid(false) {}
        Point(int16_t _x, int16_t _y) : x(_x), y(_y), valid(true)  {}
    };

    // -------------------------------------------------------------------------
    // Gesture types
    // -------------------------------------------------------------------------
    enum Gesture {
        GESTURE_NONE,
        GESTURE_TAP,           // Short touch, minimal movement
        GESTURE_HOLD,          // Touch held > HOLD_MIN_DURATION ms
        GESTURE_SWIPE_UP,
        GESTURE_SWIPE_DOWN,
        GESTURE_SWIPE_LEFT,
        GESTURE_SWIPE_RIGHT
    };

    // -------------------------------------------------------------------------
    // Timing / distance thresholds
    // -------------------------------------------------------------------------
    static constexpr uint32_t TAP_MAX_DURATION   = 300;   // ms
    static constexpr uint32_t HOLD_MIN_DURATION  = 500;   // ms
    static constexpr int16_t  SWIPE_MIN_DISTANCE = 50;    // pixels (signed, matches dx/dy type)
    static constexpr uint32_t SWIPE_MAX_DURATION = 500;   // ms

    // -------------------------------------------------------------------------
    // API
    // -------------------------------------------------------------------------
    TouchInput();

    /** Initialise touch controller. Returns true on success. */
    bool begin();

    /** Poll touch hardware — call at 100+ Hz in main loop. */
    void update();

    /** True while the screen is being touched. */
    bool isTouched() const { return _isTouched; }

    /** Current touch position (valid flag false when not touched). */
    Point   getTouchPoint() const;

    /**
     * Return and consume the last detected gesture.
     * Returns GESTURE_NONE if no gesture since last call.
     */
    Gesture getGesture();

    /**
     * The screen position where the touch that produced the last gesture began.
     * Use this (rather than getTouchPoint) for swipe hit-testing: the finger
     * started on the target widget, then moved up/down to produce a swipe.
     * getTouchPoint() returns the lifted position, which may be off the widget.
     */
    Point getGestureStart() const { return _gestureStart; }

    /**
     * Returns true if the current touch point is inside the given rect.
     * (x, y) = top-left corner, (w, h) = dimensions.
     */
    bool hitTest(int16_t x, int16_t y, int16_t w, int16_t h) const;

private:
    // -------------------------------------------------------------------------
    // Hardware driver
    // -------------------------------------------------------------------------
    Adafruit_FT6206 _touchController;


    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------
    bool    _isTouched;
    Point   _currentPoint;
    Point   _lastPoint;

    Gesture  _detectedGesture;
    Point    _gestureStart;
    uint32_t _touchStartTime;
    uint32_t _touchEndTime;

    // -------------------------------------------------------------------------
    // Private helpers — defined in TouchInput.cpp
    // -------------------------------------------------------------------------

    /** Classify a completed touch into a Gesture. */
    void  detectGesture();

    /**
     * Map raw hardware coordinates to screen coordinates.
     * Adjust calibration values in TouchInput.cpp if touch feels misaligned.
     */
    Point mapCoordinates(int16_t rawX, int16_t rawY);
};
