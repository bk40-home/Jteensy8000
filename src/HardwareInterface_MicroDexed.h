/**
 * HardwareInterface_MicroDexed.h
 *
 * Hardware abstraction for MicroDexed-style hardware:
 *   - 2× rotary encoders with pushbuttons (left/right)
 *   - ILI9341 320×240 TFT (SPI)
 *   - PCM5102A I2S DAC
 *
 * ENCODER IMPLEMENTATION — POLLED GRAY-CODE (no interrupts):
 *   Encoders on pins 28-32 MUST NOT use attachInterrupt() on Teensy 4.1.
 *   These pins (GPIO6/7 bank) have a silicon bug: the ICR register index
 *   calculation overflows for pins >= 31, corrupting adjacent memory -> crash.
 *   The polled Gray-code decoder is equivalent in resolution and effectively
 *   zero latency when update() is called each loop() at ~1 kHz+.
 *
 * BUTTON DEBOUNCING — software state machine in update():
 *   50 ms debounce + 1500 ms long-press threshold.
 *   Press events are edge-detected and consumed on first getButtonPress() call.
 */

#pragma once
#include <Arduino.h>
// NOTE: EncoderTool is deliberately NOT included.
//       It calls attachInterrupt() internally, which crashes on pins 28-32.

class HardwareInterface_MicroDexed {
public:
    // -------------------------------------------------------------------------
    // Encoder identifiers
    // -------------------------------------------------------------------------
    enum EncoderID : uint8_t {
        ENC_LEFT  = 0,  // Navigation: page select, back, cancel
        ENC_RIGHT = 1,  // Value: increment / decrement selected parameter
        ENC_COUNT = 2
    };

    // -------------------------------------------------------------------------
    // Button press types
    // -------------------------------------------------------------------------
    enum ButtonPress : uint8_t {
        PRESS_NONE  = 0,
        PRESS_SHORT = 1,  // Released before LONG_PRESS_MS
        PRESS_LONG  = 2   // Fires once after LONG_PRESS_MS while still held
    };

    HardwareInterface_MicroDexed();

    /** Initialize pins.  Call once in setup() before any reads. */
    void begin();

    /**
     * Poll encoders and button state machines.
     * Call every loop() iteration — needs > 500 Hz for reliable quadrature decode.
     * Typical cost: ~5 µs for two encoders + two buttons.
     */
    void update();

    /**
     * Return rotation delta (detents) since last call.
     * Positive = CW, negative = CCW.
     * Thread-safe: accumulated between calls, safe to read at any rate.
     */
    int getEncoderDelta(EncoderID encoder);

    /**
     * Return and consume any pending button press.
     * Returns PRESS_NONE when queue is empty.
     * Each press fires exactly once.
     */
    ButtonPress getButtonPress(EncoderID encoder);

    /**
     * Level-detect: true while button is physically held.
     * Does NOT consume the pending press event.
     */
    bool isButtonHeld(EncoderID encoder);

    /**
     * Reset accumulated encoder count to zero.
     * Call on mode/page transitions to prevent carry-over motion.
     */
    void resetEncoder(EncoderID encoder);

private:
    // -------------------------------------------------------------------------
    // Pin definitions — confirmed working wiring (Teensy 4.1)
    //
    // CRITICAL: Do NOT call attachInterrupt() on these pins.
    //   GPIO6/7 ICR register overflow bug corrupts memory on Teensy 4.1 for
    //   pins >= 31. Polled decode works correctly on all pins.
    // -------------------------------------------------------------------------
    static constexpr uint8_t ENC_L_A_PIN  = 31;
    static constexpr uint8_t ENC_L_B_PIN  = 32;
    static constexpr uint8_t ENC_L_SW_PIN = 30;

    static constexpr uint8_t ENC_R_A_PIN  = 29;
    static constexpr uint8_t ENC_R_B_PIN  = 28;
    static constexpr uint8_t ENC_R_SW_PIN = 25;

    // -------------------------------------------------------------------------
    // Polled Gray-code encoder
    //
    // Standard quadrature encoder generates 4 AB transitions per detent.
    // We accumulate raw quarter-steps and return whole detents from delta(),
    // so the API matches EncoderTool CountMode::quarter (1 count per click).
    //
    // Gray-code table maps [prev_AB << 2 | curr_AB] -> step direction:
    //   +1 = valid CW,  -1 = valid CCW,  0 = no movement or invalid transition
    // -------------------------------------------------------------------------
    struct PollEncoder {
        uint8_t pinA    = 0;
        uint8_t pinB    = 0;
        uint8_t lastAB  = 0;   // Previous 2-bit AB state
        int32_t rawCount = 0;  // Accumulated quarter-steps (not yet divided)

        // Lookup table — 16 entries cover all AB -> AB combinations.
        // Kept as a static array to avoid a copy per instance.
        static const int8_t kTable[16];

        void begin(uint8_t a, uint8_t b) {
            pinA = a;
            pinB = b;
            pinMode(a, INPUT_PULLUP);
            pinMode(b, INPUT_PULLUP);
            // Sample current state so we don't generate a spurious step on
            // the first tick() call.
            lastAB = (uint8_t)((digitalRead(a) << 1) | digitalRead(b));
        }

        /** Read pins, accumulate quarter-steps.  Call every loop(). */
        void tick() {
            const uint8_t ab = (uint8_t)((digitalRead(pinA) << 1) | digitalRead(pinB));
            rawCount += kTable[((lastAB << 2) | ab) & 0xF];
            lastAB = ab;
        }

        /**
         * Return whole detents moved since last call.
         * Remainder sub-detent motion is retained for next call —
         * prevents half-step jitter from causing spurious parameter changes.
         */
        int delta() {
            const int32_t detents = rawCount / 4;
            rawCount -= detents * 4;   // keep sub-detent remainder
            return (int)detents;
        }
    };

    PollEncoder _encoders[ENC_COUNT];

    // -------------------------------------------------------------------------
    // Button state machine per encoder
    // -------------------------------------------------------------------------
    struct ButtonState {
        bool        current      = false;      // Current physical level
        bool        lastState    = false;      // Previous level (edge detect)
        uint32_t    pressTime    = 0;          // millis() at falling edge
        uint32_t    releaseTime  = 0;          // millis() at rising edge
        ButtonPress pendingPress = PRESS_NONE; // Queued event, cleared on read
        bool        longFired    = false;      // True after long-press fires (prevents repeat)
    };

    ButtonState _buttons[ENC_COUNT];

    // Button timing thresholds
    static constexpr uint32_t DEBOUNCE_MS   = 50;    // Ignore bounces < 50 ms
    static constexpr uint32_t LONG_PRESS_MS = 1500;  // Hold >= 1.5 s -> PRESS_LONG

    /** Update one button's state machine.  Called from update(). */
    void updateButton(EncoderID id, uint8_t pin);
};
