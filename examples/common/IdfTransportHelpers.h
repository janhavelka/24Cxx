#pragma once

// SDK constants supplied by the native adapter keep this error/effect policy
// directly testable without importing ESP-IDF into the host or library core.
#include "EEPROM24Cxx/Types.h"

namespace eeprom24cxx_cli {
struct IdfResultMapper {
  int32_t ok;
  int32_t timeout;
  int32_t invalidArgument;
  int32_t invalidResponse;
  int32_t notFound;

  EEPROM24Cxx::TransportResult transaction(int32_t error, size_t tx, size_t rx,
                                            bool memoryWrite) const {
    using namespace EEPROM24Cxx;
    if (error == ok) return TransportResult::Ok(tx, rx);
    // Ordinary transfers cannot identify which byte NACKed. Only the
    // address-only probe may report the NACK_ADDRESS required by ACK polling.
    const auto code = error == timeout ? TransportCode::TIMEOUT :
        error == invalidResponse || error == notFound ? TransportCode::NACK_UNSPECIFIED :
        TransportCode::IO_ERROR;
    return TransportResult::Error(code, error, !memoryWrite ? WriteCommit::NOT_APPLICABLE :
        error == invalidArgument ? WriteCommit::NOT_COMMITTED : WriteCommit::INDETERMINATE);
  }
  EEPROM24Cxx::TransportResult probe(int32_t error) const {
    using namespace EEPROM24Cxx;
    if (error == ok) return TransportResult::Ok(0, 0);
    return TransportResult::Error(error == notFound ? TransportCode::NACK_ADDRESS :
        error == timeout ? TransportCode::TIMEOUT : TransportCode::IO_ERROR,
        error, WriteCommit::NOT_APPLICABLE);
  }
};

template <class Device, class Receive, class WriteRead>
int32_t idfReadTransaction(Device device, const uint8_t* tx, size_t txLength,
    uint8_t* rx, size_t rxLength, int timeoutMs, Receive receive, WriteRead writeRead) {
  if (txLength == 0) return receive(device, rx, rxLength, timeoutMs);
  return writeRead(device, tx, txLength, rx, rxLength, timeoutMs);
}
} // namespace eeprom24cxx_cli
