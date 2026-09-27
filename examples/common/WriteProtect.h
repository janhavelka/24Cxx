#pragma once
#include <cstdint>
#include "EEPROM24Cxx/Status.h"

namespace eeprom24cxx_cli {
// Example-owned GPIO policy, independently testable without framework headers.
// Call only while I2C and the physical programming cycle are idle. ZD24C02B
// rev. 1.1 p. 13 specifies 1.2 us WP setup/hold at <=400 kHz (0.6 us at 1 MHz).
// Five microseconds before/after transitions conservatively covers both limits.
template <class Pin>
class WriteProtectControl {
 public:
  explicit WriteProtectControl(Pin& pin) : _pin(pin) {}
  EEPROM24Cxx::Status initialize() {
    using namespace EEPROM24Cxx;
    _ready = false;
    _pin.delayUs(SETTLE_US);
    Status status = _pin.write(true); // Preload protection before output enable.
    if (status.ok()) status = _pin.enable();
    _pin.delayUs(SETTLE_US);
    if (!status.ok()) return status;
    if (!_pin.high()) return Status::Error(Err::INVALID_CONFIG, "WP GPIO did not rise to protected level");
    _ready = true;
    return Status::Ok();
  }
  EEPROM24Cxx::Status read(bool& protectedState) const {
    if (!_ready) return notReady();
    protectedState = _pin.high();
    return EEPROM24Cxx::Status::Ok();
  }
  EEPROM24Cxx::Status set(bool protectedState) {
    using namespace EEPROM24Cxx;
    if (!_ready) return notReady();
    _pin.delayUs(SETTLE_US); // Hold the previous state past the preceding STOP.
    const Status status = _pin.write(protectedState);
    _pin.delayUs(SETTLE_US); // Settle before the owner can start another transfer.
    if (!status.ok()) return status;
    if (_pin.high() != protectedState)
      return Status::Error(Err::INVALID_CONFIG, "WP GPIO level disagrees with requested protection");
    return Status::Ok();
  }
 private:
  static constexpr uint32_t SETTLE_US = 5;
  static EEPROM24Cxx::Status notReady() {
    return EEPROM24Cxx::Status::Error(EEPROM24Cxx::Err::NOT_INITIALIZED, "WP GPIO is not initialized");
  }
  Pin& _pin;
  bool _ready = false;
};
} // namespace eeprom24cxx_cli
