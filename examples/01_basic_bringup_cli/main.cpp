// One application task owns Wire, CLI, driver, bus clock and recovery policy.
#include <Arduino.h>
#include <Wire.h>
#include <cstdarg>
#include <cstdio>
#if defined(ESP32)
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_arduino_version.h>
#endif
#if !defined(ESP_ARDUINO_VERSION) || ESP_ARDUINO_VERSION < ESP_ARDUINO_VERSION_VAL(3, 3, 11)
#error "This example requires Arduino-ESP32 3.3.11 or later for safe Wire buffer cleanup"
#endif
#include "../common/BoardConfig.h"
#include "../common/Eeprom24CxxCli.h"
#include "../common/WireTransportHelpers.h"

namespace {
using namespace EEPROM24Cxx;
eeprom24cxx_cli::Cli cli;
bool cliReady = false;
eeprom24cxx_cli::TransferStats transfers;
void recordTransfer(bool ok) { transfers.record(ok); }
eeprom24cxx_cli::WireTransport<TwoWire> transport(Wire, recordTransfer);
TransportResult writeI2c(uint8_t address, const uint8_t* data, size_t length,
                         uint32_t timeoutMs, void*) {
  return transport.write(address, data, length, timeoutMs);
}
TransportResult readI2c(uint8_t address, const uint8_t* tx, size_t txLength,
                        uint8_t* rx, size_t rxLength, uint32_t timeoutMs, void*) {
  return transport.read(address, tx, txLength, rx, rxLength, timeoutMs);
}
TransportResult probeI2c(uint8_t address, uint32_t timeoutMs, void*) {
  return transport.probe(address, timeoutMs);
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
eeprom24cxx_cli::HeapStats heapStats(void*) {
  return {static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
          static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)),
          static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT))};
}
}  // namespace

void setup() {
  Serial.begin(board::SERIAL_BAUD);
  const uint32_t started = millis();
  while (!Serial && millis() - started < 3000U) delay(10);
  if (Wire.setBufferSize(transport.MAX_BYTES) < transport.MAX_BYTES ||
      !Wire.begin(board::I2C_SDA, board::I2C_SCL, board::I2C_FREQUENCY_HZ)) {
    Serial.println("[E] Application failed to initialize I2C"); return;
  }
  Wire.setTimeOut(static_cast<uint16_t>(board::I2C_TIMEOUT_MS));
  transport.setReady(true);
  EEPROM24Cxx::Config config{};
  config.i2cWrite = writeI2c;
  config.i2cWriteRead = readI2c;
  config.i2cProbe = probeI2c;
  config.nowMs = nowMs;
  config.i2cTimeoutMs = board::I2C_TIMEOUT_MS;
  config.maxTxBytes = transport.MAX_BYTES;
  config.maxRxBytes = transport.MAX_BYTES;
  eeprom24cxx_cli::Platform platform{};
  platform.vprintf = output;
  platform.nowMs = nowMs;
  platform.probeAddress = probeAddress;
  platform.transferStats = stats;
  platform.heapStats = heapStats;
  platform.framework = "Arduino-ESP32";
#ifdef ESP_ARDUINO_VERSION_STR
  platform.frameworkVersion = ESP_ARDUINO_VERSION_STR;
#endif
#ifdef CONFIG_IDF_TARGET
  platform.target = CONFIG_IDF_TARGET;
#endif
  cli.setup(platform, config);
  cliReady = true;
}
void loop() {
  if (!cliReady) { delay(10); return; }
  for (unsigned count = 0; count < 64 && Serial.available(); ++count)
    cli.feed(static_cast<char>(Serial.read()));
  cli.tick();
  delay(1);
}
