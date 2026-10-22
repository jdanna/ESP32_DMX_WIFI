// TCA9554PWR I2C expander driving the eight relays.
#pragma once

#include <stdint.h>

bool relaysBegin();
// Writes the relay bitmask (bit 0 = relay 1). Returns false if the expander didn't ACK.
bool relaysWrite(uint8_t mask);
