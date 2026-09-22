// One application task owns Wire, CLI, driver, bus clock and recovery policy.
#include <Arduino.h>
#include <Wire.h>
#include <cstdarg>
#include <cstdio>
#if defined(ESP32)
#include <esp_system.h>
#include <esp_arduino_version.h>
#endif
#include "../common/BoardConfig.h"
#include "../common/Eeprom24CxxCli.h"
#include "../common/WireTransportHelpers.h"

namespace {
using namespace EEPROM24Cxx;
eeprom24cxx_cli::Cli cli;
eeprom24cxx_cli::TransferStats transfers;
TransportResult finish(TransportResult result) { transfers.record(result.ok()); return result; }
void discardBufferedWrite(uint8_t address, size_t buffered) {
  const auto discarded = eeprom24cxx_cli::discardWireTx(Wire, address, buffered);
  if (discarded.physicalAttempt)
    (void)finish(eeprom24cxx_cli::wireResult(discarded.code, 0, 0, true));
}
TransportResult writeI2c(uint8_t address, const uint8_t* data, size_t length,
                         uint32_t timeoutMs, void*) {
  if (!data || !length || length > 32 || !timeoutMs || timeoutMs > UINT16_MAX)
    return TransportResult::Error(TransportCode::IO_ERROR, -1, WriteCommit::NOT_COMMITTED);
  Wire.setTimeOut(static_cast<uint16_t>(timeoutMs));
  Wire.beginTransmission(address);
  const size_t buffered = Wire.write(data, length);
  if (buffered != length) {
    discardBufferedWrite(address, buffered);
    return TransportResult::Error(TransportCode::IO_ERROR, -2, WriteCommit::NOT_COMMITTED);
  }
  return finish(eeprom24cxx_cli::wireResult(Wire.endTransmission(true), length));
}
TransportResult readI2c(uint8_t address, const uint8_t* tx, size_t txLength,
                        uint8_t* rx, size_t rxLength, uint32_t timeoutMs, void*) {
  if (!tx || !txLength || txLength > 2 || !rx || !rxLength || rxLength > 32 || !timeoutMs || timeoutMs > UINT16_MAX)
    return TransportResult::Error(TransportCode::IO_ERROR, -1, WriteCommit::NOT_APPLICABLE);
  Wire.setTimeOut(static_cast<uint16_t>(timeoutMs));
  Wire.beginTransmission(address);
  const size_t buffered = Wire.write(tx, txLength);
  if (buffered != txLength) {
    discardBufferedWrite(address, buffered);
    return TransportResult::Error(TransportCode::IO_ERROR, -2, WriteCommit::NOT_APPLICABLE);
  }
  // ESP32 Wire defers endTransmission(false) until requestFrom: one combined
  // repeated-START transaction with no STOP between address and payload.
  const auto result = eeprom24cxx_cli::wireResult(Wire.endTransmission(false));
  if (!result.ok()) return finish(result);
  const size_t received = Wire.requestFrom(address, rxLength, true);
  if (received != rxLength) {
    while (Wire.available()) (void)Wire.read();
    // A short read does not prove the address/data NACK phase or full TX count.
    return finish(TransportResult::Error(TransportCode::IO_ERROR, static_cast<int32_t>(received),
                                         WriteCommit::NOT_APPLICABLE, 0, received));
  }
  for (size_t index = 0; index < rxLength; ++index) rx[index] = static_cast<uint8_t>(Wire.read());
  return finish(TransportResult::Ok(txLength, rxLength));
}
TransportResult probeI2c(uint8_t address, uint32_t timeoutMs, void*) {
  Wire.setTimeOut(static_cast<uint16_t>(timeoutMs));
  Wire.beginTransmission(address);
  return finish(eeprom24cxx_cli::wireResult(Wire.endTransmission(true), 0, 0, true));
}
Status probeAddress(uint8_t address, void*) {
  const auto result = probeI2c(address, board::I2C_TIMEOUT_MS, nullptr);
  if (result.ok()) return Status::Ok();
  const Err code = result.code == TransportCode::NACK_ADDRESS ? Err::I2C_NACK_ADDR :
      result.code == TransportCode::TIMEOUT ? Err::I2C_TIMEOUT : Err::I2C_ERROR;
  return Status::Error(code, "Wire address probe", result.detail);
}
uint32_t nowMs(void*) { return millis(); }
void output(void*, const char* format, va_list args) {
  char buffer[512];
  const int length = vsnprintf(buffer, sizeof(buffer), format, args);
  if (length > 0) Serial.write(reinterpret_cast<const uint8_t*>(buffer),
      static_cast<size_t>(length) < sizeof(buffer) ? static_cast<size_t>(length) : sizeof(buffer) - 1);
}
eeprom24cxx_cli::TransferStats stats(void*) { return transfers; }
}  // namespace

void setup() {
  Serial.begin(board::SERIAL_BAUD);
  const uint32_t started = millis();
  while (!Serial && millis() - started < 3000U) delay(10);
  if (!Wire.begin(board::I2C_SDA, board::I2C_SCL, board::I2C_FREQUENCY_HZ)) {
    Serial.println("[E] Application failed to initialize I2C"); return;
  }
  EEPROM24Cxx::Config config{};
  config.i2cWrite = writeI2c;
  config.i2cWriteRead = readI2c;
  config.i2cProbe = probeI2c;
  config.nowMs = nowMs;
  config.i2cTimeoutMs = board::I2C_TIMEOUT_MS;
  config.maxTxBytes = 32;
  config.maxRxBytes = 32;
  eeprom24cxx_cli::Platform platform{};
  platform.vprintf = output;
  platform.nowMs = nowMs;
  platform.probeAddress = probeAddress;
  platform.transferStats = stats;
  platform.framework = "Arduino-ESP32";
#ifdef ESP_ARDUINO_VERSION_STR
  platform.frameworkVersion = ESP_ARDUINO_VERSION_STR;
#endif
#ifdef CONFIG_IDF_TARGET
  platform.target = CONFIG_IDF_TARGET;
#endif
  cli.setup(platform, config);
}
void loop() {
  for (unsigned count = 0; count < 64 && Serial.available(); ++count)
    cli.feed(static_cast<char>(Serial.read()));
  cli.tick();
  delay(1);
}
