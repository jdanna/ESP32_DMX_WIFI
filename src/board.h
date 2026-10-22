// Pin map for the Waveshare ESP32-S3-ETH-8DI-8RO / ESP32-S3-POE-ETH-8DI-8RO.
// Source: https://www.waveshare.com/wiki/ESP32-S3-ETH-8DI-8RO
#pragma once

// W5500 Ethernet (SPI). The reset line is not routed to a GPIO.
#define PIN_ETH_SCK   15
#define PIN_ETH_MISO  14
#define PIN_ETH_MOSI  13
#define PIN_ETH_CS    16
#define PIN_ETH_INT   12
#define PIN_ETH_RST   -1

// I2C bus shared by the TCA9554PWR relay expander and the PCF85063 RTC.
#define PIN_I2C_SDA   42
#define PIN_I2C_SCL   41
#define TCA9554_ADDR  0x20   // relays 1-8 on expander pins 0-7, active high

// Isolated RS485 port, used as the DMX input. The transceiver switches
// direction automatically from the TX line, so there is no DE/RE pin.
#define PIN_RS485_TX  17
#define PIN_RS485_RX  18

#define PIN_STATUS_LED   38     // single WS2812
#define PIN_BOOT_BTN  0      // hold 5 s for factory reset

#define RELAY_COUNT   8
