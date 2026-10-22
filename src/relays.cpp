#include "relays.h"

#include <Wire.h>
#include "board.h"

enum : uint8_t { REG_OUTPUT = 0x01, REG_CONFIG = 0x03 };

static bool writeReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(TCA9554_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool relaysBegin() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);
  // Drive outputs low before switching the pins to outputs so nothing clicks on at boot.
  return writeReg(REG_OUTPUT, 0x00) && writeReg(REG_CONFIG, 0x00);
}

bool relaysWrite(uint8_t mask) { return writeReg(REG_OUTPUT, mask); }
