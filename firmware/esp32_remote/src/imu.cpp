#include <Arduino.h>
#include <Wire.h>

#include "imu.h"

// Waveshare ESP32-S3-Touch-LCD-2.8 V2
#define IMU_SDA   11
#define IMU_SCL   10
#define IMU_ADDR  0x6B

// QMI8658 registers
#define QMI8658_WHO_AM_I  0x00
#define QMI8658_CTRL1     0x02
#define QMI8658_CTRL2     0x03
#define QMI8658_CTRL7     0x08

#define QMI8658_AX_L      0x35

static TwoWire imuWire = TwoWire(1);
static bool imuReady = false;


// -----------------------------------------------------------------------------
// Low-level I2C
// -----------------------------------------------------------------------------

static bool writeRegister(uint8_t reg, uint8_t value) {
    imuWire.beginTransmission(IMU_ADDR);
    imuWire.write(reg);
    imuWire.write(value);

    return imuWire.endTransmission() == 0;
}


static bool readRegister(uint8_t reg, uint8_t &value) {
    imuWire.beginTransmission(IMU_ADDR);
    imuWire.write(reg);

    if (imuWire.endTransmission(false) != 0) {
        return false;
    }

    if (imuWire.requestFrom(
        (uint8_t)IMU_ADDR,
        (uint8_t)1
    ) != 1) {
        return false;
    }

    value = imuWire.read();

    return true;
}


static bool readRegisters(
    uint8_t reg,
    uint8_t *buffer,
    size_t length
) {
    imuWire.beginTransmission(IMU_ADDR);
    imuWire.write(reg);

    if (imuWire.endTransmission(false) != 0) {
        return false;
    }

    size_t received = imuWire.requestFrom(
        (uint8_t)IMU_ADDR,
        length
    );

    if (received != length) {
        return false;
    }

    for (size_t i = 0; i < length; i++) {
        buffer[i] = imuWire.read();
    }

    return true;
}


// -----------------------------------------------------------------------------
// Initialization
// -----------------------------------------------------------------------------

bool initImu() {
    Serial.println("IMU: initializing QMI8658");

    imuWire.begin(
        IMU_SDA,
        IMU_SCL,
        400000
    );

    delay(20);

    // Verify that something answers at 0x6B.
    imuWire.beginTransmission(IMU_ADDR);

    if (imuWire.endTransmission() != 0) {
        Serial.println(
            "IMU: no device found at 0x6B"
        );

        return false;
    }

    Serial.println(
        "IMU: device found at 0x6B"
    );

    uint8_t whoAmI = 0;

    if (!readRegister(
        QMI8658_WHO_AM_I,
        whoAmI
    )) {
        Serial.println(
            "IMU: failed to read WHO_AM_I"
        );

        return false;
    }

    Serial.printf(
        "IMU: WHO_AM_I = 0x%02X\n",
        whoAmI
    );

    /*
     * Match the useful parts of Waveshare's
     * initialization.
     *
     * CTRL1:
     *   auto-increment enabled
     *   oscillator enabled
     */
    if (!writeRegister(
        QMI8658_CTRL1,
        0x40
    )) {
        Serial.println(
            "IMU: CTRL1 write failed"
        );

        return false;
    }

    /*
     * Accelerometer:
     *
     * ±4 g range
     * 31.25 Hz output rate
     *
     * Range bits:
     *   ACC_RANGE_4G = 1 << 4
     *
     * ODR:
     *   31.25 Hz = 8
     */
    if (!writeRegister(
        QMI8658_CTRL2,
        0x18
    )) {
        Serial.println(
            "IMU: CTRL2 write failed"
        );

        return false;
    }

    /*
     * Enable accelerometer only.
     *
     * We don't need the gyro for pickup detection.
     * Keeping it disabled also reduces power.
     */
    if (!writeRegister(
        QMI8658_CTRL7,
        0x01
    )) {
        Serial.println(
            "IMU: CTRL7 write failed"
        );

        return false;
    }

    delay(50);

    imuReady = true;

    Serial.println("IMU: ready");

    return true;
}


// -----------------------------------------------------------------------------
// Accelerometer
// -----------------------------------------------------------------------------

bool readImuAcceleration(
    float &x,
    float &y,
    float &z
) {
    if (!imuReady) {
        return false;
    }

    uint8_t data[6];

    if (!readRegisters(
        QMI8658_AX_L,
        data,
        sizeof(data)
    )) {
        return false;
    }

    int16_t rawX =
        (int16_t)(
            ((uint16_t)data[1] << 8) |
            data[0]
        );

    int16_t rawY =
        (int16_t)(
            ((uint16_t)data[3] << 8) |
            data[2]
        );

    int16_t rawZ =
        (int16_t)(
            ((uint16_t)data[5] << 8) |
            data[4]
        );

    // ±4 g scale.
    constexpr float SCALE =
        4.0f / 32768.0f;

    x = rawX * SCALE;
    y = rawY * SCALE;
    z = rawZ * SCALE;

    return true;
}