// Copyright (c) 2026 Thymos Solution s.r.o. SPDX-License-Identifier: MIT
#pragma once
#include "EEPROM24Cxx/EEPROM24Cxx.h"
#include "EEPROM24Cxx/MemoryHelpers.h"

namespace EEPROM24Cxx {
/// Application-provided wait/yield. Must return, keep Config.nowMs advancing,
/// and never re-enter this driver. Platform delay/RTOS policy stays with owner.
using WaitMsFn = void (*)(uint32_t milliseconds, void* user);

/// Optional synchronous field facade over the cooperative driver. Every method
/// is called by the serialized bus owner. The facade owns neither bus nor driver.
/// Memory methods finish and consume their own result before returning; borrowed
/// input/output is no longer retained on any return. They never retry a failed
/// write. Writes wait for tWR; use writeVerify/update when readback is needed.
/// Config.nowMs and a bounded application wait callback are required. Logical
/// deadlines cannot preempt a physical callback or a blocked application wait.
class BlockingMemory {
 public:
  static constexpr uint32_t DEFAULT_TIMEOUT_MS = 1000;
  BlockingMemory(EEPROM24Cxx& driver, WaitMsFn wait, void* user = nullptr)
      : _driver(driver), _wait(wait), _waitUser(user) {}
  BlockingMemory(const BlockingMemory&) = delete;
  BlockingMemory& operator=(const BlockingMemory&) = delete;
  Status begin(const Config& config) { return _driver.begin(config); }
  Status init(const Config& config) { return begin(config); }
  Status bind(const Config& config) { return _driver.bind(config); }
  void end() { _driver.end(); }
  EEPROM24Cxx& driver() { return _driver; }
  const EEPROM24Cxx& driver() const { return _driver; }
  /// Most recent admitted request. Preflight failures and waitUntilReady do not
  /// overwrite this evidence. crc32 reports the most recent read chunk here.
  TransferResult lastResult() const { return _last; }

  Status read(uint32_t address, uint8_t* data, size_t length, uint32_t timeoutMs = DEFAULT_TIMEOUT_MS) {
    return run(timeoutMs, [&]() { return _driver.startRead(address, data, length, timeoutMs); });
  }
  Status readByte(uint32_t address, uint8_t& value, uint32_t timeoutMs = DEFAULT_TIMEOUT_MS) {
    uint8_t raw = 0;
    const Status status = read(address, &raw, 1, timeoutMs);
    if (status.ok()) value = raw;
    return status;
  }
  Status readOnce(uint32_t address, uint8_t* data, size_t length, uint32_t timeoutMs = DEFAULT_TIMEOUT_MS) {
    const Status status = singleChunk(address, length, false);
    return status.ok() ? read(address, data, length, timeoutMs) : status;
  }
  Status readCurrentAddress(uint8_t* data, size_t length, uint32_t timeoutMs = DEFAULT_TIMEOUT_MS) {
    return run(timeoutMs, [&]() { return _driver.startCurrentRead(data, length, timeoutMs); });
  }
  Status readCurrentAddress(uint8_t& value, uint32_t timeoutMs = DEFAULT_TIMEOUT_MS) {
    uint8_t raw = 0;
    const Status status = readCurrentAddress(&raw, 1, timeoutMs);
    if (status.ok()) value = raw;
    return status;
  }
  Status write(uint32_t address, const uint8_t* data, size_t length, uint32_t timeoutMs = DEFAULT_TIMEOUT_MS) {
    return run(timeoutMs, [&]() { return _driver.startWrite(address, data, length, false, timeoutMs); });
  }
  Status writeByte(uint32_t address, uint8_t value, uint32_t timeoutMs = DEFAULT_TIMEOUT_MS) {
    return write(address, &value, 1, timeoutMs);
  }
  Status writeOnce(uint32_t address, const uint8_t* data, size_t length, uint32_t timeoutMs = DEFAULT_TIMEOUT_MS) {
    const Status status = singleChunk(address, length, true);
    return status.ok() ? write(address, data, length, timeoutMs) : status;
  }
  Status fill(uint32_t address, uint8_t value, size_t length, uint32_t timeoutMs = DEFAULT_TIMEOUT_MS) {
    return run(timeoutMs, [&]() { return _driver.startFill(address, value, length, false, timeoutMs); });
  }
  Status verify(uint32_t address, const uint8_t* expected, size_t length, uint32_t timeoutMs = DEFAULT_TIMEOUT_MS) {
    return run(timeoutMs, [&]() { return _driver.startVerify(address, expected, length, timeoutMs); });
  }
  Status verifyOnce(uint32_t address, const uint8_t* expected, size_t length, uint32_t timeoutMs = DEFAULT_TIMEOUT_MS) {
    const Status status = singleChunk(address, length, false);
    return status.ok() ? verify(address, expected, length, timeoutMs) : status;
  }
  Status writeVerify(uint32_t address, const uint8_t* data, size_t length, uint32_t timeoutMs = DEFAULT_TIMEOUT_MS) {
    return run(timeoutMs, [&]() { return _driver.startWrite(address, data, length, true, timeoutMs); });
  }
  Status fillVerify(uint32_t address, uint8_t value, size_t length, uint32_t timeoutMs = DEFAULT_TIMEOUT_MS) {
    return run(timeoutMs, [&]() { return _driver.startFill(address, value, length, true, timeoutMs); });
  }
  Status update(uint32_t address, const uint8_t* data, size_t length,
                bool verify = true, uint32_t timeoutMs = DEFAULT_TIMEOUT_MS) {
    return run(timeoutMs, [&]() { return _driver.startUpdate(address, data, length, verify, timeoutMs); });
  }
  Status updateByte(uint32_t address, uint8_t value, bool verify = true,
                    uint32_t timeoutMs = DEFAULT_TIMEOUT_MS) {
    return update(address, &value, 1, verify, timeoutMs);
  }
  /// Explicitly wait out an idle residual write barrier after an earlier fault.
  /// No probing, memory traffic or replay; retained/active work must be handled first.
  Status waitUntilReady(uint32_t timeoutMs = DEFAULT_TIMEOUT_MS) {
    const Status preflight = validateBlocking(timeoutMs);
    if (!preflight.ok()) return preflight;
    const auto snapshot = _driver.settingsSnapshot();
    if (snapshot.transferActive) return busy(BusyDetail::TRANSFER_ACTIVE);
    if (snapshot.resultPending) return busy(BusyDetail::RESULT_PENDING);
    const uint32_t started = now();
    uint32_t previous = started;
    unsigned stalled = 0;
    for (;;) {
      const uint32_t current = now();
      _driver.tick(current);
      if (!_driver.settingsSnapshot().writeCyclePending) return Status::Ok();
      if (current - started >= timeoutMs || stalled >= MAX_STALLED_WAITS)
        return Status::Error(Err::TIMEOUT, "Write barrier wait timed out");
      _wait(1, _waitUser);
      observeClock(previous, stalled);
    }
  }
  /// Whole-range CRC with fixed memory. Output is unchanged unless the full
  /// range succeeds. One deadline covers all chunks, including application waits.
  Status crc32(uint32_t address, size_t length, uint32_t& value, uint32_t timeoutMs = DEFAULT_TIMEOUT_MS) {
    const Status preflight = validateBlocking(timeoutMs);
    if (!preflight.ok()) return preflight;
    if (!memory::fitsRange(_driver.capacityBytes(), address, length))
      return Status::Error(Err::ADDRESS_OUT_OF_RANGE, "CRC range exceeds capacity");
    uint8_t buffer[MAX_TRANSPORT_RX_BYTES]{};
    uint32_t state = 0xFFFFFFFFU;
    const uint32_t started = now();
    size_t completed = 0;
    do {
      const uint32_t elapsed = now() - started;
      if (elapsed >= timeoutMs) return Status::Error(Err::TIMEOUT, "CRC deadline expired");
      const size_t remaining = length - completed;
      const size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
      const Status status = read(address + static_cast<uint32_t>(completed), buffer, chunk, timeoutMs - elapsed);
      if (!status.ok()) return status;
      if (now() - started >= timeoutMs) return Status::Error(Err::TIMEOUT, "CRC deadline expired");
      state = memory::crc32Update(state, buffer, chunk);
      completed += chunk;
    } while (completed < length);
    if (now() - started >= timeoutMs) return Status::Error(Err::TIMEOUT, "CRC deadline expired");
    value = state ^ 0xFFFFFFFFU;
    return Status::Ok();
  }

  Status readUint8(uint32_t address, uint8_t& value, uint32_t timeoutMs = DEFAULT_TIMEOUT_MS) {
    return readByte(address, value, timeoutMs);
  }
  Status writeUint8(uint32_t address, uint8_t value, uint32_t timeoutMs = DEFAULT_TIMEOUT_MS) {
    return writeByte(address, value, timeoutMs);
  }
  Status readBool(uint32_t address, bool& value, uint32_t timeoutMs = DEFAULT_TIMEOUT_MS) {
    uint8_t raw = 0;
    const Status status = readByte(address, raw, timeoutMs);
    if (status.ok()) value = raw != 0;
    return status;
  }
  Status writeBool(uint32_t address, bool value, uint32_t timeoutMs = DEFAULT_TIMEOUT_MS) {
    return writeByte(address, value ? uint8_t{1} : uint8_t{0}, timeoutMs);
  }
  Status readUint16Le(uint32_t a, uint16_t& v, uint32_t t = DEFAULT_TIMEOUT_MS) { return readValue<2>(a, v, t, memory::decodeUint16Le); }
  Status readUint32Le(uint32_t a, uint32_t& v, uint32_t t = DEFAULT_TIMEOUT_MS) { return readValue<4>(a, v, t, memory::decodeUint32Le); }
  Status readUint64Le(uint32_t a, uint64_t& v, uint32_t t = DEFAULT_TIMEOUT_MS) { return readValue<8>(a, v, t, memory::decodeUint64Le); }
  Status readUint16Be(uint32_t a, uint16_t& v, uint32_t t = DEFAULT_TIMEOUT_MS) { return readValue<2>(a, v, t, memory::decodeUint16Be); }
  Status readUint32Be(uint32_t a, uint32_t& v, uint32_t t = DEFAULT_TIMEOUT_MS) { return readValue<4>(a, v, t, memory::decodeUint32Be); }
  Status readUint64Be(uint32_t a, uint64_t& v, uint32_t t = DEFAULT_TIMEOUT_MS) { return readValue<8>(a, v, t, memory::decodeUint64Be); }
  Status readInt32Le(uint32_t a, int32_t& v, uint32_t t = DEFAULT_TIMEOUT_MS) { return readValue<4>(a, v, t, memory::decodeInt32Le); }
  Status readInt64Le(uint32_t a, int64_t& v, uint32_t t = DEFAULT_TIMEOUT_MS) { return readValue<8>(a, v, t, memory::decodeInt64Le); }
  Status readFloatLe(uint32_t a, float& v, uint32_t t = DEFAULT_TIMEOUT_MS) { return readValue<4>(a, v, t, memory::decodeFloatLe); }
  Status readDoubleLe(uint32_t a, double& v, uint32_t t = DEFAULT_TIMEOUT_MS) { return readValue<8>(a, v, t, memory::decodeDoubleLe); }
  Status writeUint16Le(uint32_t a, uint16_t v, uint32_t t = DEFAULT_TIMEOUT_MS) { return writeValue<2>(a, v, t, memory::encodeUint16Le); }
  Status writeUint32Le(uint32_t a, uint32_t v, uint32_t t = DEFAULT_TIMEOUT_MS) { return writeValue<4>(a, v, t, memory::encodeUint32Le); }
  Status writeUint64Le(uint32_t a, uint64_t v, uint32_t t = DEFAULT_TIMEOUT_MS) { return writeValue<8>(a, v, t, memory::encodeUint64Le); }
  Status writeUint16Be(uint32_t a, uint16_t v, uint32_t t = DEFAULT_TIMEOUT_MS) { return writeValue<2>(a, v, t, memory::encodeUint16Be); }
  Status writeUint32Be(uint32_t a, uint32_t v, uint32_t t = DEFAULT_TIMEOUT_MS) { return writeValue<4>(a, v, t, memory::encodeUint32Be); }
  Status writeUint64Be(uint32_t a, uint64_t v, uint32_t t = DEFAULT_TIMEOUT_MS) { return writeValue<8>(a, v, t, memory::encodeUint64Be); }
  Status writeInt32Le(uint32_t a, int32_t v, uint32_t t = DEFAULT_TIMEOUT_MS) { return writeValue<4>(a, v, t, memory::encodeInt32Le); }
  Status writeInt64Le(uint32_t a, int64_t v, uint32_t t = DEFAULT_TIMEOUT_MS) { return writeValue<8>(a, v, t, memory::encodeInt64Le); }
  Status writeFloatLe(uint32_t a, float v, uint32_t t = DEFAULT_TIMEOUT_MS) { return writeValue<4>(a, v, t, memory::encodeFloatLe); }
  Status writeDoubleLe(uint32_t a, double v, uint32_t t = DEFAULT_TIMEOUT_MS) { return writeValue<8>(a, v, t, memory::encodeDoubleLe); }
  // MB85RC TypedMemory spelling, with an explicit storage width.
  Status readFloat32Le(uint32_t a, float& v, uint32_t t = DEFAULT_TIMEOUT_MS) { return readFloatLe(a, v, t); }
  Status readFloat64Le(uint32_t a, double& v, uint32_t t = DEFAULT_TIMEOUT_MS) { return readDoubleLe(a, v, t); }
  Status writeFloat32Le(uint32_t a, float v, uint32_t t = DEFAULT_TIMEOUT_MS) { return writeFloatLe(a, v, t); }
  Status writeFloat64Le(uint32_t a, double v, uint32_t t = DEFAULT_TIMEOUT_MS) { return writeDoubleLe(a, v, t); }

 private:
  static constexpr unsigned MAX_STALLED_WAITS = 1024;
  EEPROM24Cxx& _driver;
  WaitMsFn _wait;
  void* _waitUser;
  TransferResult _last{};
  uint32_t now() const { return _driver.getConfig().nowMs(_driver.getConfig().timeUser); }
  static Status busy(BusyDetail detail) {
    return Status::Error(Err::BUSY, "Pending driver work", static_cast<int32_t>(detail));
  }
  Status validateBlocking(uint32_t timeout) const {
    if (!_driver.isBound()) return Status::Error(Err::NOT_INITIALIZED, "No binding");
    if (!timeout || timeout >= 0x80000000UL) return Status::Error(Err::INVALID_PARAM, "Blocking deadline must be 1..0x7FFFFFFF ms");
    if (!_wait || !_driver.getConfig().nowMs) return Status::Error(Err::INVALID_CONFIG, "Blocking facade requires clock and application wait");
    return Status::Ok();
  }
  Status singleChunk(uint32_t address, size_t length, bool writing) const {
    if (!_driver.isBound()) return Status::Error(Err::NOT_INITIALIZED, "No binding");
    const auto snapshot = _driver.settingsSnapshot();
    if (!memory::fitsRange(snapshot.capacityBytes, address, length))
      return Status::Error(Err::ADDRESS_OUT_OF_RANGE, "Range exceeds capacity");
    if (!length) return Status::Error(Err::INVALID_PARAM, "Single-transaction length must be nonzero");
    const size_t limit = writing ? snapshot.maxWriteDataBytes : snapshot.maxReadDataBytes;
    if (length > limit || length > memory::bankRemaining(snapshot.geometry, address) ||
        (writing && length > memory::pageRemaining(snapshot.geometry, address)))
      return Status::Error(Err::INVALID_PARAM, "Range does not fit one physical memory transfer");
    return Status::Ok();
  }
  void observeClock(uint32_t& previous, unsigned& stalled) const {
    const uint32_t current = now();
    stalled = current == previous ? stalled + 1U : 0U;
    previous = current;
  }
  template <typename Admit>
  Status run(uint32_t timeout, Admit admit) {
    const Status preflight = validateBlocking(timeout);
    if (!preflight.ok()) return preflight;
    const Status admitted = admit();
    if (!admitted.ok()) return admitted;
    const uint32_t id = _driver.transferSnapshot().requestId;
    uint32_t previous = now();
    unsigned stalled = 0;
    for (;;) {
      const Status status = _driver.pollTransfer(id, now(), 1);
      if (!status.inProgress()) {
        const Status taken = _driver.takeTransferResult(id, _last);
        return taken.ok() ? _last.status : taken;
      }
      _wait(1, _waitUser);
      observeClock(previous, stalled);
      if (stalled >= MAX_STALLED_WAITS) {
        (void)_driver.timeoutTransfer(id);
        const Status taken = _driver.takeTransferResult(id, _last);
        return taken.ok() ? _last.status : taken;
      }
    }
  }
  template <size_t N, typename Value, typename Decode>
  Status readValue(uint32_t address, Value& value, uint32_t timeout, Decode decode) {
    uint8_t bytes[N]{};
    const Status status = read(address, bytes, N, timeout);
    if (status.ok()) value = decode(bytes);
    return status;
  }
  template <size_t N, typename Value, typename Encode>
  Status writeValue(uint32_t address, Value value, uint32_t timeout, Encode encode) {
    uint8_t bytes[N]{};
    encode(value, bytes);
    return write(address, bytes, N, timeout);
  }
};
} // namespace EEPROM24Cxx
