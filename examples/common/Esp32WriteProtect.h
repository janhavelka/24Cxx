#pragma once

// Optional application GPIO control shared by both ESP32 example frameworks.
// This is a board wiring observation, never an inference from EEPROM ACKs.
#include <driver/gpio.h>
#include <esp_err.h>
#include "BoardConfig.h"
#include "EEPROM24Cxx/Status.h"

namespace eeprom24cxx_cli {
inline bool writeProtectReady = false;
inline EEPROM24Cxx::Status initializeWriteProtect() {
  using namespace EEPROM24Cxx;
  if (board::WP_PIN < 0) return Status::Ok();
  if (!GPIO_IS_VALID_OUTPUT_GPIO(board::WP_PIN))
    return Status::Error(Err::INVALID_CONFIG, "WP GPIO is not output capable");
  const auto pin = static_cast<gpio_num_t>(board::WP_PIN);
  // Load the protected level before enabling the output driver. Input remains
  // enabled so the status command can observe the actual GPIO level.
  esp_err_t error = gpio_set_level(pin, 1);
  if (error == ESP_OK) error = gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT);
  if (error != ESP_OK) return Status::Error(Err::INVALID_CONFIG, "WP GPIO initialization failed", error);
  writeProtectReady = true;
  if (!gpio_get_level(pin)) return Status::Error(Err::INVALID_CONFIG, "WP GPIO did not rise to protected level");
  return Status::Ok();
}
inline EEPROM24Cxx::Status readWriteProtect(bool& protectedState, void*) {
  using namespace EEPROM24Cxx;
  if (board::WP_PIN < 0) return Status::Error(Err::UNSUPPORTED, "WP GPIO is not configured");
  if (!writeProtectReady) return Status::Error(Err::NOT_INITIALIZED, "WP GPIO is not initialized");
  protectedState = gpio_get_level(static_cast<gpio_num_t>(board::WP_PIN)) != 0;
  return Status::Ok();
}
inline EEPROM24Cxx::Status setWriteProtect(bool protectedState, void*) {
  using namespace EEPROM24Cxx;
  if (board::WP_PIN < 0) return Status::Error(Err::UNSUPPORTED, "WP GPIO is not configured");
  if (!writeProtectReady) return Status::Error(Err::NOT_INITIALIZED, "WP GPIO is not initialized");
  const auto pin = static_cast<gpio_num_t>(board::WP_PIN);
  const esp_err_t error = gpio_set_level(pin, protectedState ? 1 : 0);
  if (error != ESP_OK) return Status::Error(Err::INVALID_CONFIG, "WP GPIO update failed", error);
  if ((gpio_get_level(pin) != 0) != protectedState)
    return Status::Error(Err::INVALID_CONFIG, "WP GPIO level disagrees with requested protection");
  return Status::Ok();
}
} // namespace eeprom24cxx_cli
