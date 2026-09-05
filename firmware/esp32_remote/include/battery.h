#pragma once
#include <stdint.h>

// Approximate single-cell Li-ion curve; interpolate rather than rounding down
// an entire voltage band. Voltage alone cannot measure remaining charge exactly.
inline uint8_t batteryVoltageToPercent(float volts) {
    static constexpr float voltage[] = {3.20f, 3.30f, 3.40f, 3.50f, 3.60f,
                                        3.70f, 3.80f, 3.90f, 4.00f, 4.10f, 4.20f};
    static constexpr uint8_t percent[] = {0, 3, 8, 15, 25, 40, 55, 70, 80, 90, 100};
    if (!(volts > voltage[0])) return 0;
    for (unsigned i = 1; i < sizeof(percent); ++i) {
        if (volts < voltage[i]) {
            const float fraction = (volts - voltage[i - 1]) /
                                   (voltage[i] - voltage[i - 1]);
            return static_cast<uint8_t>(percent[i - 1] +
                fraction * (percent[i] - percent[i - 1]) + 0.5f);
        }
    }
    return 100;
}
