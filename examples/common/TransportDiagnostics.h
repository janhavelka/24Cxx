#pragma once

#include <cstdint>
#include "EEPROM24Cxx/Types.h"

namespace eeprom24cxx_cli {
// Physical transactions, including diagnostic probes and write-cycle ACK polls.
// Keep these independent of the driver's logical operation and health counters.
enum class BusTransferKind : uint8_t { WRITE, READ, PROBE };
struct TransferStats {
  uint32_t attempts = 0;
  uint32_t successes = 0;
  uint32_t failures = 0;
  uint32_t writeAttempts = 0;
  uint32_t readAttempts = 0;
  uint32_t probeAttempts = 0;
  void record(bool ok, BusTransferKind kind) {
    if (attempts != UINT32_MAX) ++attempts;
    auto& outcome = ok ? successes : failures;
    if (outcome != UINT32_MAX) ++outcome;
    auto& count = kind == BusTransferKind::WRITE ? writeAttempts :
        kind == BusTransferKind::READ ? readAttempts : probeAttempts;
    if (count != UINT32_MAX) ++count;
  }
};

inline EEPROM24Cxx::Status probeStatus(const EEPROM24Cxx::TransportResult& result,
                                     const char* message) {
  using namespace EEPROM24Cxx;
  if (result.ok()) return Status::Ok();
  const Err code = result.code == TransportCode::NACK_ADDRESS ? Err::I2C_NACK_ADDR :
      result.code == TransportCode::TIMEOUT ? Err::I2C_TIMEOUT : Err::I2C_ERROR;
  return Status::Error(code, message, result.detail);
}
} // namespace eeprom24cxx_cli
