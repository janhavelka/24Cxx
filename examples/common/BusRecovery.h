#pragma once
#include <cstdint>
#include "EEPROM24Cxx/Status.h"

namespace eeprom24cxx_cli {
// Application-owned electrical recovery, only after the driver has settled.
// Pins must already be configured as input/output open drain; true releases a
// line and never drives it high. ZD24C02B rev. 1.1 p. 4 requires up to nine
// clocks, SDA observed high while SCL is high, then START to reset the protocol.
// Finish with STOP so the next application transaction starts on an idle bus.
template <class Pins>
EEPROM24Cxx::Status recoverOpenDrainBus(Pins& pins, uint32_t timeoutUs) {
  using namespace EEPROM24Cxx;
  const uint32_t started = pins.nowUs();
  const auto expired = [&]() { return static_cast<uint32_t>(pins.nowUs() - started) >= timeoutUs; };
  const auto releaseClock = [&]() {
    pins.scl(true);
    while (!pins.sclHigh()) {
      if (expired()) return false;
      pins.delayUs(5);
    }
    return !expired();
  };
  const auto timeout = [&]() {
    pins.sda(true); pins.scl(true);
    return Status::Error(Err::I2C_TIMEOUT, "SCL held low during interface reset");
  };
  pins.sda(true);
  if (!releaseClock()) return timeout();
  for (unsigned count = 0; count < 9 && !pins.sdaHigh(); ++count) {
    pins.scl(false); pins.delayUs(5);
    if (!releaseClock()) return timeout();
    pins.delayUs(5);
  }
  if (!pins.sdaHigh())
    return Status::Error(Err::I2C_BUS, "SDA held low after nine recovery clocks");
  // START is part of the chip's documented reset sequence. Check SDA only
  // while SCL is released high, then meet START setup/hold before taking SCL low.
  pins.delayUs(5);
  if (expired()) return timeout();
  pins.sda(false); pins.delayUs(5);
  // A final STOP returns the bus to idle. Recovery remains an explicit action:
  // line transitions can complete an interrupted write, so the CLI retains tWR.
  pins.scl(false); pins.sda(false); pins.delayUs(5);
  if (!releaseClock()) return timeout();
  pins.delayUs(5); pins.sda(true); pins.delayUs(5);
  if (expired()) return Status::Error(Err::I2C_TIMEOUT, "Interface reset deadline elapsed");
  if (!pins.sclHigh() || !pins.sdaHigh())
    return Status::Error(Err::I2C_BUS, "I2C line remains low after interface reset");
  return Status::Ok();
}
} // namespace eeprom24cxx_cli
