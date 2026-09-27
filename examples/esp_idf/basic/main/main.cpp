// Native ESP-IDF: one owner task calls the driver; input task queues bytes only.
#include <cstdarg>
#include <cstdio>
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
#include "BoardConfig.h"
#include "Eeprom24CxxCli.h"
#include "IdfTransportHelpers.h"
#include "Esp32WriteProtect.h"
#include "Esp32BusRecovery.h"

namespace {
using namespace EEPROM24Cxx;
struct App {
  i2c_master_bus_handle_t bus = nullptr;
  i2c_master_dev_handle_t devices[8]{};
  bool ready = false;
  eeprom24cxx_cli::TransferStats stats{};
  QueueHandle_t input = nullptr;
  eeprom24cxx_cli::Cli cli{};
} app;
constexpr eeprom24cxx_cli::IdfResultMapper resultMapper{
    ESP_OK, ESP_ERR_TIMEOUT, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_RESPONSE, ESP_ERR_NOT_FOUND};
TransportResult finish(esp_err_t error, size_t tx, size_t rx, bool memoryWrite) {
  app.stats.record(error == ESP_OK);
  auto& count = memoryWrite ? app.stats.writeAttempts : app.stats.readAttempts;
  if (count != UINT32_MAX) ++count;
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
  app.stats.record(result == ESP_OK);
  if (app.stats.probeAttempts != UINT32_MAX) ++app.stats.probeAttempts;
  return resultMapper.probe(result);
}
Status probeAddress(uint8_t address, void*) {
  const auto result = probeI2c(address, board::I2C_TIMEOUT_MS, nullptr);
  if (result.ok()) return Status::Ok();
  return Status::Error(result.code == TransportCode::NACK_ADDRESS ? Err::I2C_NACK_ADDR :
      result.code == TransportCode::TIMEOUT ? Err::I2C_TIMEOUT : Err::I2C_ERROR, "IDF address probe", result.detail);
}
uint32_t nowMs(void*) { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
void output(void*, const char* format, va_list args) { std::vprintf(format, args); }
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
void inputTask(void*) {
  while (true) {
    const int value = std::getchar();
    if (value == EOF) { std::clearerr(stdin); vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    const char character = static_cast<char>(value);
    // Backpressure prevents dropped input from changing a write's arguments.
    (void)xQueueSend(app.input, &character, portMAX_DELAY);
  }
}
}  // namespace

extern "C" void app_main() {
  const auto protectedState = eeprom24cxx_cli::initializeWriteProtect();
  if (!protectedState.ok()) { std::printf("[E] %s\n", protectedState.msg); return; }
  const Status initialized = initializeInterface();
  if (!initialized.ok()) { std::printf("[E] %s detail=%ld\n", initialized.msg, static_cast<long>(initialized.detail)); return; }
  app.input = xQueueCreate(384, sizeof(char));
  if (!app.input || xTaskCreate(inputTask, "eeprom_input", 3072, nullptr, 4, nullptr) != pdPASS) {
    std::puts("[E] Input queue/task creation failed");
    if (app.input) vQueueDelete(app.input);
    (void)releaseInterface(); return;
  }
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
  platform.framework = "native-esp-idf";
  platform.frameworkVersion = esp_get_idf_version();
  platform.target = CONFIG_IDF_TARGET;
  app.cli.setup(platform, config);
  while (true) {
    char value = 0;
    for (unsigned count = 0; count < 64 && xQueueReceive(app.input, &value, 0) == pdTRUE; ++count) app.cli.feed(value);
    app.cli.tick();
    vTaskDelay(1);
  }
}
