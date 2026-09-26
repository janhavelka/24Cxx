#pragma once

// Example defaults only. Change these to the pins and pull-ups on your board.
#ifndef EEPROM24CXX_I2C_SDA
#define EEPROM24CXX_I2C_SDA 8
#endif
#ifndef EEPROM24CXX_I2C_SCL
#define EEPROM24CXX_I2C_SCL 9
#endif
#ifndef EEPROM24CXX_I2C_FREQUENCY_HZ
#define EEPROM24CXX_I2C_FREQUENCY_HZ 100000
#endif
#ifndef EEPROM24CXX_I2C_TIMEOUT_MS
#define EEPROM24CXX_I2C_TIMEOUT_MS 50
#endif
#ifndef EEPROM24CXX_SERIAL_BAUD
#define EEPROM24CXX_SERIAL_BAUD 115200
#endif
namespace board {
inline constexpr int I2C_SDA = EEPROM24CXX_I2C_SDA;
inline constexpr int I2C_SCL = EEPROM24CXX_I2C_SCL;
// Conservative bring-up speed; select voltage-dependent limits from your part.
inline constexpr unsigned I2C_FREQUENCY_HZ = EEPROM24CXX_I2C_FREQUENCY_HZ;
inline constexpr unsigned I2C_TIMEOUT_MS = EEPROM24CXX_I2C_TIMEOUT_MS;
inline constexpr unsigned SERIAL_BAUD = EEPROM24CXX_SERIAL_BAUD;
static_assert(I2C_SDA >= 0 && I2C_SCL >= 0 && I2C_SDA != I2C_SCL, "Select distinct valid I2C pins");
static_assert(I2C_FREQUENCY_HZ > 0 && SERIAL_BAUD > 0, "Bus and serial rates must be positive");
static_assert(I2C_TIMEOUT_MS > 0 && I2C_TIMEOUT_MS <= 1000, "I2C timeout must be 1..1000 ms");
}  // namespace board
