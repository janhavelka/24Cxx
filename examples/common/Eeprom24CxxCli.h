#pragma once

// Example-only command processor; one application task owns this and the bus.
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include "EEPROM24Cxx/EEPROM24Cxx.h"
#include "TransportDiagnostics.h"

namespace eeprom24cxx_cli {
struct HeapStats {
  uint32_t freeBytes = 0;
  uint32_t minimumFreeBytes = 0;
  uint32_t largestFreeBlock = 0;
};
struct ConsoleStats {
  const char* backend = "unknown";
  int8_t connected = -1; // -1 means the backend cannot observe host presence.
  uint32_t baud = 0;
  uint32_t rxBytes = 0;
  uint32_t rxOverflows = 0;
  bool rxOverflowObservable = false;
  uint32_t txBytes = 0;
  uint32_t txDropped = 0;
  uint32_t writeTimeouts = 0;
  uint32_t formatOverflows = 0;
  uint32_t queuedBytes = 0;
  uint32_t highWaterBytes = 0;
  bool ready = false;
};
struct Platform {
  void (*vprintf)(void*, const char*, va_list) = nullptr;
  uint32_t (*nowMs)(void*) = nullptr;
  EEPROM24Cxx::Status (*probeAddress)(uint8_t, void*) = nullptr;
  TransferStats (*transferStats)(void*) = nullptr;
  void (*resetTransferStats)(void*) = nullptr;
  // Board-owned observation/control only. A WP setter must satisfy the fitted
  // EEPROM's GPIO setup/hold times before returning. No implicit bus transfer.
  EEPROM24Cxx::Status (*readWriteProtect)(bool&, void*) = nullptr;
  EEPROM24Cxx::Status (*setWriteProtect)(bool, void*) = nullptr;
  EEPROM24Cxx::Status (*resetInterface)(void*) = nullptr;
  HeapStats (*heapStats)(void*) = nullptr;
  ConsoleStats (*consoleStats)(void*) = nullptr;
  // Optional output backpressure: pause new polling, keep passive time/deadlines.
  bool (*outputReady)(void*) = nullptr;
  void* user = nullptr;
  const char* framework = "unknown";
  const char* frameworkVersion = "unknown";
  const char* target = "unknown";
  // Cached application configuration; negative pins/zero rate mean unavailable.
  int sdaPin = -1;
  int sclPin = -1;
  int writeProtectPin = -1;
  uint32_t frequencyHz = 0;
};

class Cli {
 public:
  // Reconfiguration is rejected while work, physical settling, or a retained
  // scratch backup owns the old context. Invalid reconfiguration preserves it.
  void setup(const Platform& platform, const EEPROM24Cxx::Config& config);
  void feed(char value);
  // A transport that loses RX bytes must discard the whole partial line.
  void discardInput() { _overflow = true; }
  void processCommand(const char* text);
  void tick();
  void printHelp(const char* topic = nullptr);
  void printPrompt();
 private:
  void print(const char* format, ...);
  void status(EEPROM24Cxx::Status value);
  void printVersion();
  void printHealth();
  void printHeap();
  void printConsole();
  void printRepeat(bool terminal = false);
  void completeRepeat(EEPROM24Cxx::Status result);
  void printSettings();
  void printBus();
  void printModels();
  void printConfiguredBanks();
  void printDetectionSummary();
  void printStats();
  void printTransferStats();
  void printScratch(bool includeResults = true);
  void printProgress(const EEPROM24Cxx::TransferResult& result);
  void printBytes(uint32_t address, size_t length);
  enum class ReadView : uint8_t { NONE, HEX_DUMP, TEXT, STRINGS, CRC, SELFTEST };
  EEPROM24Cxx::Status startReadView(ReadView view, uint32_t address, uint32_t length,
                                  uint32_t minimumStringLength = 4);
  void completeReadView(const EEPROM24Cxx::TransferResult& result);
  void printReadView(uint32_t address, size_t length);
  void finishString();
  enum class ScratchMode : uint8_t { RW_SUITE, MIX, RANDOM, TYPED, XFER };
  enum class ScratchStage : uint8_t { NONE, BACKUP, PATTERN, RANDOM_READS, INSPECT, RESTORE_WAIT, RESTORING };
  enum class ScratchAction : uint8_t { READ, WRITE, FILL, UPDATE, VERIFY, PROBE, CURRENT };
  EEPROM24Cxx::Status startScratch(ScratchMode mode, uint32_t address, uint32_t length, uint32_t rounds);
  void scheduleScratch();
  void completeScratch(const EEPROM24Cxx::TransferResult& result);
  void abortScratch(EEPROM24Cxx::Status reason);
  void finishScratchPrimary(EEPROM24Cxx::Status result);
  const char* scratchModeName() const;
  bool scratchCheck(const char* name, bool passed);
  void checkScratchAdmission();
  void checkScratchRetainedResult();
  bool checkScratchBounds();
  void stop(EEPROM24Cxx::Status reason = EEPROM24Cxx::Status::Error(EEPROM24Cxx::Err::CANCELLED, "CLI job cancelled"));
  void complete();
  void advance();
  bool workActive() const;
  void beginSession(const char* command);
  void finishSession();
  bool expireSession();
  void printJob();
  void printJobResult();
  const char* color(unsigned code) const;
  uint32_t now() const;
  Platform _platform{};
  bool _configured = false;
  EEPROM24Cxx::Config _config{};
  size_t _adapterTxLimit = 0;
  size_t _adapterRxLimit = 0;
  EEPROM24Cxx::I2cProbeFn _adapterProbe = nullptr;
  EEPROM24Cxx::EEPROM24Cxx _device{};
  char _line[256]{};
  uint8_t _data[256]{}; // Borrowed by the driver until its terminal result.
  uint8_t _backup[256]{}; // Retained until verified restoration, including end/cancel.
  uint8_t _scratchExpected[256]{}; // Independent expected contents; never used as a read destination.
  ScratchMode _scratchMode = ScratchMode::RW_SUITE;
  ScratchStage _scratchStage = ScratchStage::NONE;
  bool _backupValid = false;
  bool _scratchDirty = false;
  uint32_t _scratchAddress = 0;
  uint32_t _scratchLength = 0;
  uint32_t _scratchRounds = 0;
  uint32_t _scratchRound = 0;
  uint32_t _scratchReadRound = 0;
  uint8_t _scratchStep = 0;
  ScratchAction _scratchAction = ScratchAction::READ;
  const char* _scratchCheckName = "backup";
  uint32_t _scratchReadOffset = 0;
  uint32_t _scratchReadLength = 0;
  uint32_t _scratchChecks = 0;
  uint32_t _scratchCheckFailures = 0;
  uint32_t _scratchOperationCheckFailures = 0;
  uint32_t _scratchAdmissionAttempts = 0;
  uint32_t _scratchOperations[7]{};
  uint32_t _scratchCurrentSkipped = 0;
  uint32_t _scratchRandom = 0x24C02B01U;
  EEPROM24Cxx::Status _scratchPrimary{};
  EEPROM24Cxx::Status _scratchRestore{};
  EEPROM24Cxx::TransferResult _scratchPrimaryResult{};
  EEPROM24Cxx::TransferResult _scratchRestoreResult{};
  bool _hasScratchPrimaryResult = false;
  bool _hasScratchRestoreResult = false;
  uint32_t _scratchStartedMs = 0;
  uint32_t _scratchPrimaryElapsedMs = 0;
  uint32_t _scratchRestoreStartedMs = 0;
  uint32_t _scratchRestoreElapsedMs = 0;
  uint32_t _scratchVerifiedBytes = 0;
  TransferStats _scratchBusBefore{};
  TransferStats _scratchPrimaryBus{};
  uint32_t _jobs = 0;
  uint32_t _jobSuccesses = 0;
  uint32_t _jobFailures = 0;
  size_t _length = 0;
  bool _overflow = false;
  bool _color = true;
  bool _verbose = false;
  bool _autoPoll = true;
  bool _sessionActive = false;
  uint32_t _nextSessionId = 1;
  uint32_t _sessionId = 0;
  uint32_t _deadlineMs = 0;
  uint32_t _sessionStartedAt = 0;
  uint32_t _sessionElapsedMs = 0;
  uint32_t _sessionBudgetMs = 0;
  char _sessionCommand[16] = "none";
  EEPROM24Cxx::Status _sessionStatus{};
  bool _hasSessionResult = false;
  uint32_t _completedSessionId = 0;
  char _completedCommand[16] = "none";
  EEPROM24Cxx::Status _completedStatus{};
  bool _operation = false;
  bool _interfaceWait = false;
  uint32_t _interfaceReadyAt = 0;
  bool _hasResult = false;
  EEPROM24Cxx::TransferResult _lastResult{};
  ReadView _readView = ReadView::NONE;
  uint32_t _readRequestId = 0;
  uint32_t _readAddress = 0;
  uint32_t _readLength = 0;
  uint32_t _readCompleted = 0;
  uint32_t _crc = 0xFFFFFFFFU;
  uint32_t _minimumStringLength = 4;
  uint32_t _stringAddress = 0;
  uint32_t _stringLength = 0;
  uint32_t _stringCount = 0;
  char _stringPrefix[64]{};
  bool _scan = false;
  uint8_t _scanFirst = 0;
  uint8_t _scanNext = 0;
  uint8_t _scanLast = 0;
  unsigned _scanTested = 0;
  unsigned _scanFound = 0;
  unsigned _scanErrors = 0;
  uint8_t _scanEepromAcks = 0;
  uint8_t _scanEepromTested = 0;
  uint8_t _scanEepromErrors = 0;
  bool _stress = false;
  bool _watch = false;
  uint32_t _repeatAddress = 0;
  uint32_t _repeatLength = 16;
  uint32_t _repeatInterval = 10;
  uint32_t _remaining = 0;
  uint32_t _nextMs = 0;
  uint32_t _stressSuccess = 0;
  uint32_t _stressFailures = 0;
  bool _repeatChecking = false;
  uint8_t _repeatReference[16]{};
  uint32_t _repeatTarget = 0;
  uint32_t _repeatStartedMs = 0;
  uint32_t _repeatElapsedMs = 0;
  uint32_t _roundStartedMs = 0;
  uint32_t _repeatReportAt = 0;
  uint32_t _repeatMinMs = 0;
  uint32_t _repeatMaxMs = 0;
  uint64_t _repeatBytes = 0;
  bool _repeatWasWatch = false;
};
}  // namespace eeprom24cxx_cli
