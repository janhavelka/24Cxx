#include "WireTransportHelpers.h"
#include "IdfTransportHelpers.h"
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
  bool locked = false;
  bool deferred = false;
  bool buffers = true;
  uint16_t getTimeOut() const { return timeout; }
  void setTimeOut(uint16_t value) { timeout = value; }
  void beginTransmission(uint8_t) { locked = true; deferred = false; tx = 0; }
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
void record(bool ok) { ++observations; if (!ok) ++failures; }

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
  wire.delivered = 2; wire.readable = 2;
  result = transport.read(0x50, data, 1, output, 4, 35);
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
  std::puts("[PASS] Wire and native IDF transport fault/timeout/effect policies");
  return 0;
}
