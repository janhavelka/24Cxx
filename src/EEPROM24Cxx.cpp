// Copyright (c) 2026 Thymos Solution s.r.o. SPDX-License-Identifier: MIT
#include "EEPROM24Cxx/EEPROM24Cxx.h"
#include <algorithm>
#include <cstring>

namespace EEPROM24Cxx {
namespace {
constexpr uint32_t HALF_RANGE = 0x80000000UL;
bool reached(uint32_t now, uint32_t deadline) { return (now - deadline) < HALF_RANGE; }
bool powerOfTwo(uint32_t n) { return n != 0 && (n & (n - 1)) == 0; }
void increment(uint32_t& value) { if (value != UINT32_MAX) ++value; }
Status busy(BusyDetail d) { return Status::Error(Err::BUSY, "Operation or write cycle pending", static_cast<int32_t>(d)); }
Status progress() { return Status::Error(Err::IN_PROGRESS, "Transfer in progress"); }
bool writing(TransferKind k) {
  return k == TransferKind::WRITE || k == TransferKind::FILL ||
         k == TransferKind::VERIFIED_WRITE || k == TransferKind::VERIFIED_FILL;
}
bool filling(TransferKind k) { return k == TransferKind::FILL || k == TransferKind::VERIFIED_FILL; }
bool verifiedWrite(TransferKind k) { return k == TransferKind::VERIFIED_WRITE || k == TransferKind::VERIFIED_FILL; }
}

Geometry geometryFor(DeviceVariant v) {
  switch (v) {
    case DeviceVariant::ZETTA_ZD24C02B: case DeviceVariant::C02: return {256, 8, 1, 0, 0, 5};
    case DeviceVariant::C01: return {128, 8, 1, 0, 0, 5};
    case DeviceVariant::C04: return {512, 16, 1, 1, 0, 5};
    case DeviceVariant::C08: return {1024, 16, 1, 2, 0, 5};
    case DeviceVariant::C16: return {2048, 16, 1, 3, 0, 5};
    case DeviceVariant::C32: return {4096, 32, 2, 0, 0, 5};
    case DeviceVariant::C64: return {8192, 32, 2, 0, 0, 5};
    case DeviceVariant::C128: return {16384, 64, 2, 0, 0, 5};
    case DeviceVariant::C256: return {32768, 64, 2, 0, 0, 5};
    case DeviceVariant::C512: return {65536, 128, 2, 0, 0, 5};
    case DeviceVariant::MICROCHIP_24LC1025: return {131072, 128, 2, 1, 2, 5};
    case DeviceVariant::ST_M24M01: return {131072, 256, 2, 1, 0, 5};
    default: return {0, 0, 0, 0, 0, 0};
  }
}
const char* variantName(DeviceVariant v) {
  switch (v) {
    case DeviceVariant::ZETTA_ZD24C02B: return "Zetta ZD24C02B";
    case DeviceVariant::C01: return "24C01";
    case DeviceVariant::C02: return "24C02";
    case DeviceVariant::C04: return "24C04";
    case DeviceVariant::C08: return "24C08";
    case DeviceVariant::C16: return "24C16";
    case DeviceVariant::C32: return "24C32";
    case DeviceVariant::C64: return "24C64";
    case DeviceVariant::C128: return "24C128";
    case DeviceVariant::C256: return "24C256";
    case DeviceVariant::C512: return "24C512";
    case DeviceVariant::MICROCHIP_24LC1025: return "Microchip 24LC1025";
    case DeviceVariant::ST_M24M01: return "ST M24M01";
    case DeviceVariant::CUSTOM: return "custom";
    default: return "invalid";
  }
}

bool EEPROM24Cxx::active() const {
  return _result.state == TransferState::ACTIVE || _result.state == TransferState::WAITING_WRITE_CYCLE;
}
bool EEPROM24Cxx::terminal() const {
  return _result.state != TransferState::IDLE && !active();
}
Status EEPROM24Cxx::gate() const {
  if (!_bound) return Status::Error(Err::NOT_INITIALIZED, "No binding");
  if (active()) return busy(BusyDetail::TRANSFER_ACTIVE);
  if (terminal()) return busy(BusyDetail::RESULT_PENDING);
  if (_writePending) return busy(BusyDetail::WRITE_CYCLE);
  return Status::Ok();
}
Status EEPROM24Cxx::bind(const Config& c) {
  if (active()) return busy(BusyDetail::TRANSFER_ACTIVE);
  if (terminal()) return busy(BusyDetail::RESULT_PENDING);
  if (_writePending) return busy(BusyDetail::WRITE_CYCLE);
  const Geometry g = c.variant == DeviceVariant::CUSTOM ? c.customGeometry : geometryFor(c.variant);
  if (!c.i2cWrite || !c.i2cWriteRead || c.i2cTimeoutMs < MIN_I2C_TIMEOUT_MS ||
      c.i2cTimeoutMs > MAX_I2C_TIMEOUT_MS || c.i2cAddress < cmd::MIN_ADDRESS ||
      c.i2cAddress > cmd::MAX_ADDRESS || (g.wordAddressBytes != 1 && g.wordAddressBytes != 2) ||
      !powerOfTwo(g.capacityBytes) || !powerOfTwo(g.pageSizeBytes) ||
      g.pageSizeBytes > g.capacityBytes || g.bankAddressBits > 3 ||
      g.bankAddressShift > 2 || g.bankAddressBits + g.bankAddressShift > 3 ||
      g.writeCycleMs == 0 || g.writeCycleMs > 1000 || c.writeCycleMs > 1000 ||
      (c.writeCycleMs != 0 && c.writeCycleMs < g.writeCycleMs) ||
      c.maxTxBytes <= g.wordAddressBytes || c.maxRxBytes == 0)
    return Status::Error(Err::INVALID_CONFIG, "Invalid transport or memory geometry");
  const uint32_t bankSize = 1UL << (8U * g.wordAddressBytes);
  const uint32_t banks = (g.capacityBytes + bankSize - 1U) / bankSize;
  const uint8_t bankMask = static_cast<uint8_t>(((1U << g.bankAddressBits) - 1U) << g.bankAddressShift);
  if (banks != (1UL << g.bankAddressBits) || g.pageSizeBytes > bankSize ||
      (c.i2cAddress & bankMask) != 0)
    return Status::Error(Err::INVALID_CONFIG, "Capacity or strap address conflicts with bank bits");
  _config = c;
  _geometry = g;
  _bound = true;
  _health = {};
  _health.state = DriverState::READY;
  _result = {};
  _pendingLength = 0;
  return Status::Ok();
}
Status EEPROM24Cxx::begin(const Config& c) {
  Status s = bind(c);
  return s.ok() ? presence(true) : s;
}
void EEPROM24Cxx::end() {
  if (active()) finish(Status::Error(Err::CANCELLED, "Driver ended"), TransferState::CANCELLED);
  _bound = false;
  _config = {};
  _health = {};
  _pendingLength = 0;
  // _writePending/_writeReadyAt intentionally survive end/rebind attempts.
}
void EEPROM24Cxx::settleWrite() {
  _writePending = false;
  if (active()) {
    _result.bytesCompleted += _pendingLength;
    _result.state = TransferState::ACTIVE;
  }
  _pendingLength = 0;
}
void EEPROM24Cxx::tick(uint32_t nowMs) {
  _nowMs = nowMs;
  // ACK-polling jobs require a successful readiness transaction even after
  // the maximum tWR. Terminal/cancelled jobs release their barrier by time.
  if (_writePending && reached(nowMs, _writeReadyAt) && (!active() || !_config.i2cProbe)) settleWrite();
}
uint32_t EEPROM24Cxx::afterCallback(uint32_t fallback) const {
  return _config.nowMs ? _config.nowMs(_config.timeUser) : fallback;
}
void EEPROM24Cxx::track(Status s, uint32_t nowMs) {
  if (s.ok()) {
    increment(_health.totalSuccess);
    _health.consecutiveFailures = 0;
    _health.lastOkMs = nowMs;
    _health.state = DriverState::READY;
  } else {
    increment(_health.totalFailures);
    if (_health.consecutiveFailures < 255) ++_health.consecutiveFailures;
    _health.lastError = s;
    _health.lastErrorMs = nowMs;
    _health.state = _config.offlineThreshold && _health.consecutiveFailures >= _config.offlineThreshold
        ? DriverState::OFFLINE : DriverState::DEGRADED;
  }
}
Status EEPROM24Cxx::transportStatus(const TransportResult& r, size_t tx, size_t rx) {
  if (r.code == TransportCode::OK) {
    return r.completedTxBytes == tx && r.completedRxBytes == rx ? Status::Ok()
        : Status::Error(Err::I2C_ERROR, "Transport completion count mismatch", r.detail);
  }
  Err e = Err::I2C_ERROR;
  switch (r.code) {
    case TransportCode::NACK_ADDRESS: e = Err::I2C_NACK_ADDR; break;
    case TransportCode::NACK_DATA: e = Err::I2C_NACK_DATA; break;
    case TransportCode::NACK_UNSPECIFIED: e = Err::I2C_NACK; break;
    case TransportCode::TIMEOUT: e = Err::I2C_TIMEOUT; break;
    case TransportCode::BUS_ERROR: e = Err::I2C_BUS; break;
    default: break;
  }
  return Status::Error(e, "I2C transaction failed", r.detail);
}
size_t EEPROM24Cxx::encode(uint32_t address, uint8_t& busAddress) {
  const unsigned bits = 8U * _geometry.wordAddressBytes;
  busAddress = static_cast<uint8_t>(_config.i2cAddress | ((address >> bits) << _geometry.bankAddressShift));
  if (_geometry.wordAddressBytes == 2) _tx[0] = static_cast<uint8_t>(address >> 8U);
  _tx[_geometry.wordAddressBytes - 1U] = static_cast<uint8_t>(address);
  return _geometry.wordAddressBytes;
}
size_t EEPROM24Cxx::chunkLength(uint32_t address, size_t remaining, bool isWrite) const {
  const uint32_t bankSize = 1UL << (8U * _geometry.wordAddressBytes);
  size_t n = std::min(remaining, static_cast<size_t>(bankSize - (address % bankSize)));
  if (isWrite) {
    n = std::min(n, static_cast<size_t>(_geometry.pageSizeBytes - address % _geometry.pageSizeBytes));
    n = std::min(n, _config.maxTxBytes - _geometry.wordAddressBytes);
    n = std::min(n, cmd::MAX_WRITE_DATA_BYTES);
  } else {
    n = std::min(n, _config.maxRxBytes);
    n = std::min(n, cmd::MAX_READ_CHUNK);
  }
  return n;
}
Status EEPROM24Cxx::presence(bool tracked) {
  const Status g = gate();
  if (!g.ok()) return g;
  TransportResult r;
  size_t tx = 0, rx = 0;
  if (_config.i2cProbe) {
    r = _config.i2cProbe(_config.i2cAddress, _config.i2cTimeoutMs, _config.i2cUser);
  } else {
    uint8_t address = 0;
    tx = encode(0, address);
    rx = 1;
    r = _config.i2cWriteRead(address, _tx, tx, _rx, rx, _config.i2cTimeoutMs, _config.i2cUser);
  }
  const Status s = transportStatus(r, tx, rx);
  if (tracked) track(s, afterCallback(_nowMs));
  return s;
}
Status EEPROM24Cxx::probe() { return presence(false); }
Status EEPROM24Cxx::recover() { return presence(true); }
SettingsSnapshot EEPROM24Cxx::settingsSnapshot() const {
  SettingsSnapshot s = _health;
  s.initialized = s.online = _bound;
  s.variant = _config.variant;
  s.variantName = ::EEPROM24Cxx::variantName(_config.variant);
  s.geometry = _geometry;
  s.capacityBytes = _geometry.capacityBytes;
  s.pageSizeBytes = _geometry.pageSizeBytes;
  s.wordAddressBytes = _geometry.wordAddressBytes;
  s.i2cAddress = _config.i2cAddress;
  s.i2cTimeoutMs = _config.i2cTimeoutMs;
  s.writeCycleMs = _config.writeCycleMs ? _config.writeCycleMs : _geometry.writeCycleMs;
  s.maxTxBytes = _config.maxTxBytes;
  s.maxRxBytes = _config.maxRxBytes;
  s.maxWriteDataBytes = std::min(cmd::MAX_WRITE_DATA_BYTES,
      _config.maxTxBytes > _geometry.wordAddressBytes ? _config.maxTxBytes - _geometry.wordAddressBytes : 0);
  s.maxWriteDataBytes = std::min(s.maxWriteDataBytes, static_cast<size_t>(_geometry.pageSizeBytes));
  s.maxReadDataBytes = std::min(cmd::MAX_READ_CHUNK, _config.maxRxBytes);
  s.offlineThreshold = _config.offlineThreshold;
  s.hasNowMsHook = _config.nowMs != nullptr;
  s.hasAckPolling = _config.i2cProbe != nullptr;
  s.writeCyclePending = _writePending;
  s.writeReadyAtMs = _writeReadyAt;
  s.transferActive = active();
  s.resultPending = terminal();
  return s;
}
Status EEPROM24Cxx::admit(TransferKind kind, uint32_t address, size_t length, uint32_t timeoutMs) {
  Status s = gate();
  if (!s.ok()) return s;
  if (timeoutMs >= HALF_RANGE) return Status::Error(Err::INVALID_PARAM, "Deadline must be below 2^31 ms");
  if (timeoutMs && !_config.nowMs)
    return Status::Error(Err::INVALID_CONFIG, "Timed transfers require a post-callback nowMs hook");
  if (address > _geometry.capacityBytes || length > static_cast<size_t>(_geometry.capacityBytes - address))
    return Status::Error(Err::ADDRESS_OUT_OF_RANGE, "Range exceeds selected memory capacity");
  _result = {};
  _result.kind = kind;
  _result.state = TransferState::ACTIVE;
  _result.status = progress();
  _result.address = address;
  _result.bytesRequested = length;
  _verifyPhase = kind == TransferKind::VERIFY;
  _started = false;
  _timeoutMs = timeoutMs;
  _pendingLength = 0;
  _readBuffer = nullptr;
  _sourceBuffer = nullptr;
  if (length == 0) {
    _result.match = _verifyPhase || verifiedWrite(kind);
    finish(Status::Ok(), TransferState::SUCCEEDED);
  }
  return Status::Ok();
}
Status EEPROM24Cxx::startRead(uint32_t a, uint8_t* p, size_t n, uint32_t timeout) {
  if (n && !p) return Status::Error(Err::INVALID_PARAM, "Null read buffer");
  Status s = admit(TransferKind::READ, a, n, timeout);
  if (s.ok() && active()) _readBuffer = p;
  return s;
}
Status EEPROM24Cxx::startWrite(uint32_t a, const uint8_t* p, size_t n, bool verify, uint32_t timeout) {
  if (n && !p) return Status::Error(Err::INVALID_PARAM, "Null write buffer");
  Status s = admit(verify ? TransferKind::VERIFIED_WRITE : TransferKind::WRITE, a, n, timeout);
  if (s.ok() && active()) _sourceBuffer = p;
  return s;
}
Status EEPROM24Cxx::startFill(uint32_t a, uint8_t value, size_t n, bool verify, uint32_t timeout) {
  Status s = admit(verify ? TransferKind::VERIFIED_FILL : TransferKind::FILL, a, n, timeout);
  if (s.ok() && active()) _fillValue = value;
  return s;
}
Status EEPROM24Cxx::startVerify(uint32_t a, const uint8_t* p, size_t n, uint32_t timeout) {
  if (n && !p) return Status::Error(Err::INVALID_PARAM, "Null verify buffer");
  Status s = admit(TransferKind::VERIFY, a, n, timeout);
  if (s.ok() && active()) _sourceBuffer = p;
  return s;
}
void EEPROM24Cxx::finish(Status s, TransferState stateValue) {
  _result.status = s;
  _result.state = stateValue;
  if (s.ok()) {
    _result.failedChunkOffset = _result.bytesRequested;
    _result.failedChunkLength = 0;
  } else if (_result.failedChunkLength == 0) {
    _result.failedChunkOffset = _verifyPhase ? _result.bytesVerified : _result.bytesCompleted;
    if (_writePending) _result.failedChunkLength = _pendingLength;
  }
  _readBuffer = nullptr;
  _sourceBuffer = nullptr;
}
Status EEPROM24Cxx::cancel() {
  if (!active()) return terminal() ? busy(BusyDetail::RESULT_PENDING) : Status::Error(Err::NO_RESULT, "No active transfer");
  finish(Status::Error(Err::CANCELLED, "Cancelled by owner"), TransferState::CANCELLED);
  return Status::Ok();
}
Status EEPROM24Cxx::takeResult(TransferResult& r) {
  if (!terminal()) return Status::Error(Err::NO_RESULT, "No terminal result");
  r = _result;
  _result = {};
  _pendingLength = 0;
  return Status::Ok();
}

Status EEPROM24Cxx::poll(uint32_t nowMs, size_t maxTransactions) {
  tick(nowMs);
  if (terminal()) return _result.status;
  if (!active()) return Status::Error(Err::NO_RESULT, "No active transfer");
  if (!_started) { _started = true; _startedAt = nowMs; }
  size_t calls = 0;
  uint32_t callbackAllowance = 0; // Sum of bounded callback durations this poll without a clock hook.
  while (active()) {
    tick(nowMs);
    if (_timeoutMs && (nowMs - _startedAt) >= _timeoutMs) {
      finish(Status::Error(Err::TIMEOUT, "Owner transfer deadline expired"), TransferState::TIMED_OUT);
      break;
    }
    if (_writePending) {
      if (!_config.i2cProbe || calls == maxTransactions) break;
      const TransportResult r = _config.i2cProbe(_writeAddress, _config.i2cTimeoutMs, _config.i2cUser);
      ++calls;
      if (!_config.nowMs) callbackAllowance += _config.i2cTimeoutMs;
      nowMs = afterCallback(nowMs);
      const Status s = transportStatus(r, 0, 0);
      if (r.code == TransportCode::NACK_ADDRESS && r.completedTxBytes == 0 &&
          r.completedRxBytes == 0 && !reached(nowMs, _writeReadyAt)) {
        increment(_health.writeBusyPolls);
        if (_timeoutMs && (nowMs - _startedAt) >= _timeoutMs)
          finish(Status::Error(Err::TIMEOUT, "Owner transfer deadline expired"), TransferState::TIMED_OUT);
        break; // Never spin on a busy EEPROM inside one poll call.
      }
      track(s, nowMs);
      if (!s.ok()) {
        _result.failedChunkOffset = _result.bytesCompleted;
        _result.failedChunkLength = _pendingLength;
        finish(s, TransferState::FAILED);
        break;
      }
      settleWrite();
      continue;
    }
    size_t offset = _verifyPhase ? _result.bytesVerified : _result.bytesCompleted;
    if (offset == _result.bytesRequested) {
      if (verifiedWrite(_result.kind) && !_verifyPhase) {
        _verifyPhase = true;
        continue;
      }
      if (_verifyPhase) {
        _result.match = true;
        if (writing(_result.kind)) _result.writeCommit = WriteCommit::VERIFIED;
      }
      finish(Status::Ok(), TransferState::SUCCEEDED);
      break;
    }
    if (calls == maxTransactions) break;
    const uint32_t address = _result.address + static_cast<uint32_t>(offset);
    const bool isWrite = writing(_result.kind) && !_verifyPhase;
    const size_t n = chunkLength(address, _result.bytesRequested - offset, isWrite);
    uint8_t busAddress = 0;
    const size_t prefix = encode(address, busAddress);
    if (isWrite) {
      if (filling(_result.kind)) std::memset(_tx + prefix, _fillValue, n);
      else std::memcpy(_tx + prefix, _sourceBuffer + offset, n);
      const TransportResult r = _config.i2cWrite(busAddress, _tx, prefix + n, _config.i2cTimeoutMs, _config.i2cUser);
      ++calls;
      if (!_config.nowMs) callbackAllowance += _config.i2cTimeoutMs;
      nowMs = afterCallback(nowMs);
      const Status s = transportStatus(r, prefix + n, 0);
      track(s, nowMs);
      WriteCommit commit = WriteCommit::INDETERMINATE;
      const bool validCounts = r.completedTxBytes <= prefix + n && r.completedRxBytes == 0;
      if (s.ok()) commit = WriteCommit::ACCEPTED;
      else if (r.code != TransportCode::OK && validCounts &&
               r.writeCommit == WriteCommit::NOT_COMMITTED && r.completedTxBytes <= prefix)
        commit = WriteCommit::NOT_COMMITTED;
      else if (validCounts && r.completedTxBytes == prefix + n && r.writeCommit == WriteCommit::ACCEPTED &&
               (r.code == TransportCode::TIMEOUT || r.code == TransportCode::BUS_ERROR || r.code == TransportCode::IO_ERROR))
        commit = WriteCommit::ACCEPTED;
      _result.lastChunkCommit = commit;
      _result.writeCommit = commit == WriteCommit::NOT_COMMITTED && _result.bytesAccepted > 0
          ? WriteCommit::ACCEPTED : commit;
      _result.writeStatus = s;
      if (commit == WriteCommit::ACCEPTED) _result.bytesAccepted += n;
      if (commit != WriteCommit::NOT_COMMITTED) {
        _writePending = true;
        _pendingLength = commit == WriteCommit::ACCEPTED ? n : 0;
        _writeAddress = busAddress;
        const uint32_t t = _config.writeCycleMs ? _config.writeCycleMs : _geometry.writeCycleMs;
        // nowMs may precede the callback's STOP; timeout is its bounded duration.
        _writeReadyAt = nowMs + t + cmd::TIMER_QUANTIZATION_MARGIN_MS + callbackAllowance;
        _result.state = TransferState::WAITING_WRITE_CYCLE;
      }
      if (!s.ok()) {
        _result.failedChunkOffset = offset;
        _result.failedChunkLength = n;
        finish(s, TransferState::FAILED);
        break;
      }
    } else {
      const TransportResult r = _config.i2cWriteRead(busAddress, _tx, prefix, _rx, n,
                                                   _config.i2cTimeoutMs, _config.i2cUser);
      ++calls;
      if (!_config.nowMs) callbackAllowance += _config.i2cTimeoutMs;
      nowMs = afterCallback(nowMs);
      const Status s = transportStatus(r, prefix, n);
      track(s, nowMs);
      if (!s.ok()) {
        _result.failedChunkOffset = offset;
        _result.failedChunkLength = n;
        finish(s, TransferState::FAILED);
        break;
      }
      if (_verifyPhase) {
        for (size_t i = 0; i < n; ++i) {
          const uint8_t expected = filling(_result.kind) ? _fillValue : _sourceBuffer[offset + i];
          if (_rx[i] != expected) {
            _result.mismatchOffset = offset + i;
            _result.expected = expected;
            _result.actual = _rx[i];
            _result.failedChunkOffset = offset + i;
            _result.failedChunkLength = 1;
            finish(Status::Error(Err::VERIFY_MISMATCH, "Readback differs; check WP, geometry and ownership"), TransferState::FAILED);
            break;
          }
          ++_result.bytesVerified;
          if (_result.kind == TransferKind::VERIFY) ++_result.bytesCompleted;
        }
      } else {
        std::memcpy(_readBuffer + offset, _rx, n);
        _result.bytesCompleted += n;
      }
    }
  }
  return active() ? progress() : _result.status;
}
} // namespace EEPROM24Cxx
