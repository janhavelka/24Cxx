#include "EEPROM24Cxx/EEPROM24Cxx.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>
#include <vector>

namespace e = EEPROM24Cxx;
static_assert(!std::is_copy_constructible<e::EEPROM24Cxx>::value, "Driver state must not alias");
static int failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::printf("[FAIL] %s:%d %s\n", __func__, __LINE__, #expr); ++failures; return; } } while (false)

// Silicon model wraps page writes and bank-local read pointers. Crossing those
// boundaries in the driver therefore corrupts the independently modeled data.
struct Bus {
  struct Frame { uint8_t slave; uint32_t address; size_t length; char op; };
  e::Geometry geometry = e::geometryFor(e::DeviceVariant::ZETTA_ZD24C02B);
  std::vector<uint8_t> memory = std::vector<uint8_t>(524288, 0xFF);
  std::vector<Frame> frames;
  uint32_t ms = 0, busyUntil = 0, callbackAdvance = 0, observedTimeout = 0;
  bool busy = false;
  unsigned calls = 0, writes = 0, reads = 0, probes = 0, failAt = 0;
  bool failAll = false, acceptFailedWrite = false, writeProtected = false;
  e::TransportResult failure = e::TransportResult::Error(e::TransportCode::TIMEOUT, 77);
  bool ready() { if (busy && (ms - busyUntil) < 0x80000000UL) busy = false; return !busy; }
  bool failing() const { return failAll || (failAt && failAt == calls); }
  uint32_t address(uint8_t slave, const uint8_t* tx) const {
    const uint32_t bank = (slave >> geometry.bankAddressShift) & ((1UL << geometry.bankAddressBits) - 1UL);
    const uint32_t word = geometry.wordAddressBytes == 1 ? tx[0] : (static_cast<uint32_t>(tx[0]) << 8U) | tx[1];
    return (bank << (8U * geometry.wordAddressBytes)) | word;
  }
  static uint32_t clock(void* p) { return static_cast<Bus*>(p)->ms; }
  static e::TransportResult write(uint8_t slave, const uint8_t* tx, size_t length, uint32_t timeout, void* p) {
    auto& b = *static_cast<Bus*>(p); ++b.calls; ++b.writes; b.observedTimeout = timeout; b.ms += b.callbackAdvance;
    if (!b.ready()) return e::TransportResult::Error(e::TransportCode::NACK_ADDRESS, 0, e::WriteCommit::NOT_COMMITTED);
    if (length <= b.geometry.wordAddressBytes || !timeout) return e::TransportResult::Error(e::TransportCode::IO_ERROR);
    const uint32_t a = b.address(slave, tx); const size_t n = length - b.geometry.wordAddressBytes;
    b.frames.push_back({slave, a, n, 'w'});
    if (!b.failing() || b.acceptFailedWrite) {
      if (!b.writeProtected) {
        const uint32_t pageStart = a - a % b.geometry.pageSizeBytes;
        for (size_t i = 0; i < n; ++i)
          b.memory[pageStart + static_cast<uint32_t>((a + i) % b.geometry.pageSizeBytes)] = tx[b.geometry.wordAddressBytes + i];
      }
      b.busy = true; b.busyUntil = b.ms + b.geometry.writeCycleMs;
    }
    return b.failing() ? b.failure : e::TransportResult::Ok(length, 0);
  }
  static e::TransportResult read(uint8_t slave, const uint8_t* tx, size_t txLen, uint8_t* rx, size_t n, uint32_t timeout, void* p) {
    auto& b = *static_cast<Bus*>(p); ++b.calls; ++b.reads; b.observedTimeout = timeout; b.ms += b.callbackAdvance;
    if (!b.ready()) return e::TransportResult::Error(e::TransportCode::NACK_ADDRESS);
    if (txLen != b.geometry.wordAddressBytes || !n || !timeout) return e::TransportResult::Error(e::TransportCode::IO_ERROR);
    const uint32_t a = b.address(slave, tx); b.frames.push_back({slave, a, n, 'r'});
    if (b.failing()) { std::fill(rx, rx + n, 0xAB); return b.failure; }
    const uint32_t bankSize = 1UL << (8U * b.geometry.wordAddressBytes), bankStart = a - a % bankSize;
    for (size_t i = 0; i < n; ++i) rx[i] = b.memory[bankStart + static_cast<uint32_t>((a + i) % bankSize)];
    return e::TransportResult::Ok(txLen, n);
  }
  static e::TransportResult probe(uint8_t slave, uint32_t timeout, void* p) {
    auto& b = *static_cast<Bus*>(p); ++b.calls; ++b.probes; b.observedTimeout = timeout; b.ms += b.callbackAdvance;
    b.frames.push_back({slave, 0, 0, 'p'});
    if (b.failing()) return b.failure;
    return b.ready() ? e::TransportResult::Ok(0, 0) : e::TransportResult::Error(e::TransportCode::NACK_ADDRESS, 0, e::WriteCommit::NOT_COMMITTED);
  }
  e::Config config(e::DeviceVariant variant = e::DeviceVariant::ZETTA_ZD24C02B) {
    geometry = e::geometryFor(variant); e::Config c; c.variant = variant;
    c.i2cWrite = write; c.i2cWriteRead = read; c.i2cUser = this; c.nowMs = clock; c.timeUser = this; return c;
  }
};
static e::TransferResult finish(e::EEPROM24Cxx& d, Bus& b, size_t budget = 1) {
  for (unsigned i = 0; i < 200000 && d.settingsSnapshot().transferActive; ++i) {
    const auto calls = b.calls; d.poll(b.ms, budget);
    if (b.calls - calls > budget) { std::printf("[FAIL] callback budget\n"); ++failures; break; }
    ++b.ms;
  }
  e::TransferResult r;
  if (!d.takeResult(r).ok()) { std::printf("[FAIL] no completion\n"); ++failures; }
  return r;
}
static void lifecycleAndValidation() {
  Bus b; e::EEPROM24Cxx d; uint8_t data[4] = {};
  CHECK(d.state() == e::DriverState::UNINIT); CHECK(d.startRead(0, data, 1).is(e::Err::NOT_INITIALIZED));
  auto c = b.config(); CHECK(d.bind(c).ok()); CHECK(b.calls == 0); auto s = d.settingsSnapshot();
  CHECK(s.variant == e::DeviceVariant::ZETTA_ZD24C02B && s.capacityBytes == 256 && s.pageSizeBytes == 8);
  CHECK(s.maxWriteDataBytes == 8 && s.wordAddressBytes == 1 && s.writeCycleMs == 5);
  CHECK(d.startRead(255, data, 2).is(e::Err::ADDRESS_OUT_OF_RANGE));
  CHECK(d.startRead(UINT32_MAX, data, 1).is(e::Err::ADDRESS_OUT_OF_RANGE));
  CHECK(d.startRead(1, data, std::numeric_limits<size_t>::max()).is(e::Err::ADDRESS_OUT_OF_RANGE));
  CHECK(d.startWrite(0, nullptr, 1).is(e::Err::INVALID_PARAM));
  CHECK(d.startRead(0, data, 1, 0x80000000UL).is(e::Err::INVALID_PARAM)); CHECK(b.calls == 0);
  c.i2cWrite = nullptr; CHECK(d.bind(c).is(e::Err::INVALID_CONFIG));
  CHECK(d.isBound()); CHECK(d.probe().ok()); CHECK(b.calls == 1 && d.settingsSnapshot().totalSuccess == 0);
  CHECK(d.startRead(256, nullptr, 0).ok()); CHECK(d.startRead(0, data, 1).is(e::Err::BUSY));
  e::TransferResult r; CHECK(d.takeResult(r).ok()); CHECK(r.state == e::TransferState::SUCCEEDED);
  r.address = 99; CHECK(d.takeResult(r).is(e::Err::NO_RESULT)); CHECK(r.address == 99);
  CHECK(d.startRead(0, data, 1).ok()); CHECK(d.bind(b.config()).is(e::Err::BUSY));
  const auto before = b.calls; d.end(); CHECK(b.calls == before && !d.isBound());
  CHECK(d.bind(b.config()).is(e::Err::BUSY)); CHECK(d.takeResult(r).ok() && r.state == e::TransferState::CANCELLED);
  CHECK(d.bind(b.config()).ok());
}
static void geometryValidation() {
  Bus b; e::EEPROM24Cxx d; auto c = b.config(e::DeviceVariant::C16); c.i2cAddress = 0x51;
  CHECK(d.bind(c).is(e::Err::INVALID_CONFIG)); c = b.config(e::DeviceVariant::MICROCHIP_24LC1025); c.i2cAddress = 0x54;
  CHECK(d.bind(c).is(e::Err::INVALID_CONFIG)); c.i2cAddress = 0x53; CHECK(d.bind(c).ok());
  c.variant = e::DeviceVariant::CUSTOM; c.customGeometry = {256, 16, 1, 0, 0, 10}; c.writeCycleMs = 5;
  CHECK(d.bind(c).is(e::Err::INVALID_CONFIG)); c.writeCycleMs = 10; CHECK(d.bind(c).ok());
  CHECK(d.settingsSnapshot().pageSizeBytes == 16); c.customGeometry.bankAddressBits = 1;
  CHECK(d.bind(c).is(e::Err::INVALID_CONFIG)); c.customGeometry = {256, 7, 1, 0, 0, 5};
  CHECK(d.bind(c).is(e::Err::INVALID_CONFIG)); c.customGeometry = {256, 8, 3, 0, 0, 5};
  CHECK(d.bind(c).is(e::Err::INVALID_CONFIG)); c = b.config(e::DeviceVariant::C32); c.maxTxBytes = 2;
  CHECK(d.bind(c).is(e::Err::INVALID_CONFIG)); c.maxTxBytes = 3; c.maxRxBytes = 0;
  CHECK(d.bind(c).is(e::Err::INVALID_CONFIG)); c = b.config(); c.variant = static_cast<e::DeviceVariant>(255);
  CHECK(d.bind(c).is(e::Err::INVALID_CONFIG)); CHECK(b.calls == 0);
}
static void pagesAndAllPresets() {
  for (unsigned variant = 0; variant <= static_cast<unsigned>(e::DeviceVariant::ST_M24M01); ++variant) {
    Bus b; e::EEPROM24Cxx d; auto c = b.config(static_cast<e::DeviceVariant>(variant)); c.maxTxBytes = 7; c.maxRxBytes = 3;
    CHECK(d.bind(c).ok()); uint8_t data[37], out[37] = {};
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = static_cast<uint8_t>(i * 13 + 7);
    const uint32_t start = b.geometry.pageSizeBytes - 2U;
    CHECK(d.startWrite(start, data, sizeof(data), true).ok()); CHECK(b.calls == 0); auto r = finish(d, b, 4);
    CHECK(r.status.ok() && r.bytesAccepted == sizeof(data) && r.bytesCompleted == sizeof(data));
    CHECK(r.bytesVerified == sizeof(data) && r.match && r.writeCommit == e::WriteCommit::VERIFIED);
    CHECK(std::equal(data, data + sizeof(data), b.memory.begin() + start));
    for (const auto& f : b.frames) if (f.op == 'w') {
      CHECK(f.length + b.geometry.wordAddressBytes <= c.maxTxBytes);
      CHECK(f.address / b.geometry.pageSizeBytes == (f.address + f.length - 1) / b.geometry.pageSizeBytes);
    }
    CHECK(d.startRead(start, out, sizeof(out)).ok()); r = finish(d, b, 2);
    CHECK(r.status.ok() && std::equal(data, data + sizeof(data), out)); CHECK(b.observedTimeout == c.i2cTimeoutMs);
  }
}
static void bankBoundaries() {
  struct Case { e::DeviceVariant v; uint32_t boundary; uint8_t upperSlave; };
  const Case cases[] = {{e::DeviceVariant::C04, 256, 0x51}, {e::DeviceVariant::C08, 768, 0x53},
    {e::DeviceVariant::C16, 1792, 0x57}, {e::DeviceVariant::MICROCHIP_24LC1025, 65536, 0x54}, {e::DeviceVariant::ST_M24M01, 65536, 0x51}};
  for (const auto& item : cases) {
    Bus b; e::EEPROM24Cxx d; CHECK(d.bind(b.config(item.v)).ok()); uint8_t data[20];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = static_cast<uint8_t>(i);
    CHECK(d.startWrite(item.boundary - 4, data, sizeof(data), true).ok()); auto r = finish(d, b); CHECK(r.status.ok());
    bool seen = false;
    for (const auto& f : b.frames) {
      if (f.address >= item.boundary && f.slave == item.upperSlave) seen = true;
      const uint32_t bank = 1UL << (8U * b.geometry.wordAddressBytes);
      CHECK(f.address / bank == (f.address + f.length - 1) / bank);
    }
    CHECK(seen);
  }
}
static void timingAndAckPolling() {
  Bus b; e::EEPROM24Cxx d; auto c = b.config(); c.i2cProbe = Bus::probe;
  CHECK(d.bind(c).ok()); uint8_t bytes[10] = {}; CHECK(d.startWrite(0, bytes, sizeof(bytes)).ok());
  CHECK(d.poll(0).inProgress()); CHECK(b.writes == 1 && b.probes == 0);
  CHECK(d.transferSnapshot().bytesAccepted == 8 && d.transferSnapshot().bytesCompleted == 0);
  b.ms = 1; CHECK(d.poll(b.ms, 50).inProgress()); CHECK(b.probes == 1 && b.writes == 1);
  auto s = d.settingsSnapshot(); CHECK(s.writeBusyPolls == 1 && s.totalFailures == 0 && s.totalSuccess == 1);
  b.ms = 5; CHECK(d.poll(b.ms).inProgress()); CHECK(b.probes == 2 && b.writes == 1);
  CHECK(d.transferSnapshot().bytesCompleted == 8); auto r = finish(d, b); CHECK(r.status.ok() && b.writes == 2);
  CHECK(d.settingsSnapshot().totalFailures == 0);
  Bus slow; e::EEPROM24Cxx timer; auto tc = slow.config(); tc.nowMs = nullptr; tc.i2cTimeoutMs = 50;
  slow.callbackAdvance = 50; CHECK(timer.bind(tc).ok()); CHECK(timer.startWrite(0, bytes, 1).ok());
  CHECK(timer.poll(0).inProgress()); CHECK(slow.ms == 50 && timer.settingsSnapshot().writeReadyAtMs == 56);
  timer.tick(55); CHECK(timer.settingsSnapshot().writeCyclePending); CHECK(timer.poll(56).ok());
  CHECK(timer.takeResult(r).ok()); CHECK(r.bytesCompleted == 1 && slow.calls == 1);
  CHECK(timer.startRead(0, bytes, 1, 1).is(e::Err::INVALID_CONFIG)); CHECK(slow.calls == 1);
  // A single poll can complete an ACK and start another write: account for all
  // callback durations when no post-callback clock sample is available.
  Bus multi; e::EEPROM24Cxx md; auto mc = multi.config(); mc.nowMs = nullptr; mc.i2cProbe = Bus::probe;
  multi.callbackAdvance = 10; CHECK(md.bind(mc).ok()); CHECK(md.startWrite(0, bytes, sizeof(bytes)).ok());
  CHECK(md.poll(0, 3).inProgress()); CHECK(multi.writes == 2 && multi.probes == 1);
  CHECK(md.settingsSnapshot().writeReadyAtMs == 156); r = finish(md, multi); CHECK(r.status.ok());
}
static void healthAndReadFailures() {
  Bus b; e::EEPROM24Cxx d; auto c = b.config(); c.offlineThreshold = 2; c.maxRxBytes = 4; b.failAll = true;
  CHECK(d.begin(c).is(e::Err::I2C_TIMEOUT)); CHECK(d.isBound() && d.state() == e::DriverState::DEGRADED);
  CHECK(d.probe().is(e::Err::I2C_TIMEOUT)); CHECK(d.settingsSnapshot().totalFailures == 1);
  CHECK(d.recover().is(e::Err::I2C_TIMEOUT)); CHECK(d.state() == e::DriverState::OFFLINE); b.failAll = false;
  uint8_t out[8]; std::fill(out, out + 8, 0xCC); CHECK(d.startRead(0, out, 8).ok()); b.failAt = b.calls + 2;
  auto r = finish(d, b); CHECK(r.status.is(e::Err::I2C_TIMEOUT));
  CHECK(r.bytesCompleted == 4 && r.failedChunkOffset == 4 && r.failedChunkLength == 4);
  CHECK(out[0] == 0xFF && out[3] == 0xFF && out[4] == 0xCC && out[7] == 0xCC);
  CHECK(d.settingsSnapshot().totalSuccess == 1 && d.settingsSnapshot().totalFailures == 3);
  b.failAt = 0; CHECK(d.recover().ok()); CHECK(d.state() == e::DriverState::READY && d.settingsSnapshot().consecutiveFailures == 0);
}
static void writeProtectionAndFill() {
  Bus b; e::EEPROM24Cxx d; CHECK(d.bind(b.config()).ok()); b.writeProtected = true;
  CHECK(d.startFill(3, 0xAA, 20, true).ok()); auto r = finish(d, b);
  CHECK(r.status.is(e::Err::VERIFY_MISMATCH) && !r.match && r.expected == 0xAA && r.actual == 0xFF);
  CHECK(r.bytesAccepted == 20 && r.bytesCompleted == 20 && r.bytesVerified == 0);
  CHECK(r.writeCommit == e::WriteCommit::ACCEPTED && d.settingsSnapshot().totalFailures == 0);
  b.writeProtected = false; CHECK(d.startFill(3, 0xAA, 20, true).ok()); r = finish(d, b);
  CHECK(r.status.ok() && r.match && r.bytesVerified == 20);
  uint8_t data[20]; std::fill(data, data + 20, 0xAA); data[11] = 0xAB;
  CHECK(d.startVerify(3, data, 20).ok()); r = finish(d, b);
  CHECK(r.status.is(e::Err::VERIFY_MISMATCH) && r.bytesVerified == 11 && r.mismatchOffset == 11 && r.bytesCompleted == 11);
}
static void writeEvidenceAndNoReplay() {
  struct Case { e::TransportResult failure; e::WriteCommit expected; size_t accepted; bool pending; };
  const Case cases[] = {
    {e::TransportResult::Error(e::TransportCode::NACK_ADDRESS, 1, e::WriteCommit::NOT_COMMITTED), e::WriteCommit::NOT_COMMITTED, 0, false},
    {e::TransportResult::Error(e::TransportCode::TIMEOUT, 2), e::WriteCommit::INDETERMINATE, 0, true},
    {e::TransportResult::Error(e::TransportCode::BUS_ERROR, 3, e::WriteCommit::ACCEPTED, 5), e::WriteCommit::ACCEPTED, 4, true},
    {e::TransportResult::Error(e::TransportCode::NACK_DATA, 4, e::WriteCommit::ACCEPTED, 5), e::WriteCommit::INDETERMINATE, 0, true},
    {e::TransportResult::Error(e::TransportCode::TIMEOUT, 5, e::WriteCommit::NOT_COMMITTED, 3), e::WriteCommit::INDETERMINATE, 0, true},
    {e::TransportResult::Error(static_cast<e::TransportCode>(255), 6, e::WriteCommit::NOT_COMMITTED), e::WriteCommit::INDETERMINATE, 0, true},
    {e::TransportResult::Error(e::TransportCode::BUS_ERROR, 7, e::WriteCommit::NOT_COMMITTED, 0, 1), e::WriteCommit::INDETERMINATE, 0, true},
    {e::TransportResult::Error(e::TransportCode::IO_ERROR, 8, e::WriteCommit::ACCEPTED, 6), e::WriteCommit::INDETERMINATE, 0, true},
    {e::TransportResult::Error(e::TransportCode::NACK_UNSPECIFIED, 9, e::WriteCommit::ACCEPTED, 5), e::WriteCommit::INDETERMINATE, 0, true},
    {e::TransportResult::Error(e::TransportCode::TIMEOUT, 10, e::WriteCommit::NOT_COMMITTED, 1), e::WriteCommit::NOT_COMMITTED, 0, false},
    {e::TransportResult::Error(e::TransportCode::TIMEOUT, 11, static_cast<e::WriteCommit>(255), 5), e::WriteCommit::INDETERMINATE, 0, true},
    {e::TransportResult::Ok(4, 0), e::WriteCommit::INDETERMINATE, 0, true},
    {e::TransportResult::Ok(6, 0), e::WriteCommit::INDETERMINATE, 0, true}};
  for (const auto& item : cases) {
    Bus b; e::EEPROM24Cxx d; CHECK(d.bind(b.config()).ok()); uint8_t data[4] = {1,2,3,4};
    b.failAt = 1; b.failure = item.failure; b.acceptFailedWrite = item.pending;
    CHECK(d.startWrite(0, data, 4, true).ok()); CHECK(!d.poll(0, 100).ok()); CHECK(b.writes == 1 && b.reads == 0);
    auto r = d.transferSnapshot(); CHECK(r.writeCommit == item.expected && r.lastChunkCommit == item.expected);
    CHECK(r.bytesAccepted == item.accepted && r.bytesCompleted == 0 && r.failedChunkLength == 4);
    CHECK(d.settingsSnapshot().writeCyclePending == item.pending); CHECK(d.takeResult(r).ok());
    if (item.pending) {
      CHECK(d.startRead(0, data, 4).is(e::Err::BUSY)); const auto calls = b.calls;
      d.tick(5); CHECK(d.settingsSnapshot().writeCyclePending); d.tick(6);
      CHECK(!d.settingsSnapshot().writeCyclePending && b.calls == calls);
    }
    b.failAt = 0; b.ms = 6; CHECK(d.startRead(0, data, 4).ok()); r = finish(d, b);
    CHECK(r.status.ok() && b.writes == 1);
  }
  Bus b; e::EEPROM24Cxx d; CHECK(d.bind(b.config()).ok()); uint8_t data[12] = {};
  b.failAt = 2; b.failure = e::TransportResult::Error(e::TransportCode::NACK_ADDRESS, 0, e::WriteCommit::NOT_COMMITTED);
  CHECK(d.startWrite(0, data, 12).ok()); auto r = finish(d, b);
  CHECK(r.bytesAccepted == 8 && r.bytesCompleted == 8 && r.failedChunkOffset == 8 && r.failedChunkLength == 4);
  CHECK(r.writeCommit == e::WriteCommit::ACCEPTED && r.lastChunkCommit == e::WriteCommit::NOT_COMMITTED);
}
static void cancellationDeadlineAndRollover() {
  uint8_t data[12] = {}; e::TransferResult r; Bus b; e::EEPROM24Cxx d; CHECK(d.bind(b.config()).ok());
  CHECK(d.startWrite(0, data, sizeof(data)).ok()); CHECK(d.poll(0).inProgress());
  CHECK(d.cancel().ok()); CHECK(d.takeResult(r).ok());
  CHECK(r.state == e::TransferState::CANCELLED && r.bytesAccepted == 8 && r.bytesCompleted == 0);
  CHECK(r.failedChunkOffset == 0 && r.failedChunkLength == 8 && r.writeCommit == e::WriteCommit::ACCEPTED);
  d.end(); CHECK(!d.isBound()); CHECK(d.bind(b.config()).is(e::Err::BUSY)); d.tick(6); b.ms = 6;
  CHECK(d.bind(b.config()).ok()); CHECK(d.startWrite(0, data, 8, false, 2).ok()); CHECK(d.poll(6).inProgress());
  b.ms = 8; CHECK(d.poll(8).is(e::Err::TIMEOUT)); CHECK(d.takeResult(r).ok());
  CHECK(r.state == e::TransferState::TIMED_OUT && r.failedChunkLength == 8);
  CHECK(d.startRead(0, data, 1).is(e::Err::BUSY)); d.tick(12);
  b.ms = UINT32_MAX - 2U; b.busy = false; CHECK(d.startWrite(0, data, 1, false, 20).ok());
  CHECK(d.poll(b.ms).inProgress()); CHECK(d.settingsSnapshot().writeReadyAtMs == 3);
  b.ms = 2; CHECK(d.poll(2).inProgress()); b.ms = 3; CHECK(d.poll(3).ok());
  CHECK(d.takeResult(r).ok() && r.bytesCompleted == 1);
  Bus dl; e::EEPROM24Cxx deadline; auto c = dl.config(); c.maxRxBytes = 1;
  CHECK(deadline.bind(c).ok()); CHECK(deadline.startRead(0, data, 2, 3).ok());
  dl.ms = UINT32_MAX - 1U; CHECK(deadline.poll(dl.ms).inProgress()); dl.ms = 1;
  CHECK(deadline.poll(dl.ms).is(e::Err::TIMEOUT)); CHECK(deadline.takeResult(r).ok() && r.bytesCompleted == 1 && dl.reads == 1);
  CHECK(deadline.startRead(0, data, 1).ok()); const auto calls = dl.calls;
  CHECK(deadline.poll(dl.ms, 0).inProgress() && dl.calls == calls);
  CHECK(deadline.cancel().ok()); CHECK(deadline.takeResult(r).ok());
}
static void ackFaultsAndBufferLimits() {
  Bus b; e::EEPROM24Cxx d; auto c = b.config(); c.i2cProbe = Bus::probe;
  CHECK(d.bind(c).ok()); uint8_t data[8] = {}; CHECK(d.startWrite(0, data, sizeof(data)).ok());
  CHECK(d.poll(0).inProgress()); b.failAt = 2; b.failure = e::TransportResult::Error(e::TransportCode::NACK_UNSPECIFIED);
  b.ms = 1; CHECK(d.poll(1).is(e::Err::I2C_NACK));
  CHECK(d.settingsSnapshot().totalFailures == 1 && d.settingsSnapshot().writeBusyPolls == 0);
  e::TransferResult r; CHECK(d.takeResult(r).ok()); CHECK(r.bytesAccepted == 8 && r.bytesCompleted == 0);
  CHECK(d.settingsSnapshot().writeCyclePending);
  Bus absent; e::EEPROM24Cxx ad; auto ac = absent.config(); ac.i2cProbe = Bus::probe;
  CHECK(ad.bind(ac).ok()); CHECK(ad.startWrite(0, data, 8).ok()); CHECK(ad.poll(0).inProgress());
  absent.failAll = true; absent.failure = e::TransportResult::Error(e::TransportCode::NACK_ADDRESS);
  absent.ms = 5; CHECK(ad.poll(5).inProgress());
  CHECK(ad.settingsSnapshot().writeBusyPolls == 1 && ad.settingsSnapshot().totalFailures == 0);
  ad.tick(6); CHECK(ad.settingsSnapshot().writeCyclePending); // Time alone cannot replace final ACK.
  absent.ms = 6; CHECK(ad.poll(6).is(e::Err::I2C_NACK_ADDR));
  CHECK(ad.takeResult(r).ok()); CHECK(r.bytesAccepted == 8 && r.bytesCompleted == 0);
  CHECK(ad.settingsSnapshot().totalFailures == 1);
  ad.tick(6); CHECK(!ad.settingsSnapshot().writeCyclePending);
  Bus large; e::EEPROM24Cxx ld; auto lc = large.config(e::DeviceVariant::ST_M24M01);
  lc.maxTxBytes = std::numeric_limits<size_t>::max(); lc.maxRxBytes = lc.maxTxBytes;
  CHECK(ld.bind(lc).ok()); CHECK(ld.startFill(0, 0x37, 700, true).ok()); r = finish(ld, large, 10);
  CHECK(r.status.ok() && r.bytesVerified == 700); for (const auto& f : large.frames) CHECK(f.length <= 128);
}
static void postCallbackDeadlinesAndShortRead() {
  Bus b; e::EEPROM24Cxx d; auto c = b.config(); c.maxRxBytes = 1; b.callbackAdvance = 4;
  CHECK(d.bind(c).ok()); uint8_t data[4] = {};
  CHECK(d.startRead(0, data, 4, 3).ok()); CHECK(d.poll(0, 100).is(e::Err::TIMEOUT));
  e::TransferResult r; CHECK(d.takeResult(r).ok()); CHECK(r.bytesCompleted == 1 && b.reads == 1);
  CHECK(d.settingsSnapshot().totalSuccess == 1 && d.settingsSnapshot().totalFailures == 0);
  b.failAt = b.calls + 1; b.failure = e::TransportResult::Ok(1, 0); data[0] = 0x33;
  CHECK(d.startRead(0, data, 1).ok()); CHECK(d.poll(b.ms).is(e::Err::I2C_ERROR));
  CHECK(d.takeResult(r).ok()); CHECK(r.bytesCompleted == 0 && data[0] == 0x33);
  b.failAll = true; b.failure = e::TransportResult::Error(e::TransportCode::BUS_ERROR);
  for (unsigned i = 0; i < 270; ++i) d.recover();
  CHECK(d.settingsSnapshot().consecutiveFailures == 255);
  Bus ack; e::EEPROM24Cxx ad; auto ac = ack.config(); ac.i2cProbe = Bus::probe;
  CHECK(ad.bind(ac).ok()); CHECK(ad.startWrite(0, data, 1, false, 2).ok());
  CHECK(ad.poll(0).inProgress()); ack.callbackAdvance = 3; ack.ms = 1;
  CHECK(ad.poll(1).is(e::Err::TIMEOUT)); CHECK(ad.takeResult(r).ok());
  CHECK(r.bytesAccepted == 1 && r.failedChunkLength == 1 && ack.probes == 1);
  CHECK(ad.settingsSnapshot().writeBusyPolls == 1 && ad.settingsSnapshot().totalFailures == 0);
}
static void verificationEvidenceAndLifecycle() {
  Bus b; e::EEPROM24Cxx d; auto c = b.config(); c.maxRxBytes = 1;
  CHECK(d.bind(c).ok()); uint8_t bytes[2] = {0x31, 0x32};
  CHECK(d.startWrite(0, bytes, 2, true).ok());
  auto r = d.transferSnapshot(); CHECK(r.writeStatus.inProgress() && !r.verificationAttempted);
  CHECK(d.cancel().ok()); CHECK(b.calls == 0); CHECK(d.takeResult(r).ok());
  CHECK(r.writeStatus.inProgress() && r.writeCommit == e::WriteCommit::NOT_APPLICABLE && !r.verificationAttempted);

  CHECK(d.startWrite(0, bytes, 2, true).ok()); CHECK(d.poll(0).inProgress());
  b.ms = 6; CHECK(d.poll(b.ms).inProgress());
  r = d.transferSnapshot(); CHECK(r.bytesCompleted == 2 && r.bytesVerified == 1);
  CHECK(r.writeStatus.ok() && r.verificationAttempted && r.verifyStatus.ok());
  CHECK(d.cancel().ok()); CHECK(d.takeResult(r).ok());
  CHECK(r.state == e::TransferState::CANCELLED && r.writeStatus.ok() && r.verifyStatus.ok());
  CHECK(r.bytesVerified == 1 && r.writeCommit == e::WriteCommit::ACCEPTED);

  CHECK(d.startWrite(0, bytes, 2, true, 7).ok()); CHECK(d.poll(b.ms).inProgress());
  b.ms = 12; b.callbackAdvance = 2; CHECK(d.poll(b.ms).is(e::Err::TIMEOUT));
  CHECK(d.takeResult(r).ok()); CHECK(r.bytesVerified == 1 && r.verificationAttempted);
  CHECK(r.writeStatus.ok() && r.verifyStatus.ok() && r.state == e::TransferState::TIMED_OUT);

  b.callbackAdvance = 0; b.failAt = b.calls + 1;
  b.failure = e::TransportResult::Error(e::TransportCode::BUS_ERROR, 123);
  CHECK(d.startVerify(0, bytes, 2).ok()); CHECK(d.poll(b.ms).is(e::Err::I2C_BUS));
  CHECK(d.takeResult(r).ok()); CHECK(r.verificationAttempted && r.verifyStatus.is(e::Err::I2C_BUS));
  CHECK(r.verifyStatus.detail == 123 && r.writeCommit == e::WriteCommit::NOT_APPLICABLE);

  b.failAt = 0; bytes[1] ^= 1; CHECK(d.startVerify(0, bytes, 2).ok()); r = finish(d, b);
  CHECK(r.verifyStatus.is(e::Err::VERIFY_MISMATCH) && r.verifyStatus.detail == 1);
  CHECK(r.status.detail == 1 && r.verificationAttempted && r.bytesVerified == 1);
  CHECK(d.startVerify(0, nullptr, 0).ok()); CHECK(d.takeResult(r).ok());
  CHECK(r.match && r.status.ok() && !r.verificationAttempted);
}
static void bankAckAndEndedBarrier() {
  Bus b; e::EEPROM24Cxx d; auto c = b.config(e::DeviceVariant::MICROCHIP_24LC1025);
  c.i2cProbe = Bus::probe; CHECK(d.bind(c).ok());
  CHECK(d.startFill(65534, 0x6A, 4).ok()); auto r = finish(d, b, 4);
  CHECK(r.status.ok() && r.bytesAccepted == 4 && r.bytesCompleted == 4);
  uint8_t writtenAddress = 0; bool upperProbe = false;
  for (const auto& frame : b.frames) {
    if (frame.op == 'w') writtenAddress = frame.slave;
    if (frame.op == 'p') {
      CHECK(frame.slave == writtenAddress);
      if (frame.slave == 0x54) upperProbe = true;
    }
  }
  CHECK(upperProbe);
  CHECK(d.startFill(65536, 0x7B, 1).ok()); CHECK(d.poll(b.ms).inProgress());
  const auto readyAt = d.settingsSnapshot().writeReadyAtMs; const auto calls = b.calls;
  d.end(); d.end(); CHECK(b.calls == calls);
  CHECK(d.transferSnapshot().state == e::TransferState::CANCELLED);
  CHECK(d.settingsSnapshot().writeCyclePending && d.settingsSnapshot().writeReadyAtMs == readyAt);
  CHECK(d.settingsSnapshot().variant == e::DeviceVariant::ZETTA_ZD24C02B);
  CHECK(d.settingsSnapshot().capacityBytes == 256 && d.settingsSnapshot().maxAddress == 255);
  CHECK(!d.settingsSnapshot().bound && d.state() == e::DriverState::UNINIT);
  CHECK(d.takeResult(r).ok() && r.bytesAccepted == 1 && r.failedChunkLength == 1);
  CHECK(d.bind(c).is(e::Err::BUSY)); d.tick(readyAt - 1U);
  CHECK(d.bind(c).is(e::Err::BUSY)); d.tick(readyAt); CHECK(d.bind(c).ok());
  CHECK(b.calls == calls && d.settingsSnapshot().bound && d.settingsSnapshot().maxAddress == 131071);
}
static void cachedDiagnosticsAndErrorVocabulary() {
  Bus b; e::EEPROM24Cxx d; e::SettingsSnapshot s; e::TransferResult r;
  CHECK(d.getSettings(s).ok() && !s.bound);
  CHECK(d.capacityBytes() == 0 && d.maxAddress() == 0 && d.driverState() == e::DriverState::UNINIT);
  CHECK(d.maxWriteDataBytes() == 0 && d.maxReadDataBytes() == 0 && std::strcmp(d.variantName(), "unbound") == 0);
  r.address = 123; CHECK(d.getTransferProgress(r).is(e::Err::NO_RESULT) && r.address == 123);
  CHECK(!d.isTransferBusy() && d.getTransferStatus().ok());
  auto c = b.config(e::DeviceVariant::C04); c.offlineThreshold = 1;
  CHECK(d.bind(c).ok()); CHECK(d.getConfig().i2cUser == &b && d.getSettings().bound);
  CHECK(d.getSettingsSnapshot().capacityBytes == 512 && d.capacityBytes() == 512 && d.maxAddress() == 511);
  CHECK(d.maxWriteDataBytes() == 16 && d.maxReadDataBytes() == 128 && std::strcmp(d.variantName(), "24C04") == 0);
  CHECK(d.totalSuccess() == 0 && d.totalFailures() == 0 && d.lastError().ok() && b.calls == 0);
  b.ms = 10; b.failAll = true; b.failure = e::TransportResult::Error(e::TransportCode::NACK_DATA, 37);
  CHECK(d.recover().is(e::Err::I2C_NACK_DATA)); CHECK(d.lastError().detail == 37 && d.lastErrorMs() == 10);
  CHECK(d.driverState() == e::DriverState::OFFLINE && d.isBound() && d.isOnline() && d.isInitialized());
  CHECK(d.totalFailures() == 1 && d.consecutiveFailures() == 1 && d.writeBusyPolls() == 0);
  b.failAll = false; b.ms = 20; CHECK(d.recover().ok());
  const auto calls = b.calls;
  CHECK(d.lastOkMs() == 20 && d.lastErrorMs() == 10 && d.lastError().detail == 37);
  CHECK(d.driverState() == e::DriverState::READY && d.totalSuccess() == 1 && d.totalFailures() == 1);
  CHECK(d.consecutiveFailures() == 0 && d.getSettings(s).ok() && s.state == d.state());
  uint8_t data = 0; CHECK(d.startRead(0, &data, 1).ok());
  CHECK(d.isTransferBusy() && d.getTransferStatus().inProgress());
  CHECK(d.getTransferProgress(r).ok() && r.bytesRequested == 1 && r.kind == e::TransferKind::READ);
  CHECK(b.calls == calls); CHECK(d.cancel().ok());
  CHECK(d.takeTransferResult(r).ok() && !d.isTransferBusy());
  CHECK(d.getTransferProgress(r).is(e::Err::NO_RESULT) && r.state == e::TransferState::CANCELLED);
  d.end(); CHECK(d.lastError().ok() && d.totalFailures() == 0 && b.calls == calls);
  CHECK(std::strcmp(e::toString(e::Err::I2C_NACK), "I2C_NACK") == 0);
  CHECK(std::strcmp(e::toString(e::WriteCommit::INDETERMINATE), "INDETERMINATE") == 0);
  CHECK(std::strcmp(e::toString(e::DriverState::OFFLINE), "OFFLINE") == 0);
  CHECK(std::strcmp(e::toString(e::TransferKind::VERIFIED_FILL), "VERIFIED_FILL") == 0);
  CHECK(std::strcmp(e::toString(e::TransferState::TIMED_OUT), "TIMED_OUT") == 0);
  CHECK(std::strcmp(e::toString(e::TransportCode::NACK_UNSPECIFIED), "NACK_UNSPECIFIED") == 0);
  CHECK(std::strcmp(e::toString(static_cast<e::TransportCode>(255)), "UNKNOWN") == 0);
}
static void requestIdentityAndStaleActions() {
  Bus b; e::EEPROM24Cxx d; auto c = b.config(); c.nowMs = nullptr;
  CHECK(d.bind(c).ok()); uint8_t bytes[2] = {0x11, 0x22};
  CHECK(d.requestVerifiedWrite(42, 0, bytes, 2).ok());
  CHECK(d.transferSnapshot().requestId == 42 && b.calls == 0);
  CHECK(d.pollTransfer(42, 0, 1).inProgress());
  CHECK(d.settingsSnapshot().writeCyclePending && d.settingsSnapshot().writeReadyAtMs == 56);
  const auto calls = b.calls;
  const auto mismatch = static_cast<int32_t>(e::BusyDetail::REQUEST_ID_MISMATCH);
  auto s = d.pollTransfer(41, 1000, 50); CHECK(s.is(e::Err::BUSY) && s.detail == mismatch);
  s = d.cancelTransfer(41); CHECK(s.is(e::Err::BUSY) && s.detail == mismatch);
  s = d.timeoutTransfer(41); CHECK(s.is(e::Err::BUSY) && s.detail == mismatch);
  e::TransferResult r; r.requestId = 123;
  s = d.takeTransferResult(41, r); CHECK(s.is(e::Err::BUSY) && s.detail == mismatch && r.requestId == 123);
  CHECK(d.isTransferBusy() && d.settingsSnapshot().writeCyclePending && b.calls == calls);
  CHECK(d.transferSnapshot().bytesCompleted == 0 && d.transferSnapshot().bytesAccepted == 2);
  CHECK(d.timeoutTransfer(42).ok());
  CHECK(d.getTransferStatus().is(e::Err::TIMEOUT) && d.settingsSnapshot().writeCyclePending);
  s = d.takeTransferResult(41, r); CHECK(s.is(e::Err::BUSY) && r.requestId == 123);
  CHECK(d.takeTransferResult(42, r).ok());
  CHECK(r.requestId == 42 && r.state == e::TransferState::TIMED_OUT && r.bytesAccepted == 2);
  CHECK(r.bytesCompleted == 0 && r.failedChunkLength == 2 && r.writeCommit == e::WriteCommit::ACCEPTED);
  CHECK(r.writeStatus.ok() && !r.verificationAttempted && d.totalFailures() == 0);
  CHECK(d.pollTransfer(42, 1000, 50).is(e::Err::NO_RESULT));
  CHECK(d.settingsSnapshot().writeCyclePending && b.calls == calls);
  CHECK(d.cancelTransfer(42).is(e::Err::NO_RESULT) && d.timeoutTransfer(42).is(e::Err::NO_RESULT));
  CHECK(d.takeTransferResult(42, r).is(e::Err::NO_RESULT) && r.requestId == 42);
  d.tick(55); CHECK(d.settingsSnapshot().writeCyclePending); d.tick(56);
  CHECK(!d.settingsSnapshot().writeCyclePending && b.calls == calls);
  CHECK(d.requestRead(43, 0, bytes, 2).ok());
  CHECK(d.cancelTransfer(42).detail == mismatch && d.isTransferBusy());
  CHECK(d.cancelTransfer(43).ok()); CHECK(d.takeTransferResult(43, r).ok());
  CHECK(r.state == e::TransferState::CANCELLED && b.calls == calls);
}
static void requestIdsAndAdmissionValidation() {
  Bus b; e::EEPROM24Cxx d; auto c = b.config(); CHECK(d.bind(c).ok());
  uint8_t byte = 0; const auto automatic = e::AUTOMATIC_REQUEST_ID_FIRST;
  CHECK(d.requestRead(0, 0, &byte, 1).is(e::Err::INVALID_PARAM));
  CHECK(d.requestWrite(automatic, 0, &byte, 1).is(e::Err::INVALID_PARAM));
  CHECK(d.requestFill(UINT32_MAX, 0, 0, 1).is(e::Err::INVALID_PARAM));
  CHECK(d.requestVerify(0, 0, &byte, 1).is(e::Err::INVALID_PARAM));
  CHECK(d.requestVerifiedWrite(automatic, 0, &byte, 1).is(e::Err::INVALID_PARAM));
  CHECK(d.requestVerifiedFill(0, 0, 0, 1).is(e::Err::INVALID_PARAM));
  CHECK(d.requestWrite(7, 0, nullptr, 1).is(e::Err::INVALID_PARAM));
  CHECK(d.requestRead(7, 256, &byte, 1).is(e::Err::ADDRESS_OUT_OF_RANGE));
  CHECK(!d.isTransferBusy() && b.calls == 0);
  CHECK(d.requestVerifiedWrite(7, 256, nullptr, 0).ok());
  auto r = d.transferSnapshot(); CHECK(r.requestId == 7 && r.state == e::TransferState::SUCCEEDED && r.match);
  CHECK(d.cancelTransfer(7).is(e::Err::BUSY) && d.timeoutTransfer(7).is(e::Err::BUSY));
  CHECK(d.takeTransferResult(7, r).ok());
  CHECK(d.requestRead(0, &byte, 1).ok()); CHECK(d.transferSnapshot().requestId == automatic);
  d.end(); CHECK(d.takeTransferResult(automatic, r).ok() && r.state == e::TransferState::CANCELLED);
  CHECK(d.bind(c).ok()); CHECK(d.startRead(0, nullptr, 1).is(e::Err::INVALID_PARAM));
  CHECK(d.startRead(0, &byte, 1).ok()); CHECK(d.transferSnapshot().requestId == automatic + 1U);
  CHECK(d.cancelTransfer().ok()); CHECK(d.takeResult(r).ok());
  CHECK(d.bind(c).ok()); CHECK(d.requestFill(0, 0x77, 1).ok());
  CHECK(d.transferSnapshot().requestId == automatic + 2U && b.calls == 0);
  CHECK(d.cancel().ok()); CHECK(d.takeResult(r).ok());
  CHECK(d.requestRead(automatic - 1U, 0, &byte, 1, 2).ok());
  CHECK(d.pollTransfer(automatic - 2U, 1000, 0).is(e::Err::BUSY));
  CHECK(d.pollTransfer(automatic - 1U, 0, 0).inProgress());
  b.ms = 1; CHECK(d.pollTransfer(automatic - 1U, 1, 1).ok());
  CHECK(d.takeTransferResult(automatic - 1U, r).ok() && r.bytesCompleted == 1 && b.calls == 1);
}
static void siblingRequestOperations() {
  Bus b; e::EEPROM24Cxx d; CHECK(d.bind(b.config()).ok()); uint8_t data[20], out[20];
  std::fill(data, data + sizeof(data), 0x19);
  CHECK(d.requestVerifiedWrite(1, 3, data, sizeof(data)).ok()); auto r = finish(d, b, 4);
  CHECK(r.requestId == 1 && r.status.ok() && r.writeCommit == e::WriteCommit::VERIFIED);
  CHECK(d.requestVerifiedFill(2, 3, 0xA9, sizeof(data)).ok()); r = finish(d, b, 4);
  CHECK(r.requestId == 2 && r.status.ok() && r.match && r.bytesVerified == sizeof(data));
  std::fill(data, data + sizeof(data), 0xA9);
  CHECK(d.requestVerify(3, 3, data, sizeof(data)).ok()); r = finish(d, b);
  CHECK(r.requestId == 3 && r.status.ok() && r.match && r.verificationAttempted);
  CHECK(d.requestWrite(4, 3, data, sizeof(data)).ok()); r = finish(d, b, 4);
  CHECK(r.requestId == 4 && r.status.ok() && !r.verificationAttempted);
  CHECK(d.requestFill(5, 3, 0xB7, sizeof(data)).ok()); r = finish(d, b, 4);
  CHECK(r.requestId == 5 && r.status.ok() && r.bytesCompleted == sizeof(data));
  CHECK(d.requestRead(3, out, sizeof(out)).ok()); r = finish(d, b, 4);
  CHECK(r.status.ok() && r.requestId >= e::AUTOMATIC_REQUEST_ID_FIRST && out[0] == 0xB7 && out[19] == 0xB7);
  CHECK(d.requestWrite(3, data, sizeof(data)).ok()); r = finish(d, b, 4); CHECK(r.status.ok());
  CHECK(d.requestVerify(3, data, sizeof(data)).ok()); r = finish(d, b, 4); CHECK(r.status.ok() && r.match);
}
int main() {
  struct Test { const char* name; void (*run)(); };
  const Test tests[] = {{"lifecycle and validation", lifecycleAndValidation}, {"geometry validation", geometryValidation},
    {"pages and all presets", pagesAndAllPresets}, {"bank boundaries", bankBoundaries},
    {"timing and ACK polling", timingAndAckPolling}, {"passive health and read failure", healthAndReadFailures},
    {"WP readback and fill", writeProtectionAndFill}, {"write evidence and no replay", writeEvidenceAndNoReplay},
    {"cancellation deadlines rollover", cancellationDeadlineAndRollover}, {"ACK faults and buffer limits", ackFaultsAndBufferLimits},
    {"post-callback deadlines and short reads", postCallbackDeadlinesAndShortRead},
    {"verification evidence and lifecycle", verificationEvidenceAndLifecycle},
    {"bank ACK and ended barrier", bankAckAndEndedBarrier},
    {"cached diagnostics and error vocabulary", cachedDiagnosticsAndErrorVocabulary},
    {"request identity and stale actions", requestIdentityAndStaleActions},
    {"request IDs and admission validation", requestIdsAndAdmissionValidation},
    {"sibling request operations", siblingRequestOperations}};
  for (const auto& test : tests) { const int before = failures; test.run(); if (before == failures) std::printf("[PASS] %s\n", test.name); }
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
