# EEPROM24Cxx

Framework-neutral C++17 I2C EEPROM driver for **Zetta ZD24C02B-MAGMT** and
explicit 24Cxx-family memory layouts. The Zetta default is 256 bytes, 8-byte
pages, an 8-bit memory pointer and a 5 ms maximum programming cycle.

The closest local template is **MB85RC** for memory operations, typed transport,
write-effect evidence and application-owned I2C. CLI help, colors and common
commands follow the sibling libraries; Arduino and native ESP-IDF share one
command processor. See the [library comparison](docs/library-comparison.md).

- Byte-addressed reads, page/bank-aware writes, fill and readback verification.
- Exact Zetta preset; common C01 through C512 layouts, explicit Microchip
  24LC1025 and ST M24M01 layouts, and validated custom geometry.
- Bus-silent operation admission and bounded owner-driven polling.
- Fixed buffers, typed Status/results, passive health and partial-write evidence.
- Optional address-only ACK polling; conservative timed completion otherwise.
- Application owns bus initialization, timing, locking, WP and recovery.
- ESP32-S2/S3 Arduino and native ESP-IDF examples; no framework in core headers.

24Cxx devices are EEPROM memory, not temperature sensors. This repository
replaces the accidentally requested TMP1x2 implementation; its original commit
and separately archived references remain available. See [provenance](docs/provenance.md).

## Integration

```cpp
#include <EEPROM24Cxx/EEPROM24Cxx.h>

EEPROM24Cxx::EEPROM24Cxx memory;
uint8_t bytes[16]; // Remains valid until the operation finishes.

void initializeMemory() {
  EEPROM24Cxx::Config config;
  config.i2cWrite = applicationWrite;       // Terminal TransportResult callbacks
  config.i2cWriteRead = applicationWriteRead;
  config.i2cProbe = applicationAddressProbe; // Optional SLA+W/ACK/STOP only
  config.i2cUser = &applicationBus;
  config.nowMs = applicationClock;
  config.timeUser = &applicationClockState;
  config.variant = EEPROM24Cxx::DeviceVariant::ZETTA_ZD24C02B;
  config.i2cAddress = 0x50;                 // Actual A2:A0 board straps
  config.maxTxBytes = 32;                  // Includes the pointer byte(s)
  config.maxRxBytes = 32;
  auto status = memory.bind(config);        // No I2C, does not identify the chip
  if (status.ok()) status = memory.startRead(0, bytes, sizeof(bytes), 1000);
  handleAdmission(status);                 // OK means admitted, not completed
}

void serviceMemory(uint32_t nowMs) {
  (void)memory.poll(nowMs, 1);              // At most one physical callback
  EEPROM24Cxx::TransferResult result;
  if (memory.takeResult(result).ok()) {
    consumeResult(result, bytes);          // Consume every terminal result once
  }
}
```

Implement the named application callbacks and handlers; they are integration
points, not library-provided functions. Call the driver from one serialized
owner task. Every callback must enforce its timeout, complete before returning,
and report exact successful byte counts. Never retain callback stack buffers.
The clock hook and `poll()` argument must use the same wrapping millisecond domain.
Nonzero logical deadlines require the clock hook. Clockless owners use a zero
logical deadline and may cancel through their own scheduling policy.

For an explicit programming operation, call
`startWrite(address, data, length, true, timeoutMs)` to enable readback verification,
then service and consume it the same way. Keep `data` unchanged until completion.
Writes are split at page/bank/transport limits and never automatically replayed.
After a failed or cancelled write, inspect accepted/completed/verified byte
counts and write-commit evidence before deciding what to do next.

Read the [ownership and completion contract](docs/integration.md) before writing
an adapter. WP-high may acknowledge a write while preserving old contents;
only readback proves the requested data is present. There is no general device
ID, capacity-detection command, register map or erase instruction.

## Build and test

```sh
cmake -S . -B build-eeprom -DEEPROM24CXX_BUILD_TESTS=ON
cmake --build build-eeprom
ctest --test-dir build-eeprom --output-on-failure
python tools/check_contracts.py
python tools/check_reference_archive.py
```

On Windows use Ninja (`-G Ninja`) or MinGW Makefiles. The existing VS Code-managed
PlatformIO installation is used through the repository wrapper:

```powershell
.\scripts\pio.cmd test -e native
.\scripts\pio.cmd run -e native_core_no_arduino
.\scripts\pio.cmd run -e esp32s3dev -e esp32s2dev
.\scripts\pio.cmd run --project-dir examples/esp_idf/basic -e esp32s3 -e esp32s2
```

Native ESP-IDF 5.3 or newer, using `driver/i2c_master.h`, also supports:

```sh
idf.py -C examples/esp_idf/basic set-target esp32s3 build
```

The IDF example resolves its component dependency from the checkout folder name,
so `24Cxx` and renamed package checkouts work. `library.json` is the version source;
run `python scripts/generate_version.py sync` after changing it.

Configure example pins in `examples/common/BoardConfig.h` for your board. Startup
and ordinary diagnostics never program EEPROM. Use the explicit CLI memory-write
commands only for data you intend to change; the driver owns no persistence policy.

## References and evidence

[Manufacturer datasheets and source research](docs/reference/README.md) include
local reference files, revisions, URLs and checksums. Generic family layouts are
not interchangeable across every manufacturer: verify the fitted part's page
size, bank-bit placement, write time, address pins and supply limits.

[Validation results](docs/validation.md) distinguish host tests and firmware
builds from [physical hardware validation](docs/hardware-validation.md).
