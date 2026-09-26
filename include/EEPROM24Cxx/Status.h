// Copyright (c) 2026 Thymos Solution s.r.o. SPDX-License-Identifier: MIT
/// Status and transport conventions follow the sibling MB85RC library.
#pragma once
#include <cstdint>

namespace EEPROM24Cxx {
/// Logical/preflight statuses stay separate from physical I2C outcomes.
/// TIMEOUT is an owner deadline; I2C_TIMEOUT is a callback/controller timeout.
/// WRITE_PROTECTED is reserved for independently established protection; an
/// acknowledged write or VERIFY_MISMATCH cannot identify the WP pin state.
enum class Err : uint8_t {
  OK = 0, NOT_INITIALIZED = 1, INVALID_CONFIG = 2, I2C_ERROR = 3,
  TIMEOUT = 4, INVALID_PARAM = 5, ADDRESS_OUT_OF_RANGE = 8,
  WRITE_PROTECTED = 9, BUSY = 10, IN_PROGRESS = 11,
  I2C_NACK_ADDR = 12, I2C_NACK_DATA = 13, I2C_TIMEOUT = 14,
  I2C_BUS = 15, VERIFY_MISMATCH = 16, UNSUPPORTED = 17,
  NO_RESULT = 18, CANCELLED = 19, I2C_NACK = 20
};
enum class BusyDetail : int32_t {
  TRANSFER_ACTIVE = 2, RESULT_PENDING = 7, REQUEST_ID_MISMATCH = 8, WRITE_CYCLE = 10
};
constexpr const char* errorName(Err e) {
  switch (e) {
    case Err::OK: return "OK";
    case Err::NOT_INITIALIZED: return "NOT_INITIALIZED";
    case Err::INVALID_CONFIG: return "INVALID_CONFIG";
    case Err::I2C_ERROR: return "I2C_ERROR";
    case Err::TIMEOUT: return "TIMEOUT";
    case Err::INVALID_PARAM: return "INVALID_PARAM";
    case Err::ADDRESS_OUT_OF_RANGE: return "ADDRESS_OUT_OF_RANGE";
    case Err::WRITE_PROTECTED: return "WRITE_PROTECTED";
    case Err::BUSY: return "BUSY";
    case Err::IN_PROGRESS: return "IN_PROGRESS";
    case Err::I2C_NACK_ADDR: return "I2C_NACK_ADDR";
    case Err::I2C_NACK_DATA: return "I2C_NACK_DATA";
    case Err::I2C_TIMEOUT: return "I2C_TIMEOUT";
    case Err::I2C_BUS: return "I2C_BUS";
    case Err::VERIFY_MISMATCH: return "VERIFY_MISMATCH";
    case Err::UNSUPPORTED: return "UNSUPPORTED";
    case Err::NO_RESULT: return "NO_RESULT";
    case Err::CANCELLED: return "CANCELLED";
    case Err::I2C_NACK: return "I2C_NACK";
    default: return "UNKNOWN";
  }
}
constexpr const char* toString(Err e) { return errorName(e); }
struct Status {
  Err code = Err::OK;
  int32_t detail = 0;
  const char* msg = ""; // Always static-lifetime text.
  constexpr Status() = default;
  constexpr Status(Err c, int32_t d, const char* m) : code(c), detail(d), msg(m) {}
  constexpr bool ok() const { return code == Err::OK; }
  constexpr bool is(Err e) const { return code == e; }
  constexpr bool inProgress() const { return code == Err::IN_PROGRESS; }
  constexpr explicit operator bool() const { return ok(); }
  static constexpr Status Ok() { return {Err::OK, 0, "OK"}; }
  static constexpr Status Error(Err e, const char* m, int32_t d = 0) { return {e, d, m}; }
};
} // namespace EEPROM24Cxx
