#pragma once

// Example-only command processor; one application task owns this and the bus.
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include "EEPROM24Cxx/EEPROM24Cxx.h"

namespace eeprom24cxx_cli {
struct TransferStats {
  uint32_t attempts = 0;
  uint32_t successes = 0;
  uint32_t failures = 0;
  uint32_t writeAttempts = 0;
  uint32_t readAttempts = 0;
  uint32_t probeAttempts = 0;
  void record(bool ok) {
    if (attempts != UINT32_MAX) ++attempts;
    auto& count = ok ? successes : failures;
    if (count != UINT32_MAX) ++count;
  }
};
struct HeapStats {
  uint32_t freeBytes = 0;
  uint32_t minimumFreeBytes = 0;
  uint32_t largestFreeBlock = 0;
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
  void* user = nullptr;
  const char* framework = "unknown";
  const char* frameworkVersion = "unknown";
  const char* target = "unknown";
};

class Cli {
 public:
  // Reconfiguration is rejected while work, physical settling, or a retained
  // scratch backup owns the old context. Invalid reconfiguration preserves it.
  void setup(const Platform& platform, const EEPROM24Cxx::Config& config);
  void feed(char value);
  void processCommand(const char* text);
  void tick();
  void printHelp();
  void printPrompt();
 private:
  void print(const char* format, ...);
  void status(EEPROM24Cxx::Status value);
  void printVersion();
  void printHealth();
  void printHeap();
  void printSettings();
  void printModels();
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
  enum class ScratchStage : uint8_t { NONE, BACKUP, PATTERN, RESTORE_WAIT, RESTORING };
  EEPROM24Cxx::Status startScratch(ScratchMode mode, uint32_t address, uint32_t length, uint32_t rounds);
  void scheduleScratch();
  void completeScratch(const EEPROM24Cxx::TransferResult& result);
  void abortScratch(EEPROM24Cxx::Status reason);
  void finishScratchPrimary(EEPROM24Cxx::Status result);
  void stop();
  void complete();
  const char* color(unsigned code) const;
  uint32_t now() const;
  Platform _platform{};
  bool _configured = false;
  EEPROM24Cxx::Config _config{};
  EEPROM24Cxx::EEPROM24Cxx _device{};
  char _line[256]{};
  uint8_t _data[256]{}; // Borrowed by the driver until its terminal result.
  uint8_t _backup[256]{}; // Retained until verified restoration, including end/cancel.
  ScratchMode _scratchMode = ScratchMode::RW_SUITE;
  ScratchStage _scratchStage = ScratchStage::NONE;
  bool _backupValid = false;
  bool _scratchDirty = false;
  uint32_t _scratchAddress = 0;
  uint32_t _scratchLength = 0;
  uint32_t _scratchRounds = 0;
  uint32_t _scratchRound = 0;
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
  uint8_t _scanNext = 0;
  uint8_t _scanLast = 0;
  unsigned _scanFound = 0;
  unsigned _scanErrors = 0;
  bool _stress = false;
  bool _watch = false;
  uint32_t _repeatAddress = 0;
  uint32_t _repeatLength = 16;
  uint32_t _repeatInterval = 10;
  uint32_t _remaining = 0;
  uint32_t _nextMs = 0;
  uint32_t _stressSuccess = 0;
  uint32_t _stressFailures = 0;
};
}  // namespace eeprom24cxx_cli
