// One application task owns Wire, CLI, driver, bus clock and recovery policy.
#include <Arduino.h>
#include <Wire.h>
#include <cstdarg>
#include <atomic>
#if defined(ESP32)
#include <esp_heap_caps.h>
#include <esp_arduino_version.h>
#endif
#if !defined(ESP_ARDUINO_VERSION) || ESP_ARDUINO_VERSION < ESP_ARDUINO_VERSION_VAL(3, 3, 11)
#error "This example requires Arduino-ESP32 3.3.11 or later for safe Wire buffer cleanup"
#endif
#if ARDUINO_USB_CDC_ON_BOOT
#error "The CLI initializes its own console; disable ARDUINO_USB_CDC_ON_BOOT"
#endif
#include "../common/BoardConfig.h"
#include "../common/Eeprom24CxxCli.h"
#include "../common/ConsoleOutput.h"
#include "../common/WireTransportHelpers.h"
#include "../common/Esp32WriteProtect.h"
#include "../common/Esp32BusRecovery.h"
#if CONFIG_IDF_TARGET_ESP32S3
#include <driver/usb_serial_jtag.h>
#include <hal/usb_serial_jtag_ll.h>
#elif CONFIG_IDF_TARGET_ESP32S2
#include <USB.h>
#include <tusb.h>
#else
#error "This example console supports ESP32-S2 and ESP32-S3"
#endif

namespace {
using namespace EEPROM24Cxx;
eeprom24cxx_cli::Cli cli;
eeprom24cxx_cli::ConsoleOutput console;
bool cliReady = false;
#if CONFIG_IDF_TARGET_ESP32S2
USBCDC consoleSerial;
std::atomic<bool> rxOverflow{false};
unsigned queuedRxDiscard = 0;
void consoleEvent(void*, esp_event_base_t, int32_t event, void*) {
  if (event == ARDUINO_USB_CDC_RX_OVERFLOW_EVENT) rxOverflow.store(true);
}
#endif
bool initializeConsole() {
  console.stats().baud = board::SERIAL_BAUD;
#if CONFIG_IDF_TARGET_ESP32S3
  console.stats().backend = "usb-serial-jtag";
  usb_serial_jtag_driver_config_t config{};
  config.tx_buffer_size = 1024;
  config.rx_buffer_size = 1024;
  // CPU-only resets can preserve interrupts enabled by an earlier USB owner.
  // Preserve pending FIFO/status bits while handing interrupts to the driver.
  usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
  const bool ready = usb_serial_jtag_driver_install(&config) == ESP_OK;
#else
  console.stats().backend = "tinyusb-cdc";
  console.stats().rxOverflowObservable = true;
  consoleSerial.enableReboot(false);
  consoleSerial.setTxTimeoutMs(0);
  consoleSerial.onEvent(ARDUINO_USB_CDC_RX_OVERFLOW_EVENT, consoleEvent);
  if (consoleSerial.setRxBufferSize(1024) != 1024) return false;
  consoleSerial.begin(board::SERIAL_BAUD);
  const bool ready = USB.begin();
#endif
  console.ready(ready);
  return ready;
}
size_t writeConsole(const uint8_t* data, size_t length, void*) {
#if CONFIG_IDF_TARGET_ESP32S3
  const int written = usb_serial_jtag_write_bytes(data, length, 0);
  return written > 0 ? static_cast<size_t>(written) : 0;
#else
  return consoleSerial.write(data, length); // Configured for zero-wait partial writes.
#endif
}
int readConsole() {
#if CONFIG_IDF_TARGET_ESP32S3
  uint8_t value = 0;
  return usb_serial_jtag_read_bytes(&value, 1, 0) == 1 ? value : -1;
#else
  return consoleSerial.read();
#endif
}
eeprom24cxx_cli::ConsoleStats consoleStats(void*) {
#if CONFIG_IDF_TARGET_ESP32S3
  console.stats().connected = usb_serial_jtag_is_connected() ? 1 : 0;
#else
  // USBCDC::operator bool requires DTR and RTS. CDC writes require only DTR.
  console.stats().connected = tud_cdc_n_connected(0) ? 1 : 0;
#endif
  return console.stats();
}
bool outputReady(void*) { return console.outputReady(); }
void reportConsole(const char* format, ...) {
  va_list args; va_start(args, format); console.append(format, args); va_end(args);
}
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
  console.append(format, args);
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
  if (!initializeConsole()) return;
  const auto protectedState = eeprom24cxx_cli::initializeWriteProtect();
  if (!protectedState.ok()) { reportConsole("[E] %s\n", protectedState.msg); return; }
  const Status initialized = initializeInterface();
  if (!initialized.ok()) { reportConsole("[E] %s detail=%ld\n", initialized.msg, static_cast<long>(initialized.detail)); return; }
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
  platform.consoleStats = consoleStats;
  platform.outputReady = outputReady;
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
  console.drain(millis(), writeConsole);
  for (unsigned count = 0; cliReady && console.outputReady() && count < 64; ++count) {
#if CONFIG_IDF_TARGET_ESP32S2
    if (rxOverflow.exchange(false)) {
      console.countRxOverflow(); cli.discardInput();
      // The lost bytes were at the queue's tail, possibly after several intact
      // lines. Drop every already-queued byte before resynchronizing at an EOL.
      const int pending = consoleSerial.available();
      queuedRxDiscard = pending > 0 ? static_cast<unsigned>(pending) : 0;
    }
#endif
    const int value = readConsole();
    if (value < 0) break;
    console.countRx();
#if CONFIG_IDF_TARGET_ESP32S2
    if (queuedRxDiscard) { --queuedRxDiscard; continue; }
#endif
    cli.feed(static_cast<char>(value));
    if (value == '\r' || value == '\n') break; // At most one submitted command per loop.
  }
  cli.tick();
  delay(1);
}
