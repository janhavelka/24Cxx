#pragma once
#include "Eeprom24CxxCli.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace EEPROM24Cxx;
using eeprom24cxx_cli::BusTransferKind;
struct Fixture {
  std::string output;
  std::vector<uint8_t> addresses;
  uint8_t memory[2048]{};
  unsigned transfers = 0;
  unsigned writes = 0;
  unsigned reads = 0;
  unsigned probes = 0;
  unsigned resets = 0;
  bool failReset = false;
  uint32_t pointer = 0;
  eeprom24cxx_cli::TransferStats counters{};
  uint32_t ms = 0;
  uint32_t busyUntil = 0;
  bool writeProtected = false;
  bool failWrite = false;
  unsigned failWriteAttempt = 0;
  unsigned corruptWriteAttempt = 0;
  uint32_t corruptWriteAddress = 0;
  bool nackWrite = false;
  bool failRead = false;
  bool failProbe = false;
  Geometry geometry = geometryFor(DeviceVariant::ZETTA_ZD24C02B);
  static uint32_t clock(void* user) { return static_cast<Fixture*>(user)->ms; }
  static void print(void* user, const char* format, va_list args) {
    char text[2048]; std::vsnprintf(text, sizeof(text), format, args);
    static_cast<Fixture*>(user)->output += text;
  }
  uint32_t address(uint8_t bus, const uint8_t* prefix) const {
    uint32_t value = prefix[0];
    if (geometry.wordAddressBytes == 2) value = (value << 8U) | prefix[1];
    const uint32_t bank = (static_cast<uint32_t>(bus) >> geometry.bankAddressShift) &
        ((uint32_t{1} << geometry.bankAddressBits) - uint32_t{1});
    return value | (bank << (8U * geometry.wordAddressBytes));
  }
  static TransportResult write(uint8_t bus, const uint8_t* tx, size_t n, uint32_t timeout, void* user) {
    auto& f = *static_cast<Fixture*>(user);
    const auto result = writeBody(bus, tx, n, timeout, user);
    f.counters.record(result.ok(), BusTransferKind::WRITE); return result;
  }
  static TransportResult writeBody(uint8_t bus, const uint8_t* tx, size_t n, uint32_t, void* user) {
    auto& f = *static_cast<Fixture*>(user);
    ++f.transfers; ++f.writes; f.addresses.push_back(bus);
    if (f.ms < f.busyUntil) return TransportResult::Error(TransportCode::NACK_ADDRESS, 0, WriteCommit::NOT_COMMITTED);
    if (f.failWrite || f.writes == f.failWriteAttempt) { f.failWrite = false; f.busyUntil = f.ms + 5; return TransportResult::Error(TransportCode::TIMEOUT); }
    if (f.nackWrite) { f.nackWrite = false; return TransportResult::Error(TransportCode::NACK_ADDRESS, 0, WriteCommit::NOT_COMMITTED); }
    const uint32_t start = f.address(bus, tx);
    if (n <= f.geometry.wordAddressBytes) return TransportResult::Error(TransportCode::IO_ERROR);
    if (!f.writeProtected) for (size_t i = f.geometry.wordAddressBytes; i < n; ++i) {
      const uint32_t target = (start / f.geometry.pageSizeBytes) * f.geometry.pageSizeBytes +
          (start + static_cast<uint32_t>(i) - f.geometry.wordAddressBytes) % f.geometry.pageSizeBytes;
      if (target >= sizeof(f.memory)) return TransportResult::Error(TransportCode::IO_ERROR);
      f.memory[target] = tx[i];
    }
    if (f.writes == f.corruptWriteAttempt && f.corruptWriteAddress < sizeof(f.memory))
      f.memory[f.corruptWriteAddress] ^= 0x80;
    f.busyUntil = f.ms + 5;
    f.pointer = (start + static_cast<uint32_t>(n) - f.geometry.wordAddressBytes) % f.geometry.capacityBytes;
    return TransportResult::Ok(n, 0);
  }
  static TransportResult read(uint8_t bus, const uint8_t* tx, size_t n,
                               uint8_t* out, size_t count, uint32_t timeout, void* user) {
    auto& f = *static_cast<Fixture*>(user);
    const auto result = readBody(bus, tx, n, out, count, timeout, user);
    f.counters.record(result.ok(), BusTransferKind::READ); return result;
  }
  static TransportResult readBody(uint8_t bus, const uint8_t* tx, size_t n,
                               uint8_t* out, size_t count, uint32_t, void* user) {
    auto& f = *static_cast<Fixture*>(user);
    ++f.transfers; ++f.reads; f.addresses.push_back(bus);
    if (f.ms < f.busyUntil) return TransportResult::Error(TransportCode::NACK_ADDRESS);
    if (f.failRead) { f.failRead = false; return TransportResult::Error(TransportCode::TIMEOUT); }
    const uint32_t start = n ? f.address(bus, tx) : f.pointer;
    if ((n && n != f.geometry.wordAddressBytes) || start + count > sizeof(f.memory))
      return TransportResult::Error(TransportCode::IO_ERROR);
    std::memcpy(out, f.memory + start, count);
    f.pointer = (start + static_cast<uint32_t>(count)) % f.geometry.capacityBytes;
    return TransportResult::Ok(n, count);
  }
  static TransportResult probe(uint8_t bus, uint32_t, void* user) {
    auto& f = *static_cast<Fixture*>(user);
    ++f.transfers; ++f.probes; f.addresses.push_back(bus);
    if (f.failProbe) { f.failProbe = false; f.counters.record(false, BusTransferKind::PROBE); return TransportResult::Error(TransportCode::TIMEOUT); }
    const bool acknowledged = bus >= 0x50 && bus <= 0x57 && f.ms >= f.busyUntil;
    f.counters.record(acknowledged, BusTransferKind::PROBE);
    if (!acknowledged)
      return TransportResult::Error(TransportCode::NACK_ADDRESS);
    return TransportResult::Ok(0, 0);
  }
  static Status scan(uint8_t bus, void* user) {
    return eeprom24cxx_cli::probeStatus(probe(bus, 50, user), "test address probe");
  }
  Config config() {
    Config c;
    c.i2cWrite = write; c.i2cWriteRead = read; c.i2cProbe = probe; c.i2cUser = this;
    c.nowMs = clock; c.timeUser = this; c.maxTxBytes = 17; c.maxRxBytes = 16;
    c.offlineThreshold = 2;
    c.supportsCurrentAddressRead = true;
    return c;
  }
  eeprom24cxx_cli::Platform platform() {
    eeprom24cxx_cli::Platform p;
    p.vprintf = print; p.nowMs = clock; p.probeAddress = scan; p.user = this;
    p.transferStats = [](void* user) { return static_cast<Fixture*>(user)->counters; };
    p.resetTransferStats = [](void* user) { static_cast<Fixture*>(user)->counters = {}; };
    p.readWriteProtect = [](bool& value, void* user) { value = static_cast<Fixture*>(user)->writeProtected; return Status::Ok(); };
    p.setWriteProtect = [](bool value, void* user) { static_cast<Fixture*>(user)->writeProtected = value; return Status::Ok(); };
    p.resetInterface = [](void* user) {
      auto& f = *static_cast<Fixture*>(user); ++f.resets;
      return f.failReset ? Status::Error(Err::I2C_BUS, "injected reset failure") : Status::Ok();
    };
    return p;
  }
  bool run(eeprom24cxx_cli::Cli& cli, unsigned ticks = 80) {
    for (unsigned i = 0; i < ticks; ++i) {
      const unsigned before = transfers;
      cli.tick();
      if (transfers > before + 1) return false;
      ++ms;
    }
    return true;
  }
};
