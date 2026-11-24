#include <Arduino.h>
#include "Hardware.h"

// ESP32 Dev Module; install Espressif Arduino core 2.0.17.
// Copy/open this entire sketch folder, including Hardware.cpp and src/.
// All GPIO, watchdog, pulse capture and background tasks live in Hardware.cpp.
void setup() {
    water::hardware::begin();
}

void loop() {
    // Dedicated tasks own control, logging/display and optional networking.
    delay(1000);
}
