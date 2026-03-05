/*!
 * @file Adafruit_FT6206.cpp
 *
 * FT6206/FT6236 capacitive touch controller driver (I2C).
 *
 * Original: Limor Fried/Ladyada, Adafruit Industries — MIT licence.
 *
 * Changes for JT-8000:
 *   - Removed #ifdef CAPACITIVE_TOUCH_DISPLAY guard.
 *     The guard was preventing the .cpp from compiling at all unless that
 *     macro was defined in config.h, causing "undefined reference" linker
 *     errors for Adafruit_FT6206::begin(), touched(), getPoint() etc.
 *     TouchInput.h already guards usage with #ifdef USE_CAPACITIVE_TOUCH,
 *     so this file is only pulled into the build when touch is needed.
 *   - Removed SAM3X8E Wire1 alias (not relevant to Teensy 4.1).
 *   - readData(): local constants promoted to static constexpr for clarity.
 */

#include "Arduino.h"
#include "Adafruit_FT6206.h"
#include <Wire.h>

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

Adafruit_FT6206::Adafruit_FT6206() {
    touches = 0;
}

// ---------------------------------------------------------------------------
// begin() — I2C init and chip identification
// ---------------------------------------------------------------------------

bool Adafruit_FT6206::begin(uint8_t thresh) {
    Wire.begin();

    // Set touch detection threshold (higher = less sensitive)
    writeRegister8(FT62XX_REG_THRESHHOLD, thresh);

    // Verify vendor ID
    if (readRegister8(FT62XX_REG_VENDID) != FT62XX_VENDID) {
        return false;
    }

    // Verify chip ID — FT6206, FT6236, or FT6236U
    const uint8_t id = readRegister8(FT62XX_REG_CHIPID);
    if (id != FT6206_CHIPID && id != FT6236_CHIPID && id != FT6236U_CHIPID) {
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// touched() — returns number of active touch points (0, 1, or 2)
// ---------------------------------------------------------------------------

uint8_t Adafruit_FT6206::touched() {
    readData();
    return touches;
}

// ---------------------------------------------------------------------------
// getPoint() — retrieve touch coordinates for point n (0 or 1)
// ---------------------------------------------------------------------------

TS_Point Adafruit_FT6206::getPoint(uint8_t n) {
    if (touches == 0 || n >= MAX_NUM_TOUCH_POINTS) {
        return TS_Point(0, 0, 0);   // No touch / out of range
    }
    return TS_Point(touchX[n], touchY[n], 1);
}

// ---------------------------------------------------------------------------
// readData() — burst-read all touch data over I2C
// ---------------------------------------------------------------------------

void Adafruit_FT6206::readData() {
    // Start reading from register 0x02 (touch count)
    Wire.beginTransmission(FT62XX_ADDR);
    Wire.write((byte)0x02);
    Wire.endTransmission();

    // 6 bytes per touch point + 3 header bytes
    static constexpr uint8_t BYTES_PER_POINT = 6;
    static constexpr uint8_t BYTES_TOTAL =
        3 + MAX_NUM_TOUCH_POINTS * BYTES_PER_POINT;

    uint8_t buf[BYTES_TOTAL];
    Wire.requestFrom((byte)FT62XX_ADDR, (byte)BYTES_TOTAL);
    for (uint8_t i = 0; i < BYTES_TOTAL; ++i) {
        buf[i] = Wire.read();
    }

    // Register layout (offset from 0x02):
    //   [0]      touch count
    //   [1+i*6]  event flag (hi nibble) | x_hi (lo nibble)
    //   [2+i*6]  x_lo
    //   [3+i*6]  touch ID  (hi nibble) | y_hi (lo nibble)
    //   [4+i*6]  y_lo
    //   [5+i*6]  touch weight  (unused)
    //   [6+i*6]  touch area    (unused)

    touches = buf[0];
    if (touches > MAX_NUM_TOUCH_POINTS) {
        touches = 0;   // Sanity-clamp; never more than hardware supports
    }

    for (uint8_t i = 0; i < MAX_NUM_TOUCH_POINTS; ++i) {
        const uint8_t off = i * BYTES_PER_POINT;
        touchX[i]  = (uint16_t)(buf[1 + off] & 0x0F) << 8 | buf[2 + off];
        touchY[i]  = (uint16_t)(buf[3 + off] & 0x0F) << 8 | buf[4 + off];
        touchID[i] = buf[3 + off] >> 4;
    }
}

// ---------------------------------------------------------------------------
// Low-level I2C register access
// ---------------------------------------------------------------------------

uint8_t Adafruit_FT6206::readRegister8(uint8_t reg) {
    Wire.beginTransmission(FT62XX_ADDR);
    Wire.write((byte)reg);
    Wire.endTransmission();

    Wire.requestFrom((byte)FT62XX_ADDR, (byte)1);
    return Wire.read();
}

void Adafruit_FT6206::writeRegister8(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(FT62XX_ADDR);
    Wire.write((byte)reg);
    Wire.write((byte)val);
    Wire.endTransmission();
}

// ---------------------------------------------------------------------------
// TS_Point
// ---------------------------------------------------------------------------

TS_Point::TS_Point()                              : x(0),   y(0),   z(0)   {}
TS_Point::TS_Point(int16_t _x, int16_t _y, int16_t _z) : x(_x), y(_y), z(_z) {}

bool TS_Point::operator==(TS_Point p) { return p.x == x && p.y == y && p.z == z; }
bool TS_Point::operator!=(TS_Point p) { return p.x != x || p.y != y || p.z != z; }
