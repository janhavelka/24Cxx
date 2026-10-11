#pragma once

// Example-only single-owner staging. Transport callbacks must return immediately
// and report only bytes actually accepted; never overwrite queued bytes on loss.
#include "Eeprom24CxxCli.h"
#include <cstdio>
#include <cstring>

namespace eeprom24cxx_cli {
class ConsoleOutput {
 public:
  static constexpr size_t CAPACITY = 16384;
  static constexpr size_t RESERVE = 8192; // Covers one complete help response.
  static constexpr size_t DRAIN_BUDGET = 256;
  using WriteSome = size_t (*)(const uint8_t*, size_t, void*);
  ConsoleStats& stats() { return _stats; }
  const ConsoleStats& stats() const { return _stats; }
  void ready(bool value) { _stats.ready = value; }
  bool outputReady() const { return _stats.ready && CAPACITY - _size >= RESERVE; }
  void countRx() { add(_stats.rxBytes, 1); }
  void countRxOverflow() { add(_stats.rxOverflows, 1); }
  void append(const char* format, va_list args) {
    char formatted[512];
    const int length = std::vsnprintf(formatted, sizeof(formatted), format, args);
    if (length < 0) { add(_stats.formatOverflows, 1); return; }
    const auto count = static_cast<size_t>(length);
    if (count >= sizeof(formatted)) {
      add(_stats.formatOverflows, 1); add(_stats.txDropped, count); return;
    }
    if (count > CAPACITY - _size) { add(_stats.txDropped, count); return; }
    const size_t tail = (_head + _size) % CAPACITY;
    const size_t first = count < CAPACITY - tail ? count : CAPACITY - tail;
    std::memcpy(_data + tail, formatted, first);
    std::memcpy(_data, formatted + first, count - first);
    _size += count;
    _stats.queuedBytes = static_cast<uint32_t>(_size);
    if (_stats.queuedBytes > _stats.highWaterBytes) _stats.highWaterBytes = _stats.queuedBytes;
  }
  void drain(uint32_t nowMs, WriteSome write, void* user = nullptr) {
    size_t budget = DRAIN_BUDGET;
    bool progressed = false;
    while (_stats.ready && _size && budget) {
      size_t count = _size < 64 ? _size : 64;
      if (count > CAPACITY - _head) count = CAPACITY - _head;
      if (count > budget) count = budget;
      const size_t accepted = write(_data + _head, count, user);
      if (accepted > count) { add(_stats.formatOverflows, 1); break; }
      if (!accepted) break;
      _head = (_head + accepted) % CAPACITY;
      _size -= accepted; budget -= accepted;
      add(_stats.txBytes, accepted); progressed = true;
    }
    _stats.queuedBytes = static_cast<uint32_t>(_size);
    if (!_size || progressed) { _blocked = false; _stallReported = false; }
    else if (!_blocked) { _blocked = true; _blockedSince = nowMs; }
    else if (!_stallReported && nowMs - _blockedSince >= 1000U) {
      add(_stats.writeTimeouts, 1); _stallReported = true;
    }
  }
 private:
  static void add(uint32_t& value, size_t amount) {
    value += amount > UINT32_MAX - value ? UINT32_MAX - value : static_cast<uint32_t>(amount);
  }
  uint8_t _data[CAPACITY]{};
  size_t _head = 0;
  size_t _size = 0;
  ConsoleStats _stats{};
  uint32_t _blockedSince = 0;
  bool _blocked = false;
  bool _stallReported = false;
};
} // namespace eeprom24cxx_cli
