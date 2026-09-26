// Copyright (c) 2026 Thymos Solution s.r.o. SPDX-License-Identifier: MIT
#pragma once
#include "EEPROM24Cxx/Config.h"
#include "EEPROM24Cxx/Version.h"

namespace EEPROM24Cxx {
struct SettingsSnapshot {
  bool bound = false;
  bool initialized = false;
  bool online = false; // Bound; diagnostic health never prevents owner-directed I/O.
  DriverState state = DriverState::UNINIT;
  DeviceVariant variant = DeviceVariant::ZETTA_ZD24C02B;
  const char* variantName = "Zetta ZD24C02B";
  Geometry geometry = {};
  uint32_t capacityBytes = 256;
  uint32_t maxAddress = 255;
  uint16_t pageSizeBytes = 8;
  uint8_t wordAddressBytes = 1;
  uint8_t i2cAddress = cmd::DEFAULT_ADDRESS;
  uint32_t i2cTimeoutMs = 0;
  uint32_t writeCycleMs = 5;
  size_t maxTxBytes = 0;
  size_t maxRxBytes = 0;
  size_t maxWriteDataBytes = 0;
  size_t maxReadDataBytes = 0;
  uint8_t offlineThreshold = 0;
  uint8_t consecutiveFailures = 0;
  uint32_t totalSuccess = 0;
  uint32_t totalFailures = 0;
  uint32_t writeBusyPolls = 0; // Expected address NACKs excluded from failures.
  uint32_t lastOkMs = 0;
  uint32_t lastErrorMs = 0;
  Status lastError = Status::Ok();
  bool hasNowMsHook = false;
  bool hasAckPolling = false;
  bool writeCyclePending = false;
  uint32_t writeReadyAtMs = 0;
  bool transferActive = false;
  bool resultPending = false;
};

/// Non-owning, allocation-free driver. One owner/task must serialize every
/// method and all access to the same physical memory. The application owns bus
/// initialization, clock, pins, locking, WP and recovery. No auto-detection, IDs,
/// reset or configuration registers exist in this device family.
class EEPROM24Cxx {
public:
  EEPROM24Cxx() = default;
  EEPROM24Cxx(const EEPROM24Cxx&) = delete;
  EEPROM24Cxx& operator=(const EEPROM24Cxx&) = delete;
  EEPROM24Cxx(EEPROM24Cxx&&) = delete;
  EEPROM24Cxx& operator=(EEPROM24Cxx&&) = delete;
  /// Bus silent; invalid config preserves binding. Pending work/results/write
  /// cycles reject rebinding. Success resets passive health and logical state.
  Status bind(const Config& config);
  /// Bind, then one tracked nondestructive presence transaction. An I2C failure
  /// preserves the valid binding so the owner can diagnose/recover later.
  Status begin(const Config& config);
  /// Bus silent, cancel/retain result, release callbacks; keep physical busy barrier.
  void end();
  /// Time bookkeeping only; no bus or health changes. An active ACK-polling
  /// write still needs poll() for its final readiness check.
  void tick(uint32_t nowMs);
  /// Untracked presence check; does not establish identity/capacity.
  Status probe();
  /// One tracked presence check; no reset, retries or configuration writes.
  Status recover();
  bool isInitialized() const { return _bound; }
  bool isBound() const { return _bound; }
  /// MB85RC-compatible binding shorthand, including DEGRADED/OFFLINE.
  /// Inspect state() for passive transport health; neither gates owner work.
  bool isOnline() const { return _bound; }
  DriverState state() const { return _health.state; }
  DriverState driverState() const { return state(); }
  /// Cached configuration. Callback contexts remain owned by the application.
  const Config& getConfig() const { return _config; }
  /// Bus-silent diagnostics; geometry fields describe a selected layout only
  /// while bound, and never establish the identity of the physical chip.
  SettingsSnapshot settingsSnapshot() const;
  SettingsSnapshot getSettings() const { return settingsSnapshot(); }
  SettingsSnapshot getSettingsSnapshot() const { return settingsSnapshot(); }
  Status getSettings(SettingsSnapshot& out) const {
    out = settingsSnapshot();
    return Status::Ok();
  }
  /// Active capacity/address; zero when unbound (inspect isBound()).
  uint32_t capacityBytes() const { return _bound ? _geometry.capacityBytes : 0; }
  uint32_t maxAddress() const { return _bound ? _geometry.capacityBytes - 1U : 0; }
  const char* variantName() const {
    return _bound ? ::EEPROM24Cxx::variantName(_config.variant) : "unbound";
  }
  size_t maxWriteDataBytes() const { return _bound ? settingsSnapshot().maxWriteDataBytes : 0; }
  size_t maxReadDataBytes() const { return _bound ? settingsSnapshot().maxReadDataBytes : 0; }
  /// Passive per-binding telemetry, reset by successful bind()/end(). Counts
  /// saturate. lastError survives tracked successes. Expected busy ACK polls,
  /// validation and logical/content failures do not increment totalFailures.
  uint32_t lastOkMs() const { return _health.lastOkMs; }
  uint32_t lastErrorMs() const { return _health.lastErrorMs; }
  Status lastError() const { return _health.lastError; }
  uint8_t consecutiveFailures() const { return _health.consecutiveFailures; }
  uint32_t totalFailures() const { return _health.totalFailures; }
  uint32_t totalSuccess() const { return _health.totalSuccess; }
  uint32_t writeBusyPolls() const { return _health.writeBusyPolls; }

  /// Admission is bus silent; Ok means queued, not completed. Buffers remain
  /// borrowed through terminal completion/cancel/end and must not be mutated.
  /// Zero length is valid at an address <= capacity and immediately succeeds.
  /// timeoutMs=0 disables the logical deadline; otherwise < 2^31 milliseconds
  /// and Config.nowMs is required. Deadlines start on first poll and are checked
  /// after callbacks; one callback may exceed the logical deadline by its own
  /// bounded transport timeout. Clock samples must advance by < 2^31 ms.
  Status startRead(uint32_t address, uint8_t* data, size_t length, uint32_t timeoutMs = 0);
  Status startWrite(uint32_t address, const uint8_t* data, size_t length,
                    bool verify = false, uint32_t timeoutMs = 0);
  Status startFill(uint32_t address, uint8_t value, size_t length,
                   bool verify = false, uint32_t timeoutMs = 0);
  Status startVerify(uint32_t address, const uint8_t* data, size_t length, uint32_t timeoutMs = 0);
  /// Sibling-compatible cooperative names. start* and unqualified request*
  /// allocate upper-half IDs, retained in progress/result through end().
  Status requestRead(uint32_t address, uint8_t* data, size_t length) {
    return startRead(address, data, length);
  }
  Status requestWrite(uint32_t address, const uint8_t* data, size_t length) {
    return startWrite(address, data, length);
  }
  Status requestFill(uint32_t address, uint8_t value, size_t length) {
    return startFill(address, value, length);
  }
  Status requestVerify(uint32_t address, const uint8_t* data, size_t length) {
    return startVerify(address, data, length);
  }
  /// Caller IDs in 1..0x7FFFFFFF correlate delayed owner actions. These have
  /// the same bus-silent admission/buffer/deadline rules as start* above.
  Status requestRead(uint32_t requestId, uint32_t address, uint8_t* data,
                     size_t length, uint32_t timeoutMs = 0);
  Status requestWrite(uint32_t requestId, uint32_t address, const uint8_t* data,
                      size_t length, uint32_t timeoutMs = 0);
  Status requestFill(uint32_t requestId, uint32_t address, uint8_t value,
                     size_t length, uint32_t timeoutMs = 0);
  Status requestVerify(uint32_t requestId, uint32_t address, const uint8_t* data,
                       size_t length, uint32_t timeoutMs = 0);
  Status requestVerifiedWrite(uint32_t requestId, uint32_t address, const uint8_t* data,
                              size_t length, uint32_t timeoutMs = 0);
  Status requestVerifiedFill(uint32_t requestId, uint32_t address, uint8_t value,
                             size_t length, uint32_t timeoutMs = 0);
  /// At most maxTransactions physical callbacks; no waiting/delays/retries.
  /// No clock hook: tWR wait includes callback timeout plus 1ms rounding margin.
  /// Optional address-only ACK polling may end that conservative wait sooner;
  /// it requires a final readiness ACK and fails on NACK at/after the deadline.
  /// A write is never replayed on error. ACK does not prove WP allowed storage;
  /// verified writes/readback detect mismatch without claiming WP is its cause.
  Status poll(uint32_t nowMs, size_t maxTransactions = 1);
  Status pollTransfer(uint32_t nowMs, size_t maxTransactions = 1) {
    return poll(nowMs, maxTransactions);
  }
  /// A mismatched ID returns BUSY without advancing time or doing I2C.
  Status pollTransfer(uint32_t requestId, uint32_t nowMs, size_t maxTransactions);
  /// Preserve accepted prefix/uncertainty and physical busy barrier.
  Status cancel();
  Status cancelTransfer() { return cancel(); }
  Status cancelTransfer(uint32_t requestId);
  /// Owner-declared timeout, including clockless owners. Bus silent; retains
  /// all write evidence and the physical programming barrier, just like cancel.
  Status timeoutTransfer(uint32_t requestId);
  /// Exactly once; failed take leaves output unchanged. Results retain no
  /// caller pointers. A retained terminal result blocks further admission.
  Status takeResult(TransferResult& result);
  TransferResult transferSnapshot() const { return _result; }
  bool isTransferBusy() const { return active(); }
  Status getTransferStatus() const { return _result.status; }
  /// Copy active progress/terminal result, or leave out unchanged on NO_RESULT.
  Status getTransferProgress(TransferResult& out) const {
    if (_result.state == TransferState::IDLE)
      return Status::Error(Err::NO_RESULT, "No transfer progress");
    out = _result;
    return Status::Ok();
  }
  Status takeTransferResult(TransferResult& out) { return takeResult(out); }
  /// Mismatched IDs leave the retained result and output unchanged.
  Status takeTransferResult(uint32_t requestId, TransferResult& out);

private:
  Config _config = {};
  Geometry _geometry = {};
  SettingsSnapshot _health = {};
  bool _bound = false;
  TransferResult _result = {};
  uint32_t _nextRequestId = AUTOMATIC_REQUEST_ID_FIRST;
  uint8_t* _readBuffer = nullptr;
  const uint8_t* _sourceBuffer = nullptr;
  uint8_t _fillValue = 0;
  bool _verifyPhase = false;
  bool _started = false;
  uint32_t _startedAt = 0;
  uint32_t _timeoutMs = 0;
  uint32_t _nowMs = 0;
  bool _writePending = false;
  uint32_t _writeReadyAt = 0;
  uint8_t _writeAddress = 0;
  size_t _pendingLength = 0;
  uint8_t _tx[MAX_TRANSPORT_TX_BYTES] = {};
  uint8_t _rx[MAX_TRANSPORT_RX_BYTES] = {};

  bool active() const;
  bool terminal() const;
  Status gate() const;
  Status admit(TransferKind, uint32_t, size_t, uint32_t);
  Status request(uint32_t requestId, TransferKind kind, uint32_t address,
                 uint8_t* data, const uint8_t* source, uint8_t fillValue,
                 size_t length, uint32_t timeoutMs);
  uint32_t allocateRequestId();
  void finish(Status status, TransferState state);
  void settleWrite();
  void track(Status status, uint32_t nowMs);
  uint32_t afterCallback(uint32_t fallback) const;
  size_t encode(uint32_t address, uint8_t& busAddress);
  size_t chunkLength(uint32_t address, size_t remaining, bool writing) const;
  Status presence(bool tracked);
  static Status transportStatus(const TransportResult&, size_t tx, size_t rx);
};
} // namespace EEPROM24Cxx
