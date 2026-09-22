#pragma once

// Example adapter policy tested without Arduino headers. The pinned ESP32 Wire
// implementation maps both ESP_FAIL and ESP_ERR_NOT_FOUND to numeric code 2;
// its documented generic Wire error comments are not evidence of NACK phase.
#include <cstddef>
#include <cstdint>
#include "EEPROM24Cxx/Types.h"

namespace eeprom24cxx_cli {
inline EEPROM24Cxx::TransportResult wireResult(uint8_t code, size_t tx = 0,
                                               size_t rx = 0, bool addressOnly = false) {
  using namespace EEPROM24Cxx;
  if (!code) return TransportResult::Ok(tx, rx);
  const TransportCode error = code == 2 || code == 3 ?
      (addressOnly ? TransportCode::NACK_ADDRESS : TransportCode::NACK_UNSPECIFIED) :
      code == 5 ? TransportCode::TIMEOUT : TransportCode::IO_ERROR;
  return TransportResult::Error(error, code,
      addressOnly ? WriteCommit::NOT_APPLICABLE : WriteCommit::INDETERMINATE);
}
struct WireDiscardResult { bool physicalAttempt; uint8_t code; };
template <class WireType>
WireDiscardResult discardWireTx(WireType& wire, uint8_t address, size_t buffered) {
  // This adapter is the sole owner; beginTransmission starts with an empty TX
  // buffer, the configured capacity is >=32, and callbacks never change it.
  // Thus zero accepted bytes indicates missing storage. ESP32 requestFrom's
  // null-buffer path releases the acquired mutex before issuing any I2C.
  wire.flush();
  if (buffered == 0) {
    (void)wire.requestFrom(address, size_t{0}, true);
    return {false, 4};
  }
  // A nonzero count proves storage exists. Flush removed every queued byte;
  // endTransmission now sends an address-only probe and releases the mutex.
  return {true, wire.endTransmission(true)};
}
} // namespace eeprom24cxx_cli
