#pragma once

// Example defaults only. Change these to the pins and pull-ups on your board.
#ifndef EEPROM24CXX_I2C_SDA
#define EEPROM24CXX_I2C_SDA 8
#endif
#ifndef EEPROM24CXX_I2C_SCL
#define EEPROM24CXX_I2C_SCL 9
#endif
namespace board {
inline constexpr int I2C_SDA = EEPROM24CXX_I2C_SDA;
inline constexpr int I2C_SCL = EEPROM24CXX_I2C_SCL;
// Conservative bring-up speed; select voltage-dependent limits from your part.
inline constexpr unsigned I2C_FREQUENCY_HZ = 100000;
inline constexpr unsigned I2C_TIMEOUT_MS = 50;
inline constexpr unsigned SERIAL_BAUD = 115200;
}  // namespace board
