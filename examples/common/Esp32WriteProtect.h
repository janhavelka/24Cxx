#pragma once

// Optional application GPIO control shared by both ESP32 example frameworks.
// This is a board wiring observation, never an inference from EEPROM ACKs.
#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_rom_sys.h>
#include "BoardConfig.h"
#include "WriteProtect.h"

namespace eeprom24cxx_cli {
struct Esp32WriteProtectPin {
  EEPROM24Cxx::Status write(bool protectedState) {
    const esp_err_t error = gpio_set_level(static_cast<gpio_num_t>(board::WP_PIN), protectedState ? 1 : 0);
    return error == ESP_OK ? EEPROM24Cxx::Status::Ok() :
        EEPROM24Cxx::Status::Error(EEPROM24Cxx::Err::INVALID_CONFIG, "WP GPIO update failed", error);
  }
  EEPROM24Cxx::Status enable() {
    const esp_err_t error = gpio_set_direction(static_cast<gpio_num_t>(board::WP_PIN), GPIO_MODE_INPUT_OUTPUT);
    return error == ESP_OK ? EEPROM24Cxx::Status::Ok() :
        EEPROM24Cxx::Status::Error(EEPROM24Cxx::Err::INVALID_CONFIG, "WP GPIO initialization failed", error);
  }
  bool high() const { return gpio_get_level(static_cast<gpio_num_t>(board::WP_PIN)) != 0; }
  void delayUs(uint32_t value) { esp_rom_delay_us(value); }
};
inline Esp32WriteProtectPin writeProtectPin;
inline WriteProtectControl<Esp32WriteProtectPin> writeProtectControl(writeProtectPin);
inline EEPROM24Cxx::Status initializeWriteProtect() {
  using namespace EEPROM24Cxx;
  if (board::WP_PIN < 0) return Status::Ok();
  if (!GPIO_IS_VALID_OUTPUT_GPIO(board::WP_PIN))
    return Status::Error(Err::INVALID_CONFIG, "WP GPIO is not output capable");
  return writeProtectControl.initialize();
}
inline EEPROM24Cxx::Status readWriteProtect(bool& protectedState, void*) {
  using namespace EEPROM24Cxx;
  if (board::WP_PIN < 0) return Status::Error(Err::UNSUPPORTED, "WP GPIO is not configured");
  return writeProtectControl.read(protectedState);
}
inline EEPROM24Cxx::Status setWriteProtect(bool protectedState, void*) {
  using namespace EEPROM24Cxx;
  if (board::WP_PIN < 0) return Status::Error(Err::UNSUPPORTED, "WP GPIO is not configured");
  return writeProtectControl.set(protectedState);
}
} // namespace eeprom24cxx_cli
