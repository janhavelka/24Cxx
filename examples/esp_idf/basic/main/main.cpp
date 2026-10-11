// Native ESP-IDF: one task owns the bus, driver and bounded console polling.
#include <cstdarg>
#include <climits>
#include <driver/i2c_master.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#if !defined(CONFIG_ESP_CONSOLE_NONE) || (defined(CONFIG_IDF_TARGET_ESP32S3) && !defined(CONFIG_ESP_CONSOLE_SECONDARY_NONE))
#error "Regenerate sdkconfig with both default console channels disabled; the CLI owns its console driver"
#endif
#if CONFIG_IDF_TARGET_ESP32S3
#include <driver/usb_serial_jtag.h>
#include <hal/usb_serial_jtag_ll.h>
#elif CONFIG_IDF_TARGET_ESP32S2
#include <driver/uart.h>
#else
#error "This example console supports ESP32-S2 and ESP32-S3"
#endif
#include "BoardConfig.h"
#include "Eeprom24CxxCli.h"
#include "ConsoleOutput.h"
#include "IdfTransportHelpers.h"
#include "Esp32WriteProtect.h"
#include "Esp32BusRecovery.h"

namespace {
using namespace EEPROM24Cxx;
#if CONFIG_IDF_TARGET_ESP32S2
static_assert(board::SERIAL_TX != board::I2C_SDA && board::SERIAL_TX != board::I2C_SCL &&
              board::SERIAL_RX != board::I2C_SDA && board::SERIAL_RX != board::I2C_SCL &&
              board::SERIAL_TX != board::WP_PIN && board::SERIAL_RX != board::WP_PIN,
              "Native ESP32-S2 UART pins must be separate from I2C and EEPROM WP");
#endif
struct App {
  i2c_master_bus_handle_t bus = nullptr;
  i2c_master_dev_handle_t devices[8]{};
  bool ready = false;
  eeprom24cxx_cli::TransferStats stats{};
  bool cliReady = false;
  eeprom24cxx_cli::ConsoleOutput console{};
#if CONFIG_IDF_TARGET_ESP32S2
  QueueHandle_t uartEvents = nullptr;
#endif
  eeprom24cxx_cli::Cli cli{};
} app;
bool initializeConsole() {
  app.console.stats().baud = board::SERIAL_BAUD;
#if CONFIG_IDF_TARGET_ESP32S3
  app.console.stats().backend = "usb-serial-jtag";
  usb_serial_jtag_driver_config_t config{};
  config.tx_buffer_size = 1024;
  config.rx_buffer_size = 1024;
  // A CPU-only reset can retain interrupt enables from another USB owner.
  usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
  const bool ready = usb_serial_jtag_driver_install(&config) == ESP_OK;
#else
  app.console.stats().backend = "uart0";
  app.console.stats().rxOverflowObservable = true;
  app.console.stats().connected = -1; // UART cannot observe a host connection.
  uart_config_t config{};
  config.baud_rate = static_cast<int>(board::SERIAL_BAUD);
  config.data_bits = UART_DATA_8_BITS;
  config.parity = UART_PARITY_DISABLE;
  config.stop_bits = UART_STOP_BITS_1;
  config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  config.source_clk = UART_SCLK_DEFAULT;
  const bool ready = uart_param_config(UART_NUM_0, &config) == ESP_OK &&
      uart_set_pin(UART_NUM_0, board::SERIAL_TX, board::SERIAL_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) == ESP_OK &&
      uart_driver_install(UART_NUM_0, 1024, 0, 16, &app.uartEvents, 0) == ESP_OK;
#endif
  app.console.ready(ready);
  return ready;
}
size_t writeConsole(const uint8_t* data, size_t length, void*) {
#if CONFIG_IDF_TARGET_ESP32S3
  const int written = usb_serial_jtag_write_bytes(data, length, 0);
#else
  // No TX ring or blocking write: accept only bytes fitting the hardware FIFO.
  const int written = uart_tx_chars(UART_NUM_0, reinterpret_cast<const char*>(data), static_cast<uint32_t>(length));
#endif
  return written > 0 ? static_cast<size_t>(written) : 0;
}
int readConsole() {
  uint8_t value = 0;
#if CONFIG_IDF_TARGET_ESP32S3
  const int received = usb_serial_jtag_read_bytes(&value, 1, 0);
#else
  const int received = uart_read_bytes(UART_NUM_0, &value, 1, 0);
#endif
  return received == 1 ? value : -1;
}
bool checkConsoleInput() {
#if CONFIG_IDF_TARGET_ESP32S2
  // The SDK can drop error notifications once its event queue fills. Treat
  // saturation itself as evidence of possible loss, including UART_DATA-only
  // queues whose later overflow/parity notifications could not be enqueued.
  bool lost = app.uartEvents && uxQueueSpacesAvailable(app.uartEvents) == 0;
  uart_event_t event{};
  for (unsigned count = 0; count < 16 && app.uartEvents && xQueueReceive(app.uartEvents, &event, 0) == pdTRUE; ++count) {
    if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
        event.type == UART_FRAME_ERR || event.type == UART_PARITY_ERR) lost = true;
  }
  if (lost) {
    app.console.countRxOverflow();
    app.cli.discardInput();
    (void)uart_flush_input(UART_NUM_0);
  }
  return lost;
#else
  return false;
#endif
}
eeprom24cxx_cli::ConsoleStats consoleStats(void*) {
#if CONFIG_IDF_TARGET_ESP32S3
  app.console.stats().connected = usb_serial_jtag_is_connected() ? 1 : 0;
#endif
  return app.console.stats();
}
bool outputReady(void*) { return app.console.outputReady(); }
void reportConsole(const char* format, ...) {
  va_list args; va_start(args, format); app.console.append(format, args); va_end(args);
}
constexpr eeprom24cxx_cli::IdfResultMapper resultMapper{
    ESP_OK, ESP_ERR_TIMEOUT, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_RESPONSE, ESP_ERR_NOT_FOUND};
TransportResult finish(esp_err_t error, size_t tx, size_t rx, bool memoryWrite) {
  app.stats.record(error == ESP_OK,
      memoryWrite ? eeprom24cxx_cli::BusTransferKind::WRITE : eeprom24cxx_cli::BusTransferKind::READ);
  return resultMapper.transaction(error, tx, rx, memoryWrite);
}
TransportResult writeI2c(uint8_t address, const uint8_t* data, size_t length,
                         uint32_t timeoutMs, void*) {
  if (!app.ready || address < 0x50 || address > 0x57 || !app.devices[address - 0x50] ||
      !data || !length || !timeoutMs || timeoutMs > INT_MAX)
    return TransportResult::Error(TransportCode::IO_ERROR, ESP_ERR_INVALID_ARG, WriteCommit::NOT_COMMITTED);
  return finish(i2c_master_transmit(app.devices[address - 0x50], data, length, static_cast<int>(timeoutMs)), length, 0, true);
}
TransportResult readI2c(uint8_t address, const uint8_t* tx, size_t txLength,
                        uint8_t* rx, size_t rxLength, uint32_t timeoutMs, void*) {
  if (!app.ready || address < 0x50 || address > 0x57 || !app.devices[address - 0x50] ||
      (txLength && !tx) || txLength > 2 || !rx || !rxLength || !timeoutMs || timeoutMs > INT_MAX)
    return TransportResult::Error(TransportCode::IO_ERROR, ESP_ERR_INVALID_ARG, WriteCommit::NOT_APPLICABLE);
  return finish(eeprom24cxx_cli::idfReadTransaction(app.devices[address - 0x50], tx, txLength, rx, rxLength,
      static_cast<int>(timeoutMs), i2c_master_receive, i2c_master_transmit_receive), txLength, rxLength, false);
}
TransportResult probeI2c(uint8_t address, uint32_t timeoutMs, void*) {
  if (!app.ready || !app.bus || address < 0x08 || address > 0x77 || !timeoutMs || timeoutMs > INT_MAX)
    return TransportResult::Error(TransportCode::IO_ERROR, ESP_ERR_INVALID_ARG, WriteCommit::NOT_APPLICABLE);
  const esp_err_t result = i2c_master_probe(app.bus, address, static_cast<int>(timeoutMs));
  app.stats.record(result == ESP_OK, eeprom24cxx_cli::BusTransferKind::PROBE);
  return resultMapper.probe(result);
}
Status probeAddress(uint8_t address, void*) {
  return eeprom24cxx_cli::probeStatus(probeI2c(address, board::I2C_TIMEOUT_MS, nullptr), "IDF address probe");
}
uint32_t nowMs(void*) { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
void output(void*, const char* format, va_list args) { app.console.append(format, args); }
eeprom24cxx_cli::TransferStats stats(void*) { return app.stats; }
void resetTransferStats(void*) { app.stats = {}; }
Status releaseInterface() {
  app.ready = false;
  for (auto& device : app.devices) {
    if (!device) continue;
    const esp_err_t error = i2c_master_bus_rm_device(device);
    if (error != ESP_OK) return Status::Error(Err::I2C_ERROR, "IDF device release failed", error);
    device = nullptr;
  }
  if (app.bus) {
    const esp_err_t error = i2c_del_master_bus(app.bus);
    if (error != ESP_OK) return Status::Error(Err::I2C_ERROR, "IDF bus release failed", error);
    app.bus = nullptr;
  }
  return Status::Ok();
}
Status initializeInterface() {
  app.ready = false;
  i2c_master_bus_config_t bus{};
  bus.i2c_port = I2C_NUM_0;
  bus.sda_io_num = static_cast<gpio_num_t>(board::I2C_SDA);
  bus.scl_io_num = static_cast<gpio_num_t>(board::I2C_SCL);
  bus.clk_source = I2C_CLK_SRC_DEFAULT;
  bus.glitch_ignore_cnt = 7;
  bus.flags.enable_internal_pullup = true;
  esp_err_t error = i2c_new_master_bus(&bus, &app.bus);
  if (error != ESP_OK) return Status::Error(Err::I2C_ERROR, "IDF bus creation failed", error);
  // Handles cover bank aliases; registration is SDK setup without memory I/O.
  for (unsigned index = 0; index < 8; ++index) {
    i2c_device_config_t device{};
    device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device.device_address = static_cast<uint16_t>(0x50U + index);
    device.scl_speed_hz = board::I2C_FREQUENCY_HZ;
    error = i2c_master_bus_add_device(app.bus, &device, &app.devices[index]);
    if (error != ESP_OK) {
      (void)releaseInterface(); // Retain any unreleased handle for explicit recovery.
      return Status::Error(Err::I2C_ERROR, "IDF device registration failed", error);
    }
  }
  app.ready = true;
  return Status::Ok();
}
Status resetInterface(void*) {
  // A controller FSM reset alone does not establish Zetta's documented START
  // sequence. Detach the controller, use the same electrical recovery as Wire,
  // then rebuild the application's handles. No pending I/O exists here.
  const Status released = releaseInterface();
  if (!released.ok()) return released;
  const Status recovered = eeprom24cxx_cli::recoverEsp32Bus(board::I2C_TIMEOUT_MS * 1000U);
  return recovered.ok() ? initializeInterface() : recovered;
}
eeprom24cxx_cli::HeapStats heapStats(void*) {
  return {static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
          static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)),
          static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT))};
}
}  // namespace

void startCli() {
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
  platform.framework = "native-esp-idf";
  platform.frameworkVersion = esp_get_idf_version();
  platform.target = CONFIG_IDF_TARGET;
  app.cli.setup(platform, config);
  app.cliReady = true;
}
extern "C" void app_main() {
  if (initializeConsole()) startCli();
  while (true) {
    app.console.drain(nowMs(nullptr), writeConsole);
    (void)checkConsoleInput();
    for (unsigned count = 0; app.cliReady && app.console.outputReady() && count < 64; ++count) {
      const int value = readConsole();
      if (value < 0) break;
      app.console.countRx();
      // An ISR can report loss after the initial queue check. Recheck before
      // submitting a command; discard this already-read EOL too, so it cannot
      // clear the discard state set for a damaged fragment still in flight.
      if ((value == '\r' || value == '\n') && checkConsoleInput()) break;
      app.cli.feed(static_cast<char>(value));
      if (value == '\r' || value == '\n') break;
    }
    app.cli.tick();
    vTaskDelay(1);
  }
}
