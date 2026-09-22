// Copyright (c) 2026 Thymos Solution s.r.o. SPDX-License-Identifier: MIT
#pragma once
#include "EEPROM24Cxx/Config.h"
#include "EEPROM24Cxx/Version.h"

namespace EEPROM24Cxx {
struct SettingsSnapshot {
  bool initialized = false;
  bool online = false; // Bound; diagnostic health never prevents owner-directed I/O.
  DriverState state = DriverState::UNINIT;
  DeviceVariant variant = DeviceVariant::ZETTA_ZD24C02B;
  const char* variantName = "Zetta ZD24C02B";
  Geometry geometry = {};
  uint32_t capacityBytes = 256;
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
  bool isOnline() const { return _bound; }
  DriverState state() const { return _health.state; }
  SettingsSnapshot settingsSnapshot() const;

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
  /// At most maxTransactions physical callbacks; no waiting/delays/retries.
  /// No clock hook: tWR wait includes callback timeout plus 1ms rounding margin.
  /// Optional address-only ACK polling may end that conservative wait sooner;
  /// it requires a final readiness ACK and fails on NACK at/after the deadline.
  /// A write is never replayed on error. ACK does not prove WP allowed storage;
  /// verified writes/readback detect mismatch without claiming WP is its cause.
  Status poll(uint32_t nowMs, size_t maxTransactions = 1);
  /// Preserve accepted prefix/uncertainty and physical busy barrier.
  Status cancel();
  /// Exactly once; failed take leaves output unchanged. Results retain no
  /// caller pointers. A retained terminal result blocks further admission.
  Status takeResult(TransferResult& result);
  TransferResult transferSnapshot() const { return _result; }

private:
  Config _config = {};
  Geometry _geometry = {};
  SettingsSnapshot _health = {};
  bool _bound = false;
  TransferResult _result = {};
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
