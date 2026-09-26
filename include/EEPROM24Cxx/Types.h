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
constexpr const char* transportCodeName(TransportCode code) {
  switch (code) {
    case TransportCode::OK: return "OK";
    case TransportCode::NACK_ADDRESS: return "NACK_ADDRESS";
    case TransportCode::NACK_DATA: return "NACK_DATA";
    case TransportCode::TIMEOUT: return "TIMEOUT";
    case TransportCode::BUS_ERROR: return "BUS_ERROR";
    case TransportCode::IO_ERROR: return "IO_ERROR";
    case TransportCode::NACK_UNSPECIFIED: return "NACK_UNSPECIFIED";
    default: return "UNKNOWN";
  }
}
enum class WriteCommit : uint8_t {
  NOT_APPLICABLE = 0, ///< No memory-data write attempted.
  NOT_COMMITTED, ///< Transport proves none of the requested memory data accepted.
  ACCEPTED, ///< Data acknowledged; hardware WP may still suppress storage.
  INDETERMINATE, ///< Some/all data may have reached memory. Never replay automatically.
  VERIFIED ///< Complete requested content observed by readback.
};
constexpr const char* writeCommitName(WriteCommit commit) {
  switch (commit) {
    case WriteCommit::NOT_APPLICABLE: return "NOT_APPLICABLE";
    case WriteCommit::NOT_COMMITTED: return "NOT_COMMITTED";
    case WriteCommit::ACCEPTED: return "ACCEPTED";
    case WriteCommit::INDETERMINATE: return "INDETERMINATE";
    case WriteCommit::VERIFIED: return "VERIFIED";
    default: return "UNKNOWN";
  }
}
/// Exact completion counts are mandatory on success. A failed write can claim
/// NOT_COMMITTED only with independent proof and no completed memory-data bytes.
/// ACCEPTED on failure needs full counts and TIMEOUT/BUS_ERROR/IO_ERROR after
/// acceptance. Unknown codes, malformed counts and contradictory failure
/// claims normalize to INDETERMINATE. Exact successful counts prove transport
/// acceptance regardless of the writeCommit hint. Failed reads expose no
/// received bytes to application buffers.
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
constexpr const char* transferKindName(TransferKind kind) {
  switch (kind) {
    case TransferKind::NONE: return "NONE";
    case TransferKind::READ: return "READ";
    case TransferKind::WRITE: return "WRITE";
    case TransferKind::FILL: return "FILL";
    case TransferKind::VERIFY: return "VERIFY";
    case TransferKind::VERIFIED_WRITE: return "VERIFIED_WRITE";
    case TransferKind::VERIFIED_FILL: return "VERIFIED_FILL";
    default: return "UNKNOWN";
  }
}
constexpr const char* transferStateName(TransferState state) {
  switch (state) {
    case TransferState::IDLE: return "IDLE";
    case TransferState::ACTIVE: return "ACTIVE";
    case TransferState::WAITING_WRITE_CYCLE: return "WAITING_WRITE_CYCLE";
    case TransferState::SUCCEEDED: return "SUCCEEDED";
    case TransferState::FAILED: return "FAILED";
    case TransferState::CANCELLED: return "CANCELLED";
    case TransferState::TIMED_OUT: return "TIMED_OUT";
    default: return "UNKNOWN";
  }
}
constexpr const char* toString(TransportCode code) { return transportCodeName(code); }
constexpr const char* toString(WriteCommit commit) { return writeCommitName(commit); }
constexpr const char* toString(DriverState state) { return driverStateName(state); }
constexpr const char* toString(TransferKind kind) { return transferKindName(kind); }
constexpr const char* toString(TransferState state) { return transferStateName(state); }
inline const char* toString(DeviceVariant variant) { return variantName(variant); }
/// Automatic request IDs reserve the upper half. Explicit owner IDs are
/// 1..0x7FFFFFFF and must not be reused while stale owner messages can arrive.
constexpr uint32_t AUTOMATIC_REQUEST_ID_FIRST = 0x80000000UL;
struct TransferResult {
  uint32_t requestId = 0;
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
  /// Most recent physical write result, IN_PROGRESS before the first nonempty
  /// write, or OK for operations that need no memory-data write.
  Status writeStatus = Status::Ok();
  bool verificationAttempted = false;
  /// Last readback transport/content result; meaningful only if attempted.
  /// Overall status can subsequently become CANCELLED/TIMEOUT without losing it.
  Status verifyStatus = Status::Ok();
  bool match = false;
  size_t mismatchOffset = 0;
  uint8_t expected = 0;
  uint8_t actual = 0;
};
} // namespace EEPROM24Cxx
