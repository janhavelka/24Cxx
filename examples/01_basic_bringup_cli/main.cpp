// One application task owns Wire, CLI, driver, bus clock and recovery policy.
#include <Arduino.h>
#include <Wire.h>
#include <cstdarg>
#include <cstdio>
#if defined(ESP32)
#include <esp_heap_caps.h>
#include <esp_arduino_version.h>
#endif
#if !defined(ESP_ARDUINO_VERSION) || ESP_ARDUINO_VERSION < ESP_ARDUINO_VERSION_VAL(3, 3, 11)
#error "This example requires Arduino-ESP32 3.3.11 or later for safe Wire buffer cleanup"
#endif
#include "../common/BoardConfig.h"
#include "../common/Eeprom24CxxCli.h"
#include "../common/WireTransportHelpers.h"
#include "../common/Esp32WriteProtect.h"
#include "../common/Esp32BusRecovery.h"

namespace {
using namespace EEPROM24Cxx;
eeprom24cxx_cli::Cli cli;
bool cliReady = false;
eeprom24cxx_cli::TransferStats transfers;
void recordTransfer(bool ok, eeprom24cxx_cli::BusTransferKind kind) {
  transfers.record(ok, kind);
}
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
  return eeprom24cxx_cli::probeStatus(probeI2c(address, board::I2C_TIMEOUT_MS, nullptr), "Wire address probe");
}
uint32_t nowMs(void*) { return millis(); }
void output(void*, const char* format, va_list args) {
  char buffer[512];
  const int length = vsnprintf(buffer, sizeof(buffer), format, args);
  if (length > 0) Serial.write(reinterpret_cast<const uint8_t*>(buffer),
      static_cast<size_t>(length) < sizeof(buffer) ? static_cast<size_t>(length) : sizeof(buffer) - 1);
}
eeprom24cxx_cli::TransferStats stats(void*) { return transfers; }
void resetTransferStats(void*) { transfers = {}; }
Status initializeInterface() {
  transport.setReady(false);
  if (Wire.setBufferSize(transport.MAX_BYTES) < transport.MAX_BYTES ||
      !Wire.begin(board::I2C_SDA, board::I2C_SCL, board::I2C_FREQUENCY_HZ))
    return Status::Error(Err::I2C_ERROR, "Wire initialization failed");
  Wire.setTimeOut(static_cast<uint16_t>(board::I2C_TIMEOUT_MS));
  transport.setReady(true);
  return Status::Ok();
}
Status resetInterface(void*) {
  // The CLI admits this only when the previous operation and tWR have settled.
  // Reinitialization is owned by this application and never replays a transfer.
  transport.setReady(false);
  if (!Wire.end()) return Status::Error(Err::I2C_ERROR, "Wire teardown failed");
  const Status recovered = eeprom24cxx_cli::recoverEsp32Bus(board::I2C_TIMEOUT_MS * 1000U);
  return recovered.ok() ? initializeInterface() : recovered;
}
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
  const auto protectedState = eeprom24cxx_cli::initializeWriteProtect();
  if (!protectedState.ok()) { Serial.println(protectedState.msg); return; }
  const Status initialized = initializeInterface();
  if (!initialized.ok()) { Serial.println(initialized.msg); return; }
  EEPROM24Cxx::Config config{};
  config.i2cWrite = writeI2c;
  config.i2cWriteRead = readI2c;
  config.i2cProbe = probeI2c;
  config.supportsCurrentAddressRead = true;
  config.nowMs = nowMs;
  config.i2cTimeoutMs = board::I2C_TIMEOUT_MS;
  config.maxTxBytes = transport.MAX_BYTES;
  config.maxRxBytes = transport.MAX_BYTES;
  eeprom24cxx_cli::Platform platform{};
  platform.vprintf = output;
  platform.nowMs = nowMs;
  platform.probeAddress = probeAddress;
  platform.transferStats = stats;
  platform.resetTransferStats = resetTransferStats;
  platform.readWriteProtect = eeprom24cxx_cli::readWriteProtect;
  platform.setWriteProtect = eeprom24cxx_cli::setWriteProtect;
  platform.resetInterface = resetInterface;
  platform.heapStats = heapStats;
  platform.sdaPin = board::I2C_SDA;
  platform.sclPin = board::I2C_SCL;
  platform.writeProtectPin = board::WP_PIN;
  platform.frequencyHz = board::I2C_FREQUENCY_HZ;
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
