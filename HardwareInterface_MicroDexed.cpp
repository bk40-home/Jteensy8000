/**
 * HardwareInterface_MicroDexed.cpp
 *
 * Polled Gray-code encoder + debounced button implementation.
 * No interrupts — safe on Teensy 4.1 GPIO6/7 pins (28-32).
 *
 * Encoder resolution: 4 quarter-steps per detent.
 * delta() returns whole detents, sub-detent remainder is retained.
 *
 * Button: 50 ms debounce, 1.5 s long-press.
 * PRESS_LONG fires once while still held (doesn't wait for release).
 * PRESS_SHORT fires on release (after debounce, before long-press threshold).
 */

#include "HardwareInterface_MicroDexed.h"

// ---------------------------------------------------------------------------
// Gray-code transition table
// Index = (prevAB << 2) | currAB  (4 bits, 16 entries)
// +1 = CW step, -1 = CCW step, 0 = no motion or illegal transition
// ---------------------------------------------------------------------------
const int8_t HardwareInterface_MicroDexed::PollEncoder::kTable[16] = {
//  00  01  10  11   <- currAB
     0, -1, +1,  0,  // prevAB = 00
    +1,  0,  0, -1,  // prevAB = 01
    -1,  0,  0, +1,  // prevAB = 10
     0, +1, -1,  0   // prevAB = 11
};

// ---------------------------------------------------------------------------
// Constructor — button states zero-initialised by member defaults in header
// ---------------------------------------------------------------------------
HardwareInterface_MicroDexed::HardwareInterface_MicroDexed() {
    // ButtonState and PollEncoder use C++ member default-initialisers.
    // Nothing extra needed here.
}

// ---------------------------------------------------------------------------
// begin() — configure pins, sample initial encoder state
// ---------------------------------------------------------------------------
void HardwareInterface_MicroDexed::begin() {
    // Left encoder (navigation)
    _encoders[ENC_LEFT].begin(ENC_L_A_PIN, ENC_L_B_PIN);
    pinMode(ENC_L_SW_PIN, INPUT_PULLUP);

    // Right encoder (value)
    _encoders[ENC_RIGHT].begin(ENC_R_A_PIN, ENC_R_B_PIN);
    pinMode(ENC_R_SW_PIN, INPUT_PULLUP);

    Serial.println("HW: encoders polled (no interrupts), buttons ready");
}

// ---------------------------------------------------------------------------
// update() — must be called every loop() iteration
// ---------------------------------------------------------------------------
void HardwareInterface_MicroDexed::update() {
    // Tick both encoders — reads pins, updates rawCount
    _encoders[ENC_LEFT].tick();
    _encoders[ENC_RIGHT].tick();

    // Update button state machines
    updateButton(ENC_LEFT,  ENC_L_SW_PIN);
    updateButton(ENC_RIGHT, ENC_R_SW_PIN);
}

// ---------------------------------------------------------------------------
// getEncoderDelta() — return detents moved since last call
// ---------------------------------------------------------------------------
int HardwareInterface_MicroDexed::getEncoderDelta(EncoderID encoder) {
    if (encoder >= ENC_COUNT) return 0;
    return _encoders[encoder].delta();
}

// ---------------------------------------------------------------------------
// getButtonPress() — return and consume pending press event
// ---------------------------------------------------------------------------
HardwareInterface_MicroDexed::ButtonPress
HardwareInterface_MicroDexed::getButtonPress(EncoderID encoder) {
    if (encoder >= ENC_COUNT) return PRESS_NONE;
    ButtonPress p = _buttons[encoder].pendingPress;
    _buttons[encoder].pendingPress = PRESS_NONE;  // consume
    return p;
}

// ---------------------------------------------------------------------------
// isButtonHeld() — level detect, does not consume pending press
// ---------------------------------------------------------------------------
bool HardwareInterface_MicroDexed::isButtonHeld(EncoderID encoder) {
    if (encoder >= ENC_COUNT) return false;
    return _buttons[encoder].current;
}

// ---------------------------------------------------------------------------
// resetEncoder() — clear accumulated count on page/mode change
// ---------------------------------------------------------------------------
void HardwareInterface_MicroDexed::resetEncoder(EncoderID encoder) {
    if (encoder >= ENC_COUNT) return;
    _encoders[encoder].rawCount = 0;
}

// ---------------------------------------------------------------------------
// updateButton() — debounce + short/long press state machine
//
// State machine:
//   IDLE -> PRESSED (falling edge, after DEBOUNCE_MS since last release)
//   PRESSED -> LONG FIRE (still held, >= LONG_PRESS_MS) -> sets PRESS_LONG once
//   PRESSED -> RELEASED (rising edge, before LONG_PRESS_MS) -> sets PRESS_SHORT
// ---------------------------------------------------------------------------
void HardwareInterface_MicroDexed::updateButton(EncoderID id, uint8_t pin) {
    ButtonState& btn = _buttons[id];
    const uint32_t now = millis();

    // Buttons are INPUT_PULLUP — LOW when pressed
    const bool pressed = (digitalRead(pin) == LOW);
    btn.current = pressed;

    if (pressed && !btn.lastState) {
        // --- Falling edge: button just pressed ---
        // Apply debounce: ignore if released very recently (contact bounce)
        if ((now - btn.releaseTime) >= DEBOUNCE_MS) {
            btn.pressTime  = now;
            btn.longFired  = false;  // Reset long-press guard for new press
        }

    } else if (!pressed && btn.lastState) {
        // --- Rising edge: button just released ---
        const uint32_t held = now - btn.pressTime;

        // Only count if held long enough to be real (not a bounce on release)
        if (held >= DEBOUNCE_MS) {
            btn.releaseTime = now;

            // Short press: released before long-press threshold, and long
            // event hasn't already fired (which would make this a long-press release)
            if (held < LONG_PRESS_MS && !btn.longFired) {
                btn.pendingPress = PRESS_SHORT;
            }
            // If longFired is true, we silently absorb the release —
            // the action already fired when the threshold was crossed.
        }

    } else if (pressed && btn.lastState) {
        // --- Button held: check for long-press threshold ---
        // Fires exactly once per hold (longFired guard prevents repeat)
        if (!btn.longFired && (now - btn.pressTime) >= LONG_PRESS_MS) {
            btn.pendingPress = PRESS_LONG;
            btn.longFired    = true;  // Prevent repeat fires during continued hold
        }
    }

    btn.lastState = pressed;
}
