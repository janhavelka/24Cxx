// Copyright (c) 2026 Thymos Solution s.r.o. SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>
#include "EEPROM24Cxx/Status.h"

namespace EEPROM24Cxx {
enum class DeviceVariant : uint8_t {
  ZETTA_ZD24C02B = 0, C01, C02, C04, C08, C16, C32, C64,
  C128, C256, C512,
  MICROCHIP_24LC1025, ///< A16 in SLA bit 2; physical A2 must be tied high.
  ST_M24M01, ///< M24M01-R/-DF DocID12943 layout; not configurable M24M01E-F.
  CUSTOM
};
/// Cxx presets are common/conservative addressing layouts, not detected silicon.
/// Verify the fitted manufacturer's page size, tWR and address-pin behavior.
struct Geometry {
  uint32_t capacityBytes = 256;
  uint16_t pageSizeBytes = 8;
  uint8_t wordAddressBytes = 1;
  uint8_t bankAddressBits = 0;
  uint8_t bankAddressShift = 0; // Location of bank bits in the 7-bit I2C address.
  uint32_t writeCycleMs = 5; // Manufacturer's maximum tWR, rounded up.
};
/// Return a preset; CUSTOM and invalid enum values return an invalid zero geometry.
Geometry geometryFor(DeviceVariant variant);
const char* variantName(DeviceVariant variant);

/// One terminal physical transaction; no queued outcomes or hidden retries.
enum class TransportCode : uint8_t {
  OK = 0, NACK_ADDRESS, NACK_DATA, TIMEOUT, BUS_ERROR, IO_ERROR, NACK_UNSPECIFIED
};
enum class WriteCommit : uint8_t {
  NOT_APPLICABLE = 0, ///< No memory-data write attempted.
  NOT_COMMITTED, ///< Transport proves none of the requested memory data accepted.
  ACCEPTED, ///< Data acknowledged; hardware WP may still suppress storage.
  INDETERMINATE, ///< Some/all data may have reached memory. Never replay automatically.
  VERIFIED ///< Complete requested content observed by readback.
};
/// Exact completion counts are mandatory on success. A failed write can claim
/// NOT_COMMITTED only with independent proof and no completed memory-data bytes.
/// ACCEPTED on failure needs full counts and TIMEOUT/BUS_ERROR/IO_ERROR after
/// acceptance. NACKs, malformed counts and contradictory claims normalize to
/// INDETERMINATE. Failed reads expose no received bytes to application buffers.
struct TransportResult {
  TransportCode code = TransportCode::IO_ERROR;
  int32_t detail = 0;
  WriteCommit writeCommit = WriteCommit::INDETERMINATE;
  size_t completedTxBytes = 0; // Includes memory-address prefix, excludes slave address.
  size_t completedRxBytes = 0;
  constexpr TransportResult(TransportCode c = TransportCode::IO_ERROR, int32_t d = 0,
                            WriteCommit w = WriteCommit::INDETERMINATE,
                            size_t tx = 0, size_t rx = 0)
      : code(c), detail(d), writeCommit(w), completedTxBytes(tx), completedRxBytes(rx) {}
  constexpr bool ok() const { return code == TransportCode::OK; }
  static constexpr TransportResult Ok(size_t tx, size_t rx) {
    return {TransportCode::OK, 0, WriteCommit::NOT_APPLICABLE, tx, rx};
  }
  static constexpr TransportResult Error(TransportCode c, int32_t d = 0,
      WriteCommit w = WriteCommit::INDETERMINATE, size_t tx = 0, size_t rx = 0) {
    return {c, d, w, tx, rx};
  }
};
enum class DriverState : uint8_t { UNINIT = 0, READY, DEGRADED, OFFLINE };
constexpr const char* driverStateName(DriverState state) {
  switch (state) {
    case DriverState::UNINIT: return "UNINIT";
    case DriverState::READY: return "READY";
    case DriverState::DEGRADED: return "DEGRADED";
    case DriverState::OFFLINE: return "OFFLINE";
    default: return "UNKNOWN";
  }
}
enum class TransferKind : uint8_t { NONE = 0, READ, WRITE, FILL, VERIFY, VERIFIED_WRITE, VERIFIED_FILL };
enum class TransferState : uint8_t { IDLE = 0, ACTIVE, WAITING_WRITE_CYCLE, SUCCEEDED, FAILED, CANCELLED, TIMED_OUT };
struct TransferResult {
  TransferKind kind = TransferKind::NONE;
  TransferState state = TransferState::IDLE;
  Status status = Status::Ok();
  uint32_t address = 0;
  size_t bytesRequested = 0;
  size_t bytesAccepted = 0; // Definite whole-chunk transport acceptance, not persistence.
  size_t bytesCompleted = 0; // Read prefix, or accepted write prefix whose tWR elapsed.
  size_t bytesVerified = 0; // Prefix observed equal by readback.
  size_t failedChunkOffset = 0;
  size_t failedChunkLength = 0;
  /// Aggregate knowledge: ACCEPTED means a nonempty definite prefix, not that
  /// every requested byte was accepted. INDETERMINATE preserves any uncertainty.
  WriteCommit writeCommit = WriteCommit::NOT_APPLICABLE;
  WriteCommit lastChunkCommit = WriteCommit::NOT_APPLICABLE;
  Status writeStatus = Status::Ok();
  bool match = false;
  size_t mismatchOffset = 0;
  uint8_t expected = 0;
  uint8_t actual = 0;
};
} // namespace EEPROM24Cxx
