#pragma once

// Example adapter policy tested without Arduino headers. The pinned ESP32 Wire
// implementation maps both ESP_FAIL and ESP_ERR_NOT_FOUND to numeric code 2;
// its documented generic Wire error comments are not evidence of NACK phase.
#include <cstddef>
#include <cstdint>
#include "EEPROM24Cxx/Types.h"

namespace eeprom24cxx_cli {
inline EEPROM24Cxx::TransportResult wireResult(uint8_t code, size_t tx = 0,
                                               size_t rx = 0, bool addressOnly = false,
                                               bool memoryWriteMayCommit = true) {
  using namespace EEPROM24Cxx;
  if (!code) return TransportResult::Ok(tx, rx);
  const TransportCode error = code == 2 || code == 3 ?
      (addressOnly ? TransportCode::NACK_ADDRESS : TransportCode::NACK_UNSPECIFIED) :
      code == 5 ? TransportCode::TIMEOUT : TransportCode::IO_ERROR;
  return TransportResult::Error(error, code,
      addressOnly || !memoryWriteMayCommit ? WriteCommit::NOT_APPLICABLE :
      code == 1 ? WriteCommit::NOT_COMMITTED : WriteCommit::INDETERMINATE);
}
struct WireDiscardResult { bool physicalAttempt; uint8_t code; };
template <class WireType>
WireDiscardResult discardWireTx(WireType& wire, uint8_t address, size_t buffered) {
  // This adapter is the sole owner; beginTransmission starts with an empty TX
  // buffer, the configured capacity is >=32, and callbacks never change it.
  // Thus zero accepted bytes indicates missing storage. ESP32 requestFrom's
  // null-buffer path releases the acquired mutex before issuing any I2C.
  wire.flush();
  if (buffered == 0) {
    (void)wire.requestFrom(address, size_t{0}, true);
    return {false, 4};
  }
  // A nonzero count proves storage exists. Flush removed every queued byte;
  // endTransmission now sends an address-only probe and releases the mutex.
  return {true, wire.endTransmission(true)};
}

// ESP32-specific transport policy, independent of framework headers so the same
// callbacks exercised by firmware can be fault-tested on the host. The owner
// must reserve this controller for the entire callback: Wire mutex acquisition
// is not bounded by setTimeOut(). Requires Arduino-ESP32 >=3.3.11 for null-buffer
// cleanup; never resize or end Wire while this adapter is ready.
template <class WireType>
class WireTransport {
 public:
  static constexpr size_t MAX_BYTES = 32;
  explicit WireTransport(WireType& wire, void (*record)(bool) = nullptr)
      : _wire(wire), _record(record) {}
  void setReady(bool ready) { _ready = ready; }
  bool ready() const { return _ready; }

  EEPROM24Cxx::TransportResult write(uint8_t address, const uint8_t* data,
                                      size_t length, uint32_t timeoutMs) {
    using namespace EEPROM24Cxx;
    if (!_ready || !memoryAddress(address) || !data || !length ||
        length > MAX_BYTES || !validTimeout(timeoutMs))
      return TransportResult::Error(TransportCode::IO_ERROR, -1, WriteCommit::NOT_COMMITTED);
    Timeout timeout(_wire, static_cast<uint16_t>(timeoutMs));
    _wire.beginTransmission(address);
    const size_t buffered = _wire.write(data, length);
    if (buffered != length) {
      discard(address, buffered);
      return TransportResult::Error(TransportCode::IO_ERROR, -2, WriteCommit::NOT_COMMITTED);
    }
    return finish(wireResult(_wire.endTransmission(true), length));
  }

  EEPROM24Cxx::TransportResult read(uint8_t address, const uint8_t* tx,
      size_t txLength, uint8_t* rx, size_t rxLength, uint32_t timeoutMs) {
    using namespace EEPROM24Cxx;
    if (!_ready || !memoryAddress(address) || !tx || !txLength || txLength > 2 ||
        !rx || !rxLength || rxLength > MAX_BYTES || !validTimeout(timeoutMs))
      return TransportResult::Error(TransportCode::IO_ERROR, -1, WriteCommit::NOT_APPLICABLE);
    Timeout timeout(_wire, static_cast<uint16_t>(timeoutMs));
    _wire.beginTransmission(address);
    const size_t buffered = _wire.write(tx, txLength);
    if (buffered != txLength) {
      discard(address, buffered);
      return TransportResult::Error(TransportCode::IO_ERROR, -2, WriteCommit::NOT_APPLICABLE);
    }
    // ESP32 defers this call until requestFrom, producing one repeated-START
    // transaction. A failure here performs no bus transfer; release the lock
    // and invalidate the adapter without sending the buffered address prefix.
    const uint8_t deferred = _wire.endTransmission(false);
    if (deferred) {
      discard(address, buffered);
      return wireResult(deferred, 0, 0, false, false);
    }
    const size_t received = _wire.requestFrom(address, rxLength, true);
    if (received != rxLength) {
      while (_wire.available() > 0) (void)_wire.read();
      return finish(TransportResult::Error(TransportCode::IO_ERROR,
          static_cast<int32_t>(received), WriteCommit::NOT_APPLICABLE, 0, received));
    }
    for (size_t index = 0; index < rxLength; ++index) {
      const int value = _wire.read();
      if (value < 0) return finish(TransportResult::Error(TransportCode::IO_ERROR,
          static_cast<int32_t>(index), WriteCommit::NOT_APPLICABLE, 0, index));
      rx[index] = static_cast<uint8_t>(value);
    }
    return finish(TransportResult::Ok(txLength, rxLength));
  }

  EEPROM24Cxx::TransportResult probe(uint8_t address, uint32_t timeoutMs) {
    using namespace EEPROM24Cxx;
    if (!_ready || address < 0x08 || address > 0x77 || !validTimeout(timeoutMs))
      return TransportResult::Error(TransportCode::IO_ERROR, -1, WriteCommit::NOT_APPLICABLE);
    Timeout timeout(_wire, static_cast<uint16_t>(timeoutMs));
    _wire.beginTransmission(address);
    return finish(wireResult(_wire.endTransmission(true), 0, 0, true));
  }

 private:
  class Timeout {
   public:
    Timeout(WireType& wire, uint16_t value) : _wire(wire), _previous(wire.getTimeOut()) {
      _wire.setTimeOut(value);
    }
    ~Timeout() { _wire.setTimeOut(_previous); }
    Timeout(const Timeout&) = delete;
    Timeout& operator=(const Timeout&) = delete;
   private:
    WireType& _wire;
    uint16_t _previous;
  };
  static bool validTimeout(uint32_t value) { return value > 0 && value <= UINT16_MAX; }
  static bool memoryAddress(uint8_t value) { return value >= 0x50 && value <= 0x57; }
  EEPROM24Cxx::TransportResult finish(EEPROM24Cxx::TransportResult result) {
    if (_record) _record(result.ok());
    return result;
  }
  void discard(uint8_t address, size_t buffered) {
    const auto result = discardWireTx(_wire, address, buffered);
    if (result.physicalAttempt) (void)finish(wireResult(result.code, 0, 0, true));
    _ready = false; // A violated buffer invariant needs explicit owner reinitialization.
  }
  WireType& _wire;
  void (*_record)(bool);
  bool _ready = false;
};
} // namespace eeprom24cxx_cli
