#include "WireTransportHelpers.h"
#include "IdfTransportHelpers.h"
#include "BusRecovery.h"
#include "WriteProtect.h"
#include <cstdio>
#include <initializer_list>
#include <limits>

using namespace EEPROM24Cxx;
#define CHECK(x) do { if (!(x)) { std::printf("[FAIL] transport line %d: %s\n", __LINE__, #x); return 1; } } while (false)

// Model the pinned Wire implementation's deferred repeated START, lock lifetime,
// partial buffering and nullable buffers. No part of the transport policy is
// duplicated here; these calls drive the exact helpers used by the firmware.
struct WireStub {
  uint16_t timeout = 700;
  uint16_t observedTimeout = 0;
  size_t tx = 0;
  size_t capacity = 32;
  size_t delivered = 4;
  size_t readable = 4;
  size_t readIndex = 0;
  uint8_t error = 0;
  unsigned physical = 0;
  unsigned payloadSent = 0;
  unsigned beginnings = 0;
  bool locked = false;
  bool deferred = false;
  bool buffers = true;
  uint16_t getTimeOut() const { return timeout; }
  void setTimeOut(uint16_t value) { timeout = value; }
  void beginTransmission(uint8_t) { ++beginnings; locked = true; deferred = false; tx = 0; }
  size_t write(const uint8_t*, size_t length) {
    tx = !buffers ? 0 : length < capacity ? length : capacity;
    return tx;
  }
  void flush() { tx = 0; readIndex = readable; }
  uint8_t endTransmission(bool stop) {
    if (!buffers) return 4;
    if (!stop) { deferred = true; return 0; }
    ++physical; observedTimeout = timeout;
    payloadSent += static_cast<unsigned>(tx);
    locked = false;
    return error;
  }
  size_t requestFrom(uint8_t, size_t, bool) {
    locked = false;
    if (!buffers) return 0;
    ++physical; observedTimeout = timeout;
    if (deferred) payloadSent += static_cast<unsigned>(tx);
    deferred = false; readIndex = 0;
    return delivered;
  }
  int available() const { return static_cast<int>(readable - readIndex); }
  int read() { return readIndex < readable ? static_cast<int>(0x20U + readIndex++) : -1; }
};
unsigned observations = 0;
unsigned failures = 0;
unsigned writeAttempts = 0, readAttempts = 0, probeAttempts = 0;
void record(bool ok, eeprom24cxx_cli::WireTransferKind kind) {
  ++observations; if (!ok) ++failures;
  if (kind == eeprom24cxx_cli::WireTransferKind::WRITE) ++writeAttempts;
  else if (kind == eeprom24cxx_cli::WireTransferKind::READ) ++readAttempts;
  else ++probeAttempts;
}

struct RecoveryPins {
  uint32_t us = 0;
  bool sclReleased = true;
  bool sdaReleased = true;
  bool clockStuck = false;
  unsigned rises = 0;
  unsigned starts = 0;
  unsigned stops = 0;
  unsigned dataReleaseAfter = 0;
  void sda(bool release) {
    const bool before = sdaHigh();
    sdaReleased = release;
    if (sclHigh() && before && !sdaHigh()) ++starts;
    if (sclHigh() && !before && sdaHigh()) ++stops;
  }
  void scl(bool release) {
    if (release && !sclReleased) ++rises;
    sclReleased = release;
  }
  bool sclHigh() const { return sclReleased && !clockStuck; }
  bool sdaHigh() const { return sdaReleased && rises >= dataReleaseAfter; }
  uint32_t nowUs() const { return us; }
  void delayUs(uint32_t delay) { us += delay; }
};

struct WriteProtectPin {
  uint32_t us = 0, changedAt = 0, enabledAt = 0;
  bool level = false, enabled = false, protectedAtEnable = false;
  bool stuckLow = false, failWrite = false, failEnable = false;
  Status write(bool value) {
    changedAt = us;
    if (failWrite) return Status::Error(Err::INVALID_CONFIG, "Injected WP write failure");
    level = value;
    return Status::Ok();
  }
  Status enable() {
    if (failEnable) return Status::Error(Err::INVALID_CONFIG, "Injected WP enable failure");
    enabledAt = us; protectedAtEnable = level; enabled = true;
    return Status::Ok();
  }
  bool high() const { return enabled && level && !stuckLow; }
  void delayUs(uint32_t delay) { us += delay; }
};

int main() {
  WireStub wire;
  eeprom24cxx_cli::WireTransport<WireStub> transport(wire, record);
  const uint8_t data[4]{0, 1, 2, 3};
  uint8_t output[4]{};
  CHECK(transport.write(0x50, data, 4, 50).writeCommit == WriteCommit::NOT_COMMITTED);
  CHECK(!transport.probe(0x50, 50).ok());
  CHECK(wire.physical == 0 && observations == 0 && !wire.locked);
  transport.setReady(true);
  CHECK(!transport.write(0x50, data, 33, 50).ok());
  CHECK(!transport.write(0x49, data, 4, 50).ok());
  CHECK(!transport.write(0x50, data, 4, 0).ok());
  CHECK(!transport.read(0x50, data, 3, output, 4, 50).ok());
  CHECK(!transport.read(0x50, nullptr, 1, output, 4, 50).ok());
  CHECK(!transport.read(0x50, data, 1, output, 4, 65536).ok());
  CHECK(!transport.probe(0x7F, 50).ok());
  CHECK(!transport.probe(0x50, std::numeric_limits<uint32_t>::max()).ok());
  CHECK(wire.physical == 0 && observations == 0 && wire.timeout == 700);

  auto result = transport.write(0x50, data, 4, 50);
  CHECK(result.ok() && result.completedTxBytes == 4);
  CHECK(wire.physical == 1 && observations == 1 && failures == 0);
  CHECK(wire.observedTimeout == 50 && wire.timeout == 700 && !wire.locked);
  wire.error = 2;
  result = transport.write(0x50, data, 4, 25);
  CHECK(result.code == TransportCode::NACK_UNSPECIFIED);
  CHECK(result.writeCommit == WriteCommit::INDETERMINATE && result.completedTxBytes == 0);
  CHECK(wire.observedTimeout == 25 && wire.timeout == 700 && !wire.locked);
  result = transport.probe(0x50, 15);
  CHECK(result.code == TransportCode::NACK_ADDRESS && result.writeCommit == WriteCommit::NOT_APPLICABLE);
  CHECK(wire.observedTimeout == 15 && wire.timeout == 700);
  wire.error = 5;
  CHECK(transport.write(0x50, data, 4, 50).code == TransportCode::TIMEOUT);
  CHECK(wire.timeout == 700 && !wire.locked);
  wire.error = 0;

  const auto beforeRead = wire.physical;
  result = transport.read(0x50, data, 2, output, 4, 35);
  CHECK(result.ok() && result.completedTxBytes == 2 && result.completedRxBytes == 4);
  CHECK(output[0] == 0x20 && output[3] == 0x23 && wire.physical == beforeRead + 1);
  CHECK(!wire.locked && !wire.deferred && wire.timeout == 700 && wire.observedTimeout == 35);
  const auto beforeCurrent = wire.physical;
  const auto beforeCurrentTx = wire.payloadSent;
  const auto beforeCurrentBegins = wire.beginnings;
  result = transport.read(0x50, nullptr, 0, output, 4, 20);
  CHECK(result.ok() && result.completedTxBytes == 0 && result.completedRxBytes == 4);
  CHECK(wire.physical == beforeCurrent + 1 && wire.payloadSent == beforeCurrentTx);
  CHECK(wire.beginnings == beforeCurrentBegins && !wire.locked && wire.observedTimeout == 20 && wire.timeout == 700);
  wire.delivered = 2; wire.readable = 2;
  result = transport.read(0x50, data, 1, output, 4, 35);
  CHECK(result.code == TransportCode::IO_ERROR && result.completedRxBytes == 2);
  CHECK(result.completedTxBytes == 0 && result.writeCommit == WriteCommit::NOT_APPLICABLE);
  CHECK(wire.available() == 0 && wire.timeout == 700 && !wire.locked);
  result = transport.read(0x50, nullptr, 0, output, 4, 35);
  CHECK(result.code == TransportCode::IO_ERROR && result.completedRxBytes == 2);
  CHECK(result.completedTxBytes == 0 && result.writeCommit == WriteCommit::NOT_APPLICABLE);
  CHECK(wire.available() == 0 && wire.timeout == 700 && !wire.locked);
  wire.delivered = 4; wire.readable = 2;
  result = transport.read(0x50, data, 1, output, 4, 35);
  CHECK(result.code == TransportCode::IO_ERROR && result.completedRxBytes == 2);
  CHECK(result.writeCommit == WriteCommit::NOT_APPLICABLE && wire.timeout == 700);

  // Insufficient storage must never transmit a partial EEPROM page. Flushing
  // before the cleanup STOP yields only an address probe, then disables use.
  wire = {}; wire.capacity = 2;
  result = transport.write(0x50, data, 4, 50);
  CHECK(result.writeCommit == WriteCommit::NOT_COMMITTED && !transport.ready());
  CHECK(wire.physical == 1 && wire.payloadSent == 0 && !wire.locked && wire.timeout == 700);
  CHECK(!transport.write(0x50, data, 4, 50).ok() && wire.physical == 1);
  wire = {}; wire.buffers = false; transport.setReady(true);
  const auto beforeMissing = observations;
  result = transport.write(0x50, data, 4, 50);
  CHECK(result.writeCommit == WriteCommit::NOT_COMMITTED && !transport.ready());
  CHECK(wire.physical == 0 && observations == beforeMissing && !wire.locked && wire.timeout == 700);
  wire = {}; wire.capacity = 1; transport.setReady(true);
  result = transport.read(0x50, data, 2, output, 4, 50);
  CHECK(result.writeCommit == WriteCommit::NOT_APPLICABLE && !transport.ready());
  CHECK(wire.physical == 1 && wire.payloadSent == 0 && !wire.locked && wire.timeout == 700);

  constexpr eeprom24cxx_cli::IdfResultMapper idf{0, 1, 2, 3, 4};
  result = idf.transaction(0, 9, 0, true);
  CHECK(result.ok() && result.completedTxBytes == 9);
  for (const int32_t error : {int32_t{3}, int32_t{4}}) {
    result = idf.transaction(error, 9, 0, true);
    CHECK(result.code == TransportCode::NACK_UNSPECIFIED);
    CHECK(result.writeCommit == WriteCommit::INDETERMINATE && result.completedTxBytes == 0);
  }
  CHECK(idf.transaction(2, 9, 0, true).writeCommit == WriteCommit::NOT_COMMITTED);
  CHECK(idf.transaction(1, 9, 0, true).writeCommit == WriteCommit::INDETERMINATE);
  CHECK(idf.transaction(1, 1, 4, false).writeCommit == WriteCommit::NOT_APPLICABLE);
  CHECK(idf.transaction(1, 1, 4, false).completedRxBytes == 0);
  CHECK(idf.probe(4).code == TransportCode::NACK_ADDRESS);
  CHECK(idf.probe(3).code == TransportCode::IO_ERROR);
  CHECK(idf.probe(1).code == TransportCode::TIMEOUT);
  CHECK(idf.probe(4).writeCommit == WriteCommit::NOT_APPLICABLE);
  unsigned receives = 0, combinedReads = 0;
  const auto receive = [&](int device, uint8_t* rx, size_t count, int timeout) {
    ++receives;
    return device == 42 && rx == output && count == 4 && timeout == 15 ? 0 : -1;
  };
  const auto combined = [&](int device, const uint8_t* tx, size_t txCount, uint8_t* rx, size_t count, int timeout) {
    ++combinedReads;
    return device == 42 && tx == data && txCount == 1 && rx == output && count == 4 && timeout == 15 ? 0 : -1;
  };
  CHECK(eeprom24cxx_cli::idfReadTransaction(42, nullptr, 0, output, 4, 15, receive, combined) == 0);
  CHECK(receives == 1 && combinedReads == 0);
  CHECK(eeprom24cxx_cli::idfReadTransaction(42, data, 1, output, 4, 15, receive, combined) == 0);
  CHECK(receives == 1 && combinedReads == 1);
  CHECK(observations == writeAttempts + readAttempts + probeAttempts);
  CHECK(writeAttempts == 3 && readAttempts == 5 && probeAttempts == 3);

  RecoveryPins pins;
  CHECK(eeprom24cxx_cli::recoverOpenDrainBus(pins, 100).ok());
  CHECK(pins.sclReleased && pins.sdaReleased && pins.rises == 1);
  CHECK(pins.starts == 1 && pins.stops == 1); // Datasheet START, then idle STOP.
  pins = {}; pins.dataReleaseAfter = 3; pins.us = UINT32_MAX - 10U;
  CHECK(eeprom24cxx_cli::recoverOpenDrainBus(pins, 100).ok());
  CHECK(pins.rises == 4 && pins.sclReleased && pins.sdaReleased); // Three clocks then STOP.
  CHECK(pins.starts == 1 && pins.stops == 1);
  pins = {}; pins.clockStuck = true;
  CHECK(eeprom24cxx_cli::recoverOpenDrainBus(pins, 100).is(Err::I2C_TIMEOUT));
  CHECK(pins.us == 100 && pins.sclReleased && pins.sdaReleased);
  pins = {}; pins.dataReleaseAfter = 100;
  CHECK(eeprom24cxx_cli::recoverOpenDrainBus(pins, 1000).is(Err::I2C_BUS));
  CHECK(pins.rises == 9 && pins.sclReleased && pins.sdaReleased);
  CHECK(pins.starts == 0 && pins.stops == 0); // Cannot create START with SDA stuck.
  pins = {}; pins.dataReleaseAfter = 4;
  CHECK(eeprom24cxx_cli::recoverOpenDrainBus(pins, 10).is(Err::I2C_TIMEOUT));
  CHECK(pins.sclReleased && pins.sdaReleased);
  pins = {}; pins.dataReleaseAfter = 9;
  CHECK(eeprom24cxx_cli::recoverOpenDrainBus(pins, 200).ok());
  CHECK(pins.rises == 10 && pins.starts == 1 && pins.stops == 1);

  WriteProtectPin wp;
  eeprom24cxx_cli::WriteProtectControl<WriteProtectPin> protection(wp);
  bool observed = true;
  CHECK(protection.read(observed).is(Err::NOT_INITIALIZED) && observed);
  CHECK(protection.set(false).is(Err::NOT_INITIALIZED) && wp.us == 0);
  CHECK(protection.initialize().ok());
  CHECK(wp.protectedAtEnable && wp.enabledAt == wp.changedAt && wp.us - wp.changedAt >= 2);
  CHECK(protection.read(observed).ok() && observed);
  const uint32_t previousStop = wp.us;
  CHECK(protection.set(false).ok());
  CHECK(wp.changedAt - previousStop >= 2 && wp.us - wp.changedAt >= 2);
  CHECK(protection.read(observed).ok() && !observed);
  wp.stuckLow = true;
  CHECK(protection.set(true).is(Err::INVALID_CONFIG));
  CHECK(protection.initialize().is(Err::INVALID_CONFIG));
  CHECK(protection.read(observed).is(Err::NOT_INITIALIZED));
  wp.stuckLow = false; wp.failEnable = true;
  CHECK(protection.initialize().is(Err::INVALID_CONFIG));
  CHECK(protection.set(false).is(Err::NOT_INITIALIZED));
  wp.failEnable = false; wp.failWrite = true;
  CHECK(protection.initialize().is(Err::INVALID_CONFIG));
  wp.failWrite = false;
  CHECK(protection.initialize().ok());
  wp.failWrite = true;
  CHECK(protection.set(false).is(Err::INVALID_CONFIG));
  CHECK(wp.us - wp.changedAt >= 2);
  std::puts("[PASS] Wire/IDF reads, transport effects, bounded START recovery and WP setup/hold");
  return 0;
}
