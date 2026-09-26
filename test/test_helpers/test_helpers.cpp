#include "EEPROM24Cxx/BlockingMemory.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <limits>

namespace e = EEPROM24Cxx;
namespace m = EEPROM24Cxx::memory;
#define CHECK(x) do { if (!(x)) { std::printf("[FAIL] field helpers line %d: %s\n", __LINE__, #x); return 1; } } while (false)

struct Model {
  uint8_t data[256]{};
  uint8_t pointer = 0;
  uint32_t ms = 0, readyAt = 0;
  unsigned reads = 0, writes = 0, waits = 0, calls = 0, failAt = 0;
  uint32_t callbackTime = 0;
  bool wp = false, stalled = false, advanceClockSample = false;
  static uint32_t clock(void* user) {
    auto& model = *static_cast<Model*>(user);
    if (model.advanceClockSample) ++model.ms;
    return model.ms;
  }
  static void wait(uint32_t milliseconds, void* user) {
    auto& model = *static_cast<Model*>(user);
    ++model.waits;
    if (!model.stalled) model.ms += milliseconds;
  }
  static e::TransportResult read(uint8_t, const uint8_t* tx, size_t txLength,
      uint8_t* rx, size_t length, uint32_t, void* user) {
    auto& model = *static_cast<Model*>(user);
    ++model.reads; ++model.calls; model.ms += model.callbackTime;
    if (model.ms < model.readyAt) return e::TransportResult::Error(e::TransportCode::NACK_ADDRESS);
    if (model.failAt == model.calls) {
      std::memset(rx, 0xDE, length);
      return e::TransportResult::Error(e::TransportCode::TIMEOUT);
    }
    if (txLength) model.pointer = tx[0];
    for (size_t i = 0; i < length; ++i) rx[i] = model.data[model.pointer++];
    return e::TransportResult::Ok(txLength, length);
  }
  static e::TransportResult write(uint8_t, const uint8_t* tx, size_t length, uint32_t, void* user) {
    auto& model = *static_cast<Model*>(user);
    ++model.writes; ++model.calls; model.ms += model.callbackTime;
    if (model.ms < model.readyAt) return e::TransportResult::Error(e::TransportCode::NACK_ADDRESS, 0, e::WriteCommit::NOT_COMMITTED);
    if (model.failAt == model.calls) return e::TransportResult::Error(e::TransportCode::TIMEOUT);
    const uint8_t page = static_cast<uint8_t>(tx[0] & 0xF8U);
    for (size_t i = 1; i < length; ++i) {
      const uint8_t address = static_cast<uint8_t>(page | ((tx[0] + i - 1U) & 7U));
      if (!model.wp) model.data[address] = tx[i];
    }
    model.pointer = static_cast<uint8_t>(page | ((tx[0] + length - 1U) & 7U));
    model.readyAt = model.ms + 5;
    return e::TransportResult::Ok(length, 0);
  }
  e::Config config() {
    e::Config c;
    c.i2cWrite = write; c.i2cWriteRead = read; c.i2cUser = this;
    c.nowMs = clock; c.timeUser = this; c.maxRxBytes = 3; c.maxTxBytes = 6;
    c.supportsCurrentAddressRead = true;
    return c;
  }
};

int main() {
  uint8_t bytes[8]{};
  m::encodeUint32Le(0x78563412U, bytes);
  CHECK(bytes[0] == 0x12 && bytes[1] == 0x34 && bytes[2] == 0x56 && bytes[3] == 0x78);
  CHECK(m::decodeUint32Le(bytes) == 0x78563412U && m::decodeUint32Be(bytes) == 0x12345678U);
  m::encodeUint64Be(UINT64_C(0x0123456789ABCDEF), bytes);
  CHECK(bytes[0] == 1 && bytes[7] == 0xEF && m::decodeUint64Be(bytes) == UINT64_C(0x0123456789ABCDEF));
  m::encodeInt32Le(INT32_MIN, bytes); CHECK(m::decodeInt32Le(bytes) == INT32_MIN);
  m::encodeInt64Le(INT64_MIN, bytes); CHECK(m::decodeInt64Le(bytes) == INT64_MIN);
  m::encodeFloatLe(1.0F, bytes); CHECK(m::decodeUint32Le(bytes) == 0x3F800000U);
  m::encodeDoubleLe(-0.0, bytes); CHECK(m::decodeUint64Le(bytes) == UINT64_C(0x8000000000000000));
  CHECK(std::signbit(m::decodeDoubleLe(bytes)));
  const uint8_t vector[]{'1','2','3','4','5','6','7','8','9'};
  CHECK(m::crc32(vector, sizeof(vector)) == 0xCBF43926U);
  CHECK((m::crc32Update(m::crc32Update(0xFFFFFFFFU, vector, 4), vector + 4, 5) ^ 0xFFFFFFFFU) == 0xCBF43926U);
  CHECK(m::crc32(nullptr, 0) == 0);
  CHECK(m::fitsRange(256, 256, 0) && !m::fitsRange(256, 255, 2));
  CHECK(!m::fitsRange(256, UINT32_MAX, 1) && !m::fitsRange(256, 1, std::numeric_limits<size_t>::max()));
  const auto geometry = e::geometryFor(e::DeviceVariant::ZETTA_ZD24C02B);
  CHECK(m::pageStart(geometry, 255) == 248 && m::pageRemaining(geometry, 255) == 1);
  CHECK(m::bankRemaining(geometry, 255) == 1 && m::bankRemaining(geometry, 256) == 0);

  Model model; e::EEPROM24Cxx device; e::BlockingMemory field(device, Model::wait, &model);
  CHECK(field.bind(model.config()).ok() && model.calls == 0);
  e::BlockingMemory missingWait(device, nullptr);
  CHECK(missingWait.writeByte(0, 1).is(e::Err::INVALID_CONFIG) && model.calls == 0);
  CHECK(field.writeByte(0, 1, 0).is(e::Err::INVALID_PARAM) && model.calls == 0);
  CHECK(field.writeUint32Le(6, 0x78563412U).ok());
  CHECK(model.writes == 2 && model.ms >= model.readyAt);
  CHECK(model.data[6] == 0x12 && model.data[9] == 0x78);
  CHECK(field.lastResult().bytesCompleted == 4 && !device.isTransferBusy());
  uint32_t u32 = 0;
  CHECK(field.readUint32Le(6, u32).ok() && u32 == 0x78563412U);
  const auto calls = model.calls;
  CHECK(field.writeOnce(7, bytes, 2).is(e::Err::INVALID_PARAM));
  CHECK(field.readOnce(0, bytes, 4).is(e::Err::INVALID_PARAM));
  CHECK(field.readOnce(0, bytes, 0).is(e::Err::INVALID_PARAM));
  CHECK(field.writeOnce(0, bytes, 0).is(e::Err::INVALID_PARAM));
  CHECK(field.writeUint32Le(254, 1).is(e::Err::ADDRESS_OUT_OF_RANGE));
  CHECK(model.calls == calls);
  model.failAt = model.calls + 2;
  u32 = 0xCAFEBABEU;
  CHECK(field.readUint32Le(6, u32).is(e::Err::I2C_TIMEOUT) && u32 == 0xCAFEBABEU);
  CHECK(field.lastResult().bytesCompleted == 3);
  model.failAt = 0;
  CHECK(field.writeFloat32Le(15, 1.25F).ok()); float f32 = 0;
  CHECK(field.readFloat32Le(15, f32).ok() && f32 == 1.25F);
  CHECK(field.writeFloat64Le(23, -8.125).ok()); double f64 = 0;
  CHECK(field.readFloat64Le(23, f64).ok() && f64 == -8.125);
  CHECK(field.writeBool(40, true).ok()); bool flag = false;
  CHECK(field.readBool(40, flag).ok() && flag);
  std::memcpy(model.data + 64, vector, sizeof(vector));
  uint32_t crc = 0;
  CHECK(field.crc32(64, sizeof(vector), crc).ok() && crc == 0xCBF43926U);
  model.failAt = model.calls + 2; crc = 0xCAFEU;
  CHECK(field.crc32(0, sizeof(model.data), crc).is(e::Err::I2C_TIMEOUT) && crc == 0xCAFEU);
  model.failAt = 0;
  CHECK(field.readByte(63, bytes[0]).ok());
  CHECK(field.readCurrentAddress(bytes[0]).ok() && bytes[0] == '1');
  model.advanceClockSample = true; crc = 0xCAFEU;
  CHECK(field.crc32(64, 1, crc, 4).is(e::Err::TIMEOUT) && crc == 0xCAFEU);
  model.advanceClockSample = false;

  const unsigned writesBeforeUpdate = model.writes;
  CHECK(field.update(64, vector, sizeof(vector)).ok());
  CHECK(model.writes == writesBeforeUpdate && field.lastResult().bytesSkipped == sizeof(vector));
  CHECK(field.updateByte(64, 0xEE).ok() && model.writes == writesBeforeUpdate + 1 && model.data[64] == 0xEE);
  model.wp = true;
  CHECK(field.writeVerify(64, vector, sizeof(vector)).is(e::Err::VERIFY_MISMATCH));
  CHECK(field.lastResult().writeStatus.ok() && field.lastResult().verifyStatus.is(e::Err::VERIFY_MISMATCH));
  model.wp = false;

  // Owner work cannot be consumed by a convenience call that fails admission.
  CHECK(device.requestRead(77, 0, bytes, 1).ok());
  CHECK(field.readByte(0, bytes[1]).is(e::Err::BUSY));
  CHECK(device.transferSnapshot().requestId == 77);
  CHECK(device.cancelTransfer(77).ok()); e::TransferResult result;
  CHECK(device.takeTransferResult(77, result).ok());

  // Local typed buffers are released even when a write times out mid-cycle.
  CHECK(field.writeUint32Le(80, 0x12345678U, 1).is(e::Err::TIMEOUT));
  CHECK(!device.isTransferBusy() && !device.settingsSnapshot().resultPending && device.settingsSnapshot().writeCyclePending);
  const unsigned afterTimeout = model.writes;
  CHECK(field.waitUntilReady().ok() && model.writes == afterTimeout);
  CHECK(field.lastResult().status.is(e::Err::TIMEOUT));
  model.failAt = model.calls + 1;
  CHECK(field.writeByte(90, 0x11).is(e::Err::I2C_TIMEOUT));
  CHECK(field.lastResult().writeCommit == e::WriteCommit::INDETERMINATE);
  CHECK(field.waitUntilReady().ok() && model.writes == afterTimeout + 1);
  model.failAt = 0;
  model.stalled = true;
  CHECK(field.writeByte(100, 0x55).is(e::Err::TIMEOUT));
  CHECK(model.writes == afterTimeout + 2 && device.settingsSnapshot().writeCyclePending);
  CHECK(field.waitUntilReady().is(e::Err::TIMEOUT));
  model.stalled = false;
  CHECK(field.waitUntilReady().ok());
  std::puts("[PASS] field codecs, typed access, CRC, update, deadlines and retained write evidence");
  return 0;
}
