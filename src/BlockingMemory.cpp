// Copyright (c) 2026 Thymos Solution s.r.o. SPDX-License-Identifier: MIT
#include "EEPROM24Cxx/BlockingMemory.h"

namespace EEPROM24Cxx {

Status BlockingMemory::read(uint32_t address, uint8_t* data, size_t length, uint32_t timeoutMs) {
  return run(timeoutMs, [&]() { return _driver.startRead(address, data, length, timeoutMs); });
}

Status BlockingMemory::readByte(uint32_t address, uint8_t& value, uint32_t timeoutMs) {
  uint8_t raw = 0;
  const Status status = read(address, &raw, 1, timeoutMs);
  if (status.ok()) value = raw;
  return status;
}

Status BlockingMemory::readOnce(uint32_t address, uint8_t* data, size_t length, uint32_t timeoutMs) {
  const Status status = singleChunk(address, length, false);
  return status.ok() ? read(address, data, length, timeoutMs) : status;
}

Status BlockingMemory::readCurrentAddress(uint8_t* data, size_t length, uint32_t timeoutMs) {
  return run(timeoutMs, [&]() { return _driver.startCurrentRead(data, length, timeoutMs); });
}

Status BlockingMemory::readCurrentAddress(uint8_t& value, uint32_t timeoutMs) {
  uint8_t raw = 0;
  const Status status = readCurrentAddress(&raw, 1, timeoutMs);
  if (status.ok()) value = raw;
  return status;
}

Status BlockingMemory::write(uint32_t address, const uint8_t* data, size_t length, uint32_t timeoutMs) {
  return run(timeoutMs, [&]() { return _driver.startWrite(address, data, length, false, timeoutMs); });
}

Status BlockingMemory::writeByte(uint32_t address, uint8_t value, uint32_t timeoutMs) {
  return write(address, &value, 1, timeoutMs);
}

Status BlockingMemory::writeOnce(uint32_t address, const uint8_t* data, size_t length, uint32_t timeoutMs) {
  const Status status = singleChunk(address, length, true);
  return status.ok() ? write(address, data, length, timeoutMs) : status;
}

Status BlockingMemory::fill(uint32_t address, uint8_t value, size_t length, uint32_t timeoutMs) {
  return run(timeoutMs, [&]() { return _driver.startFill(address, value, length, false, timeoutMs); });
}

Status BlockingMemory::verify(uint32_t address, const uint8_t* expected, size_t length, uint32_t timeoutMs) {
  return run(timeoutMs, [&]() { return _driver.startVerify(address, expected, length, timeoutMs); });
}

Status BlockingMemory::verifyOnce(uint32_t address, const uint8_t* expected, size_t length, uint32_t timeoutMs) {
  const Status status = singleChunk(address, length, false);
  return status.ok() ? verify(address, expected, length, timeoutMs) : status;
}

Status BlockingMemory::writeVerify(uint32_t address, const uint8_t* data, size_t length, uint32_t timeoutMs) {
  return run(timeoutMs, [&]() { return _driver.startWrite(address, data, length, true, timeoutMs); });
}

Status BlockingMemory::fillVerify(uint32_t address, uint8_t value, size_t length, uint32_t timeoutMs) {
  return run(timeoutMs, [&]() { return _driver.startFill(address, value, length, true, timeoutMs); });
}

Status BlockingMemory::update(uint32_t address, const uint8_t* data, size_t length,
                             bool verify, uint32_t timeoutMs) {
  return run(timeoutMs, [&]() { return _driver.startUpdate(address, data, length, verify, timeoutMs); });
}

Status BlockingMemory::updateByte(uint32_t address, uint8_t value, bool verify, uint32_t timeoutMs) {
  return update(address, &value, 1, verify, timeoutMs);
}

Status BlockingMemory::waitUntilReady(uint32_t timeoutMs) {
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

Status BlockingMemory::crc32(uint32_t address, size_t length, uint32_t& value, uint32_t timeoutMs) {
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

uint32_t BlockingMemory::now() const {
  return _driver.getConfig().nowMs(_driver.getConfig().timeUser);
}

Status BlockingMemory::busy(BusyDetail detail) {
  return Status::Error(Err::BUSY, "Pending driver work", static_cast<int32_t>(detail));
}

Status BlockingMemory::validateBlocking(uint32_t timeout) const {
  if (!_driver.isBound()) return Status::Error(Err::NOT_INITIALIZED, "No binding");
  if (!timeout || timeout >= 0x80000000UL)
    return Status::Error(Err::INVALID_PARAM, "Blocking deadline must be 1..0x7FFFFFFF ms");
  if (!_wait || !_driver.getConfig().nowMs)
    return Status::Error(Err::INVALID_CONFIG, "Blocking facade requires clock and application wait");
  return Status::Ok();
}

Status BlockingMemory::singleChunk(uint32_t address, size_t length, bool writing) const {
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

void BlockingMemory::observeClock(uint32_t& previous, unsigned& stalled) const {
  const uint32_t current = now();
  stalled = current == previous ? stalled + 1U : 0U;
  previous = current;
}

Status BlockingMemory::runAdmitted(Status admitted) {
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

} // namespace EEPROM24Cxx
