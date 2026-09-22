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
  void record(bool ok) {
    if (attempts != UINT32_MAX) ++attempts;
    auto& count = ok ? successes : failures;
    if (count != UINT32_MAX) ++count;
  }
};
struct Platform {
  void (*vprintf)(void*, const char*, va_list) = nullptr;
  uint32_t (*nowMs)(void*) = nullptr;
  EEPROM24Cxx::Status (*probeAddress)(uint8_t, void*) = nullptr;
  TransferStats (*transferStats)(void*) = nullptr;
  void* user = nullptr;
  const char* framework = "unknown";
  const char* frameworkVersion = "unknown";
  const char* target = "unknown";
};

class Cli {
 public:
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
  void printSettings();
  void printProgress(const EEPROM24Cxx::TransferResult& result);
  void printBytes(uint32_t address, size_t length);
  void stop();
  void complete();
  const char* color(unsigned code) const;
  uint32_t now() const;
  Platform _platform{};
  EEPROM24Cxx::Config _config{};
  EEPROM24Cxx::EEPROM24Cxx _device{};
  char _line[256]{};
  uint8_t _data[256]{}; // Borrowed by the driver until its terminal result.
  size_t _length = 0;
  bool _overflow = false;
  bool _color = true;
  bool _operation = false;
  bool _hasResult = false;
  EEPROM24Cxx::TransferResult _lastResult{};
  bool _scan = false;
  uint8_t _scanNext = 0;
  uint8_t _scanLast = 0;
  unsigned _scanFound = 0;
  unsigned _scanErrors = 0;
  bool _stress = false;
  uint32_t _remaining = 0;
  uint32_t _nextMs = 0;
  uint32_t _stressSuccess = 0;
  uint32_t _stressFailures = 0;
};
}  // namespace eeprom24cxx_cli
