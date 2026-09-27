#pragma once

// Framework-specific GPIO adapter shared by Arduino-ESP32 and native ESP-IDF.
// The application must detach/delete its I2C controller before calling this,
// and recreate it only after recovery succeeds. No other task may own the pins.
#include <driver/gpio.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include "BoardConfig.h"
#include "BusRecovery.h"

namespace eeprom24cxx_cli {
struct Esp32RecoveryPins {
  void sda(bool release) { (void)gpio_set_level(static_cast<gpio_num_t>(board::I2C_SDA), release ? 1 : 0); }
  void scl(bool release) { (void)gpio_set_level(static_cast<gpio_num_t>(board::I2C_SCL), release ? 1 : 0); }
  bool sdaHigh() const { return gpio_get_level(static_cast<gpio_num_t>(board::I2C_SDA)) != 0; }
  bool sclHigh() const { return gpio_get_level(static_cast<gpio_num_t>(board::I2C_SCL)) != 0; }
  uint32_t nowUs() const { return static_cast<uint32_t>(esp_timer_get_time()); }
  void delayUs(uint32_t value) { esp_rom_delay_us(value); }
};
inline EEPROM24Cxx::Status recoverEsp32Bus(uint32_t timeoutUs) {
  using namespace EEPROM24Cxx;
  if (!GPIO_IS_VALID_OUTPUT_GPIO(board::I2C_SDA) || !GPIO_IS_VALID_OUTPUT_GPIO(board::I2C_SCL))
    return Status::Error(Err::INVALID_CONFIG, "Recovery requires output-capable I2C pins");
  const auto sda = static_cast<gpio_num_t>(board::I2C_SDA);
  const auto scl = static_cast<gpio_num_t>(board::I2C_SCL);
  // Disconnect the old controller routes and preload released levels before
  // enabling open-drain output. External pull-ups are still required on hardware.
  esp_err_t error = gpio_reset_pin(sda);
  if (error == ESP_OK) error = gpio_reset_pin(scl);
  if (error == ESP_OK) error = gpio_set_level(sda, 1);
  if (error == ESP_OK) error = gpio_set_level(scl, 1);
  if (error == ESP_OK) error = gpio_set_direction(sda, GPIO_MODE_INPUT_OUTPUT_OD);
  if (error == ESP_OK) error = gpio_set_direction(scl, GPIO_MODE_INPUT_OUTPUT_OD);
  if (error != ESP_OK) return Status::Error(Err::INVALID_CONFIG, "Recovery GPIO setup failed", error);
  Esp32RecoveryPins pins;
  return recoverOpenDrainBus(pins, timeoutUs);
}
} // namespace eeprom24cxx_cli
