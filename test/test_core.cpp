#include "EEPROM24Cxx/EEPROM24Cxx.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
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
int main() {
  struct Test { const char* name; void (*run)(); };
  const Test tests[] = {{"lifecycle and validation", lifecycleAndValidation}, {"geometry validation", geometryValidation},
    {"pages and all presets", pagesAndAllPresets}, {"bank boundaries", bankBoundaries},
    {"timing and ACK polling", timingAndAckPolling}, {"passive health and read failure", healthAndReadFailures},
    {"WP readback and fill", writeProtectionAndFill}, {"write evidence and no replay", writeEvidenceAndNoReplay},
    {"cancellation deadlines rollover", cancellationDeadlineAndRollover}, {"ACK faults and buffer limits", ackFaultsAndBufferLimits},
    {"post-callback deadlines and short reads", postCallbackDeadlinesAndShortRead}};
  for (const auto& test : tests) { const int before = failures; test.run(); if (before == failures) std::printf("[PASS] %s\n", test.name); }
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
