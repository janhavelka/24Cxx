// Native ESP-IDF: one owner task calls the driver; input task queues bytes only.
#include <cstdarg>
#include <cstdio>
#include <climits>
#include <driver/i2c_master.h>
#include <esp_err.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include "BoardConfig.h"
#include "Eeprom24CxxCli.h"

namespace {
using namespace EEPROM24Cxx;
struct App {
  i2c_master_bus_handle_t bus = nullptr;
  i2c_master_dev_handle_t devices[8]{};
  eeprom24cxx_cli::TransferStats stats{};
  QueueHandle_t input = nullptr;
  eeprom24cxx_cli::Cli cli{};
} app;
TransportResult finish(esp_err_t error, size_t tx = 0, size_t rx = 0) {
  app.stats.record(error == ESP_OK);
  if (error == ESP_OK) return TransportResult::Ok(tx, rx);
  // IDF transaction errors do not identify NACK phase or accepted byte count.
  // Never fabricate NOT_COMMITTED or full acceptance from an SDK error alone.
  return TransportResult::Error(error == ESP_ERR_TIMEOUT ? TransportCode::TIMEOUT : TransportCode::IO_ERROR, error);
}
TransportResult writeI2c(uint8_t address, const uint8_t* data, size_t length,
                         uint32_t timeoutMs, void*) {
  if (address < 0x50 || address > 0x57 || !data || !length || !timeoutMs || timeoutMs > INT_MAX)
    return TransportResult::Error(TransportCode::IO_ERROR, ESP_ERR_INVALID_ARG, WriteCommit::NOT_COMMITTED);
  return finish(i2c_master_transmit(app.devices[address - 0x50], data, length, static_cast<int>(timeoutMs)), length);
}
TransportResult readI2c(uint8_t address, const uint8_t* tx, size_t txLength,
                        uint8_t* rx, size_t rxLength, uint32_t timeoutMs, void*) {
  if (address < 0x50 || address > 0x57 || !tx || !txLength || !rx || !rxLength || !timeoutMs || timeoutMs > INT_MAX)
    return TransportResult::Error(TransportCode::IO_ERROR, ESP_ERR_INVALID_ARG, WriteCommit::NOT_APPLICABLE);
  return finish(i2c_master_transmit_receive(app.devices[address - 0x50], tx, txLength, rx, rxLength,
                                           static_cast<int>(timeoutMs)), txLength, rxLength);
}
TransportResult probeI2c(uint8_t address, uint32_t timeoutMs, void*) {
  const esp_err_t result = i2c_master_probe(app.bus, address, static_cast<int>(timeoutMs));
  app.stats.record(result == ESP_OK);
  if (result == ESP_OK) return TransportResult::Ok(0, 0);
  // The address-only probe can reliably identify address NACK (including tWR).
  return TransportResult::Error(result == ESP_ERR_NOT_FOUND ? TransportCode::NACK_ADDRESS :
      result == ESP_ERR_TIMEOUT ? TransportCode::TIMEOUT : TransportCode::IO_ERROR, result,
      WriteCommit::NOT_APPLICABLE);
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
  i2c_master_bus_config_t bus{};
  bus.i2c_port = I2C_NUM_0;
  bus.sda_io_num = static_cast<gpio_num_t>(board::I2C_SDA);
  bus.scl_io_num = static_cast<gpio_num_t>(board::I2C_SCL);
  bus.clk_source = I2C_CLK_SRC_DEFAULT;
  bus.glitch_ignore_cnt = 7;
  bus.flags.enable_internal_pullup = true;
  esp_err_t error = i2c_new_master_bus(&bus, &app.bus);
  if (error != ESP_OK) { std::printf("[E] Bus creation failed: %s\n", esp_err_to_name(error)); return; }
  // Every 0x50..0x57 address has a handle, including bank aliases. Device handle
  // registration is local SDK setup and performs no EEPROM access.
  for (unsigned index = 0; index < 8; ++index) {
    i2c_device_config_t device{};
    device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device.device_address = static_cast<uint16_t>(0x50U + index);
    device.scl_speed_hz = board::I2C_FREQUENCY_HZ;
    error = i2c_master_bus_add_device(app.bus, &device, &app.devices[index]);
    if (error != ESP_OK) {
      std::printf("[E] Device handle creation failed: %s\n", esp_err_to_name(error));
      for (unsigned previous = 0; previous < index; ++previous) (void)i2c_master_bus_rm_device(app.devices[previous]);
      (void)i2c_del_master_bus(app.bus); return;
    }
  }
  app.input = xQueueCreate(384, sizeof(char));
  if (!app.input || xTaskCreate(inputTask, "eeprom_input", 3072, nullptr, 4, nullptr) != pdPASS) {
    std::puts("[E] Input queue/task creation failed");
    if (app.input) vQueueDelete(app.input);
    for (auto device : app.devices) (void)i2c_master_bus_rm_device(device);
    (void)i2c_del_master_bus(app.bus); return;
  }
  EEPROM24Cxx::Config config{};
  config.i2cWrite = writeI2c;
  config.i2cWriteRead = readI2c;
  config.i2cProbe = probeI2c;
  config.nowMs = nowMs;
  config.i2cTimeoutMs = board::I2C_TIMEOUT_MS;
  eeprom24cxx_cli::Platform platform{};
  platform.vprintf = output;
  platform.nowMs = nowMs;
  platform.probeAddress = probeAddress;
  platform.transferStats = stats;
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
